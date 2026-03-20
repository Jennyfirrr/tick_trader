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
};
//======================================================================================================
template <unsigned F> inline ControllerConfig<F> ControllerConfig_Default() {
    ControllerConfig<F> cfg;
    cfg.poll_interval   = 100;
    cfg.warmup_ticks    = MAX_WINDOW * 8; // 64 ticks, enough for one full ROR cycle
    cfg.r2_threshold    = FPN_FromDouble<F>(0.30);
    cfg.slope_scale_buy = FPN_FromDouble<F>(0.50);
    cfg.max_shift       = FPN_FromDouble<F>(5.00);
    cfg.take_profit_pct = FPN_FromDouble<F>(0.03);
    cfg.stop_loss_pct   = FPN_FromDouble<F>(0.015);
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
        else if (strcmp(key, "stop_loss_pct") == 0)    cfg.stop_loss_pct   = FPN_FromDouble<F>(atof(val) / 100.0);
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
    FPN<F> portfolio_delta;

    RegressionFeederX<F> feeder;
    RORRegressor<F> ror;

    BuySideGateConditions<F> buy_conds;
    BuySideGateConditions<F> buy_conds_initial;

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

    ctrl->feeder = RegressionFeederX_Init<F>();
    ctrl->ror    = RORRegressor_Init<F>();

    // during warmup, buy gate is disabled - price 0 means nothing passes LessThanOrEqual
    ctrl->buy_conds.price   = FPN_Zero<F>();
    ctrl->buy_conds.volume  = FPN_Zero<F>();
    ctrl->buy_conds_initial = ctrl->buy_conds;

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

        // feed prices to regression feeder during warmup (build market structure data)
        RegressionFeederX_Push(&ctrl->feeder, current_price);

        if (ctrl->warmup_count >= ctrl->config.warmup_ticks) {
            // compute initial gate conditions from observed data
            FPN<F> count_fp    = FPN_FromDouble<F>((double)ctrl->warmup_count);
            FPN<F> mean_price  = FPN_DivNoAssert(ctrl->price_sum, count_fp);
            FPN<F> mean_volume = FPN_DivNoAssert(ctrl->volume_sum, count_fp);

            ctrl->buy_conds.price   = mean_price;
            ctrl->buy_conds.volume  = mean_volume;
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

        // new position path: only if not found AND portfolio has room
        int is_new     = !found & !Portfolio_IsFull(&ctrl->portfolio);
        if (is_new) {
            FPN<F> one      = FPN_FromDouble<F>(1.0);
            FPN<F> tp_mult  = FPN_AddSat(one, ctrl->config.take_profit_pct);
            FPN<F> sl_mult  = FPN_SubSat(one, ctrl->config.stop_loss_pct);
            FPN<F> tp_price = FPN_Mul(fill_price, tp_mult);
            FPN<F> sl_price = FPN_Mul(fill_price, sl_mult);

            Portfolio_AddPositionWithExits(&ctrl->portfolio, fill_qty, fill_price, tp_price, sl_price);

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

        const char *reason = (rec->reason == 0) ? "TP" : "SL";
        TradeLog_Sell(trade_log, rec->tick, exit_d, qty_d, entry_d, delta_pct, reason);
    }
    ExitBuffer_Clear(&ctrl->exit_buf);

    // compute unrealized P&L and push to regression
    ctrl->portfolio_delta = Portfolio_ComputePnL(&ctrl->portfolio, current_price);
    RegressionFeederX_Push(&ctrl->feeder, ctrl->portfolio_delta);

    // regression + ROR
    // inner slope is the direct P&L trend - used for gate adjustment in v1
    // ROR (slope-of-slopes) is computed and stored for future use (trend acceleration detection)
    if (ctrl->feeder.count >= MAX_WINDOW) {
        LinearRegression3XResult<F> inner = RegressionFeederX_Compute(&ctrl->feeder);
        RORRegressor_Push(&ctrl->ror, inner);

        // adjust based on inner regression slope (direct P&L trend)
        PortfolioController_AdjustBuyGate(ctrl, &inner);
    }
}
//======================================================================================================
//======================================================================================================
//======================================================================================================
#endif // PORTFOLIO_CONTROLLER_HPP
