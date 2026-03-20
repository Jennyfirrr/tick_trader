//======================================================================================================
// [PORTFOLIO CONTROLLER]
//======================================================================================================
// this just tracks the portfolio delta and tracks performance over time, uses linear regression and the GCN to edit and update the gate conditions for buying and selling based on portoflio performance, probably gonna add configrurebale parameters for polling rates and stuff, this should be a seperate module from the actual order engine, as it shouldnt interfere with the order execution, this simply pipes conditions to the gates
//======================================================================================================
// [INCLUDE]
//======================================================================================================
#ifndef PORTFOLIO_CONTROLLER_HPP
#define PORTFOLIO_CONTROLLER_HPP

#include "Portfolio.hpp"
#include "OrderGates.hpp"
#include "../FixedPoint/FixedPointN.hpp"
#include "../ML_Headers/LinearRegression3X.hpp"
#include "../ML_Headers/ROR_regressor.hpp"
#include "../ML_Headers/RollingStats.hpp"
#include "../DataStream/TradeLog.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
//======================================================================================================
// [CONFIG]
//======================================================================================================
template <unsigned F> struct ControllerConfig {
    uint32_t poll_interval;     // ticks between slow-path runs
    uint32_t warmup_ticks;      // ticks to observe before trading
    FPN<F> r2_threshold;    // min R^2 to trust regression
    FPN<F> slope_scale_buy; // how much slope shifts buy price threshold
    FPN<F> max_shift;       // max drift from initial buy conditions
    FPN<F> take_profit_pct; // per-position take profit (e.g. 0.03 = 3%)
    FPN<F> stop_loss_pct;   // per-position stop loss (e.g. 0.015 = 1.5%)
    FPN<F> starting_balance; // paper trading starting balance (e.g. 10000.0)
    // market microstructure filters (initial values - adapted at runtime by P&L regression)
    FPN<F> volume_multiplier;  // buy only when tick volume >= this * rolling_avg (e.g. 3.0)
    FPN<F> entry_offset_pct;   // buy gate offset below rolling mean (e.g. 0.0015 = 0.15%)
    FPN<F> spacing_multiplier; // min entry spacing = stddev * this (e.g. 2.0)
    // adaptation clamps - how far the filters can drift from their initial values
    FPN<F> offset_min;         // min entry_offset_pct (most aggressive, e.g. 0.0005 = 0.05%)
    FPN<F> offset_max;         // max entry_offset_pct (most defensive, e.g. 0.005 = 0.5%)
    FPN<F> vol_mult_min;       // min volume_multiplier (most aggressive, e.g. 1.5)
    FPN<F> vol_mult_max;       // max volume_multiplier (most defensive, e.g. 6.0)
    FPN<F> filter_scale;       // how much P&L slope shifts the filters (e.g. 0.50)
};
//======================================================================================================
template <unsigned F> inline ControllerConfig<F> ControllerConfig_Default() {
    ControllerConfig<F> cfg;
    cfg.poll_interval   = 100;
    cfg.warmup_ticks    = MAX_WINDOW * 8; // 64 ticks, enough for one full ROR cycle
    cfg.r2_threshold    = FPN_FromDouble<F>(0.30);
    cfg.slope_scale_buy = FPN_FromDouble<F>(0.50);
    cfg.max_shift       = FPN_FromDouble<F>(5.00);
    cfg.take_profit_pct    = FPN_FromDouble<F>(0.03);
    cfg.stop_loss_pct      = FPN_FromDouble<F>(0.015);
    cfg.starting_balance   = FPN_FromDouble<F>(1000000.0); // 1M default so tests arent balance-limited
    cfg.volume_multiplier  = FPN_FromDouble<F>(3.0);
    cfg.entry_offset_pct   = FPN_FromDouble<F>(0.0015);
    cfg.spacing_multiplier = FPN_FromDouble<F>(2.0);
    cfg.offset_min         = FPN_FromDouble<F>(0.0005);  // 0.05% - most aggressive
    cfg.offset_max         = FPN_FromDouble<F>(0.005);   // 0.5%  - most defensive
    cfg.vol_mult_min       = FPN_FromDouble<F>(1.5);     // 1.5x  - most aggressive
    cfg.vol_mult_max       = FPN_FromDouble<F>(6.0);     // 6.0x  - most defensive
    cfg.filter_scale       = FPN_FromDouble<F>(0.50);    // how fast filters adapt
    return cfg;
}
//======================================================================================================
// [CONFIG PARSER]
//======================================================================================================
// simple key=value text file parser, no JSON, no external libs
// returns defaults if file is missing or unreadable
//======================================================================================================
template <unsigned F> inline ControllerConfig<F> ControllerConfig_Load(const char *filepath) {
    ControllerConfig<F> cfg = ControllerConfig_Default<F>();

    FILE *f = fopen(filepath, "r");
    if (!f) return cfg;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        // strip \r\n
        int len = 0;
        while (line[len] && line[len] != '\n' && line[len] != '\r') len++;
        line[len] = '\0';

        // skip empty lines and comments
        if (len == 0 || line[0] == '#') continue;

        // find '='
        int eq_pos = -1;
        for (int i = 0; i < len; i++) {
            if (line[i] == '=') { eq_pos = i; break; }
        }
        if (eq_pos < 0) continue;

        // null-terminate key, value starts after '='
        line[eq_pos] = '\0';
        char *key    = line;
        char *val    = &line[eq_pos + 1];

        if (strcmp(key, "poll_interval") == 0)        cfg.poll_interval   = (uint32_t)atol(val);
        else if (strcmp(key, "warmup_ticks") == 0)     cfg.warmup_ticks    = (uint32_t)atol(val);
        else if (strcmp(key, "r2_threshold") == 0)     cfg.r2_threshold    = FPN_FromDouble<F>(atof(val));
        else if (strcmp(key, "slope_scale_buy") == 0)  cfg.slope_scale_buy = FPN_FromDouble<F>(atof(val));
        else if (strcmp(key, "max_shift") == 0)        cfg.max_shift       = FPN_FromDouble<F>(atof(val));
        else if (strcmp(key, "take_profit_pct") == 0)  cfg.take_profit_pct = FPN_FromDouble<F>(atof(val) / 100.0);
        else if (strcmp(key, "stop_loss_pct") == 0)      cfg.stop_loss_pct      = FPN_FromDouble<F>(atof(val) / 100.0);
        else if (strcmp(key, "starting_balance") == 0)  cfg.starting_balance   = FPN_FromDouble<F>(atof(val));
        else if (strcmp(key, "volume_multiplier") == 0)  cfg.volume_multiplier  = FPN_FromDouble<F>(atof(val));
        else if (strcmp(key, "entry_offset_pct") == 0)   cfg.entry_offset_pct   = FPN_FromDouble<F>(atof(val) / 100.0);
        else if (strcmp(key, "spacing_multiplier") == 0)  cfg.spacing_multiplier = FPN_FromDouble<F>(atof(val));
        else if (strcmp(key, "offset_min") == 0)          cfg.offset_min         = FPN_FromDouble<F>(atof(val) / 100.0);
        else if (strcmp(key, "offset_max") == 0)          cfg.offset_max         = FPN_FromDouble<F>(atof(val) / 100.0);
        else if (strcmp(key, "vol_mult_min") == 0)        cfg.vol_mult_min       = FPN_FromDouble<F>(atof(val));
        else if (strcmp(key, "vol_mult_max") == 0)        cfg.vol_mult_max       = FPN_FromDouble<F>(atof(val));
        else if (strcmp(key, "filter_scale") == 0)        cfg.filter_scale       = FPN_FromDouble<F>(atof(val));
    }

    fclose(f);
    return cfg;
}
//======================================================================================================
// [CONTROLLER STRUCT]
//======================================================================================================
#define CONTROLLER_WARMUP 0
#define CONTROLLER_ACTIVE 1
//======================================================================================================
template <unsigned F> struct PortfolioController {
    Portfolio<F> portfolio;
    FPN<F> portfolio_delta;       // unrealized P&L (current open positions)
    FPN<F> realized_pnl;          // cumulative realized P&L (closed positions)
    FPN<F> balance;               // paper trading balance (deducted on buy, added on sell)

    RegressionFeederX<F> feeder;
    RORRegressor<F> ror;
    RollingStats<F> rolling;

    BuySideGateConditions<F> buy_conds;
    BuySideGateConditions<F> buy_conds_initial;

    // live adaptive filter values - start from config, adjusted by P&L regression
    FPN<F> live_offset_pct;     // current entry offset (adapts between offset_min..offset_max)
    FPN<F> live_vol_mult;       // current volume multiplier (adapts between vol_mult_min..vol_mult_max)

    ExitBuffer<F> exit_buf;

    uint64_t prev_bitmap;
    uint64_t tick_count;
    uint64_t total_ticks;

    int state;
    FPN<F> price_sum;
    FPN<F> volume_sum;
    uint64_t warmup_count;

    ControllerConfig<F> config;
};
//======================================================================================================
// [INIT]
//======================================================================================================
template <unsigned F> inline void PortfolioController_Init(PortfolioController<F> *ctrl, ControllerConfig<F> config) {
    Portfolio_Init(&ctrl->portfolio);
    ctrl->portfolio_delta = FPN_Zero<F>();
    ctrl->realized_pnl    = FPN_Zero<F>();
    ctrl->balance          = config.starting_balance;

    ctrl->feeder  = RegressionFeederX_Init<F>();
    ctrl->ror     = RORRegressor_Init<F>();
    ctrl->rolling = RollingStats_Init<F>();

    // during warmup, buy gate is disabled - price 0 means nothing passes LessThanOrEqual
    ctrl->buy_conds.price   = FPN_Zero<F>();
    ctrl->buy_conds.volume  = FPN_Zero<F>();
    ctrl->buy_conds_initial = ctrl->buy_conds;

    // adaptive filters start at config values
    ctrl->live_offset_pct = config.entry_offset_pct;
    ctrl->live_vol_mult   = config.volume_multiplier;

    ExitBuffer_Init(&ctrl->exit_buf);

    ctrl->prev_bitmap  = 0;
    ctrl->tick_count   = 0;
    ctrl->total_ticks  = 0;

    ctrl->state        = CONTROLLER_WARMUP;
    ctrl->price_sum    = FPN_Zero<F>();
    ctrl->volume_sum   = FPN_Zero<F>();
    ctrl->warmup_count = 0;

    ctrl->config = config;
}
//======================================================================================================
// [BRANCHLESS GATE ADJUSTMENT]
//======================================================================================================
// shifts buy price condition based on regression slope, masked by R^2 confidence,
// clamped to max_shift from initial conditions - all operations are branchless
//======================================================================================================
template <unsigned F>
inline void PortfolioController_AdjustBuyGate(PortfolioController<F> *ctrl, LinearRegression3XResult<F> *result) {
    constexpr unsigned N = FPN<F>::N;

    int confident = FPN_GreaterThanOrEqual(result->r_squared, ctrl->config.r2_threshold);

    FPN<F> shift = FPN_Mul(result->model.slope, ctrl->config.slope_scale_buy);

    // clamp shift magnitude to max_shift (Min/Max are already branchless)
    shift = FPN_Min(shift, ctrl->config.max_shift);
    shift = FPN_Max(shift, FPN_Negate(ctrl->config.max_shift));

    // mask shift to zero if not confident - word-level branchless mask
    FPN<F> masked_shift;
    uint64_t conf_mask = -(uint64_t)confident;
#pragma GCC unroll 65534
    for (unsigned i = 0; i < N; i++) {
        masked_shift.w[i] = shift.w[i] & conf_mask;
    }
    masked_shift.sign = shift.sign & confident;

    // apply shift
    FPN<F> new_price = FPN_AddSat(ctrl->buy_conds.price, masked_shift);

    // clamp to initial +/- max_shift
    FPN<F> upper = FPN_AddSat(ctrl->buy_conds_initial.price, ctrl->config.max_shift);
    FPN<F> lower = FPN_SubSat(ctrl->buy_conds_initial.price, ctrl->config.max_shift);
    new_price = FPN_Max(new_price, lower);
    new_price = FPN_Min(new_price, upper);

    ctrl->buy_conds.price = new_price;
}
//======================================================================================================
// [TICK - MAIN CONTROLLER FUNCTION]
//======================================================================================================
// called every tick. fill consumption runs every tick (zero unprotected exposure).
// regression/adjustment runs every poll_interval ticks (slow path).
//======================================================================================================
template <unsigned F>
inline void PortfolioController_Tick(PortfolioController<F> *ctrl, OrderPool<F> *pool, FPN<F> current_price,
                                     FPN<F> current_volume, TradeLog *trade_log) {
    // always increment tick counter (branchless, single add)
    ctrl->total_ticks++;

    //==================================================================================================
    // WARMUP PHASE
    //==================================================================================================
    if (ctrl->state == CONTROLLER_WARMUP) {
        ctrl->price_sum  = FPN_AddSat(ctrl->price_sum, current_price);
        ctrl->volume_sum = FPN_AddSat(ctrl->volume_sum, current_volume);
        ctrl->warmup_count++;

        // feed prices to regression feeder and rolling stats during warmup
        RegressionFeederX_Push(&ctrl->feeder, current_price);
        RollingStats_Push(&ctrl->rolling, current_price, current_volume);

        if (ctrl->warmup_count >= ctrl->config.warmup_ticks) {
            // use rolling stats for initial buy gate - with offset below mean
            // this means "buy when price dips entry_offset_pct below the rolling average"
            // volume gate uses rolling avg * multiplier so only significant trades pass
            FPN<F> buy_price = RollingStats_BuyPrice(&ctrl->rolling, ctrl->live_offset_pct);
            FPN<F> buy_vol   = FPN_Mul(ctrl->rolling.volume_avg, ctrl->live_vol_mult);

            ctrl->buy_conds.price   = buy_price;
            ctrl->buy_conds.volume  = buy_vol;
            ctrl->buy_conds_initial = ctrl->buy_conds;

            // reset feeder for P&L tracking in active phase
            ctrl->feeder = RegressionFeederX_Init<F>();
            ctrl->ror    = RORRegressor_Init<F>();

            ctrl->state = CONTROLLER_ACTIVE;
        }
        return;
    }

    //==================================================================================================
    // ACTIVE PHASE - EVERY TICK: consume new fills immediately
    //==================================================================================================
    // branchless: mask new_fills to zero if portfolio is full, then the while loop body count is 0
    // the loop itself is unavoidable (variable fill count) but the outer gate is eliminated
    //==================================================================================================
    uint64_t new_fills = pool->bitmap & ~ctrl->prev_bitmap;
    uint64_t can_fill  = -(uint64_t)(!Portfolio_IsFull(&ctrl->portfolio)); // all 1s if room, all 0s if full
    uint64_t fills     = new_fills & can_fill;
    uint64_t consumed  = fills; // track what we actually process for pool clearing

    while (fills) {
        uint32_t idx = __builtin_ctzll(fills);

        FPN<F> fill_price = pool->slots[idx].price;
        FPN<F> fill_qty   = pool->slots[idx].quantity;

        // consolidation: find existing position at same price
        // branchless path selection: found_mask is all 1s if existing position, all 0s if new
        int existing   = Portfolio_FindByPrice(&ctrl->portfolio, fill_price);
        int found      = (existing >= 0);
        uint64_t found_mask = -(uint64_t)found;

        // consolidation path: add quantity to existing position
        // always compute the index (clamp to 0 if not found to avoid OOB, result is masked anyway)
        int safe_idx = existing & (int)(found_mask & 0xF); // 0 if not found, actual index if found
        FPN<F> old_qty = ctrl->portfolio.positions[safe_idx].quantity;
        FPN<F> new_qty = FPN_AddSat(old_qty, fill_qty);
        // apply consolidation: write new quantity only if found (word-level mask)
        for (unsigned w = 0; w < FPN<F>::N; w++) {
            ctrl->portfolio.positions[safe_idx].quantity.w[w] =
                (new_qty.w[w] & found_mask) | (old_qty.w[w] & ~found_mask);
        }
        ctrl->portfolio.positions[safe_idx].quantity.sign =
            (new_qty.sign & (int)found) | (old_qty.sign & (int)(!found));

        // entry spacing check: reject fills that are too close to existing positions
        // walks the portfolio bitmap to find the minimum distance from fill_price to any entry
        // this spreads the 16 slots across actual price levels instead of clustering
        FPN<F> min_spacing = RollingStats_EntrySpacing(&ctrl->rolling, ctrl->config.spacing_multiplier);
        int too_close = 0;
        {
            uint16_t active_pos = ctrl->portfolio.active_bitmap;
            while (active_pos) {
                int pidx = __builtin_ctz(active_pos);
                FPN<F> dist = FPN_Sub(fill_price, ctrl->portfolio.positions[pidx].entry_price);
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

        // balance check: can we afford this position? (branchless)
        FPN<F> cost = FPN_Mul(fill_price, fill_qty);
        int can_afford = FPN_GreaterThanOrEqual(ctrl->balance, cost);

        // new position path: only if not found AND portfolio has room AND not too close AND can afford
        int is_new     = !found & !Portfolio_IsFull(&ctrl->portfolio) & !too_close & can_afford;
        if (is_new) {
            // volatility-based TP/SL: scale exit distances by rolling price stddev
            // in calm markets, exits tighten (take smaller profits faster)
            // in volatile markets, exits widen (give positions room to breathe)
            // falls back to percentage-based if rolling stats not populated yet
            FPN<F> stddev = ctrl->rolling.price_stddev;
            int has_stats = !FPN_IsZero(stddev);

            // volatility path: TP = entry + stddev * tp_pct_as_multiplier, SL = entry - stddev * sl_pct_as_multiplier
            // we reuse take_profit_pct and stop_loss_pct as stddev multipliers in this mode
            // tp_pct=0.03 means TP is 0.03 * stddev above entry (configurable ratio)
            // but that would be tiny - so we scale: tp = stddev * (tp_pct * 100) = stddev * 3.0
            FPN<F> hundred = FPN_FromDouble<F>(100.0);
            FPN<F> tp_mult = FPN_Mul(ctrl->config.take_profit_pct, hundred);
            FPN<F> sl_mult = FPN_Mul(ctrl->config.stop_loss_pct, hundred);
            FPN<F> tp_offset = FPN_Mul(stddev, tp_mult);
            FPN<F> sl_offset = FPN_Mul(stddev, sl_mult);

            // percentage fallback: used when rolling stats arent ready yet
            FPN<F> one       = FPN_FromDouble<F>(1.0);
            FPN<F> tp_pct_up = FPN_Mul(fill_price, FPN_AddSat(one, ctrl->config.take_profit_pct));
            FPN<F> sl_pct_dn = FPN_Mul(fill_price, FPN_SubSat(one, ctrl->config.stop_loss_pct));

            // branchless select: volatility-based if stats ready, percentage-based otherwise
            FPN<F> vol_tp = FPN_AddSat(fill_price, tp_offset);
            FPN<F> vol_sl = FPN_SubSat(fill_price, sl_offset);

            uint64_t stats_mask = -(uint64_t)has_stats;
            FPN<F> tp_price, sl_price;
            for (unsigned w = 0; w < FPN<F>::N; w++) {
                tp_price.w[w] = (vol_tp.w[w] & stats_mask) | (tp_pct_up.w[w] & ~stats_mask);
                sl_price.w[w] = (vol_sl.w[w] & stats_mask) | (sl_pct_dn.w[w] & ~stats_mask);
            }
            tp_price.sign = (vol_tp.sign & has_stats) | (tp_pct_up.sign & !has_stats);
            sl_price.sign = (vol_sl.sign & has_stats) | (sl_pct_dn.sign & !has_stats);

            Portfolio_AddPositionWithExits(&ctrl->portfolio, fill_qty, fill_price, tp_price, sl_price);

            // deduct from paper trading balance
            ctrl->balance = FPN_SubSat(ctrl->balance, cost);

            // log buy
            double price_d = FPN_ToDouble(fill_price);
            double qty_d   = FPN_ToDouble(fill_qty);
            double tp_d    = FPN_ToDouble(tp_price);
            double sl_d    = FPN_ToDouble(sl_price);
            double bc_p    = FPN_ToDouble(ctrl->buy_conds.price);
            double bc_v    = FPN_ToDouble(ctrl->buy_conds.volume);
            TradeLog_Buy(trade_log, ctrl->total_ticks, price_d, qty_d, tp_d, sl_d, bc_p, bc_v);
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
    if (ctrl->tick_count < ctrl->config.poll_interval) return;
    ctrl->tick_count = 0;

    // drain exit buffer - log sells
    for (uint32_t i = 0; i < ctrl->exit_buf.count; i++) {
        ExitRecord<F> *rec = &ctrl->exit_buf.records[i];
        Position<F> *pos   = &ctrl->portfolio.positions[rec->position_index];

        double entry_d   = FPN_ToDouble(pos->entry_price);
        double exit_d    = FPN_ToDouble(rec->exit_price);
        double qty_d     = FPN_ToDouble(pos->quantity);
        double delta_pct = 0.0;
        if (entry_d != 0.0) delta_pct = ((exit_d - entry_d) / entry_d) * 100.0;

        // accumulate realized P&L: (exit - entry) * quantity
        FPN<F> pos_pnl = FPN_Mul(FPN_Sub(rec->exit_price, pos->entry_price), pos->quantity);
        ctrl->realized_pnl = FPN_AddSat(ctrl->realized_pnl, pos_pnl);

        // return proceeds to paper trading balance: exit_price * quantity
        FPN<F> proceeds = FPN_Mul(rec->exit_price, pos->quantity);
        ctrl->balance = FPN_AddSat(ctrl->balance, proceeds);

        const char *reason = (rec->reason == 0) ? "TP" : "SL";
        TradeLog_Sell(trade_log, rec->tick, exit_d, qty_d, entry_d, delta_pct, reason);
    }
    ExitBuffer_Clear(&ctrl->exit_buf);

    // update rolling market stats - tracks price/volume trends for dynamic gate adjustment
    RollingStats_Push(&ctrl->rolling, current_price, current_volume);

    // IDLE SQUEEZE: when portfolio is empty, use price slope to loosen/tighten filters
    // solves the chicken-and-egg problem: no positions -> no P&L -> no adaptation
    // if price is trending up and we have nothing, we're missing the move -> squeeze offset down
    // if price is trending down and we have nothing, we're correctly staying out -> no change
    //
    // branchless: empty_mask is all 1s when portfolio empty, all 0s when holding
    // positive price slope -> squeeze offset toward offset_min
    // the squeeze rate is proportional to the slope magnitude
    {
        constexpr unsigned N = FPN<F>::N;
        int is_empty     = (ctrl->portfolio.active_bitmap == 0);
        int slope_positive = !ctrl->rolling.volume_slope.sign;  // reuse sign bit of price movement
        // actually check price slope via (price_avg > buy_conds_initial.price) as a proxy
        // better: directly check if current price > buy gate price (we're trailing behind)
        int trailing = FPN_GreaterThan(current_price, ctrl->buy_conds.price);

        // squeeze fires when: portfolio empty AND current price above buy gate
        int should_squeeze = is_empty & trailing;
        uint64_t sq_mask = -(uint64_t)should_squeeze;

        // squeeze step: move offset toward offset_min by a fixed step per slow-path tick
        // step = (current_offset - offset_min) * 0.05 -> 5% closer to minimum each cycle
        FPN<F> offset_gap  = FPN_Sub(ctrl->live_offset_pct, ctrl->config.offset_min);
        FPN<F> squeeze_step = FPN_Mul(offset_gap, FPN_FromDouble<F>(0.05));

        // mask squeeze to zero if we shouldnt squeeze
        FPN<F> masked_squeeze;
        for (unsigned i = 0; i < N; i++) {
            masked_squeeze.w[i] = squeeze_step.w[i] & sq_mask;
        }
        masked_squeeze.sign = squeeze_step.sign & should_squeeze;

        // apply: decrease offset (more aggressive)
        ctrl->live_offset_pct = FPN_SubSat(ctrl->live_offset_pct, masked_squeeze);
        ctrl->live_offset_pct = FPN_Max(ctrl->live_offset_pct, ctrl->config.offset_min);

        // also squeeze volume multiplier toward vol_mult_min
        FPN<F> vmult_gap = FPN_Sub(ctrl->live_vol_mult, ctrl->config.vol_mult_min);
        FPN<F> vmult_step = FPN_Mul(vmult_gap, FPN_FromDouble<F>(0.05));
        FPN<F> masked_vmult;
        for (unsigned i = 0; i < N; i++) {
            masked_vmult.w[i] = vmult_step.w[i] & sq_mask;
        }
        masked_vmult.sign = vmult_step.sign & should_squeeze;

        ctrl->live_vol_mult = FPN_SubSat(ctrl->live_vol_mult, masked_vmult);
        ctrl->live_vol_mult = FPN_Max(ctrl->live_vol_mult, ctrl->config.vol_mult_min);
    }

    // update buy gate from rolling stats using LIVE adaptive filter values
    ctrl->buy_conds.price  = RollingStats_BuyPrice(&ctrl->rolling, ctrl->live_offset_pct);
    ctrl->buy_conds.volume = FPN_Mul(ctrl->rolling.volume_avg, ctrl->live_vol_mult);

    // compute unrealized P&L and push to regression
    ctrl->portfolio_delta = Portfolio_ComputePnL(&ctrl->portfolio, current_price);
    RegressionFeederX_Push(&ctrl->feeder, ctrl->portfolio_delta);

    // regression + ROR + adaptive filter adjustment
    if (ctrl->feeder.count >= MAX_WINDOW) {
        LinearRegression3XResult<F> inner = RegressionFeederX_Compute(&ctrl->feeder);
        RORRegressor_Push(&ctrl->ror, inner);

        // adjust buy gate price based on P&L slope (existing behavior)
        PortfolioController_AdjustBuyGate(ctrl, &inner);

        // ADAPTIVE FILTERS: adjust offset and volume multiplier based on P&L trend
        // positive slope (making money) -> loosen filters (smaller offset, lower vol mult)
        // negative slope (losing money) -> tighten filters (larger offset, higher vol mult)
        //
        // the shift direction is INVERTED from buy price adjustment:
        // - buy price: positive P&L -> shift price UP (buy more aggressively)
        // - offset: positive P&L -> shift offset DOWN (require less dip)
        // - vol mult: positive P&L -> shift mult DOWN (accept smaller trades)
        //
        // all branchless: same R^2 confidence mask, same clamp pattern
        {
            constexpr unsigned N = FPN<F>::N;
            int confident = FPN_GreaterThanOrEqual(inner.r_squared, ctrl->config.r2_threshold);
            uint64_t conf_mask = -(uint64_t)confident;

            // compute filter shift from slope: negate because positive P&L -> loosen (decrease)
            FPN<F> filter_shift = FPN_Mul(inner.model.slope, ctrl->config.filter_scale);
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
            ctrl->live_offset_pct = FPN_AddSat(ctrl->live_offset_pct, offset_shift);
            ctrl->live_offset_pct = FPN_Max(ctrl->live_offset_pct, ctrl->config.offset_min);
            ctrl->live_offset_pct = FPN_Min(ctrl->live_offset_pct, ctrl->config.offset_max);

            // apply shift to volume multiplier and clamp to [vol_mult_min, vol_mult_max]
            FPN<F> vol_shift = FPN_Mul(masked_shift, FPN_FromDouble<F>(0.1));
            ctrl->live_vol_mult = FPN_AddSat(ctrl->live_vol_mult, vol_shift);
            ctrl->live_vol_mult = FPN_Max(ctrl->live_vol_mult, ctrl->config.vol_mult_min);
            ctrl->live_vol_mult = FPN_Min(ctrl->live_vol_mult, ctrl->config.vol_mult_max);
        }
    }
}
//======================================================================================================
// [SNAPSHOT SAVE/LOAD]
//======================================================================================================
// saves portfolio + realized P&L + adaptive filter state to a binary snapshot
// call on the slow path or after any position change
//======================================================================================================
template <unsigned F>
inline void PortfolioController_SaveSnapshot(const PortfolioController<F> *ctrl, const char *filepath) {
    Portfolio_Save(&ctrl->portfolio, ctrl->realized_pnl,
                   ctrl->live_offset_pct, ctrl->live_vol_mult, ctrl->balance, filepath);
}

template <unsigned F>
inline int PortfolioController_LoadSnapshot(PortfolioController<F> *ctrl, const char *filepath) {
    FPN<F> realized, offset, vmult, bal;
    int ok = Portfolio_Load(&ctrl->portfolio, &realized, &offset, &vmult, &bal, filepath);
    if (ok) {
        ctrl->realized_pnl    = realized;
        ctrl->live_offset_pct = offset;
        ctrl->live_vol_mult   = vmult;
        ctrl->balance         = bal;
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
