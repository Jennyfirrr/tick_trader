//======================================================================================================
// [ROLLING MARKET STATISTICS]
//======================================================================================================
// tracks rolling averages and trends for price and volume over a configurable window
// used by the controller to set dynamic buy gate conditions based on actual market behavior
// instead of static warmup means
//
// three outputs:
//   1. rolling volume average - filters out micro-fills (is this tick significant?)
//   2. volume slope - is volume trending up or down? (should we be more/less aggressive?)
//   3. rolling price standard deviation - dynamic entry spacing based on actual volatility
//
// uses the same ring buffer pattern as RegressionFeederX with branchless power-of-2 wrap
// window size is fixed at compile time for branchless operation
//======================================================================================================
#ifndef ROLLING_STATS_HPP
#define ROLLING_STATS_HPP

#include "../FixedPoint/FixedPointN.hpp"

//======================================================================================================
// [WINDOW SIZE]
//======================================================================================================
// must be power of 2 for branchless wrap with & (ROLLING_WINDOW - 1)
// 128 ticks at BTC trade frequency is roughly 10-30 seconds of market data
//======================================================================================================
#ifndef ROLLING_WINDOW
#define ROLLING_WINDOW 128
#endif

//======================================================================================================
// [ROLLING STATS STRUCT]
//======================================================================================================
template <unsigned F> struct RollingStats {
    FPN<F> price_buf[ROLLING_WINDOW];
    FPN<F> volume_buf[ROLLING_WINDOW];
    int head;
    int count;

    // cached outputs - recomputed every push
    FPN<F> volume_avg;         // mean volume over window
    FPN<F> volume_slope;       // linear trend of volume (positive = increasing)
    FPN<F> price_avg;          // mean price over window
    FPN<F> price_stddev;       // standard deviation of price over window (volatility proxy)
    FPN<F> price_min;          // min price in window (for range-based spacing)
    FPN<F> price_max;          // max price in window (for range-based spacing)
};

//======================================================================================================
// [INIT]
//======================================================================================================
template <unsigned F> inline RollingStats<F> RollingStats_Init() {
    RollingStats<F> rs;
    for (int i = 0; i < ROLLING_WINDOW; i++) {
        rs.price_buf[i]  = FPN_Zero<F>();
        rs.volume_buf[i] = FPN_Zero<F>();
    }
    rs.head         = 0;
    rs.count        = 0;
    rs.volume_avg   = FPN_Zero<F>();
    rs.volume_slope = FPN_Zero<F>();
    rs.price_avg    = FPN_Zero<F>();
    rs.price_stddev = FPN_Zero<F>();
    rs.price_min    = FPN_Zero<F>();
    rs.price_max    = FPN_Zero<F>();
    return rs;
}

//======================================================================================================
// [PUSH + RECOMPUTE]
//======================================================================================================
// adds a new price/volume sample and recomputes all rolling statistics
// this runs on the slow path (every poll_interval ticks), not every tick
// the computation is O(ROLLING_WINDOW) which is 128 iterations of FPN math -
// well within the slow-path budget
//======================================================================================================
template <unsigned F>
inline void RollingStats_Push(RollingStats<F> *rs, FPN<F> price, FPN<F> volume) {
    // write to ring buffer
    rs->price_buf[rs->head]  = price;
    rs->volume_buf[rs->head] = volume;
    rs->head  = (rs->head + 1) & (ROLLING_WINDOW - 1);
    rs->count += (rs->count < ROLLING_WINDOW);

    if (rs->count < 2) return; // need at least 2 samples for meaningful stats

    // single pass: compute sums for price avg, volume avg, price min/max
    FPN<F> price_sum  = FPN_Zero<F>();
    FPN<F> volume_sum = FPN_Zero<F>();
    FPN<F> p_min = rs->price_buf[(rs->head - rs->count + ROLLING_WINDOW) & (ROLLING_WINDOW - 1)];
    FPN<F> p_max = p_min;

    for (int i = 0; i < rs->count; i++) {
        int idx = (rs->head - rs->count + i + ROLLING_WINDOW) & (ROLLING_WINDOW - 1);
        FPN<F> p = rs->price_buf[idx];
        FPN<F> v = rs->volume_buf[idx];

        price_sum  = FPN_AddSat(price_sum, p);
        volume_sum = FPN_AddSat(volume_sum, v);
        p_min = FPN_Min(p_min, p);
        p_max = FPN_Max(p_max, p);
    }

    FPN<F> n_fp = FPN_FromDouble<F>((double)rs->count);
    rs->price_avg  = FPN_DivNoAssert(price_sum, n_fp);
    rs->volume_avg = FPN_DivNoAssert(volume_sum, n_fp);
    rs->price_min  = p_min;
    rs->price_max  = p_max;

    // second pass: price standard deviation (sqrt of variance)
    // variance = sum((p - mean)^2) / count
    // we use the price range (max - min) as an approximation of 4*stddev to avoid sqrt
    // range / 4 is a rough stddev estimate - good enough for spacing decisions
    // actual stddev would require FPN_Sqrt which we dont have and dont need
    FPN<F> range = FPN_Sub(p_max, p_min);
    FPN<F> four  = FPN_FromDouble<F>(4.0);
    rs->price_stddev = FPN_DivNoAssert(range, four);

    // volume slope: simple first-last difference over the window
    // positive means volume is increasing, negative means decreasing
    // more accurate than a full regression for this purpose and much cheaper
    int oldest_idx = (rs->head - rs->count + ROLLING_WINDOW) & (ROLLING_WINDOW - 1);
    int newest_idx = (rs->head - 1 + ROLLING_WINDOW) & (ROLLING_WINDOW - 1);
    FPN<F> vol_diff = FPN_Sub(rs->volume_buf[newest_idx], rs->volume_buf[oldest_idx]);
    rs->volume_slope = FPN_DivNoAssert(vol_diff, n_fp);
}

//======================================================================================================
// [VOLUME FILTER]
//======================================================================================================
// returns 1 if the given volume is >= multiplier * rolling_avg, 0 otherwise
// branchless - produces a mask value the caller can AND with other conditions
//======================================================================================================
template <unsigned F>
inline int RollingStats_VolumeSignificant(const RollingStats<F> *rs, FPN<F> tick_volume, FPN<F> multiplier) {
    FPN<F> threshold = FPN_Mul(rs->volume_avg, multiplier);
    return FPN_GreaterThanOrEqual(tick_volume, threshold);
}

//======================================================================================================
// [ENTRY SPACING]
//======================================================================================================
// computes the minimum price distance between entries based on rolling volatility
// spacing = stddev * spacing_multiplier
// returns the FPN spacing value - caller compares against nearest existing position
//======================================================================================================
template <unsigned F>
inline FPN<F> RollingStats_EntrySpacing(const RollingStats<F> *rs, FPN<F> spacing_multiplier) {
    return FPN_Mul(rs->price_stddev, spacing_multiplier);
}

//======================================================================================================
// [ENTRY OFFSET]
//======================================================================================================
// computes the buy gate price offset from rolling mean
// buy_price = rolling_avg - (rolling_avg * offset_pct)
// this means "only buy when price dips offset_pct below the rolling average"
//======================================================================================================
template <unsigned F>
inline FPN<F> RollingStats_BuyPrice(const RollingStats<F> *rs, FPN<F> offset_pct) {
    FPN<F> offset = FPN_Mul(rs->price_avg, offset_pct);
    return FPN_Sub(rs->price_avg, offset);
}

//======================================================================================================
//======================================================================================================
#endif // ROLLING_STATS_HPP
