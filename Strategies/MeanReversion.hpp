//======================================================================================================
// [MEAN REVERSION STRATEGY]
//======================================================================================================
// buys dips below the rolling average, sells at fixed TP/SL per position
// adaptive filters (entry offset and volume multiplier) are adjusted by P&L regression:
//   positive P&L slope -> loosen filters (buy more aggressively)
//   negative P&L slope -> tighten filters (buy more defensively)
//
// idle squeeze: when portfolio is empty and price runs away from buy gate, the filters
// squeeze toward zero so the gate catches up to the market. prevents the strategy from
// sitting idle forever after a breakout.
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
    BuySideGateConditions<F> buy_conds_initial; // anchor for max_shift clamp, tracks rolling avg
    LinearRegression3XResult<F> last_regression; // stored for BuySignal to apply gate shift
    int has_regression;              // 1 if last_regression is valid, 0 otherwise
};

//======================================================================================================
// [INIT]
//======================================================================================================
// called once at warmup completion. computes initial buy conditions from rolling stats
// and resets the regression feeder for P&L tracking in the active phase.
//======================================================================================================
template <unsigned F>
inline void MeanReversion_Init(MeanReversionState<F> *state,
                                const RollingStats<F> *rolling,
                                BuySideGateConditions<F> *buy_conds) {
    // use rolling stats for initial buy gate - with offset below mean
    // this means "buy when price dips entry_offset_pct below the rolling average"
    // volume gate uses rolling avg * multiplier so only significant trades pass
    FPN<F> buy_price = RollingStats_BuyPrice(rolling, state->live_offset_pct);
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
//======================================================================================================
template <unsigned F>
inline void MeanReversion_Adapt(MeanReversionState<F> *state,
                                 FPN<F> current_price,
                                 FPN<F> portfolio_delta,
                                 uint16_t active_bitmap,
                                 const BuySideGateConditions<F> *buy_conds,
                                 const ControllerConfig<F> *cfg) {
    constexpr unsigned N = FPN<F>::N;

    //==================================================================================================
    // IDLE SQUEEZE: when portfolio is empty, use price slope to loosen/tighten filters
    // solves the chicken-and-egg problem: no positions -> no P&L -> no adaptation
    // if price is trending up and we have nothing, we're missing the move -> squeeze offset down
    // if price is trending down and we have nothing, we're correctly staying out -> no change
    //
    // branchless: empty_mask is all 1s when portfolio empty, all 0s when holding
    // positive price slope -> squeeze offset toward offset_min
    // the squeeze rate is proportional to the slope magnitude
    //==================================================================================================
    {
        int is_empty = (active_bitmap == 0);
        int trailing = FPN_GreaterThan(current_price, buy_conds->price);

        // squeeze fires when: portfolio empty AND current price above buy gate
        int should_squeeze = is_empty & trailing;
        uint64_t sq_mask = -(uint64_t)should_squeeze;

        // TWO-PHASE SQUEEZE:
        // Phase 1: squeeze offset toward zero (not offset_min — go all the way)
        // Phase 2: if offset is already at zero and still trailing, the buy gate
        //   is at the rolling average which lags price. Nothing more to squeeze —
        //   the rolling average will catch up naturally as new ticks enter the
        //   window. But we can also squeeze the volume multiplier to 1.0 (accept
        //   any volume) so the only remaining barrier is the price itself catching
        //   up.
        FPN<F> zero = FPN_Zero<F>();

        // squeeze offset toward zero: step = current_offset * 0.10 (10% per cycle, fast)
        FPN<F> squeeze_step = FPN_Mul(state->live_offset_pct, FPN_FromDouble<F>(0.10));
        FPN<F> masked_squeeze;
        for (unsigned i = 0; i < N; i++) {
            masked_squeeze.w[i] = squeeze_step.w[i] & sq_mask;
        }
        masked_squeeze.sign = squeeze_step.sign & should_squeeze;

        state->live_offset_pct = FPN_SubSat(state->live_offset_pct, masked_squeeze);
        state->live_offset_pct = FPN_Max(state->live_offset_pct, zero); // floor at zero, not offset_min

        // squeeze volume multiplier toward 1.0 (accept any trade size)
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

        // compute filter shift from slope: negate because positive P&L -> loosen (decrease)
        FPN<F> filter_shift = FPN_Mul(inner.model.slope, cfg->filter_scale);
        FPN<F> neg_shift = FPN_Negate(filter_shift);

        // mask to zero if not confident
        FPN<F> masked_shift;
        for (unsigned i = 0; i < N; i++) {
            masked_shift.w[i] = neg_shift.w[i] & conf_mask;
        }
        masked_shift.sign = neg_shift.sign & confident;

        // apply shift to offset and clamp to [offset_min, offset_max]
        // scale shift for offset (its a small number ~0.001, so scale down)
        FPN<F> offset_scale = FPN_FromDouble<F>(0.001);
        FPN<F> offset_shift = FPN_Mul(masked_shift, offset_scale);
        state->live_offset_pct = FPN_AddSat(state->live_offset_pct, offset_shift);
        state->live_offset_pct = FPN_Max(state->live_offset_pct, cfg->offset_min);
        state->live_offset_pct = FPN_Min(state->live_offset_pct, cfg->offset_max);

        // apply shift to volume multiplier and clamp to [vol_mult_min, vol_mult_max]
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
// available. returns the conditions for the engine to write to ctrl->buy_conds.
//======================================================================================================
template <unsigned F>
inline BuySideGateConditions<F> MeanReversion_BuySignal(MeanReversionState<F> *state,
                                                         const RollingStats<F> *rolling,
                                                         const ControllerConfig<F> *cfg) {
    BuySideGateConditions<F> conds;

    // compute base conditions from rolling stats using LIVE adaptive filter values
    conds.price = RollingStats_BuyPrice(rolling, state->live_offset_pct);
    conds.volume = FPN_Mul(rolling->volume_avg, state->live_vol_mult);

    // update initial conditions to track the market - prevents the gate shift clamp
    // from anchoring to stale warmup prices. the clamp window (max_shift) moves with
    // the rolling average so the gate stays in the current price neighborhood
    state->buy_conds_initial = conds;

    // apply regression-based gate shift if available
    // shifts buy price condition based on regression slope, masked by R^2 confidence,
    // clamped to max_shift from initial conditions - all branchless
    if (state->has_regression) {
        constexpr unsigned N = FPN<F>::N;

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

    return conds;
}

//======================================================================================================
//======================================================================================================
#endif // MEAN_REVERSION_HPP
