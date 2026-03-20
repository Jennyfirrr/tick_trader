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
    EngineTUI tui;
    TUI_Init(&tui, 1, 10);  // TODO: read tui_enabled and render_interval from config

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
        if (ready & POLL_STDIN) {
            TUI_HandleInput(&tui, &ctrl, cfg_path, &running);
        }

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
            BuyGate(&ctrl.buy_conds, &last_stream, &pool);
            PositionExitGate(&ctrl.portfolio, last_stream.price, &ctrl.exit_buf, ctrl.total_ticks);
            PortfolioController_Tick(&ctrl, &pool, last_stream.price, last_stream.volume, &log);

            // save snapshot every slow-path cycle (portfolio state survives crashes)
            if (ctrl.tick_count == 0) {
                PortfolioController_SaveSnapshot(&ctrl, snapshot_path);
            }
        }

        //==============================================================================================
        // TUI RENDER
        //==============================================================================================
        TUI_Render(&tui, &ctrl, &last_stream, ctrl.total_ticks);
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
    fprintf(stderr, "==================================================\n");

    // save snapshot WITH positions intact - they'll be resumed on next startup
    PortfolioController_SaveSnapshot(&ctrl, snapshot_path);
    fprintf(stderr, "  Snapshot saved (%d positions persisted)\n",
            Portfolio_CountActive(&ctrl.portfolio));

    //==================================================================================================
    // cleanup
    //==================================================================================================
    TUI_Cleanup(&tui);
    TradeLog_Close(&log);
    BinanceStream_Close(&bs);
    free(pool.slots);

    return 0;
}
