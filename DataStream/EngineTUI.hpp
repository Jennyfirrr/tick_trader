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
// [STRUCT]
//======================================================================================================
struct EngineTUI {
    int enabled;
    uint64_t last_render_tick;
    uint32_t render_interval;      // render every N ticks (not every tick - would thrash the terminal)
    struct termios original_term;  // saved terminal state for cleanup
    int raw_mode_active;
    uint64_t start_time;           // for uptime display
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

    // cursor home - overwrites in place, no flicker
    printf("\033[H\033[2J");

    printf("================================================================\n");
    printf("  TICK TRADER ENGINE                          tick: %-8lu\n", (unsigned long)tick);
    printf("================================================================\n");
    printf("  STATE: %-8s  |  UPTIME: %02u:%02u:%02u\n", state_str, hours, mins, secs);
    printf("----------------------------------------------------------------\n");
    printf("  PRICE: %-12.2f  |  VOLUME: %-12.8f\n", price, volume);
    printf("----------------------------------------------------------------\n");
    printf("  MARKET STRUCTURE (rolling %d-tick window):\n", ctrl->rolling.count);
    printf("    avg price:  %-12.2f  |  stddev: %-10.2f\n", roll_price_avg, roll_stddev);
    printf("    range:      %-12.2f  -  %-12.2f\n", roll_p_min, roll_p_max);
    printf("    avg volume: %-12.8f  |  vol slope: %+.8f\n", roll_vol_avg, roll_vol_slope);
    printf("----------------------------------------------------------------\n");
    // adaptive filter state
    double live_offset = FPN_ToDouble(ctrl->live_offset_pct) * 100.0;  // display as %
    double live_vmult  = FPN_ToDouble(ctrl->live_vol_mult);

    printf("  BUY GATE (adaptive):\n");
    printf("    price <= %-12.2f  (offset: %.3f%%)\n", buy_p, live_offset);
    printf("    vol   >= %-12.8f  (mult: %.2fx)\n", buy_v, live_vmult);
    printf("    distance:   $%-10.2f  (%.3f%% away)\n", gate_dist, gate_dist_pct);
    printf("    spacing:    $%-10.2f  (min between entries)\n", spacing);
    printf("----------------------------------------------------------------\n");
    printf("  POSITIONS (%d/16):\n", active_count);

    // walk active bitmap and display each position
    uint16_t active = ctrl->portfolio.active_bitmap;
    int displayed = 0;
    while (active) {
        int idx = __builtin_ctz(active);
        const Position<F> *pos = &ctrl->portfolio.positions[idx];

        double entry   = FPN_ToDouble(pos->entry_price);
        double qty     = FPN_ToDouble(pos->quantity);
        double tp      = FPN_ToDouble(pos->take_profit_price);
        double sl      = FPN_ToDouble(pos->stop_loss_price);
        double pos_pnl = 0.0;
        if (entry != 0.0) pos_pnl = ((price - entry) / entry) * 100.0;

        // show distance to TP and SL
        double to_tp = tp - price;
        double to_sl = price - sl;

        printf("  #%-2d  %.2f  qty:%.6f  TP:%.2f(%+.0f)  SL:%.2f(-%0.f)  %+.2f%%\n",
               idx, entry, qty, tp, to_tp, sl, to_sl, pos_pnl);
        displayed++;
        active &= active - 1;
    }
    // clear remaining position lines from previous renders
    for (int i = displayed; i < 16; i++) {
        printf("  %-70s\n", "");
    }

    double realized = FPN_ToDouble(ctrl->realized_pnl);
    double balance  = FPN_ToDouble(ctrl->balance);
    double starting = FPN_ToDouble(ctrl->config.starting_balance);
    double fees     = FPN_ToDouble(ctrl->total_fees);
    double total_pnl = realized + pnl;
    double return_pct = (starting != 0.0) ? (total_pnl / starting) * 100.0 : 0.0;
    double risk_amt = FPN_ToDouble(ctrl->config.risk_pct) * 100.0;

    printf("----------------------------------------------------------------\n");
    printf("  BALANCE:        $%-12.4f  (started: $%.0f)\n", balance, starting);
    printf("  REALIZED P&L:   $%-+12.4f  (after fees)\n", realized);
    printf("  UNREALIZED P&L: $%-+12.4f  (open positions)\n", pnl);
    printf("  TOTAL P&L:      $%-+12.4f  (%+.2f%%)\n", total_pnl, return_pct);
    double deployed = starting - balance;
    double exposure_pct = (starting != 0.0) ? (deployed / starting) * 100.0 : 0.0;
    double max_exp = FPN_ToDouble(ctrl->config.max_exposure_pct) * 100.0;
    double max_dd  = FPN_ToDouble(ctrl->config.max_drawdown_pct) * 100.0;
    int breaker_tripped = (total_pnl < -(starting * FPN_ToDouble(ctrl->config.max_drawdown_pct)));

    printf("  FEES PAID:      $%-12.4f  (%.1f%% rate)\n", fees,
           FPN_ToDouble(ctrl->config.fee_rate) * 100.0);
    printf("  RISK/POSITION:  %.1f%%  |  EXPOSURE: %.1f%%/%.0f%%\n",
           risk_amt, exposure_pct, max_exp);
    printf("  CIRCUIT BREAKER: %s  (max drawdown: %.0f%%)\n",
           breaker_tripped ? "TRIPPED" : "OK", max_dd);
    printf("  MODE: PAPER TRADING (simulated fills)\n");
    printf("----------------------------------------------------------------\n");
    printf("  TICKS: %-8lu  |  TRADES: %-8lu\n",
           (unsigned long)tick, (unsigned long)ctrl->total_ticks);
    printf("  LOG: btcusdt_order_history.csv\n");
    printf("==================================================\n");
    printf("  [q]uit  [p]ause  [r]eload config                \n");

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
            // unpause - restore initial conditions, regression will adjust from here
            ctrl->buy_conds = ctrl->buy_conds_initial;
        } else {
            ctrl->buy_conds.price  = FPN_Zero<F>();
            ctrl->buy_conds.volume = FPN_Zero<F>();
        }
        return 'p';
    }

    if (c == 'r' || c == 'R') {
        // hot-swap config reload
        ControllerConfig<F> new_cfg = ControllerConfig_Load<F>(config_path);
        // only swap hot-swappable fields
        ctrl->config.poll_interval   = new_cfg.poll_interval;
        ctrl->config.r2_threshold    = new_cfg.r2_threshold;
        ctrl->config.slope_scale_buy = new_cfg.slope_scale_buy;
        ctrl->config.max_shift       = new_cfg.max_shift;
        ctrl->config.take_profit_pct = new_cfg.take_profit_pct;
        ctrl->config.stop_loss_pct   = new_cfg.stop_loss_pct;
        fprintf(stderr, "[TUI] config reloaded from %s\n", config_path);
        return 'r';
    }

    return 0;
}

//======================================================================================================
//======================================================================================================
#endif // ENGINE_TUI_HPP
