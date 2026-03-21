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
    // cold start: stay in RANGING until rolling stats have enough data
    // R² is zero until the regression feeder fills (~8 slow-path cycles)
    // without this guard, we'd classify on garbage data
    if (rolling->count < 64 || FPN_IsZero(rolling->price_avg))
        return state->current_regime;

    // classify: relative slope magnitude + R² consistency + volatility level
    FPN<F> relative_slope = FPN_DivNoAssert(rolling->price_slope, rolling->price_avg);
    FPN<F> abs_slope = FPN_Abs(relative_slope);

    int strong_slope = FPN_GreaterThan(abs_slope, cfg->regime_slope_threshold);
    int consistent   = FPN_GreaterThan(r_squared, cfg->regime_r2_threshold);
    FPN<F> vol_threshold = FPN_Mul(rolling->price_avg, cfg->regime_volatile_stddev);
    int high_vol     = FPN_GreaterThan(rolling->price_stddev, vol_threshold);

    // trending: strong directional move with consistent pattern
    // volatile: big swings but no direction (high stddev, low R²)
    // ranging:  everything else (low slope, or calm, or choppy)
    int detected;
    if (strong_slope && consistent)
        detected = REGIME_TRENDING;
    else if (high_vol && !consistent)
        detected = REGIME_VOLATILE;
    else
        detected = REGIME_RANGING;

    // hysteresis: proposed regime must hold for N consecutive cycles before switching
    // prevents whipsawing between strategies on noisy transitions
    if (detected == state->proposed_regime) {
        state->hysteresis_count++;
    } else {
        state->proposed_regime = detected;
        state->hysteresis_count = 1;
    }

    if (state->hysteresis_count >= state->hysteresis_threshold
        && state->proposed_regime != state->current_regime) {
        state->current_regime = state->proposed_regime;
    }

    (void)rolling_long; // available for future multi-timeframe regime detection
    return state->current_regime;
}

//======================================================================================================
// [STRATEGY MAPPING]
//======================================================================================================
// maps regime to strategy_id. must be defined before Regime_AdjustPositions (which calls it).
//======================================================================================================
static inline int Regime_ToStrategy(int regime) {
    switch (regime) {
        case REGIME_TRENDING: return STRATEGY_MOMENTUM;
        case REGIME_VOLATILE: return STRATEGY_MEAN_REVERSION;
        case REGIME_RANGING:
        default:              return STRATEGY_MEAN_REVERSION;
    }
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
    // one-shot adjustment: walks active positions entered under the OLD strategy
    // and adjusts their TP/SL to match the new regime's risk profile
    int old_strategy = Regime_ToStrategy(old_regime);
    FPN<F> stddev = rolling->price_stddev;

    // TP/SL multipliers use the same pattern as fill-time computation:
    // multiply stddev by a scaling factor (e.g. momentum_tp_mult * 100 → stddev units)
    FPN<F> hundred = FPN_FromDouble<F>(100.0);

    uint16_t active = portfolio->active_bitmap;
    while (active) {
        int idx = __builtin_ctz(active);

        // only adjust positions entered under the old strategy
        if (entry_strategy[idx] == old_strategy) {
            Position<F> *pos = &portfolio->positions[idx];

            if (old_regime == REGIME_RANGING && new_regime == REGIME_TRENDING) {
                // MR → momentum: widen TP (let trend run), tighten SL (cut if wrong)
                FPN<F> wide_tp_offset = FPN_Mul(stddev, FPN_Mul(cfg->momentum_tp_mult, hundred));
                FPN<F> wide_tp = FPN_AddSat(pos->entry_price, wide_tp_offset);
                pos->take_profit_price = FPN_Max(pos->take_profit_price, wide_tp);

                FPN<F> tight_sl_offset = FPN_Mul(stddev, FPN_Mul(cfg->momentum_sl_mult, hundred));
                FPN<F> tight_sl = FPN_SubSat(pos->entry_price, tight_sl_offset);
                pos->stop_loss_price = FPN_Max(pos->stop_loss_price, tight_sl);
            }
            else if (old_regime == REGIME_TRENDING && new_regime == REGIME_RANGING) {
                // momentum → MR: tighten TP (take profit before reversal), widen SL (allow chop)
                FPN<F> tight_tp_offset = FPN_Mul(stddev, FPN_Mul(cfg->take_profit_pct, hundred));
                FPN<F> tight_tp = FPN_AddSat(pos->entry_price, tight_tp_offset);
                pos->take_profit_price = FPN_Min(pos->take_profit_price, tight_tp);

                FPN<F> wide_sl_offset = FPN_Mul(stddev, FPN_Mul(cfg->stop_loss_pct, hundred));
                FPN<F> wide_sl = FPN_SubSat(pos->entry_price, wide_sl_offset);
                pos->stop_loss_price = FPN_Min(pos->stop_loss_price, wide_sl);
            }
            // REGIME_VOLATILE: don't adjust — existing positions keep their TP/SL
            // panic-adjusting in volatility causes more harm than good
        }

        active &= active - 1;
    }
}

#endif // REGIME_DETECTOR_HPP
