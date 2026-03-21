//======================================================================================================
// [REGIME DETECTOR]
//======================================================================================================
// classifies market state and switches the active strategy
//
// regimes:
//   REGIME_RANGING    — price oscillating around mean, low directional slope, low R²
//                       → mean reversion (buy dips, sell rips)
//   REGIME_TRENDING   — strong directional move, high R², sustained slope
//                       → momentum (buy breakouts, trail stops)
//   REGIME_VOLATILE   — high stddev, choppy, no clear direction
//                       → reduce position size or pause buying entirely
//
// detection uses data already available (zero new computation):
//   - rolling.price_slope     → direction and magnitude
//   - rolling.price_stddev    → volatility level
//   - mean_rev.last_regression.r_squared → trend consistency
//   - rolling_long.price_slope → broader context
//
// hysteresis: regime must persist for N slow-path cycles before switching.
// prevents whipsawing between strategies on noisy transitions.
//
// position handling on regime switch (complex mode):
//   the detector doesn't close positions. instead it adjusts TP/SL on existing positions
//   to match the new regime's risk profile. the position was entered under one assumption
//   (e.g. "price will revert to mean") but the market regime changed (e.g. "price is
//   trending"). rather than force-close at a loss, we adjust exits to match the new reality.
//
//   MR → momentum transition:
//     - widen TP: the "dip" we bought might actually be the start of a trend
//       new_tp = max(current_tp, entry + stddev * momentum_tp_mult)
//     - tighten SL: if it's NOT a trend, get out faster
//       new_sl = max(current_sl, entry - stddev * momentum_sl_mult)
//       (max because SL is below entry — higher SL = tighter)
//
//   momentum → MR transition:
//     - tighten TP: the trend is ending, take profits before reversal
//       new_tp = min(current_tp, entry + stddev * mr_tp_mult)
//     - widen SL: allow for mean-reversion chop around the new equilibrium
//       new_sl = min(current_sl, entry - stddev * mr_sl_mult)
//
//   the adjustment is one-shot (applied at the moment of transition, not every tick).
//   positions entered AFTER the switch get the new strategy's TP/SL natively.
//======================================================================================================
#ifndef REGIME_DETECTOR_HPP
#define REGIME_DETECTOR_HPP

#include "StrategyInterface.hpp"
#include "../CoreFrameworks/ControllerConfig.hpp"
#include "../CoreFrameworks/Portfolio.hpp"
#include "../ML_Headers/RollingStats.hpp"

#define REGIME_RANGING  0
#define REGIME_TRENDING 1
#define REGIME_VOLATILE 2

//======================================================================================================
// [STATE]
//======================================================================================================
template <unsigned F> struct RegimeState {
    int current_regime;          // REGIME_RANGING, REGIME_TRENDING, REGIME_VOLATILE
    int proposed_regime;         // what the classifier thinks (before hysteresis)
    int hysteresis_count;        // how many consecutive cycles the proposed regime has held
    int hysteresis_threshold;    // must hold for N cycles before switching (e.g. 5)
    int last_strategy_id;        // tracks which strategy was active before transition
};

//======================================================================================================
// [INIT]
//======================================================================================================
template <unsigned F>
inline void Regime_Init(RegimeState<F> *state, int hysteresis_threshold) {
    state->current_regime = REGIME_RANGING;  // start conservative
    state->proposed_regime = REGIME_RANGING;
    state->hysteresis_count = 0;
    state->hysteresis_threshold = hysteresis_threshold;
    state->last_strategy_id = STRATEGY_MEAN_REVERSION;
}

//======================================================================================================
// [CLASSIFY]
//======================================================================================================
// called every slow-path tick. returns the detected regime (may differ from current_regime
// if hysteresis hasn't been satisfied yet). the engine reads current_regime to decide
// which strategy to run.
//
// classification logic:
//   trending  = |relative_slope| > slope_threshold AND R² > r2_threshold
//   volatile  = stddev > volatile_threshold AND R² < r2_threshold (big moves, no direction)
//   ranging   = everything else (low slope, or low stddev, or choppy)
//
// thresholds should come from config (TODO: add to ControllerConfig):
//   regime_slope_threshold    — how steep a slope counts as "trending" (relative, e.g. 0.001%)
//   regime_r2_threshold       — how consistent the trend must be (e.g. 0.7)
//   regime_volatile_stddev    — stddev multiplier for "volatile" classification
//   regime_hysteresis         — cycles before switching (e.g. 5)
//======================================================================================================
template <unsigned F>
inline int Regime_Classify(RegimeState<F> *state,
                            const RollingStats<F> *rolling,
                            const RollingStats<F, 512> *rolling_long,
                            FPN<F> r_squared,
                            const ControllerConfig<F> *cfg) {
    // TODO: implement classification
    // sketch:
    //
    // FPN<F> relative_slope = FPN_DivNoAssert(rolling->price_slope, rolling->price_avg);
    // FPN<F> abs_slope = FPN_Abs(relative_slope);
    //
    // int strong_slope = FPN_GreaterThan(abs_slope, cfg->regime_slope_threshold);
    // int consistent   = FPN_GreaterThan(r_squared, cfg->regime_r2_threshold);
    // int high_vol     = FPN_GreaterThan(rolling->price_stddev,
    //                        FPN_Mul(rolling->price_avg, cfg->regime_volatile_stddev));
    //
    // int detected;
    // if (strong_slope && consistent)
    //     detected = REGIME_TRENDING;
    // else if (high_vol && !consistent)
    //     detected = REGIME_VOLATILE;
    // else
    //     detected = REGIME_RANGING;
    //
    // // hysteresis: proposed must hold for N consecutive cycles
    // if (detected == state->proposed_regime) {
    //     state->hysteresis_count++;
    // } else {
    //     state->proposed_regime = detected;
    //     state->hysteresis_count = 1;
    // }
    //
    // if (state->hysteresis_count >= state->hysteresis_threshold
    //     && state->proposed_regime != state->current_regime) {
    //     state->current_regime = state->proposed_regime;
    // }
    //
    // return state->current_regime;

    (void)state; (void)rolling; (void)rolling_long; (void)r_squared; (void)cfg;
    return REGIME_RANGING;
}

//======================================================================================================
// [ADJUST POSITIONS ON REGIME SWITCH]
//======================================================================================================
// called once when current_regime changes. walks active positions and adjusts TP/SL
// to match the new regime's risk profile. this is the "complex" option — positions
// entered under one strategy get their exits adjusted for the new market conditions.
//
// the key insight: a mean reversion position that bought a "dip" might actually be
// the start of a trend. if we detect the regime shifted to trending, we WIDEN the TP
// so the position can ride the trend, and TIGHTEN the SL in case we're wrong.
// conversely, if a momentum position is caught in a regime shift to ranging,
// we TIGHTEN the TP (take profit before reversal) and WIDEN the SL (allow chop).
//
// only adjusts positions that were entered under the PREVIOUS strategy.
// positions entered after the switch already have the new strategy's TP/SL.
//======================================================================================================
template <unsigned F>
inline void Regime_AdjustPositions(Portfolio<F> *portfolio,
                                     const RollingStats<F> *rolling,
                                     int old_regime, int new_regime,
                                     const uint8_t *entry_strategy,
                                     const ControllerConfig<F> *cfg) {
    // TODO: implement
    // sketch:
    //
    // uint16_t active = portfolio->active_bitmap;
    // while (active) {
    //     int idx = __builtin_ctz(active);
    //     if (entry_strategy[idx] == Regime_ToStrategy(old_regime)) {  // only adjust old strategy positions
    //     Position<F> *pos = &portfolio->positions[idx];
    //     FPN<F> stddev = rolling->price_stddev;
    //
    //     if (old_regime == REGIME_RANGING && new_regime == REGIME_TRENDING) {
    //         // MR → momentum: widen TP, tighten SL
    //         FPN<F> wide_tp = FPN_AddSat(pos->entry_price,
    //                            FPN_Mul(stddev, cfg->momentum_tp_mult));
    //         pos->take_profit_price = FPN_Max(pos->take_profit_price, wide_tp);
    //
    //         FPN<F> tight_sl = FPN_SubSat(pos->entry_price,
    //                             FPN_Mul(stddev, cfg->momentum_sl_mult));
    //         pos->stop_loss_price = FPN_Max(pos->stop_loss_price, tight_sl);
    //     }
    //     else if (old_regime == REGIME_TRENDING && new_regime == REGIME_RANGING) {
    //         // momentum → MR: tighten TP, widen SL
    //         FPN<F> tight_tp = FPN_AddSat(pos->entry_price,
    //                             FPN_Mul(stddev, cfg->take_profit_pct));
    //         pos->take_profit_price = FPN_Min(pos->take_profit_price, tight_tp);
    //
    //         FPN<F> wide_sl = FPN_SubSat(pos->entry_price,
    //                            FPN_Mul(stddev, cfg->stop_loss_pct));
    //         pos->stop_loss_price = FPN_Min(pos->stop_loss_price, wide_sl);
    //     }
    //     // REGIME_VOLATILE: could pause buying entirely (set buy_conds to zero)
    //     // existing positions keep current TP/SL — don't panic-adjust in volatility
    //
    //     active &= active - 1;
    // }

    (void)portfolio; (void)rolling; (void)old_regime; (void)new_regime; (void)cfg;
}

//======================================================================================================
// [STRATEGY MAPPING]
//======================================================================================================
// maps regime to strategy_id. called by the engine to set ctrl->strategy_id.
//======================================================================================================
static inline int Regime_ToStrategy(int regime) {
    switch (regime) {
        case REGIME_TRENDING: return STRATEGY_MOMENTUM;
        case REGIME_VOLATILE: return STRATEGY_MEAN_REVERSION; // conservative: trade less, not differently
        case REGIME_RANGING:
        default:              return STRATEGY_MEAN_REVERSION;
    }
}

#endif // REGIME_DETECTOR_HPP
