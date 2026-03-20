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
    FPN<F> fee_rate;         // per-trade fee rate (e.g. 0.001 = 0.1% for Binance)
    FPN<F> risk_pct;         // fraction of balance to risk per position (e.g. 0.02 = 2%)
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
    // risk management
    FPN<F> max_drawdown_pct;   // halt trading if total P&L drops below this % of starting balance (e.g. 0.10 = 10%)
    FPN<F> max_exposure_pct;   // max fraction of balance deployed in positions (e.g. 0.50 = 50%)
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
    cfg.fee_rate           = FPN_FromDouble<F>(0.001);    // 0.1% per trade (Binance default)
    cfg.risk_pct           = FPN_FromDouble<F>(0.02);     // risk 2% of balance per position
    cfg.volume_multiplier  = FPN_FromDouble<F>(3.0);
    cfg.entry_offset_pct   = FPN_FromDouble<F>(0.0015);
    cfg.spacing_multiplier = FPN_FromDouble<F>(2.0);
    cfg.offset_min         = FPN_FromDouble<F>(0.0005);  // 0.05% - most aggressive
    cfg.offset_max         = FPN_FromDouble<F>(0.005);   // 0.5%  - most defensive
    cfg.vol_mult_min       = FPN_FromDouble<F>(1.5);     // 1.5x  - most aggressive
    cfg.vol_mult_max       = FPN_FromDouble<F>(6.0);     // 6.0x  - most defensive
    cfg.filter_scale       = FPN_FromDouble<F>(0.50);    // how fast filters adapt
    cfg.max_drawdown_pct   = FPN_FromDouble<F>(0.10);    // halt at 10% drawdown
    cfg.max_exposure_pct   = FPN_FromDouble<F>(0.50);    // max 50% of balance in positions
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
        else if (strcmp(key, "fee_rate") == 0)          cfg.fee_rate           = FPN_FromDouble<F>(atof(val) / 100.0);
        else if (strcmp(key, "risk_pct") == 0)          cfg.risk_pct           = FPN_FromDouble<F>(atof(val) / 100.0);
        else if (strcmp(key, "volume_multiplier") == 0)  cfg.volume_multiplier  = FPN_FromDouble<F>(atof(val));
        else if (strcmp(key, "entry_offset_pct") == 0)   cfg.entry_offset_pct   = FPN_FromDouble<F>(atof(val) / 100.0);
        else if (strcmp(key, "spacing_multiplier") == 0)  cfg.spacing_multiplier = FPN_FromDouble<F>(atof(val));
        else if (strcmp(key, "offset_min") == 0)          cfg.offset_min         = FPN_FromDouble<F>(atof(val) / 100.0);
        else if (strcmp(key, "offset_max") == 0)          cfg.offset_max         = FPN_FromDouble<F>(atof(val) / 100.0);
        else if (strcmp(key, "vol_mult_min") == 0)        cfg.vol_mult_min       = FPN_FromDouble<F>(atof(val));
        else if (strcmp(key, "vol_mult_max") == 0)        cfg.vol_mult_max       = FPN_FromDouble<F>(atof(val));
        else if (strcmp(key, "filter_scale") == 0)        cfg.filter_scale       = FPN_FromDouble<F>(atof(val));
        else if (strcmp(key, "max_drawdown_pct") == 0)   cfg.max_drawdown_pct   = FPN_FromDouble<F>(atof(val) / 100.0);
        else if (strcmp(key, "max_exposure_pct") == 0)    cfg.max_exposure_pct   = FPN_FromDouble<F>(atof(val) / 100.0);
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
    FPN<F> total_fees;            // cumulative fees paid (tracked for display)

    uint32_t wins;                // TP exits
    uint32_t losses;              // SL exits
    uint32_t total_buys;          // total entries

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
    ctrl->total_fees       = FPN_Zero<F>();
    ctrl->wins             = 0;
    ctrl->losses           = 0;
    ctrl->total_buys       = 0;

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
        // ignore stream quantity - we use position sizing based on balance
        // fill_price is the signal, not fill_qty

        // consolidation: find existing position at same price
        // with position sizing, consolidation is DISABLED - each fill at the same price
        // would just be a duplicate signal. we only want one position per price level.
        // the entry spacing check below handles preventing duplicate entries at nearby prices.
        int existing   = Portfolio_FindByPrice(&ctrl->portfolio, fill_price);
        int found      = (existing >= 0);

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

        // POSITION SIZING: compute quantity from balance and risk percentage
        // qty = (balance * risk_pct) / price
        // this replaces the stream quantity - we decide how much to buy, not the stream
        FPN<F> risk_amount = FPN_Mul(ctrl->balance, ctrl->config.risk_pct);
        FPN<F> sized_qty   = FPN_DivNoAssert(risk_amount, fill_price);

        // balance check: can we afford this position + entry fee? (branchless)
        FPN<F> cost      = FPN_Mul(fill_price, sized_qty);
        FPN<F> entry_fee = FPN_Mul(cost, ctrl->config.fee_rate);
        FPN<F> total_cost = FPN_AddSat(cost, entry_fee);
        int can_afford = FPN_GreaterThanOrEqual(ctrl->balance, total_cost);

        // CIRCUIT BREAKER: halt if total P&L has dropped below max_drawdown_pct of starting balance
        // total_pnl = realized + unrealized, drawdown_limit = -(starting_balance * max_drawdown_pct)
        FPN<F> total_pnl_check = FPN_AddSat(ctrl->realized_pnl, ctrl->portfolio_delta);
        FPN<F> drawdown_limit  = FPN_Negate(FPN_Mul(ctrl->config.starting_balance, ctrl->config.max_drawdown_pct));
        int not_blown = FPN_GreaterThan(total_pnl_check, drawdown_limit);

        // EXPOSURE LIMIT: cap total deployed capital at max_exposure_pct of starting balance
        // deployed = starting_balance - current_balance (how much is in positions)
        FPN<F> deployed     = FPN_Sub(ctrl->config.starting_balance, ctrl->balance);
        FPN<F> max_deployed = FPN_Mul(ctrl->config.starting_balance, ctrl->config.max_exposure_pct);
        int under_limit = FPN_LessThan(FPN_AddSat(deployed, total_cost), max_deployed);

        // new position: room AND spacing AND balance AND not blown AND under exposure limit
        int is_new = !found & !Portfolio_IsFull(&ctrl->portfolio) & !too_close
                   & can_afford & not_blown & under_limit;
        if (is_new) {
            // volatility-based TP/SL with fee floor
            FPN<F> stddev = ctrl->rolling.price_stddev;
            int has_stats = !FPN_IsZero(stddev);

            // volatility path: TP = entry + stddev * tp_mult, SL = entry - stddev * sl_mult
            FPN<F> hundred = FPN_FromDouble<F>(100.0);
            FPN<F> tp_mult = FPN_Mul(ctrl->config.take_profit_pct, hundred);
            FPN<F> sl_mult = FPN_Mul(ctrl->config.stop_loss_pct, hundred);
            FPN<F> tp_offset = FPN_Mul(stddev, tp_mult);
            FPN<F> sl_offset = FPN_Mul(stddev, sl_mult);

            // percentage fallback when rolling stats arent ready
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

            // TP FLOOR: ensure TP is above the round-trip fee breakeven point
            // min_tp = entry + entry * fee_rate * 3 (2x for round-trip fees + 1x safety margin)
            // if volatility TP is below this, use the floor instead
            FPN<F> three = FPN_FromDouble<F>(3.0);
            FPN<F> fee_floor_offset = FPN_Mul(fill_price, FPN_Mul(ctrl->config.fee_rate, three));
            FPN<F> tp_floor = FPN_AddSat(fill_price, fee_floor_offset);
            tp_price = FPN_Max(tp_price, tp_floor);

            Portfolio_AddPositionWithExits(&ctrl->portfolio, sized_qty, fill_price, tp_price, sl_price);
            ctrl->total_buys++;

            // deduct cost + entry fee from balance
            ctrl->balance   = FPN_SubSat(ctrl->balance, total_cost);
            ctrl->total_fees = FPN_AddSat(ctrl->total_fees, entry_fee);

            // log buy
            double price_d = FPN_ToDouble(fill_price);
            double qty_d   = FPN_ToDouble(sized_qty);
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

        // exit fee: fee_rate * (exit_price * quantity)
        FPN<F> gross_proceeds = FPN_Mul(rec->exit_price, pos->quantity);
        FPN<F> exit_fee = FPN_Mul(gross_proceeds, ctrl->config.fee_rate);
        FPN<F> net_proceeds = FPN_SubSat(gross_proceeds, exit_fee);

        // realized P&L: net proceeds - total cost (entry cost + entry fee)
        // entry fee = entry_price * qty * fee_rate (reconstructed since we dont store it per position)
        FPN<F> entry_cost = FPN_Mul(pos->entry_price, pos->quantity);
        FPN<F> entry_fee_recon = FPN_Mul(entry_cost, ctrl->config.fee_rate);
        FPN<F> total_entry_cost = FPN_AddSat(entry_cost, entry_fee_recon);
        FPN<F> pos_pnl = FPN_Sub(net_proceeds, total_entry_cost);
        ctrl->realized_pnl = FPN_AddSat(ctrl->realized_pnl, pos_pnl);

        // return net proceeds to paper trading balance (after exit fee)
        ctrl->balance    = FPN_AddSat(ctrl->balance, net_proceeds);
        ctrl->total_fees = FPN_AddSat(ctrl->total_fees, exit_fee);

        const char *reason = (rec->reason == 0) ? "TP" : "SL";
        ctrl->wins   += (rec->reason == 0);  // TP
        ctrl->losses += (rec->reason == 1);  // SL
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
        int is_empty = (ctrl->portfolio.active_bitmap == 0);
        int trailing = FPN_GreaterThan(current_price, ctrl->buy_conds.price);

        // squeeze fires when: portfolio empty AND current price above buy gate
        int should_squeeze = is_empty & trailing;
        uint64_t sq_mask = -(uint64_t)should_squeeze;

        // TWO-PHASE SQUEEZE:
        // Phase 1: squeeze offset toward zero (not offset_min — go all the way)
        // Phase 2: if offset is already at zero and still trailing, the buy gate
        //   is at the rolling average which lags price. Nothing more to squeeze —
        //   the rolling average will catch up naturally as new ticks enter the window.
        //   But we can also squeeze the volume multiplier to 1.0 (accept any volume)
        //   so the only remaining barrier is the price itself catching up.
        FPN<F> zero = FPN_Zero<F>();

        // squeeze offset toward zero: step = current_offset * 0.10 (10% per cycle, fast)
        FPN<F> squeeze_step = FPN_Mul(ctrl->live_offset_pct, FPN_FromDouble<F>(0.10));
        FPN<F> masked_squeeze;
        for (unsigned i = 0; i < N; i++) {
            masked_squeeze.w[i] = squeeze_step.w[i] & sq_mask;
        }
        masked_squeeze.sign = squeeze_step.sign & should_squeeze;

        ctrl->live_offset_pct = FPN_SubSat(ctrl->live_offset_pct, masked_squeeze);
        ctrl->live_offset_pct = FPN_Max(ctrl->live_offset_pct, zero); // floor at zero, not offset_min

        // squeeze volume multiplier toward 1.0 (accept any trade size)
        FPN<F> one = FPN_FromDouble<F>(1.0);
        FPN<F> vmult_gap = FPN_Sub(ctrl->live_vol_mult, one);
        FPN<F> vmult_step = FPN_Mul(vmult_gap, FPN_FromDouble<F>(0.10));
        FPN<F> masked_vmult;
        for (unsigned i = 0; i < N; i++) {
            masked_vmult.w[i] = vmult_step.w[i] & sq_mask;
        }
        masked_vmult.sign = vmult_step.sign & should_squeeze;

        ctrl->live_vol_mult = FPN_SubSat(ctrl->live_vol_mult, masked_vmult);
        ctrl->live_vol_mult = FPN_Max(ctrl->live_vol_mult, one); // floor at 1.0x
    }

    // update buy gate from rolling stats using LIVE adaptive filter values
    ctrl->buy_conds.price  = RollingStats_BuyPrice(&ctrl->rolling, ctrl->live_offset_pct);
    ctrl->buy_conds.volume = FPN_Mul(ctrl->rolling.volume_avg, ctrl->live_vol_mult);

    // update initial conditions to track the market - prevents the AdjustBuyGate clamp
    // from anchoring to stale warmup prices. the clamp window (max_shift) moves with
    // the rolling average so the gate stays in the current price neighborhood
    ctrl->buy_conds_initial.price  = ctrl->buy_conds.price;
    ctrl->buy_conds_initial.volume = ctrl->buy_conds.volume;

    // compute unrealized P&L and estimate exit fees on open positions
    // gross P&L is what Portfolio_ComputePnL returns (price delta * qty)
    // net P&L subtracts estimated exit fees so the regression optimizes on real profitability
    FPN<F> gross_pnl = Portfolio_ComputePnL(&ctrl->portfolio, current_price);
    FPN<F> portfolio_value = Portfolio_ComputeValue(&ctrl->portfolio, current_price);
    FPN<F> estimated_exit_fees = FPN_Mul(portfolio_value, ctrl->config.fee_rate);
    ctrl->portfolio_delta = FPN_Sub(gross_pnl, estimated_exit_fees);
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
