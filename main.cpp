//======================================================================================================
// [TICK TRADER ENGINE]
//======================================================================================================
// main event loop - wires the Binance data stream to the execution pipeline
// single-threaded, poll-based: BinanceStream -> BuyGate -> PositionExitGate -> PortfolioController
//
// the engine runs identically whether data is arriving or not - poll() timeout ensures
// exit gates are always checked against last known price
//
// burst drain: when multiple frames buffer between poll cycles (volatile markets),
// we drain ALL of them, running exit gates on each intermediate price so TP/SL triggers
// arent missed in a burst. BuyGate only runs on the final/freshest price
//
// session lifecycle: 24-hour cycle with clean wind-down, position close, and reconnect
// reconnect procedure is airtight - verifies bitmap is zero before proceeding
//======================================================================================================
#include "DataStream/BinanceCrypto.hpp"
#include "DataStream/EngineTUI.hpp"
#include "CoreFrameworks/PortfolioController.hpp"
#include "MemHeaders/PoolAllocator.hpp"
#include "DataStream/TradeLog.hpp"

#include <stdio.h>
#include <stdlib.h>

#ifdef LATENCY_PROFILING
#include <x86intrin.h>
#include <unistd.h>
#endif

constexpr unsigned FP = 64;

//======================================================================================================
// [AIRTIGHT RECONNECT]
//======================================================================================================
// the key guarantee: we NEVER reconnect with orphaned positions
// if the bitmap isnt zero after force-close, thats a bug and we halt rather than lose track of money
//======================================================================================================
static inline void engine_force_close_all(PortfolioController<FP> *ctrl, TradeLog *log, FPN<FP> last_price) {
    uint16_t active = ctrl->portfolio.active_bitmap;
    while (active) {
        int idx = __builtin_ctz(active);
        Position<FP> *pos = &ctrl->portfolio.positions[idx];

        double entry_d   = FPN_ToDouble(pos->entry_price);
        double exit_d    = FPN_ToDouble(last_price);
        double qty_d     = FPN_ToDouble(pos->quantity);
        double delta_pct = 0.0;
        if (entry_d != 0.0) delta_pct = ((exit_d - entry_d) / entry_d) * 100.0;

        TradeLog_Sell(log, ctrl->total_ticks, exit_d, qty_d, entry_d, delta_pct, "SESSION_CLOSE");

        Portfolio_RemovePosition(&ctrl->portfolio, idx);
        active &= active - 1;
    }

    // hard gate: verify bitmap is zero
    if (ctrl->portfolio.active_bitmap != 0) {
        fprintf(stderr, "[ENGINE] FATAL: bitmap not zero after force-close: 0x%04X\n",
                ctrl->portfolio.active_bitmap);
        fprintf(stderr, "[ENGINE] halting - refusing to reconnect with orphaned positions\n");
        exit(1);
    }
}

//======================================================================================================
// [MAIN]
//======================================================================================================
int main(int argc, char *argv[]) {
    const char *cfg_path = (argc > 1) ? argv[1] : "engine.cfg";

    //==================================================================================================
    // load configs
    //==================================================================================================
    BinanceConfig bcfg       = BinanceConfig_Load(cfg_path);
    ControllerConfig<FP> ccfg = ControllerConfig_Load<FP>(cfg_path);

    //==================================================================================================
    // init stream
    //==================================================================================================
    BinanceStream bs;
    if (!BinanceStream_Init(&bs, &bcfg)) {
        fprintf(stderr, "[ENGINE] failed to connect - exiting\n");
        return 1;
    }

    //==================================================================================================
    // init controller
    //==================================================================================================
    PortfolioController<FP> ctrl;
    PortfolioController_Init(&ctrl, ccfg);

    // try to load snapshot from previous session
    const char *snapshot_path = "portfolio.snapshot";
    if (PortfolioController_LoadSnapshot(&ctrl, snapshot_path)) {
        int pos_count = Portfolio_CountActive(&ctrl.portfolio);
        fprintf(stderr, "[ENGINE] resumed %d positions from snapshot\n", pos_count);
        fprintf(stderr, "[ENGINE] realized P&L: $%.4f\n", FPN_ToDouble(ctrl.realized_pnl));
    }

    //==================================================================================================
    // init order pool
    //==================================================================================================
    OrderPool<FP> pool;
    OrderPool_init(&pool, 64);

    //==================================================================================================
    // init trade log
    //==================================================================================================
    TradeLog log;
    TradeLog_Init(&log, bcfg.symbol);

    //==================================================================================================
    // init TUI
    //==================================================================================================
#ifdef MULTICORE_TUI
    // multicore: TUI runs on separate thread, engine thread never renders
    TUISharedState shared = {};
    shared.config_path = cfg_path;
    shared.active_idx = 0;
    shared.quit_requested = 0;
    shared.pause_requested = 0;
    shared.reload_requested = 0;

    pthread_t tui_tid;
    pthread_create(&tui_tid, NULL, tui_thread_fn, &shared);

#ifdef __linux__
    // pin engine to core 0, TUI to core 1
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset); CPU_SET(0, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    CPU_ZERO(&cpuset); CPU_SET(1, &cpuset);
    pthread_setaffinity_np(tui_tid, sizeof(cpuset), &cpuset);
#endif

    // latency stats live on engine thread (not shared TUI struct)
    EngineTUI tui = {};  // dummy for latency stats in profiling mode
    tui.start_time = (uint64_t)time(NULL);
#else
    EngineTUI tui;
#ifdef LATENCY_BENCH
    TUI_Init(&tui, 0, 10);  // bench mode: TUI disabled for clean latency measurement
#else
    TUI_Init(&tui, 1, 10);  // TODO: read tui_enabled and render_interval from config
#endif
#endif // MULTICORE_TUI

#ifdef LATENCY_PROFILING
    // calibrate TSC: measure cycles over a known 10ms sleep to get cycles-per-nanosecond
    {
        unsigned int tsc_aux;
        uint64_t cal_start = __rdtscp(&tsc_aux);
        usleep(10000); // 10ms
        uint64_t cal_end = __rdtscp(&tsc_aux);
        tui.tsc_per_ns = (double)(cal_end - cal_start) / 10000000.0; // 10ms = 10M ns
        tui.hot_min = UINT64_MAX; tui.hot_max = 0; tui.hot_sum = 0; tui.hot_count = 0;
        tui.slow_min = UINT64_MAX; tui.slow_max = 0; tui.slow_sum = 0; tui.slow_count = 0;
        fprintf(stderr, "[LATENCY] TSC calibrated: %.2f cycles/ns (%.1f GHz)\n",
                tui.tsc_per_ns, tui.tsc_per_ns);
    }
#endif

    //==================================================================================================
    // main loop
    //==================================================================================================
    DataStream<FP> last_stream = {};
    int running = 1;
    int has_data = 0;  // dont run engine until we have at least one real tick

    while (running) {
        int ready = BinanceStream_Poll(&bs, bcfg.poll_timeout_ms);

        //==============================================================================================
        // DATA INGESTION - drain all buffered frames
        //==============================================================================================
        // during volatile markets binance can burst 100+ trades/sec
        // we drain ALL frames, running exit gates on each intermediate price
        // so TP/SL triggers arent missed in a burst
        // BuyGate only runs on the final/freshest price after the drain
        //==============================================================================================
        if (ready & POLL_SOCKET) {
            while (1) {
                DataStream<FP> tick;
                int ok = BinanceStream_ReadTick(&bs, &tick);
                if (!ok) {
                    // save snapshot before reconnect - positions survive
                    PortfolioController_SaveSnapshot(&ctrl, snapshot_path);
                    fprintf(stderr, "[ENGINE] connection lost - reconnecting (positions preserved)\n");
                    BinanceStream_Reconnect(&bs, &bcfg);
                    break;
                }
                last_stream = tick;
                has_data = 1;

                // exit gate on EVERY tick in the burst
                PositionExitGate(&ctrl.portfolio, last_stream.price, &ctrl.exit_buf, ctrl.total_ticks);

                // drain until SSL has no more buffered data
                if (!BinanceStream_HasPending(&bs)) break;
            }
        }

        //==============================================================================================
        // TUI INPUT
        //==============================================================================================
#ifdef MULTICORE_TUI
        // multicore: check atomic flags from TUI thread
        if (__atomic_load_n(&shared.quit_requested, __ATOMIC_ACQUIRE))
            running = 0;
        if (__atomic_exchange_n(&shared.pause_requested, 0, __ATOMIC_ACQ_REL)) {
            int is_paused = FPN_IsZero(ctrl.buy_conds.price);
            if (is_paused)
                ctrl.buy_conds = MeanReversion_BuySignal(&ctrl.mean_rev, &ctrl.rolling,
                                                          &ctrl.rolling_long, &ctrl.config);
            else {
                ctrl.buy_conds.price  = FPN_Zero<FP>();
                ctrl.buy_conds.volume = FPN_Zero<FP>();
            }
        }
        if (__atomic_exchange_n(&shared.reload_requested, 0, __ATOMIC_ACQ_REL)) {
            ControllerConfig<FP> new_cfg = ControllerConfig_Load<FP>(cfg_path);
            ctrl.config.poll_interval     = new_cfg.poll_interval;
            ctrl.config.r2_threshold      = new_cfg.r2_threshold;
            ctrl.config.slope_scale_buy   = new_cfg.slope_scale_buy;
            ctrl.config.max_shift         = new_cfg.max_shift;
            ctrl.config.take_profit_pct   = new_cfg.take_profit_pct;
            ctrl.config.stop_loss_pct     = new_cfg.stop_loss_pct;
            ctrl.config.fee_rate          = new_cfg.fee_rate;
            ctrl.config.risk_pct          = new_cfg.risk_pct;
            ctrl.config.volume_multiplier = new_cfg.volume_multiplier;
            ctrl.config.entry_offset_pct  = new_cfg.entry_offset_pct;
            ctrl.config.spacing_multiplier = new_cfg.spacing_multiplier;
            ctrl.config.offset_min        = new_cfg.offset_min;
            ctrl.config.offset_max        = new_cfg.offset_max;
            ctrl.config.vol_mult_min      = new_cfg.vol_mult_min;
            ctrl.config.vol_mult_max      = new_cfg.vol_mult_max;
            ctrl.config.filter_scale      = new_cfg.filter_scale;
            ctrl.config.max_drawdown_pct  = new_cfg.max_drawdown_pct;
            ctrl.config.max_exposure_pct  = new_cfg.max_exposure_pct;
            ctrl.config.offset_stddev_mult = new_cfg.offset_stddev_mult;
            ctrl.config.offset_stddev_min  = new_cfg.offset_stddev_min;
            ctrl.config.offset_stddev_max  = new_cfg.offset_stddev_max;
            ctrl.config.min_long_slope     = new_cfg.min_long_slope;
            ctrl.config.tp_hold_score      = new_cfg.tp_hold_score;
            ctrl.config.tp_trail_mult      = new_cfg.tp_trail_mult;
            ctrl.config.sl_trail_mult      = new_cfg.sl_trail_mult;
            ctrl.config.max_hold_ticks     = new_cfg.max_hold_ticks;
            ctrl.config.min_hold_gain_pct  = new_cfg.min_hold_gain_pct;
            ctrl.mean_rev.live_offset_pct  = new_cfg.entry_offset_pct;
            ctrl.mean_rev.live_vol_mult    = new_cfg.volume_multiplier;
            ctrl.mean_rev.live_stddev_mult = new_cfg.offset_stddev_mult;
            fprintf(stderr, "[ENGINE] config reloaded from %s\n", cfg_path);
        }
#else
        if (ready & POLL_STDIN) {
            TUI_HandleInput(&tui, &ctrl, cfg_path, &running);
        }
#endif

        //==============================================================================================
        // SESSION LIFECYCLE
        //==============================================================================================
        if (BinanceStream_InWindDown(&bs, bcfg.wind_down_minutes)) {
            // disable buy gate during wind-down
            ctrl.buy_conds.price  = FPN_Zero<FP>();
            ctrl.buy_conds.volume = FPN_Zero<FP>();
        }

        if (BinanceStream_ShouldReconnect(&bs)) {
            // 24-hour session boundary - planned reconnect
            // save snapshot before force-close in case something goes wrong
            PortfolioController_SaveSnapshot(&ctrl, snapshot_path);
            fprintf(stderr, "[ENGINE] 24h session boundary - closing positions and reconnecting\n");

            // airtight reconnect procedure:
            // 1. buy gate already disabled (wind-down)
            // 2. force-close all remaining positions
            // 3. verify bitmap is zero (halts if not)
            // 4. clear pool, reconnect, restart warmup
            engine_force_close_all(&ctrl, &log, last_stream.price);

            // clear pool
            pool.bitmap = 0;

            // reconnect
            BinanceStream_Reconnect(&bs, &bcfg);

            // reset controller to warmup
            PortfolioController_Init(&ctrl, ccfg);

            continue;
        }

        //==============================================================================================
        // ENGINE TICK - runs on timeout with last known price, but only after first real tick
        //==============================================================================================
        if (has_data) {
#ifdef LATENCY_PROFILING
            unsigned int tsc_aux;
            uint64_t tick_start = __rdtscp(&tsc_aux);
#endif
            BuyGate(&ctrl.buy_conds, &last_stream, &pool);
            PositionExitGate(&ctrl.portfolio, last_stream.price, &ctrl.exit_buf, ctrl.total_ticks);
            PortfolioController_Tick(&ctrl, &pool, last_stream.price, last_stream.volume, &log);
#ifdef LATENCY_PROFILING
            uint64_t tick_end = __rdtscp(&tsc_aux);
            uint64_t cycles = tick_end - tick_start;
            // tick_count == 0 means slow path just ran, > 0 means hot path only
            if (ctrl.tick_count == 0) {
                if (cycles < tui.slow_min) tui.slow_min = cycles;
                if (cycles > tui.slow_max) tui.slow_max = cycles;
                tui.slow_sum += cycles;
                tui.slow_count++;
            } else {
                if (cycles < tui.hot_min) tui.hot_min = cycles;
                if (cycles > tui.hot_max) tui.hot_max = cycles;
                tui.hot_sum += cycles;
                tui.hot_count++;
            }
#endif

            // save snapshot + copy TUI snapshot every slow-path cycle
            if (ctrl.tick_count == 0) {
                PortfolioController_SaveSnapshot(&ctrl, snapshot_path);
#ifdef MULTICORE_TUI
                // copy display state to back buffer, then atomic swap
                int back = !__atomic_load_n(&shared.active_idx, __ATOMIC_ACQUIRE);
                TUI_CopySnapshot(&shared.snapshots[back], &ctrl, &last_stream);
#ifdef LATENCY_PROFILING
                // copy latency stats to snapshot (engine-side stats → display)
                if (tui.hot_count > 0) {
                    shared.snapshots[back].hot_avg_ns = (double)tui.hot_sum / tui.hot_count / tui.tsc_per_ns;
                    shared.snapshots[back].hot_min_ns = (double)tui.hot_min / tui.tsc_per_ns;
                    shared.snapshots[back].hot_max_ns = (double)tui.hot_max / tui.tsc_per_ns;
                    shared.snapshots[back].hot_count  = tui.hot_count;
                }
                if (tui.slow_count > 0) {
                    shared.snapshots[back].slow_avg_ns = (double)tui.slow_sum / tui.slow_count / tui.tsc_per_ns;
                    shared.snapshots[back].slow_min_ns = (double)tui.slow_min / tui.tsc_per_ns;
                    shared.snapshots[back].slow_max_ns = (double)tui.slow_max / tui.tsc_per_ns;
                    shared.snapshots[back].slow_count  = tui.slow_count;
                }
#endif
                __atomic_store_n(&shared.active_idx, back, __ATOMIC_RELEASE);
#endif
            }
        }

        //==============================================================================================
        // TUI RENDER (single-threaded mode only)
        //==============================================================================================
#ifndef MULTICORE_TUI
#ifdef LATENCY_BENCH
        // bench mode: TUI disabled, dump stats to stderr periodically
        if (ctrl.total_ticks > 0 && (ctrl.total_ticks % 10000) == 0) {
            double hot_avg = (tui.hot_count > 0) ? (double)tui.hot_sum / tui.hot_count / tui.tsc_per_ns : 0;
            double hot_min_ns = (tui.hot_count > 0) ? (double)tui.hot_min / tui.tsc_per_ns : 0;
            double hot_max_ns = (tui.hot_count > 0) ? (double)tui.hot_max / tui.tsc_per_ns : 0;
            double slow_avg = (tui.slow_count > 0) ? (double)tui.slow_sum / tui.slow_count / tui.tsc_per_ns : 0;
            fprintf(stderr, "[BENCH] tick %lu | hot: avg %.0fns min %.0fns max %.0fns (%lu) | slow: avg %.0fns (%lu)\n",
                    (unsigned long)ctrl.total_ticks, hot_avg, hot_min_ns, hot_max_ns,
                    (unsigned long)tui.hot_count, slow_avg, (unsigned long)tui.slow_count);
        }
#else
        TUI_Render(&tui, &ctrl, &last_stream, ctrl.total_ticks);
#endif
#endif // !MULTICORE_TUI
    }

    //==================================================================================================
    // session summary
    //==================================================================================================
    fprintf(stderr, "\n==================================================\n");
    fprintf(stderr, "  SESSION SUMMARY\n");
    fprintf(stderr, "==================================================\n");
    fprintf(stderr, "  Total ticks:      %lu\n", (unsigned long)ctrl.total_ticks);
    fprintf(stderr, "  Trade log:        %lu entries\n", (unsigned long)log.trade_count);
    fprintf(stderr, "  Open positions:   %d\n", Portfolio_CountActive(&ctrl.portfolio));
    fprintf(stderr, "  Unrealized P&L:   $%.4f\n", FPN_ToDouble(ctrl.portfolio_delta));
#ifdef LATENCY_PROFILING
    fprintf(stderr, "--------------------------------------------------\n");
    if (tui.hot_count > 0) {
        double hot_avg = (double)tui.hot_sum / tui.hot_count / tui.tsc_per_ns;
        double hot_min_ns = (double)tui.hot_min / tui.tsc_per_ns;
        double hot_max_ns = (double)tui.hot_max / tui.tsc_per_ns;
        double hot_p99 = hot_max_ns; // TODO: proper percentile tracking
        fprintf(stderr, "  Hot path:         avg %.0fns  min %.0fns  max %.0fns  (%lu ticks)\n",
                hot_avg, hot_min_ns, hot_max_ns, (unsigned long)tui.hot_count);
    }
    if (tui.slow_count > 0) {
        double slow_avg = (double)tui.slow_sum / tui.slow_count / tui.tsc_per_ns;
        double slow_min_ns = (double)tui.slow_min / tui.tsc_per_ns;
        double slow_max_ns = (double)tui.slow_max / tui.tsc_per_ns;
        fprintf(stderr, "  Slow path:        avg %.0fns  min %.0fns  max %.0fns  (%lu cycles)\n",
                slow_avg, slow_min_ns, slow_max_ns, (unsigned long)tui.slow_count);
    }
#endif
    fprintf(stderr, "==================================================\n");

    // save snapshot WITH positions intact - they'll be resumed on next startup
    PortfolioController_SaveSnapshot(&ctrl, snapshot_path);
    fprintf(stderr, "  Snapshot saved (%d positions persisted)\n",
            Portfolio_CountActive(&ctrl.portfolio));

    //==================================================================================================
    // cleanup
    //==================================================================================================
#ifdef MULTICORE_TUI
    // signal TUI thread to exit, wait for clean terminal restore
    __atomic_store_n(&shared.quit_requested, 1, __ATOMIC_RELEASE);
    pthread_join(tui_tid, NULL);
#else
    TUI_Cleanup(&tui);
#endif
    TradeLog_Close(&log);
    BinanceStream_Close(&bs);
    free(pool.slots);

    return 0;
}
