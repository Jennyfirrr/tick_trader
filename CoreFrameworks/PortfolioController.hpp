//======================================================================================================
// [PORTFOLIO CONTROLLER]
//======================================================================================================
// this just tracks the portfolio delta and tracks performance over time, uses
// linear regression and the GCN to edit and update the gate conditions for
// buying and selling based on portoflio performance, probably gonna add
// configrurebale parameters for polling rates and stuff, this should be a
// seperate module from the actual order engine, as it shouldnt interfere with
// the order execution, this simply pipes conditions to the gates
//======================================================================================================
// [INCLUDE]
//======================================================================================================
#ifndef PORTFOLIO_CONTROLLER_HPP
#define PORTFOLIO_CONTROLLER_HPP

#include "ControllerConfig.hpp"
#include "OrderGates.hpp"
#include "Portfolio.hpp"
#include "../DataStream/TradeLog.hpp"
#include "../ML_Headers/RollingStats.hpp"
#include "../Strategies/MeanReversion.hpp"
#include "../Strategies/Momentum.hpp"
#include "../Strategies/RegimeDetector.hpp"
#include <stdio.h>
//======================================================================================================
// [CONTROLLER STRUCT]
//======================================================================================================
#define CONTROLLER_WARMUP 0
#define CONTROLLER_ACTIVE 1
//======================================================================================================
template <unsigned F> struct PortfolioController {
  //================================================================================================
  // HOT — touched every tick, grouped for L1 cache locality
  // target: all hot fields within first ~3KB so they share cache lines
  //================================================================================================
  Portfolio<F> portfolio;
  uint64_t prev_bitmap;   // fill detection: pool->bitmap & ~prev_bitmap
  uint64_t tick_count;    // slow-path gate: tick_count < config.poll_interval
  uint64_t total_ticks;
  BuySideGateConditions<F> buy_conds;
  ExitBuffer<F> exit_buf;

  //================================================================================================
  // WARM — accessed on fills or slow path, but not every tick
  //================================================================================================
  ControllerConfig<F> config;
  FPN<F> portfolio_delta; // unrealized P&L (current open positions)
  FPN<F> realized_pnl;    // cumulative realized P&L (closed positions)
  FPN<F> balance;    // paper trading balance (deducted on buy, added on sell)
  FPN<F> total_fees; // cumulative fees paid (tracked for display)

  uint32_t wins;       // TP exits
  uint32_t losses;     // SL exits
  uint32_t total_buys; // total entries
  FPN<F> gross_wins;   // cumulative dollar gains from TP exits
  FPN<F>
      gross_losses; // cumulative dollar losses from SL exits (positive number)
  uint64_t
      total_hold_ticks;     // cumulative ticks held across all closed positions
  uint64_t entry_ticks[16]; // tick at which each position was entered (indexed
                            // by slot)
  uint8_t entry_strategy[16]; // which strategy_id entered each position (for regime adjustment)

  int state;
  FPN<F> price_sum;
  FPN<F> volume_sum;
  uint64_t warmup_count;

  int strategy_id;
  MeanReversionState<F> mean_rev;
  MomentumState<F> momentum;
  RegimeState<F> regime;
  TradeLogBuffer trade_buf;  // buffered trade log — hot path pushes, slow path drains

  //================================================================================================
  // COLD — slow path only, kept at end to avoid polluting hot cache lines
  //================================================================================================
  RollingStats<F> rolling;
  RollingStats<F, 512> *rolling_long;  // heap-allocated (24KB), slow path only
};
//======================================================================================================
// [INIT]
//======================================================================================================
template <unsigned F>
inline void PortfolioController_Init(PortfolioController<F> *ctrl,
                                     ControllerConfig<F> config) {
  Portfolio_Init(&ctrl->portfolio);
  ctrl->portfolio_delta = FPN_Zero<F>();
  ctrl->realized_pnl = FPN_Zero<F>();
  ctrl->balance = config.starting_balance;
  ctrl->total_fees = FPN_Zero<F>();
  ctrl->wins = 0;
  ctrl->losses = 0;
  ctrl->total_buys = 0;
  ctrl->gross_wins = FPN_Zero<F>();
  ctrl->gross_losses = FPN_Zero<F>();
  ctrl->total_hold_ticks = 0;
  for (int i = 0; i < 16; i++) {
    ctrl->entry_ticks[i] = 0;
    ctrl->entry_strategy[i] = STRATEGY_MEAN_REVERSION;
  }

  ctrl->rolling = RollingStats_Init<F>();

  // during warmup, buy gate is disabled - price 0 means nothing passes
  // LessThanOrEqual
  ctrl->buy_conds.price = FPN_Zero<F>();
  ctrl->buy_conds.volume = FPN_Zero<F>();
  ctrl->buy_conds.gate_direction = 0;  // default: buy below (mean reversion)

  // init strategy states — both ready at startup so regime switch is instant
  ctrl->strategy_id = STRATEGY_MEAN_REVERSION;
  // mean reversion
  ctrl->mean_rev.feeder = RegressionFeederX_Init<F>();
  ctrl->mean_rev.price_feeder = RegressionFeederX_Init<F>();
  ctrl->mean_rev.ror = RORRegressor_Init<F>();
  ctrl->mean_rev.live_offset_pct = config.entry_offset_pct;
  ctrl->mean_rev.live_vol_mult = config.volume_multiplier;
  ctrl->mean_rev.live_stddev_mult = config.offset_stddev_mult;
  ctrl->mean_rev.buy_conds_initial = ctrl->buy_conds;
  ctrl->mean_rev.has_regression = 0;
  // momentum
  ctrl->momentum.feeder = RegressionFeederX_Init<F>();
  ctrl->momentum.price_feeder = RegressionFeederX_Init<F>();
  ctrl->momentum.ror = RORRegressor_Init<F>();
  ctrl->momentum.live_breakout_mult = config.momentum_breakout_mult;
  ctrl->momentum.live_vol_mult = config.volume_multiplier;
  ctrl->momentum.buy_conds_initial = ctrl->buy_conds;
  ctrl->momentum.has_regression = 0;
  // regime detector
  Regime_Init(&ctrl->regime, config.regime_hysteresis);

  ExitBuffer_Init(&ctrl->exit_buf);
  TradeLogBuffer_Init(&ctrl->trade_buf);

  ctrl->prev_bitmap = 0;
  ctrl->tick_count = 0;
  ctrl->total_ticks = 0;

  ctrl->state = CONTROLLER_WARMUP;
  ctrl->price_sum = FPN_Zero<F>();
  ctrl->volume_sum = FPN_Zero<F>();
  ctrl->warmup_count = 0;

  ctrl->config = config;

  // heap-allocate rolling_long (24KB) — keeps it out of the hot struct
  if (ctrl->rolling_long) free(ctrl->rolling_long);  // safe on reinit (24h reconnect)
  ctrl->rolling_long = (RollingStats<F, 512>*)malloc(sizeof(RollingStats<F, 512>));
  *ctrl->rolling_long = RollingStats_Init<F, 512>();
}
//======================================================================================================
// [TICK - MAIN CONTROLLER FUNCTION]
//======================================================================================================
// called every tick. fill consumption runs every tick (zero unprotected
// exposure). regression/adjustment runs every poll_interval ticks (slow path).
//======================================================================================================
template <unsigned F>
inline void PortfolioController_Tick(PortfolioController<F> *ctrl,
                                     OrderPool<F> *pool, FPN<F> current_price,
                                     FPN<F> current_volume,
                                     TradeLog *trade_log) {
  // always increment tick counter (branchless, single add)
  ctrl->total_ticks++;

  //==================================================================================================
  // WARMUP PHASE
  //==================================================================================================
  if (ctrl->state == CONTROLLER_WARMUP) {
    ctrl->price_sum = FPN_AddSat(ctrl->price_sum, current_price);
    ctrl->volume_sum = FPN_AddSat(ctrl->volume_sum, current_volume);
    ctrl->warmup_count++;

    // feed rolling stats during warmup (both windows so long window has data at activation)
    RollingStats_Push(&ctrl->rolling, current_price, current_volume);
    RollingStats_Push(ctrl->rolling_long, current_price, current_volume);

    if (ctrl->warmup_count >= ctrl->config.warmup_ticks) {
      // init both strategies so either can activate instantly on regime switch
      MeanReversion_Init(&ctrl->mean_rev, &ctrl->rolling, &ctrl->buy_conds);
      Momentum_Init(&ctrl->momentum, &ctrl->rolling, &ctrl->buy_conds);
      // apply active strategy's buy signal immediately
      ctrl->buy_conds = MeanReversion_BuySignal(&ctrl->mean_rev, &ctrl->rolling,
                                                 ctrl->rolling_long, &ctrl->config);
      ctrl->buy_conds.gate_direction = 0;
      ctrl->state = CONTROLLER_ACTIVE;
    }
    return;
  }

  //==================================================================================================
  // ACTIVE PHASE - EVERY TICK: consume new fills immediately
  //==================================================================================================
  // branchless: mask new_fills to zero if portfolio is full, then the while
  // loop body count is 0 the loop itself is unavoidable (variable fill count)
  // but the outer gate is eliminated
  //==================================================================================================
  uint64_t new_fills = pool->bitmap & ~ctrl->prev_bitmap;
  uint64_t can_fill = -(uint64_t)(!Portfolio_IsFull(
      &ctrl->portfolio)); // all 1s if room, all 0s if full
  uint64_t fills = new_fills & can_fill;
  uint64_t consumed = fills; // track what we actually process for pool clearing

  while (fills) {
    uint32_t idx = __builtin_ctzll(fills);

    FPN<F> fill_price = pool->slots[idx].price;
    // ignore stream quantity - we use position sizing based on balance
    // fill_price is the signal, not fill_qty

    // consolidation: find existing position at same price
    // with position sizing, consolidation is DISABLED - each fill at the same
    // price would just be a duplicate signal. we only want one position per
    // price level. the entry spacing check below handles preventing duplicate
    // entries at nearby prices.
    int existing = Portfolio_FindByPrice(&ctrl->portfolio, fill_price);
    int found = (existing >= 0);

    // entry spacing check: reject fills that are too close to existing
    // positions walks the portfolio bitmap to find the minimum distance from
    // fill_price to any entry this spreads the 16 slots across actual price
    // levels instead of clustering
    FPN<F> min_spacing = RollingStats_EntrySpacing(
        &ctrl->rolling, ctrl->config.spacing_multiplier);
    int too_close = 0;
    {
      uint16_t active_pos = ctrl->portfolio.active_bitmap;
      while (active_pos) {
        int pidx = __builtin_ctz(active_pos);
        FPN<F> dist =
            FPN_Sub(fill_price, ctrl->portfolio.positions[pidx].entry_price);
        // branchless absolute value: mask-select between dist and negated dist
        // neg_mask is all 1s if negative, all 0s if positive
        FPN<F> neg_dist = FPN_Negate(dist);
        uint64_t neg_mask = -(uint64_t)(dist.sign);
        FPN<F> abs_dist;
        for (unsigned w = 0; w < FPN<F>::N; w++) {
          abs_dist.w[w] = (neg_dist.w[w] & neg_mask) | (dist.w[w] & ~neg_mask);
        }
        abs_dist.sign = 0; // absolute value is always positive
        too_close |= FPN_LessThan(abs_dist, min_spacing);
        active_pos &= active_pos - 1;
      }
    }

    // POSITION SIZING: compute quantity from balance and risk percentage
    // qty = (balance * risk_pct) / price
    // this replaces the stream quantity - we decide how much to buy, not the
    // stream
    FPN<F> risk_amount = FPN_Mul(ctrl->balance, ctrl->config.risk_pct);
    FPN<F> sized_qty = FPN_DivNoAssert(risk_amount, fill_price);

    // balance check: can we afford this position + entry fee? (branchless)
    FPN<F> cost = FPN_Mul(fill_price, sized_qty);
    FPN<F> entry_fee = FPN_Mul(cost, ctrl->config.fee_rate);
    FPN<F> total_cost = FPN_AddSat(cost, entry_fee);
    int can_afford = FPN_GreaterThanOrEqual(ctrl->balance, total_cost);

    // CIRCUIT BREAKER: halt if total P&L has dropped below max_drawdown_pct of
    // starting balance total_pnl = realized + unrealized, drawdown_limit =
    // -(starting_balance * max_drawdown_pct)
    FPN<F> total_pnl_check =
        FPN_AddSat(ctrl->realized_pnl, ctrl->portfolio_delta);
    FPN<F> drawdown_limit = FPN_Negate(
        FPN_Mul(ctrl->config.starting_balance, ctrl->config.max_drawdown_pct));
    int not_blown = FPN_GreaterThan(total_pnl_check, drawdown_limit);

    // EXPOSURE LIMIT: cap total deployed capital at max_exposure_pct of
    // starting balance deployed = starting_balance - current_balance (how much
    // is in positions)
    FPN<F> deployed = FPN_Sub(ctrl->config.starting_balance, ctrl->balance);
    FPN<F> max_deployed =
        FPN_Mul(ctrl->config.starting_balance, ctrl->config.max_exposure_pct);
    int under_limit =
        FPN_LessThan(FPN_AddSat(deployed, total_cost), max_deployed);

    // new position: room AND spacing AND balance AND not blown AND under
    // exposure limit
    int is_new = !found & !Portfolio_IsFull(&ctrl->portfolio) & !too_close &
                 can_afford & not_blown & under_limit;
    if (is_new) {
      // volatility-based TP/SL with fee floor
      FPN<F> stddev = ctrl->rolling.price_stddev;
      int has_stats = !FPN_IsZero(stddev);

      // volatility path: TP = entry + stddev * tp_mult, SL = entry - stddev *
      // sl_mult
      FPN<F> hundred = FPN_FromDouble<F>(100.0);
      FPN<F> tp_mult = FPN_Mul(ctrl->config.take_profit_pct, hundred);
      FPN<F> sl_mult = FPN_Mul(ctrl->config.stop_loss_pct, hundred);
      FPN<F> tp_offset = FPN_Mul(stddev, tp_mult);
      FPN<F> sl_offset = FPN_Mul(stddev, sl_mult);

      // percentage fallback when rolling stats arent ready
      FPN<F> one = FPN_FromDouble<F>(1.0);
      FPN<F> tp_pct_up =
          FPN_Mul(fill_price, FPN_AddSat(one, ctrl->config.take_profit_pct));
      FPN<F> sl_pct_dn =
          FPN_Mul(fill_price, FPN_SubSat(one, ctrl->config.stop_loss_pct));

      // branchless select: volatility-based if stats ready, percentage-based
      // otherwise
      FPN<F> vol_tp = FPN_AddSat(fill_price, tp_offset);
      FPN<F> vol_sl = FPN_SubSat(fill_price, sl_offset);

      uint64_t stats_mask = -(uint64_t)has_stats;
      FPN<F> tp_price, sl_price;
      for (unsigned w = 0; w < FPN<F>::N; w++) {
        tp_price.w[w] =
            (vol_tp.w[w] & stats_mask) | (tp_pct_up.w[w] & ~stats_mask);
        sl_price.w[w] =
            (vol_sl.w[w] & stats_mask) | (sl_pct_dn.w[w] & ~stats_mask);
      }
      tp_price.sign = (vol_tp.sign & has_stats) | (tp_pct_up.sign & !has_stats);
      sl_price.sign = (vol_sl.sign & has_stats) | (sl_pct_dn.sign & !has_stats);

      // TP FLOOR: ensure TP is above the round-trip fee breakeven point
      // min_tp = entry + entry * fee_rate * 3 (2x for round-trip fees + 1x
      // safety margin)
      FPN<F> three = FPN_FromDouble<F>(3.0);
      FPN<F> fee_floor_offset =
          FPN_Mul(fill_price, FPN_Mul(ctrl->config.fee_rate, three));
      FPN<F> tp_floor = FPN_AddSat(fill_price, fee_floor_offset);
      tp_price = FPN_Max(tp_price, tp_floor);

      // SL FLOOR: ensure SL distance is at least half the TP distance
      // prevents SL from being so tight that normal price fluctuations trigger
      // it with TP at +$209 (fee floor), SL should be at least -$104.50 this
      // gives a minimum 2:1 reward-to-risk ratio
      FPN<F> tp_dist =
          FPN_Sub(tp_price, fill_price); // how far TP is from entry
      FPN<F> half = FPN_FromDouble<F>(0.5);
      FPN<F> min_sl_dist =
          FPN_Mul(tp_dist, half); // SL must be at least half that
      FPN<F> sl_floor = FPN_SubSat(fill_price, min_sl_dist);
      sl_price = FPN_Min(
          sl_price, sl_floor); // Min because SL is below entry (lower = wider)

      int slot = Portfolio_AddPositionWithExits(&ctrl->portfolio, sized_qty,
                                                fill_price, tp_price, sl_price);
      ctrl->total_buys++;
      if (slot >= 0) {
        ctrl->entry_ticks[slot] = ctrl->total_ticks;
        ctrl->entry_strategy[slot] = (uint8_t)ctrl->strategy_id;
        ctrl->portfolio.positions[slot].original_tp = tp_price;
        ctrl->portfolio.positions[slot].original_sl = sl_price;
      }

      // deduct cost + entry fee from balance
      ctrl->balance = FPN_SubSat(ctrl->balance, total_cost);
      ctrl->total_fees = FPN_AddSat(ctrl->total_fees, entry_fee);

      // buffer buy record (no file I/O on hot path)
      TradeLogBuffer_PushBuy(&ctrl->trade_buf, ctrl->total_ticks,
                              FPN_ToDouble(fill_price), FPN_ToDouble(sized_qty),
                              FPN_ToDouble(tp_price), FPN_ToDouble(sl_price),
                              FPN_ToDouble(ctrl->buy_conds.price),
                              FPN_ToDouble(ctrl->buy_conds.volume));
    }

    fills &= fills - 1;
  }

  // clear consumed fills from pool - free slots for BuyGate
  pool->bitmap &= ~consumed;
  ctrl->prev_bitmap = pool->bitmap;

  //==================================================================================================
  // ACTIVE PHASE - EVERY N TICKS: slow-path operations
  //==================================================================================================
  ctrl->tick_count++;
  if (ctrl->tick_count < ctrl->config.poll_interval)
    return;
  ctrl->tick_count = 0;

  // drain exit buffer - log sells
  for (uint32_t i = 0; i < ctrl->exit_buf.count; i++) {
    ExitRecord<F> *rec = &ctrl->exit_buf.records[i];
    Position<F> *pos = &ctrl->portfolio.positions[rec->position_index];

    double entry_d = FPN_ToDouble(pos->entry_price);
    double exit_d = FPN_ToDouble(rec->exit_price);
    double qty_d = FPN_ToDouble(pos->quantity);
    double delta_pct = 0.0;
    if (entry_d != 0.0)
      delta_pct = ((exit_d - entry_d) / entry_d) * 100.0;

    // exit fee: fee_rate * (exit_price * quantity)
    FPN<F> gross_proceeds = FPN_Mul(rec->exit_price, pos->quantity);
    FPN<F> exit_fee = FPN_Mul(gross_proceeds, ctrl->config.fee_rate);
    FPN<F> net_proceeds = FPN_SubSat(gross_proceeds, exit_fee);

    // realized P&L: net proceeds - total cost (entry cost + entry fee)
    // entry fee = entry_price * qty * fee_rate (reconstructed since we dont
    // store it per position)
    FPN<F> entry_cost = FPN_Mul(pos->entry_price, pos->quantity);
    FPN<F> entry_fee_recon = FPN_Mul(entry_cost, ctrl->config.fee_rate);
    FPN<F> total_entry_cost = FPN_AddSat(entry_cost, entry_fee_recon);
    FPN<F> pos_pnl = FPN_Sub(net_proceeds, total_entry_cost);
    ctrl->realized_pnl = FPN_AddSat(ctrl->realized_pnl, pos_pnl);

    // return net proceeds to paper trading balance (after exit fee)
    ctrl->balance = FPN_AddSat(ctrl->balance, net_proceeds);
    ctrl->total_fees = FPN_AddSat(ctrl->total_fees, exit_fee);

    const char *reason = (rec->reason == 0) ? "TP" : "SL";
    ctrl->wins += (rec->reason == 0);
    ctrl->losses += (rec->reason == 1);

    // track win/loss dollar amounts for profit factor
    // pos_pnl is net (after fees), positive for wins, negative for losses
    int is_win = !pos_pnl.sign & !FPN_IsZero(pos_pnl);
    int is_loss = pos_pnl.sign;
    // branchless accumulate: add to wins if positive, add negated to losses if
    // negative
    {
      constexpr unsigned N2 = FPN<F>::N;
      uint64_t win_mask = -(uint64_t)is_win;
      uint64_t loss_mask = -(uint64_t)is_loss;
      FPN<F> neg_pnl = FPN_Negate(pos_pnl);
      FPN<F> win_add, loss_add;
      for (unsigned w = 0; w < N2; w++) {
        win_add.w[w] = pos_pnl.w[w] & win_mask;
        loss_add.w[w] = neg_pnl.w[w] & loss_mask;
      }
      win_add.sign = 0;
      loss_add.sign = 0;
      ctrl->gross_wins = FPN_AddSat(ctrl->gross_wins, win_add);
      ctrl->gross_losses = FPN_AddSat(ctrl->gross_losses, loss_add);
    }

    // track hold time
    uint64_t entry_tick = ctrl->entry_ticks[rec->position_index];
    ctrl->total_hold_ticks +=
        (rec->tick > entry_tick) ? (rec->tick - entry_tick) : 0;
    TradeLogBuffer_PushSell(&ctrl->trade_buf, rec->tick, exit_d, qty_d, entry_d,
                            delta_pct, reason);
  }
  ExitBuffer_Clear(&ctrl->exit_buf);

  // TIME-BASED EXIT: close positions held too long with insufficient gain
  // frees capital trapped in positions where TP became unreachable (e.g. volatility
  // dropped after entry, making the stddev-based TP too far away)
  if (ctrl->config.max_hold_ticks > 0) {
    uint16_t active_check = ctrl->portfolio.active_bitmap;
    while (active_check) {
      int idx = __builtin_ctz(active_check);
      Position<F> *pos = &ctrl->portfolio.positions[idx];
      uint64_t entry_tick = ctrl->entry_ticks[idx];
      uint64_t held = (ctrl->total_ticks > entry_tick) ? (ctrl->total_ticks - entry_tick) : 0;

      if (held >= ctrl->config.max_hold_ticks) {
        // check if gain is below threshold — don't time-exit winners
        FPN<F> gain = FPN_Sub(current_price, pos->entry_price);
        FPN<F> gain_pct = FPN_DivNoAssert(gain, pos->entry_price);
        int low_gain = FPN_LessThan(gain_pct, ctrl->config.min_hold_gain_pct);

        if (low_gain) {
          double entry_d = FPN_ToDouble(pos->entry_price);
          double exit_d  = FPN_ToDouble(current_price);
          double qty_d   = FPN_ToDouble(pos->quantity);
          double delta_pct = 0.0;
          if (entry_d != 0.0) delta_pct = ((exit_d - entry_d) / entry_d) * 100.0;

          // compute realized P&L with fees (same as exit buffer drain)
          FPN<F> gross_proceeds = FPN_Mul(current_price, pos->quantity);
          FPN<F> exit_fee = FPN_Mul(gross_proceeds, ctrl->config.fee_rate);
          FPN<F> net_proceeds = FPN_SubSat(gross_proceeds, exit_fee);
          FPN<F> entry_cost = FPN_Mul(pos->entry_price, pos->quantity);
          FPN<F> entry_fee_recon = FPN_Mul(entry_cost, ctrl->config.fee_rate);
          FPN<F> total_entry_cost = FPN_AddSat(entry_cost, entry_fee_recon);
          FPN<F> pos_pnl = FPN_Sub(net_proceeds, total_entry_cost);
          ctrl->realized_pnl = FPN_AddSat(ctrl->realized_pnl, pos_pnl);

          ctrl->balance = FPN_AddSat(ctrl->balance, net_proceeds);
          ctrl->total_fees = FPN_AddSat(ctrl->total_fees, exit_fee);
          ctrl->losses++;
          ctrl->gross_losses = FPN_AddSat(ctrl->gross_losses, FPN_Negate(pos_pnl));
          ctrl->total_hold_ticks += held;

          TradeLogBuffer_PushSell(&ctrl->trade_buf, ctrl->total_ticks, exit_d, qty_d,
                                  entry_d, delta_pct, "TIME");
          Portfolio_RemovePosition(&ctrl->portfolio, idx);
        }
      }
      active_check &= active_check - 1;
    }
  }

  // drain buffered trade records to CSV (file I/O moved off hot path)
  TradeLogBuffer_Drain(&ctrl->trade_buf, trade_log);

  // update rolling market stats - tracks price/volume trends for dynamic gate
  // adjustment
  RollingStats_Push(&ctrl->rolling, current_price, current_volume);
  RollingStats_Push(ctrl->rolling_long, current_price, current_volume);

  // compute unrealized P&L and estimate exit fees on open positions
  // gross P&L is what Portfolio_ComputePnL returns (price delta * qty)
  // net P&L subtracts estimated exit fees so the regression optimizes on real
  // profitability
  FPN<F> gross_pnl = Portfolio_ComputePnL(&ctrl->portfolio, current_price);
  FPN<F> portfolio_value =
      Portfolio_ComputeValue(&ctrl->portfolio, current_price);
  FPN<F> estimated_exit_fees = FPN_Mul(portfolio_value, ctrl->config.fee_rate);
  ctrl->portfolio_delta = FPN_Sub(gross_pnl, estimated_exit_fees);

  // regime detection: classify market state and switch strategy if needed
  {
    int old_regime = ctrl->regime.current_regime;
    Regime_Classify(&ctrl->regime, &ctrl->rolling, ctrl->rolling_long,
                    ctrl->mean_rev.last_regression.r_squared, &ctrl->config);
    int new_regime = ctrl->regime.current_regime;
    if (new_regime != old_regime) {
      int old_strategy = ctrl->strategy_id;
      ctrl->strategy_id = Regime_ToStrategy(new_regime);
      ctrl->regime.regime_start_tick = ctrl->total_ticks;
      if (ctrl->strategy_id != old_strategy)
        Regime_AdjustPositions(&ctrl->portfolio, &ctrl->rolling,
                                old_regime, new_regime, ctrl->entry_strategy, &ctrl->config);
    }
  }

  // strategy dispatch: adapt filters + compute buy gate based on active strategy
  switch (ctrl->strategy_id) {
  case STRATEGY_MEAN_REVERSION:
    MeanReversion_Adapt(&ctrl->mean_rev, current_price, ctrl->portfolio_delta,
                         ctrl->portfolio.active_bitmap, &ctrl->buy_conds,
                         &ctrl->config);
    MeanReversion_ExitAdjust(&ctrl->portfolio, current_price, &ctrl->rolling,
                              &ctrl->mean_rev, &ctrl->config);
    ctrl->buy_conds = MeanReversion_BuySignal(&ctrl->mean_rev, &ctrl->rolling,
                                               ctrl->rolling_long, &ctrl->config);
    ctrl->buy_conds.gate_direction = 0;  // buy below (dips)
    break;
  case STRATEGY_MOMENTUM:
    Momentum_Adapt(&ctrl->momentum, current_price, ctrl->portfolio_delta,
                    ctrl->portfolio.active_bitmap, &ctrl->buy_conds,
                    &ctrl->config);
    Momentum_ExitAdjust(&ctrl->portfolio, current_price, &ctrl->rolling,
                         &ctrl->momentum, &ctrl->config);
    ctrl->buy_conds = Momentum_BuySignal(&ctrl->momentum, &ctrl->rolling,
                                          ctrl->rolling_long, &ctrl->config);
    ctrl->buy_conds.gate_direction = 1;  // buy above (breakouts)
    break;
  }

  // volatile regime: pause buying entirely (existing positions keep running)
  if (ctrl->regime.current_regime == REGIME_VOLATILE) {
    ctrl->buy_conds.price = FPN_Zero<F>();
    ctrl->buy_conds.volume = FPN_Zero<F>();
  }
}
//======================================================================================================
// [SNAPSHOT SAVE/LOAD]
//======================================================================================================
// saves portfolio + realized P&L + adaptive filter state to a binary snapshot
// call on the slow path or after any position change
//======================================================================================================
template <unsigned F>
inline void PortfolioController_SaveSnapshot(const PortfolioController<F> *ctrl,
                                             const char *filepath) {
  Portfolio_Save(&ctrl->portfolio, ctrl->realized_pnl,
                 ctrl->mean_rev.live_offset_pct, ctrl->mean_rev.live_vol_mult,
                 ctrl->mean_rev.live_stddev_mult, ctrl->balance, filepath);
}

template <unsigned F>
inline int PortfolioController_LoadSnapshot(PortfolioController<F> *ctrl,
                                            const char *filepath) {
  FPN<F> realized, offset, vmult, stdmult, bal;
  int ok = Portfolio_Load(&ctrl->portfolio, &realized, &offset, &vmult, &stdmult,
                          &bal, filepath);
  if (ok) {
    ctrl->realized_pnl = realized;
    ctrl->mean_rev.live_offset_pct = offset;
    ctrl->mean_rev.live_vol_mult = vmult;
    ctrl->mean_rev.live_stddev_mult = stdmult;
    ctrl->balance = bal;
    // skip warmup if we have positions - we're resuming a session
    if (ctrl->portfolio.active_bitmap != 0) {
      ctrl->state = CONTROLLER_ACTIVE;
    }
  }
  return ok;
}
//======================================================================================================
//======================================================================================================
//======================================================================================================
#endif // PORTFOLIO_CONTROLLER_HPP
