//======================================================================================================
// [MOMENTUM / TREND-FOLLOWING STRATEGY]
//======================================================================================================
// the complement to mean reversion — buys breakouts instead of dips
// activates when regime detector identifies a strong directional trend
//
// entry: buy when price breaks ABOVE rolling average + offset (confirms trend strength)
//   - opposite of mean reversion which buys BELOW the average
//   - volume confirmation ensures breakout has participation, not just a thin spike
//   - stddev-scaled offset: wider breakout required in volatile markets
//
// exit: tighter SL (protect against false breakouts), wider TP (let trends run)
//   - SL trails aggressively — momentum failures reverse fast
//   - TP uses a larger stddev multiplier than mean reversion
//
// adaptation: similar P&L regression but adapts breakout threshold and trailing tightness
//   - positive P&L → loosen breakout threshold (buy earlier in the trend)
//   - negative P&L → require stronger breakouts before entry
//
// regime transition (complex mode):
//   when switching FROM mean reversion → momentum:
//     - existing MR positions keep their TP/SL (they were entered for a dip, not a trend)
//     - OR: widen their TP and tighten SL to match momentum profile (see RegimeDetector)
//   when switching FROM momentum → mean reversion:
//     - existing momentum positions: tighten TP (trend is ending, take profit early)
//     - widen SL slightly (allow for mean-reversion chop)
//
// all adaptation is branchless. strategy only runs on slow path.
// BuyGate on hot path just reads buy_conds — same as mean reversion.
//======================================================================================================
#ifndef MOMENTUM_HPP
#define MOMENTUM_HPP

#include "StrategyInterface.hpp"
#include "../CoreFrameworks/ControllerConfig.hpp"
#include "../CoreFrameworks/OrderGates.hpp"
#include "../CoreFrameworks/Portfolio.hpp"
#include "../ML_Headers/LinearRegression3X.hpp"
#include "../ML_Headers/ROR_regressor.hpp"
#include "../ML_Headers/RollingStats.hpp"

//======================================================================================================
// [STATE]
//======================================================================================================
template <unsigned F> struct MomentumState {
    RegressionFeederX<F> feeder;       // P&L regression for adaptive filters
    RORRegressor<F> ror;               // slope-of-slopes
    FPN<F> live_breakout_mult;         // adaptive breakout threshold (stddev multiplier)
    FPN<F> live_vol_mult;              // adaptive volume multiplier
    BuySideGateConditions<F> buy_conds_initial; // anchor for max_shift clamp
    LinearRegression3XResult<F> last_regression;
    int has_regression;
    RegressionFeederX<F> price_feeder; // for trailing R² computation
};

//======================================================================================================
// [INIT]
//======================================================================================================
// called once when regime detector switches to momentum. sets initial buy conditions
// to buy ABOVE the rolling average (opposite of mean reversion).
//======================================================================================================
template <unsigned F>
inline void Momentum_Init(MomentumState<F> *state,
                           const RollingStats<F> *rolling,
                           BuySideGateConditions<F> *buy_conds) {
    // TODO: implement
    // buy_price = avg + (stddev * breakout_mult)  ← ABOVE average, opposite of MR
    // volume threshold same pattern as MR
    // reset regression feeders
    (void)state; (void)rolling; (void)buy_conds;
}

//======================================================================================================
// [ADAPT]
//======================================================================================================
// adjusts breakout threshold and volume multiplier based on P&L regression
// same feedback loop as mean reversion but tuning different parameters
//======================================================================================================
template <unsigned F>
inline void Momentum_Adapt(MomentumState<F> *state, FPN<F> current_price,
                            FPN<F> portfolio_delta, uint16_t active_bitmap,
                            const BuySideGateConditions<F> *buy_conds,
                            const ControllerConfig<F> *cfg) {
    // TODO: implement
    // - push portfolio_delta to regression feeder
    // - if confident (R² > threshold): adjust live_breakout_mult
    //   positive slope → lower threshold (more aggressive, enter breakouts earlier)
    //   negative slope → raise threshold (wait for stronger confirmation)
    // - idle squeeze when no positions: lower breakout threshold toward minimum
    (void)state; (void)current_price; (void)portfolio_delta;
    (void)active_bitmap; (void)buy_conds; (void)cfg;
}

//======================================================================================================
// [BUY SIGNAL]
//======================================================================================================
// computes buy conditions for breakout entries
// buy_price = avg + (stddev * live_breakout_mult)  ← price must be ABOVE this to buy
// volume = vol_avg * live_vol_mult                 ← same volume confirmation as MR
//
// NOTE: BuyGate checks price <= buy_conds.price, so for momentum we need to invert.
// Option A: set buy_conds.price very high and use a separate momentum gate
// Option B: add a gate_direction flag to BuySideGateConditions (1 = buy below, -1 = buy above)
// Option C: momentum sets a price floor instead — BuyGate needs a second condition
//
// this is the key design decision — BuyGate currently only checks price <= X.
// momentum needs price >= X. see RegimeDetector.hpp for the proposed approach.
//======================================================================================================
template <unsigned F>
inline BuySideGateConditions<F> Momentum_BuySignal(MomentumState<F> *state,
                                                     const RollingStats<F> *rolling,
                                                     const RollingStats<F, 512> *rolling_long,
                                                     const ControllerConfig<F> *cfg) {
    // TODO: implement
    // key issue: BuyGate checks price <= conds.price
    // momentum needs price >= breakout_level
    // proposed: add gate_direction to BuySideGateConditions
    //   direction = 1:  BuyGate passes when price <= conds.price  (mean reversion, buy dips)
    //   direction = -1: BuyGate passes when price >= conds.price  (momentum, buy breakouts)
    (void)state; (void)rolling; (void)rolling_long; (void)cfg;
    BuySideGateConditions<F> conds;
    conds.price = FPN_Zero<F>();
    conds.volume = FPN_Zero<F>();
    return conds;
}

//======================================================================================================
// [EXIT ADJUST]
//======================================================================================================
// momentum-specific trailing: tighter SL trail (failures reverse fast),
// more aggressive TP trail (let winners run further than MR)
//======================================================================================================
template <unsigned F>
inline void Momentum_ExitAdjust(Portfolio<F> *portfolio, FPN<F> current_price,
                                  const RollingStats<F> *rolling,
                                  MomentumState<F> *state,
                                  const ControllerConfig<F> *cfg) {
    // TODO: implement
    // same trailing TP pattern as MR but with different multipliers:
    //   tp_trail_mult: higher (let trends run, e.g. 2.0x vs MR's 1.0x)
    //   sl_trail_mult: tighter (cut losers fast, e.g. 0.5x vs MR's 2.0x)
    (void)portfolio; (void)current_price; (void)rolling; (void)state; (void)cfg;
}

#endif // MOMENTUM_HPP
