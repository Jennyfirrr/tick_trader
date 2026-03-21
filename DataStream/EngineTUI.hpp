//======================================================================================================
// [ENGINE TUI]
//======================================================================================================
// simple terminal UI using ANSI escape codes for monitoring the engine state
// no framework, no dependencies beyond standard POSIX terminal control
//
// the engine runs identically with or without the TUI - tui_enabled=0 skips all display calls
// the TUI only READS engine state, never writes it (except explicit user commands: pause/reload/quit)
//
// terminal is set to raw mode for single-char input (no enter needed for commands)
// signal handler restores terminal on crash so the user doesnt have to type `reset`
//======================================================================================================
#ifndef ENGINE_TUI_HPP
#define ENGINE_TUI_HPP

#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <termios.h>
#include <signal.h>
#include <time.h>

#include "../CoreFrameworks/PortfolioController.hpp"
#include "../CoreFrameworks/OrderGates.hpp"

using namespace std;

//======================================================================================================
// [FOXML THEME - truecolor ANSI]
//======================================================================================================
// colors pulled from the FoxML neovim colorscheme palette
// uses 24-bit truecolor: \033[38;2;R;G;Bm (foreground)
//======================================================================================================
#define C_RESET   "\033[0m"
#define C_BOLD    "\033[1m"

// palette - earthy tones from the FoxML neovim colorscheme
#define C_PEACH   "\033[38;2;212;152;90m"    // #d4985a - titles, headers, accent
#define C_WHEAT   "\033[38;2;212;180;131m"   // #d4b483 - warm accent, price
#define C_FG      "\033[38;2;213;196;176m"   // #d5c4b0 - normal text
#define C_DIM     "\033[38;2;90;98;112m"     // #5a6270 - comments, hints
#define C_GREEN   "\033[38;2;122;171;136m"   // #7aab88 - positive P&L
#define C_RED     "\033[38;2;192;104;104m"   // #c06868 - negative P&L
#define C_YELLOW  "\033[38;2;196;180;138m"   // #c4b48a - warnings
#define C_SAND    "\033[38;2;168;154;122m"   // #a89a7a - labels, separators
#define C_WARM    "\033[38;2;176;164;152m"   // #b0a498 - secondary labels
#define C_PINK    "\033[38;2;184;150;122m"   // #b8967a - secondary accent
#define C_SURF    "\033[38;2;58;65;75m"      // #3a414b - dim separators
#define C_LAV     "\033[38;2;138;154;122m"   // #8a9a7a - lavender/muted green

// conditional P&L color: green if >= 0, red if < 0
#define C_PNL(v) ((v) >= 0.0 ? C_GREEN : C_RED)

//======================================================================================================
// [STRUCT]
//======================================================================================================
struct EngineTUI {
    int enabled;
    uint64_t last_render_tick;
    uint32_t render_interval;      // render every N ticks (not every tick - would thrash the terminal)
    struct termios original_term;  // saved terminal state for cleanup
    int raw_mode_active;
    uint64_t start_time;           // for uptime display
#ifdef LATENCY_PROFILING
    // hot path = BuyGate + PositionExitGate + PortfolioController_Tick (fast portion)
    // slow path = full tick including slow-path operations (every poll_interval)
    uint64_t hot_min, hot_max, hot_sum, hot_count;
    uint64_t slow_min, slow_max, slow_sum, slow_count;
    double tsc_per_ns;  // TSC cycles per nanosecond, calibrated at startup
#endif
};

//======================================================================================================
// [TERMINAL RAW MODE]
//======================================================================================================
// global pointer for signal handler cleanup - only one TUI instance per process
//======================================================================================================
static EngineTUI *g_tui_instance = NULL;

static void tui_signal_handler(int sig) {
    if (g_tui_instance && g_tui_instance->raw_mode_active) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_tui_instance->original_term);
        // show cursor
        write(STDOUT_FILENO, "\033[?25h", 6);
    }
    // re-raise to get default behavior (core dump, exit, etc)
    signal(sig, SIG_DFL);
    raise(sig);
}

//======================================================================================================
// [INIT]
//======================================================================================================
static inline void TUI_Init(EngineTUI *tui, int enabled, uint32_t render_interval) {
    tui->enabled          = enabled;
    tui->last_render_tick = 0;
    tui->render_interval  = render_interval;
    tui->raw_mode_active  = 0;
    tui->start_time       = (uint64_t)time(NULL);

    if (!enabled) return;

    // save terminal state and switch to raw mode for single-char input
    tcgetattr(STDIN_FILENO, &tui->original_term);

    struct termios raw = tui->original_term;
    raw.c_lflag &= ~(ICANON | ECHO);  // no line buffering, no echo
    raw.c_cc[VMIN]  = 0;               // non-blocking read
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    tui->raw_mode_active = 1;

    // install signal handlers for clean terminal restore on crash
    g_tui_instance = tui;
    signal(SIGINT,  tui_signal_handler);
    signal(SIGTERM, tui_signal_handler);
    signal(SIGSEGV, tui_signal_handler);

    // hide cursor during rendering
    printf("\033[?25l");
    fflush(stdout);
}

//======================================================================================================
// [CLEANUP]
//======================================================================================================
static inline void TUI_Cleanup(EngineTUI *tui) {
    if (!tui->raw_mode_active) return;

    tcsetattr(STDIN_FILENO, TCSANOW, &tui->original_term);
    tui->raw_mode_active = 0;

    // show cursor, clear screen
    printf("\033[?25h\033[2J\033[H");
    fflush(stdout);

    g_tui_instance = NULL;
}

//======================================================================================================
// [RENDER]
//======================================================================================================
// clears the screen and draws the full dashboard
// only renders every render_interval ticks to avoid thrashing the terminal
//
// uses cursor home (\033[H) instead of clear (\033[2J) to reduce flicker -
// overwrites in place rather than clearing and redrawing
//======================================================================================================
template <unsigned F>
static inline void TUI_Render(EngineTUI *tui, const PortfolioController<F> *ctrl,
                               const DataStream<F> *stream, uint64_t tick) {
    if (!tui->enabled) return;
    if (tick - tui->last_render_tick < tui->render_interval) return;
    tui->last_render_tick = tick;

    // compute uptime
    uint64_t now     = (uint64_t)time(NULL);
    uint64_t elapsed = now - tui->start_time;
    uint32_t hours   = (uint32_t)(elapsed / 3600);
    uint32_t mins    = (uint32_t)((elapsed % 3600) / 60);
    uint32_t secs    = (uint32_t)(elapsed % 60);

    // state string
    const char *state_str = (ctrl->state == CONTROLLER_WARMUP) ? "WARMUP" : "ACTIVE";

    // convert FPN values to doubles for display
    double price  = FPN_ToDouble(stream->price);
    double volume = FPN_ToDouble(stream->volume);
    double buy_p  = FPN_ToDouble(ctrl->buy_conds.price);
    double buy_v  = FPN_ToDouble(ctrl->buy_conds.volume);
    double pnl    = FPN_ToDouble(ctrl->portfolio_delta);

    // rolling stats
    double roll_price_avg = FPN_ToDouble(ctrl->rolling.price_avg);
    double roll_vol_avg   = FPN_ToDouble(ctrl->rolling.volume_avg);
    double roll_stddev    = FPN_ToDouble(ctrl->rolling.price_stddev);
    double roll_vol_slope = FPN_ToDouble(ctrl->rolling.volume_slope);
    double roll_p_min     = FPN_ToDouble(ctrl->rolling.price_min);
    double roll_p_max     = FPN_ToDouble(ctrl->rolling.price_max);

    // distance from buy gate (how far price needs to drop to trigger)
    double gate_dist = price - buy_p;
    double gate_dist_pct = (roll_price_avg != 0.0) ? (gate_dist / roll_price_avg) * 100.0 : 0.0;

    // entry spacing
    double spacing = FPN_ToDouble(RollingStats_EntrySpacing(&ctrl->rolling, ctrl->config.spacing_multiplier));

    int active_count = Portfolio_CountActive(&ctrl->portfolio);

    //==================================================================================================
    // [PHASE 1] pre-render positions into buffer (compute totals needed by left column)
    //==================================================================================================
    #define POS_MAX_LINES 70
    #define POS_LINE_W 200
    char pos_buf[POS_MAX_LINES][POS_LINE_W];
    int pln = 0;

    snprintf(pos_buf[pln++], POS_LINE_W,
             C_BOLD C_PEACH "POSITIONS " C_DIM "(%d/16):" C_RESET, active_count);

    uint16_t active = ctrl->portfolio.active_bitmap;
    int displayed = 0;
    double total_value = 0.0;
    double total_qty   = 0.0;
    double fee_r = FPN_ToDouble(ctrl->config.fee_rate);
    while (active) {
        int idx = __builtin_ctz(active);
        const Position<F> *pos = &ctrl->portfolio.positions[idx];

        double entry   = FPN_ToDouble(pos->entry_price);
        double qty     = FPN_ToDouble(pos->quantity);
        double tp      = FPN_ToDouble(pos->take_profit_price);
        double sl      = FPN_ToDouble(pos->stop_loss_price);
        double pos_pnl = 0.0;
        if (entry != 0.0) pos_pnl = ((price - entry) / entry) * 100.0;

        double to_tp = tp - price;
        double to_sl = price - sl;
        double value = price * qty;
        double net_pnl = pos_pnl - (fee_r * 200.0);

        total_value += value;
        total_qty   += qty;

        double price_diff = price - entry;
        if (displayed > 0)
            snprintf(pos_buf[pln++], POS_LINE_W, C_SURF "·" C_RESET);
        snprintf(pos_buf[pln++], POS_LINE_W,
                 C_WHEAT "#%-2d " C_FG "$%.2f" C_DIM "->" C_WHEAT "$%.2f"
                 " %s%+.2f" C_RESET,
                 idx, entry, price, C_PNL(price_diff), price_diff);
        snprintf(pos_buf[pln++], POS_LINE_W,
                 C_SAND "    qty:" C_FG "%.6f" C_SAND " val:" C_FG "$%.2f" C_RESET,
                 qty, value);
        snprintf(pos_buf[pln++], POS_LINE_W,
                 C_SAND "    TP:" C_FG "%+.0f" C_SAND " SL:" C_FG "%.0f"
                 C_SAND " g:" "%s%+.2f%%" C_SAND " n:" "%s%+.2f%%" C_RESET,
                 to_tp, to_sl, C_PNL(pos_pnl), pos_pnl, C_PNL(net_pnl), net_pnl);
        displayed++;
        active &= active - 1;
        if (pln >= POS_MAX_LINES - 4) break;  // safety
    }

    //==================================================================================================
    // [PHASE 2] print left column (no positions - they go right)
    //==================================================================================================
    // cursor home + clear
    printf("\033[H\033[2J");

    int row = 1;  // track current row for right-column overlay

    printf(C_SAND "  ================================================================" C_RESET "\n"); row++;
    printf(C_BOLD C_PEACH "     /\\_/\\   FOXML TRADER" C_RESET
           C_DIM "                       tick: " C_RESET C_FG "%-8lu" C_RESET "\n", (unsigned long)tick); row++;
    printf(C_BOLD C_PEACH "    ( o.o )  " C_WHEAT "engine v0.6" C_RESET "\n"); row++;
    printf(C_BOLD C_PEACH "     > ^ <" C_RESET "\n"); row++;
    printf(C_SAND "  ================================================================" C_RESET "\n"); row++;
    int is_paused = FPN_IsZero(ctrl->buy_conds.price) && (ctrl->state == CONTROLLER_ACTIVE);
    printf(C_SAND "  STATE: " C_FG "%-8s" C_RESET
           C_DIM "  |  " C_SAND "UPTIME: " C_FG "%02u:%02u:%02u" C_RESET
           "%s" "\n",
           state_str, hours, mins, secs,
           is_paused ? C_DIM "  |  " C_BOLD C_YELLOW "PAUSED" C_RESET : ""); row++;
    printf(C_SURF "  ----------------------------------------------------------------" C_RESET "\n"); row++;
    printf(C_SAND "  PRICE: " C_BOLD C_WHEAT "%-12.2f" C_RESET
           C_DIM "  |  " C_SAND "VOLUME: " C_FG "%-12.8f" C_RESET "\n", price, volume); row++;
    printf(C_SURF "  ----------------------------------------------------------------" C_RESET "\n"); row++;

    int pos_start_row = row;  // positions start alongside market structure

    printf(C_BOLD C_PEACH "  MARKET STRUCTURE " C_DIM "(rolling %d-tick window):" C_RESET "\n", ctrl->rolling.count); row++;
    printf(C_SAND "    avg price:  " C_FG "%-12.2f" C_DIM "  |  " C_SAND "stddev: " C_FG "%-10.2f" C_RESET "\n", roll_price_avg, roll_stddev); row++;
    printf(C_SAND "    range:      " C_FG "%-12.2f" C_DIM "  -  " C_FG "%-12.2f" C_RESET "\n", roll_p_min, roll_p_max); row++;
    double roll_price_slope = FPN_ToDouble(ctrl->rolling.price_slope);
    // normalize slopes by price for display (percentage per tick, price-independent)
    double slope_pct = (roll_price_avg != 0.0) ? (roll_price_slope / roll_price_avg) * 100.0 : 0.0;
    printf(C_SAND "    avg volume: " C_FG "%-12.8f" C_DIM "  |  " C_SAND "vol slope: " C_FG "%+.8f" C_RESET "\n", roll_vol_avg, roll_vol_slope); row++;
    const char *trend_color = (slope_pct > 0.001) ? C_GREEN : (slope_pct < -0.001) ? C_RED : C_DIM;
    const char *trend_str   = (slope_pct > 0.001) ? "UP" : (slope_pct < -0.001) ? "DOWN" : "FLAT";
    printf(C_SAND "    price slope: " C_FG "%+.6f%%/tick" C_DIM "  |  " C_SAND "trend: " "%s%s" C_RESET "\n",
           slope_pct, trend_color, trend_str); row++;
    // long-window trend (512-tick), also normalized by price
    double long_slope = FPN_ToDouble(ctrl->rolling_long.price_slope);
    double long_avg   = FPN_ToDouble(ctrl->rolling_long.price_avg);
    double long_slope_pct = (long_avg != 0.0) ? (long_slope / long_avg) * 100.0 : 0.0;
    int long_count = ctrl->rolling_long.count;
    const char *long_trend_color = (long_slope_pct > 0.001) ? C_GREEN : (long_slope_pct < -0.001) ? C_RED : C_DIM;
    const char *long_trend_str   = (long_slope_pct > 0.001) ? "UP" : (long_slope_pct < -0.001) ? "DOWN" : "FLAT";
    printf(C_SAND "    long window " C_DIM "(%d-tick):" C_FG " %+.6f%%/tick"
           C_DIM "  |  " "%s%s" C_RESET "\n",
           long_count, long_slope_pct, long_trend_color, long_trend_str); row++;
    printf(C_SURF "  ----------------------------------------------------------------" C_RESET "\n"); row++;
    // adaptive filter state
    double live_offset = FPN_ToDouble(ctrl->mean_rev.live_offset_pct) * 100.0;  // display as %
    double live_vmult  = FPN_ToDouble(ctrl->mean_rev.live_vol_mult);
    int stddev_mode = !FPN_IsZero(ctrl->config.offset_stddev_mult);

    printf(C_BOLD C_PEACH "  BUY GATE " C_DIM "(adaptive):" C_RESET "\n"); row++;
    if (stddev_mode) {
        double live_sm = FPN_ToDouble(ctrl->mean_rev.live_stddev_mult);
        printf(C_SAND "    price <= " C_FG "%-12.2f" C_DIM "  (stddev: %.2fx)" C_RESET "\n", buy_p, live_sm); row++;
    } else {
        printf(C_SAND "    price <= " C_FG "%-12.2f" C_DIM "  (offset: %.3f%%)" C_RESET "\n", buy_p, live_offset); row++;
    }
    printf(C_SAND "    vol   >= " C_FG "%-12.8f" C_DIM "  (mult: %.2fx)" C_RESET "\n", buy_v, live_vmult); row++;
    double spacing_pct = (roll_price_avg != 0.0) ? (spacing / roll_price_avg) * 100.0 : 0.0;
    if (buy_p > 0.01) {
        printf(C_SAND "    distance:   " C_FG "$%-10.2f" C_DIM "  (%.3f%% away)" C_RESET "\n", gate_dist, gate_dist_pct); row++;
    } else {
        printf(C_SAND "    distance:   " C_DIM "—  (gate disabled)" C_RESET "\n"); row++;
    }
    printf(C_SAND "    spacing:    " C_FG "$%-10.2f" C_DIM "  (%.3f%% of avg)" C_RESET "\n", spacing, spacing_pct); row++;
    // multi-timeframe gate status
    int long_gate_enabled = !FPN_IsZero(ctrl->config.min_long_slope);
    if (long_gate_enabled) {
        double min_ls = FPN_ToDouble(ctrl->config.min_long_slope);
        // gate uses relative slope (slope/price_avg), match the comparison
        double rel_slope = (long_avg != 0.0) ? long_slope / long_avg : 0.0;
        int long_gate_ok = (rel_slope >= min_ls);
        if (long_gate_ok) {
            printf(C_SAND "    long trend: " C_GREEN "OK" C_RESET "\n"); row++;
        } else {
            printf(C_SAND "    long trend: " C_BOLD C_RED "BLOCKED" C_RESET
                   C_DIM " (%+.6f < %+.6f)" C_RESET "\n", rel_slope, min_ls); row++;
        }
    }
    printf(C_SURF "  ----------------------------------------------------------------" C_RESET "\n"); row++;

    double realized = FPN_ToDouble(ctrl->realized_pnl);
    double balance  = FPN_ToDouble(ctrl->balance);
    double starting = FPN_ToDouble(ctrl->config.starting_balance);
    double fees     = FPN_ToDouble(ctrl->total_fees);
    double total_pnl = realized + pnl;
    double return_pct = (starting != 0.0) ? (total_pnl / starting) * 100.0 : 0.0;
    double risk_amt = FPN_ToDouble(ctrl->config.risk_pct) * 100.0;

    // ==== PORTFOLIO section ====
    double equity = balance + total_value;
    double deployed = starting - balance;
    double exposure_pct = (starting != 0.0) ? (deployed / starting) * 100.0 : 0.0;
    double max_exp = FPN_ToDouble(ctrl->config.max_exposure_pct) * 100.0;

    printf(C_BOLD C_PEACH "  PORTFOLIO:" C_RESET "\n"); row++;
    printf(C_SAND "    equity:     " C_BOLD C_FG "$%-12.4f" C_RESET C_DIM "  (cash + positions)" C_RESET "\n", equity); row++;
    printf(C_SAND "    balance:    " C_FG "$%-12.4f" C_RESET C_DIM "  (started: $%.0f)" C_RESET "\n", balance, starting); row++;
    printf(C_SAND "    held:       " C_FG "$%-12.4f" C_RESET C_DIM "  (qty: %.6f)" C_RESET "\n", total_value, total_qty); row++;
    printf(C_SAND "    exposure:   " C_FG "%.1f%%/%.0f%%" C_RESET
           C_DIM "  |  " C_SAND "fees: " C_FG "$%.4f" C_DIM " (%.1f%%)" C_RESET "\n",
           exposure_pct, max_exp, fees, FPN_ToDouble(ctrl->config.fee_rate) * 100.0); row++;
    printf(C_SURF "  ----------------------------------------------------------------" C_RESET "\n"); row++;

    // ==== P&L section ====
    printf(C_BOLD C_PEACH "  P&L:" C_RESET "\n"); row++;
    printf(C_SAND "    realized:   " "%s$%-+12.4f" C_RESET C_DIM "  (after fees)" C_RESET "\n", C_PNL(realized), realized); row++;
    printf(C_SAND "    unrealized: " "%s$%-+12.4f" C_RESET C_DIM "  (open positions)" C_RESET "\n", C_PNL(pnl), pnl); row++;
    printf(C_SAND "    total:      " C_BOLD "%s$%-+12.4f" C_RESET C_DIM "  (" "%s%+.2f%%" C_DIM ")" C_RESET "\n",
           C_PNL(total_pnl), total_pnl, C_PNL(return_pct), return_pct); row++;
    printf(C_SURF "  ----------------------------------------------------------------" C_RESET "\n"); row++;

    // ==== RISK section ====
    double max_dd  = FPN_ToDouble(ctrl->config.max_drawdown_pct) * 100.0;
    int breaker_tripped = (total_pnl < -(starting * FPN_ToDouble(ctrl->config.max_drawdown_pct)));

    printf(C_BOLD C_PEACH "  RISK:" C_RESET "\n"); row++;
    printf(C_SAND "    risk/pos:   " C_FG "%.1f%%" C_RESET
           C_DIM "  |  " C_SAND "breaker: " "%s%s" C_RESET C_DIM " (max dd: %.0f%%)" C_RESET "\n",
           risk_amt, breaker_tripped ? C_BOLD C_RED : C_GREEN,
           breaker_tripped ? "TRIPPED" : "OK", max_dd); row++;
    const char *offset_mode_str = stddev_mode ? "stddev" : "pct";
    printf(C_SAND "    strategy:   " C_FG "MEAN REVERSION" C_RESET
           C_DIM " (" C_FG "%s" C_DIM ")  |  " C_YELLOW "PAPER" C_RESET "\n", offset_mode_str); row++;
    printf(C_SURF "  ----------------------------------------------------------------" C_RESET "\n"); row++;

    // ==== STATS section ====
    uint32_t total_exits = ctrl->wins + ctrl->losses;
    double win_rate = (total_exits > 0) ? ((double)ctrl->wins / total_exits) * 100.0 : 0.0;
    double g_wins  = FPN_ToDouble(ctrl->gross_wins);
    double g_losses = FPN_ToDouble(ctrl->gross_losses);
    double profit_factor = (g_losses > 0.001) ? g_wins / g_losses : 0.0;
    double avg_win  = (ctrl->wins > 0) ? g_wins / ctrl->wins : 0.0;
    double avg_loss = (ctrl->losses > 0) ? g_losses / ctrl->losses : 0.0;
    double avg_hold = (total_exits > 0) ? (double)ctrl->total_hold_ticks / total_exits : 0.0;

    printf(C_BOLD C_PEACH "  STATS:" C_RESET "\n"); row++;
    printf(C_SAND "    buys: " C_FG "%-4u" C_RESET
           C_DIM "  |  " C_SAND "exits: " C_FG "%-4u" C_RESET
           C_DIM "  |  " C_SAND "hold: " C_FG "%.0f ticks" C_RESET "\n",
           ctrl->total_buys, total_exits, avg_hold); row++;
    printf(C_SAND "    wins: " C_GREEN "%-4u" C_RESET
           C_SAND "  losses: " C_RED "%-4u" C_RESET
           C_SAND "  rate: " "%s%.1f%%" C_RESET
           C_DIM "  |  " C_SAND "pf: " "%s%.2f" C_RESET "\n",
           ctrl->wins, ctrl->losses,
           (win_rate >= 50.0) ? C_GREEN : (total_exits > 0 ? C_RED : C_DIM), win_rate,
           (profit_factor >= 1.0) ? C_GREEN : (total_exits > 0 ? C_RED : C_DIM), profit_factor); row++;
    printf(C_SAND "    avg win: " C_GREEN "$%.4f" C_RESET
           C_SAND "  avg loss: " C_RED "$%.4f" C_RESET "\n",
           avg_win, avg_loss); row++;
    printf(C_DIM "    log: btcusdt_order_history.csv" C_RESET "\n"); row++;
    printf(C_SAND "  ================================================" C_RESET "\n"); row++;
#ifdef LATENCY_PROFILING
    printf(C_SURF "  ----------------------------------------------------------------" C_RESET "\n"); row++;
    printf(C_BOLD C_PEACH "  LATENCY " C_DIM "(profiling enabled):" C_RESET "\n"); row++;
    if (tui->hot_count > 0) {
        double hot_avg = (double)tui->hot_sum / tui->hot_count / tui->tsc_per_ns;
        double hot_min_ns = (double)tui->hot_min / tui->tsc_per_ns;
        double hot_max_ns = (double)tui->hot_max / tui->tsc_per_ns;
        printf(C_SAND "    hot path:  " C_FG "avg %.0fns" C_DIM "  min " C_FG "%.0fns"
               C_DIM "  max " C_FG "%.0fns" C_DIM "  (%lu ticks)" C_RESET "\n",
               hot_avg, hot_min_ns, hot_max_ns, (unsigned long)tui->hot_count); row++;
    }
    if (tui->slow_count > 0) {
        double slow_avg = (double)tui->slow_sum / tui->slow_count / tui->tsc_per_ns;
        double slow_min_ns = (double)tui->slow_min / tui->tsc_per_ns;
        double slow_max_ns = (double)tui->slow_max / tui->tsc_per_ns;
        const char *slow_unit = (slow_avg >= 1000.0) ? "us" : "ns";
        double slow_avg_d = (slow_avg >= 1000.0) ? slow_avg / 1000.0 : slow_avg;
        double slow_min_d = (slow_min_ns >= 1000.0) ? slow_min_ns / 1000.0 : slow_min_ns;
        double slow_max_d = (slow_max_ns >= 1000.0) ? slow_max_ns / 1000.0 : slow_max_ns;
        printf(C_SAND "    slow path: " C_FG "avg %.1f%s" C_DIM "  min " C_FG "%.1f%s"
               C_DIM "  max " C_FG "%.1f%s" C_DIM "  (%lu cycles)" C_RESET "\n",
               slow_avg_d, slow_unit, slow_min_d, slow_unit, slow_max_d, slow_unit,
               (unsigned long)tui->slow_count); row++;
    }
#endif
    printf(C_PINK "  [q]" C_DIM "uit  " C_PINK "[p]" C_DIM "ause  " C_PINK "[r]" C_DIM "eload config" C_RESET "                \n"); row++;

    //==================================================================================================
    // [PHASE 3] overlay positions on right column using cursor positioning
    //==================================================================================================
    // separator at column 60, right content starts at column 64
    int sep_col = 66;

    // draw || separator and position lines (only for position height, not full left column)
    for (int i = 0; i < pln; i++) {
        int r = pos_start_row + i;
        printf("\033[%d;%dH" C_SURF "||" C_RESET " %s", r, sep_col, pos_buf[i]);
    }

    fflush(stdout);
}

//======================================================================================================
// [HANDLE INPUT]
//======================================================================================================
// reads a single char from stdin and handles TUI commands
// returns: 0 = no action, 'q' = quit requested, 'p' = pause toggled, 'r' = reload requested
//
// pause: sets buy gate price to 0 (disables buys), exit gate keeps running
// unpause: restores buy gate to current conditions (regression will readjust on next slow path)
//======================================================================================================
template <unsigned F>
static inline char TUI_HandleInput(EngineTUI *tui, PortfolioController<F> *ctrl,
                                    const char *config_path, int *running) {
    if (!tui->enabled) return 0;

    char c = 0;
    int n = read(STDIN_FILENO, &c, 1);
    if (n <= 0) return 0;

    if (c == 'q' || c == 'Q') {
        *running = 0;
        return 'q';
    }

    if (c == 'p' || c == 'P') {
        // toggle pause - zero out buy gate to stop new entries
        // exit gate still runs so existing positions are protected
        int is_paused = FPN_IsZero(ctrl->buy_conds.price);
        if (is_paused) {
            // unpause - recompute buy conditions with all gates applied
            // (restoring buy_conds_initial would bypass the multi-timeframe gate)
            ctrl->buy_conds = MeanReversion_BuySignal(&ctrl->mean_rev, &ctrl->rolling,
                                                       &ctrl->rolling_long, &ctrl->config);
        } else {
            ctrl->buy_conds.price  = FPN_Zero<F>();
            ctrl->buy_conds.volume = FPN_Zero<F>();
        }
        return 'p';
    }

    if (c == 'r' || c == 'R') {
        // hot-swap config reload - all fields except symbol/testnet/warmup (need restart)
        ControllerConfig<F> new_cfg = ControllerConfig_Load<F>(config_path);
        ctrl->config.poll_interval     = new_cfg.poll_interval;
        ctrl->config.r2_threshold      = new_cfg.r2_threshold;
        ctrl->config.slope_scale_buy   = new_cfg.slope_scale_buy;
        ctrl->config.max_shift         = new_cfg.max_shift;
        ctrl->config.take_profit_pct   = new_cfg.take_profit_pct;
        ctrl->config.stop_loss_pct     = new_cfg.stop_loss_pct;
        ctrl->config.fee_rate          = new_cfg.fee_rate;
        ctrl->config.risk_pct          = new_cfg.risk_pct;
        ctrl->config.volume_multiplier = new_cfg.volume_multiplier;
        ctrl->config.entry_offset_pct  = new_cfg.entry_offset_pct;
        ctrl->config.spacing_multiplier = new_cfg.spacing_multiplier;
        ctrl->config.offset_min        = new_cfg.offset_min;
        ctrl->config.offset_max        = new_cfg.offset_max;
        ctrl->config.vol_mult_min      = new_cfg.vol_mult_min;
        ctrl->config.vol_mult_max      = new_cfg.vol_mult_max;
        ctrl->config.filter_scale      = new_cfg.filter_scale;
        ctrl->config.max_drawdown_pct  = new_cfg.max_drawdown_pct;
        ctrl->config.max_exposure_pct  = new_cfg.max_exposure_pct;
        ctrl->config.offset_stddev_mult = new_cfg.offset_stddev_mult;
        ctrl->config.offset_stddev_min  = new_cfg.offset_stddev_min;
        ctrl->config.offset_stddev_max  = new_cfg.offset_stddev_max;
        ctrl->config.min_long_slope     = new_cfg.min_long_slope;
        // reset live filters to new config values
        ctrl->mean_rev.live_offset_pct  = new_cfg.entry_offset_pct;
        ctrl->mean_rev.live_vol_mult   = new_cfg.volume_multiplier;
        ctrl->mean_rev.live_stddev_mult = new_cfg.offset_stddev_mult;
        fprintf(stderr, "[TUI] config reloaded from %s\n", config_path);
        return 'r';
    }

    return 0;
}

//======================================================================================================
//======================================================================================================
#endif // ENGINE_TUI_HPP
