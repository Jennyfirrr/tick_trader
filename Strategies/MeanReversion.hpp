//======================================================================================================
// [MEAN REVERSION STRATEGY]
//======================================================================================================
// buys dips below the rolling average, sells at fixed TP/SL per position
// adaptive filters (entry offset and volume multiplier) are adjusted by P&L regression:
//   positive P&L slope -> loosen filters (buy more aggressively)
//   negative P&L slope -> tighten filters (buy more defensively)
//
// two offset modes (selected by config, branchless mask-select):
//   percentage mode: buy_price = avg - (avg * offset_pct)        [default]
//   stddev mode:     buy_price = avg - (stddev * offset_mult)    [volatility-aware]
//
// idle squeeze: when portfolio is empty and price runs away from buy gate, the filters
// squeeze toward their minimum so the gate catches up to the market. prevents the strategy
// from sitting idle forever after a breakout.
//
// multi-timeframe gate: optionally requires the long-window (512 tick) price slope to be
// non-negative before allowing buys. prevents buying short-term dips inside broader crashes.
//
// all adaptation is branchless on the hot path. the strategy only runs on the slow path
// (every poll_interval ticks). BuyGate on the hot path just reads the conditions.
//======================================================================================================
#ifndef MEAN_REVERSION_HPP
#define MEAN_REVERSION_HPP

#include "StrategyInterface.hpp"
#include "../CoreFrameworks/ControllerConfig.hpp"
#include "../CoreFrameworks/OrderGates.hpp"
#include "../ML_Headers/LinearRegression3X.hpp"
#include "../ML_Headers/ROR_regressor.hpp"
#include "../ML_Headers/RollingStats.hpp"

//======================================================================================================
// [STATE]
//======================================================================================================
template <unsigned F> struct MeanReversionState {
    RegressionFeederX<F> feeder;     // P&L regression ring buffer
    RORRegressor<F> ror;             // slope-of-slopes (second derivative of P&L)
    FPN<F> live_offset_pct;          // adaptive entry offset (between offset_min..offset_max)
    FPN<F> live_vol_mult;            // adaptive volume multiplier (between vol_mult_min..vol_mult_max)
    FPN<F> live_stddev_mult;         // adaptive stddev multiplier (between offset_stddev_min..max)
    BuySideGateConditions<F> buy_conds_initial; // anchor for max_shift clamp, tracks rolling avg
    LinearRegression3XResult<F> last_regression; // stored for BuySignal to apply gate shift
    int has_regression;              // 1 if last_regression is valid, 0 otherwise
};

//======================================================================================================
// [INIT]
//======================================================================================================
// called once at warmup completion. computes initial buy conditions from rolling stats
// and resets the regression feeder for P&L tracking in the active phase.
// handles both percentage and stddev offset modes via branchless select.
//======================================================================================================
template <unsigned F>
inline void MeanReversion_Init(MeanReversionState<F> *state,
                                const RollingStats<F> *rolling,
                                BuySideGateConditions<F> *buy_conds) {
    constexpr unsigned N = FPN<F>::N;

    // compute initial buy price in both modes, select based on which is active
    int use_stddev = !FPN_IsZero(state->live_stddev_mult);

    FPN<F> pct_price = RollingStats_BuyPrice(rolling, state->live_offset_pct);
    FPN<F> stddev_offset = FPN_Mul(rolling->price_stddev, state->live_stddev_mult);
    FPN<F> stddev_price = FPN_Sub(rolling->price_avg, stddev_offset);

    // branchless select
    uint64_t sm = -(uint64_t)use_stddev;
    FPN<F> buy_price;
    for (unsigned i = 0; i < N; i++) {
        buy_price.w[i] = (stddev_price.w[i] & sm) | (pct_price.w[i] & ~sm);
    }
    buy_price.sign = (stddev_price.sign & use_stddev) | (pct_price.sign & !use_stddev);

    // volume gate uses rolling avg * multiplier so only significant trades pass
    FPN<F> buy_vol = FPN_Mul(rolling->volume_avg, state->live_vol_mult);

    buy_conds->price = buy_price;
    buy_conds->volume = buy_vol;
    state->buy_conds_initial = *buy_conds;

    // reset feeder/ROR for P&L tracking in active phase
    state->feeder = RegressionFeederX_Init<F>();
    state->ror = RORRegressor_Init<F>();
    state->has_regression = 0;
}

//======================================================================================================
// [ADAPT]
//======================================================================================================
// called every slow-path tick. three responsibilities:
//   1. idle squeeze - loosen filters when portfolio is empty and price is running away
//   2. feeder push - feed P&L into regression buffer
//   3. regression + filter adjustment - adapt offset/vol_mult based on P&L trend
//
// offset mode conditioning: only the ACTIVE mode's offset (percentage or stddev) is
// adapted. the inactive mode's value stays frozen at its init/reload value. this prevents
// idle squeeze and regression from drifting the inactive value (e.g. squeezing stddev_mult
// from 0 to offset_stddev_min while in percentage mode).
//======================================================================================================
template <unsigned F>
inline void MeanReversion_Adapt(MeanReversionState<F> *state,
                                 FPN<F> current_price,
                                 FPN<F> portfolio_delta,
                                 uint16_t active_bitmap,
                                 const BuySideGateConditions<F> *buy_conds,
                                 const ControllerConfig<F> *cfg) {
    constexpr unsigned N = FPN<F>::N;

    // mode detection: stddev if offset_stddev_mult > 0 in config
    int use_stddev = !FPN_IsZero(cfg->offset_stddev_mult);

    //==================================================================================================
    // IDLE SQUEEZE: when portfolio is empty, use price slope to loosen/tighten filters
    // solves the chicken-and-egg problem: no positions -> no P&L -> no adaptation
    // if price is trending up and we have nothing, we're missing the move -> squeeze offset down
    // if price is trending down and we have nothing, we're correctly staying out -> no change
    //
    // branchless: empty_mask is all 1s when portfolio empty, all 0s when holding
    // positive price slope -> squeeze offset toward offset_min
    // the squeeze rate is proportional to the slope magnitude
    //
    // mode-conditional: only squeeze the active mode's offset value
    //==================================================================================================
    {
        int is_empty = (active_bitmap == 0);
        int trailing = FPN_GreaterThan(current_price, buy_conds->price);

        // squeeze fires when: portfolio empty AND current price above buy gate
        int should_squeeze = is_empty & trailing;
        uint64_t sq_mask = -(uint64_t)should_squeeze;

        // mode-conditional squeeze masks
        uint64_t pct_sq_mask    = sq_mask & -(uint64_t)(!use_stddev);
        uint64_t stddev_sq_mask = sq_mask & -(uint64_t)use_stddev;

        // PERCENTAGE MODE: squeeze offset toward zero (not offset_min — go all the way)
        FPN<F> zero = FPN_Zero<F>();
        FPN<F> squeeze_step = FPN_Mul(state->live_offset_pct, FPN_FromDouble<F>(0.10));
        FPN<F> masked_squeeze;
        for (unsigned i = 0; i < N; i++) {
            masked_squeeze.w[i] = squeeze_step.w[i] & pct_sq_mask;
        }
        masked_squeeze.sign = squeeze_step.sign & (should_squeeze & !use_stddev);

        state->live_offset_pct = FPN_SubSat(state->live_offset_pct, masked_squeeze);
        state->live_offset_pct = FPN_Max(state->live_offset_pct, zero); // floor at zero, not offset_min

        // STDDEV MODE: squeeze stddev_mult toward offset_stddev_min
        // uses gap-based decay: step = (current - min) * 0.10
        FPN<F> stddev_gap = FPN_Sub(state->live_stddev_mult, cfg->offset_stddev_min);
        FPN<F> stddev_step = FPN_Mul(stddev_gap, FPN_FromDouble<F>(0.10));
        FPN<F> masked_stddev;
        for (unsigned i = 0; i < N; i++) {
            masked_stddev.w[i] = stddev_step.w[i] & stddev_sq_mask;
        }
        masked_stddev.sign = stddev_step.sign & (should_squeeze & use_stddev);

        state->live_stddev_mult = FPN_SubSat(state->live_stddev_mult, masked_stddev);
        state->live_stddev_mult = FPN_Max(state->live_stddev_mult, cfg->offset_stddev_min);

        // squeeze volume multiplier toward 1.0 (accept any trade size) — both modes
        FPN<F> one = FPN_FromDouble<F>(1.0);
        FPN<F> vmult_gap = FPN_Sub(state->live_vol_mult, one);
        FPN<F> vmult_step = FPN_Mul(vmult_gap, FPN_FromDouble<F>(0.10));
        FPN<F> masked_vmult;
        for (unsigned i = 0; i < N; i++) {
            masked_vmult.w[i] = vmult_step.w[i] & sq_mask;
        }
        masked_vmult.sign = vmult_step.sign & should_squeeze;

        state->live_vol_mult = FPN_SubSat(state->live_vol_mult, masked_vmult);
        state->live_vol_mult = FPN_Max(state->live_vol_mult, one); // floor at 1.0x
    }

    //==================================================================================================
    // FEEDER PUSH: feed unrealized P&L into regression buffer
    //==================================================================================================
    RegressionFeederX_Push(&state->feeder, portfolio_delta);

    //==================================================================================================
    // REGRESSION + ADAPTIVE FILTER ADJUSTMENT
    // positive slope (making money) -> loosen filters (smaller offset, lower vol mult)
    // negative slope (losing money) -> tighten filters (larger offset, higher vol mult)
    //
    // the shift direction is INVERTED from buy price adjustment:
    // - buy price: positive P&L -> shift price UP (buy more aggressively)
    // - offset: positive P&L -> shift offset DOWN (require less dip)
    // - vol mult: positive P&L -> shift mult DOWN (accept smaller trades)
    //
    // mode-conditional: offset_pct and stddev_mult are adapted independently,
    // only the active mode's value changes. prevents drift of inactive values.
    //
    // all branchless: same R^2 confidence mask, same clamp pattern
    //==================================================================================================
    state->has_regression = 0;
    if (state->feeder.count >= MAX_WINDOW) {
        LinearRegression3XResult<F> inner = RegressionFeederX_Compute(&state->feeder);
        RORRegressor_Push(&state->ror, inner);
        state->last_regression = inner;
        state->has_regression = 1;

        int confident = FPN_GreaterThanOrEqual(inner.r_squared, cfg->r2_threshold);
        uint64_t conf_mask = -(uint64_t)confident;

        // mode-conditional regression masks
        uint64_t pct_reg_mask    = conf_mask & -(uint64_t)(!use_stddev);
        uint64_t stddev_reg_mask = conf_mask & -(uint64_t)use_stddev;

        // compute filter shift from slope: negate because positive P&L -> loosen (decrease)
        FPN<F> filter_shift = FPN_Mul(inner.model.slope, cfg->filter_scale);
        FPN<F> neg_shift = FPN_Negate(filter_shift);

        // PERCENTAGE MODE: apply shift to offset_pct, scale 0.001, clamp [offset_min, offset_max]
        {
            FPN<F> masked_pct_shift;
            for (unsigned i = 0; i < N; i++) {
                masked_pct_shift.w[i] = neg_shift.w[i] & pct_reg_mask;
            }
            masked_pct_shift.sign = neg_shift.sign & (confident & !use_stddev);

            FPN<F> offset_scale = FPN_FromDouble<F>(0.001);
            FPN<F> offset_shift = FPN_Mul(masked_pct_shift, offset_scale);
            state->live_offset_pct = FPN_AddSat(state->live_offset_pct, offset_shift);
            state->live_offset_pct = FPN_Max(state->live_offset_pct, cfg->offset_min);
            state->live_offset_pct = FPN_Min(state->live_offset_pct, cfg->offset_max);
        }

        // STDDEV MODE: apply shift to stddev_mult, scale 0.1, clamp [stddev_min, stddev_max]
        // scale is 100x larger than percentage mode because stddev_mult ranges 0.5-4.0
        // while offset_pct ranges 0.0005-0.005 (~1000x difference in magnitude)
        {
            FPN<F> masked_stddev_shift;
            for (unsigned i = 0; i < N; i++) {
                masked_stddev_shift.w[i] = neg_shift.w[i] & stddev_reg_mask;
            }
            masked_stddev_shift.sign = neg_shift.sign & (confident & use_stddev);

            FPN<F> stddev_adapt_scale = FPN_FromDouble<F>(0.1);
            FPN<F> stddev_shift = FPN_Mul(masked_stddev_shift, stddev_adapt_scale);
            state->live_stddev_mult = FPN_AddSat(state->live_stddev_mult, stddev_shift);
            state->live_stddev_mult = FPN_Max(state->live_stddev_mult, cfg->offset_stddev_min);
            state->live_stddev_mult = FPN_Min(state->live_stddev_mult, cfg->offset_stddev_max);
        }

        // apply shift to volume multiplier and clamp to [vol_mult_min, vol_mult_max] — both modes
        FPN<F> masked_shift;
        for (unsigned i = 0; i < N; i++) {
            masked_shift.w[i] = neg_shift.w[i] & conf_mask;
        }
        masked_shift.sign = neg_shift.sign & confident;

        FPN<F> vol_shift = FPN_Mul(masked_shift, FPN_FromDouble<F>(0.1));
        state->live_vol_mult = FPN_AddSat(state->live_vol_mult, vol_shift);
        state->live_vol_mult = FPN_Max(state->live_vol_mult, cfg->vol_mult_min);
        state->live_vol_mult = FPN_Min(state->live_vol_mult, cfg->vol_mult_max);
    }
}

//======================================================================================================
// [BUY SIGNAL]
//======================================================================================================
// called every slow-path tick after Adapt. computes buy gate conditions from rolling stats
// using the current adaptive filter values, then applies regression-based gate shift if
// available. optionally gates on long-window trend for multi-timeframe confirmation.
// returns the conditions for the engine to write to ctrl->buy_conds.
//======================================================================================================
template <unsigned F>
inline BuySideGateConditions<F> MeanReversion_BuySignal(MeanReversionState<F> *state,
                                                         const RollingStats<F> *rolling,
                                                         const RollingStats<F, 512> *rolling_long,
                                                         const ControllerConfig<F> *cfg) {
    constexpr unsigned N = FPN<F>::N;
    BuySideGateConditions<F> conds;

    // compute base buy price in both modes, branchless select the active one
    // percentage mode: buy_price = avg - (avg * offset_pct)
    // stddev mode:     buy_price = avg - (stddev * offset_mult) — scales with volatility
    int use_stddev = !FPN_IsZero(cfg->offset_stddev_mult);

    FPN<F> pct_price = RollingStats_BuyPrice(rolling, state->live_offset_pct);
    FPN<F> stddev_offset = FPN_Mul(rolling->price_stddev, state->live_stddev_mult);
    FPN<F> stddev_price = FPN_Sub(rolling->price_avg, stddev_offset);

    uint64_t sm = -(uint64_t)use_stddev;
    for (unsigned i = 0; i < N; i++) {
        conds.price.w[i] = (stddev_price.w[i] & sm) | (pct_price.w[i] & ~sm);
    }
    conds.price.sign = (stddev_price.sign & use_stddev) | (pct_price.sign & !use_stddev);

    conds.volume = FPN_Mul(rolling->volume_avg, state->live_vol_mult);

    // update initial conditions to track the market - prevents the gate shift clamp
    // from anchoring to stale warmup prices. the clamp window (max_shift) moves with
    // the rolling average so the gate stays in the current price neighborhood
    state->buy_conds_initial = conds;

    // apply regression-based gate shift if available
    // shifts buy price condition based on regression slope, masked by R^2 confidence,
    // clamped to max_shift from initial conditions - all branchless
    // works identically in both offset modes — it's an absolute price shift
    if (state->has_regression) {
        int confident = FPN_GreaterThanOrEqual(state->last_regression.r_squared,
                                                cfg->r2_threshold);

        FPN<F> shift = FPN_Mul(state->last_regression.model.slope, cfg->slope_scale_buy);

        // clamp shift magnitude to max_shift (Min/Max are already branchless)
        shift = FPN_Min(shift, cfg->max_shift);
        shift = FPN_Max(shift, FPN_Negate(cfg->max_shift));

        // mask shift to zero if not confident - word-level branchless mask
        FPN<F> masked_shift;
        uint64_t conf_mask = -(uint64_t)confident;
        #pragma GCC unroll 65534
        for (unsigned i = 0; i < N; i++) {
            masked_shift.w[i] = shift.w[i] & conf_mask;
        }
        masked_shift.sign = shift.sign & confident;

        // apply shift
        FPN<F> new_price = FPN_AddSat(conds.price, masked_shift);

        // clamp to initial +/- max_shift
        FPN<F> upper = FPN_AddSat(state->buy_conds_initial.price, cfg->max_shift);
        FPN<F> lower = FPN_SubSat(state->buy_conds_initial.price, cfg->max_shift);
        new_price = FPN_Max(new_price, lower);
        new_price = FPN_Min(new_price, upper);

        conds.price = new_price;
    }

    //==================================================================================================
    // MULTI-TIMEFRAME GATE: require long-window trend to be flat or rising
    // when enabled (min_long_slope > 0), masks buy_conds.price to zero if the 512-tick
    // window shows a negative slope — blocks buying dips inside broader crashes
    // when disabled (min_long_slope = 0), gate always passes
    //==================================================================================================
    {
        int long_enabled = !FPN_IsZero(cfg->min_long_slope);
        int long_pass = FPN_GreaterThanOrEqual(rolling_long->price_slope, cfg->min_long_slope);
        int long_ok = long_pass | !long_enabled;

        uint64_t signal_mask = -(uint64_t)long_ok;
        for (unsigned i = 0; i < N; i++) {
            conds.price.w[i] &= signal_mask;
        }
        conds.price.sign &= long_ok;
    }

    return conds;
}

//======================================================================================================
//======================================================================================================
#endif // MEAN_REVERSION_HPP
