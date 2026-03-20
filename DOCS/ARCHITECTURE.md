# Architecture

## Overview

Tick-level crypto trading engine. Single-threaded, poll-based event loop. Branchless fixed-point arithmetic on the hot path. Connects to Binance websocket for live market data, paper trades with simulated fills.

## Data Flow

```
[Binance WSS] -> [TLS Socket] -> [WebSocket Frame Parser] -> [JSON Parser]
                                                                    |
                                                             [FPN_FromString]
                                                                    |
                                                              [DataStream<F>]
                                                                    |
                           +----------------------------------------+
                           |                    |                   |
                        [BuyGate]      [PositionExitGate]   [PortfolioController]
                           |                    |                   |
                      [OrderPool]         [ExitBuffer]     [Regression + Adaptive Filters]
                           |                    |                   |
                           +--------------------+-------------------+
                                                |
                                         [Trade Decision]
                                                |
                                     [TradeLog CSV + TUI]
```

## Event Loop (main.cpp)

Single-threaded, poll-based. One iteration:

1. `BinanceStream_Poll()` - checks SSL_pending first (avoids SSL/poll mismatch), then poll() on socket + stdin
2. **Burst drain** - if data arrived, read ALL buffered frames. Exit gates run on every intermediate price (catches TP/SL spikes mid-burst). Only the final price is used for BuyGate
3. **TUI input** - handle q/p/r commands from stdin
4. **Session lifecycle** - wind-down and reconnect checks
5. **Engine tick** - BuyGate -> PositionExitGate -> PortfolioController_Tick
6. **TUI render** - every N ticks, display dashboard

The engine runs identically whether data is arriving or not. Poll timeout ensures exit gates are always checked against last known price.

## Hot Path (every tick)

All branchless except unavoidable bitmap loops:

| Function | Description | Branches |
|---|---|---|
| BuyGate | price/volume threshold check, mask-write to pool | None |
| PositionExitGate | bitmap walk, per-position TP/SL check | while(active) only |
| Fill consumption | pool -> portfolio transfer, consolidation | while(fills) only |
| Entry spacing | min distance check against existing positions | while(active_pos) only |

Bitmap loops are unavoidable (variable active count) but all inner logic is branchless - mask tricks with `-(uint64_t)pass`, word-level mask-select.

## Slow Path (every poll_interval ticks)

Runs every N ticks (configurable, default 100):

1. Drain exit buffer, log sells, accumulate realized P&L
2. Update rolling market stats (128-tick window)
3. Update buy gate from rolling stats using live adaptive filter values
4. Compute unrealized P&L, push to regression feeder
5. Run regression + ROR (slope-of-slopes)
6. Adjust buy gate price based on P&L slope
7. Adjust adaptive filters (offset, volume multiplier) based on P&L slope

## Network Stack (DataStream/BinanceCrypto.hpp)

Six layers, all in one header:

1. **TCP** - POSIX socket + getaddrinfo
2. **TLS** - OpenSSL with SNI
3. **WebSocket** - HTTP upgrade, frame parser with partial read handling
4. **Ping/Pong** - transparent inside frame reader, masked client frames
5. **JSON** - fixed-format scan for "p" and "q" fields
6. **DataStream** - FPN_FromString conversion to fixed-point

Endpoints:
- `data-stream.binance.vision:443` - production, no geo-restriction
- `stream.binance.com:9443` - production, geo-restricted
- `stream.binance.us:9443` - US endpoint
- `testnet.binance.vision:443` - testnet

## Adaptive Filter System

Three filters that adapt based on portfolio performance:

### Entry Offset
- Initial: buy at rolling_avg - 0.15%
- Adapts: P&L positive -> shrinks (more aggressive), P&L negative -> grows (more defensive)
- Clamps: [0.05%, 0.50%]

### Volume Multiplier
- Initial: require volume >= 3.0x rolling average
- Adapts: P&L positive -> lowers (accept smaller trades), P&L negative -> raises (require larger trades)
- Clamps: [1.5x, 6.0x]

### Entry Spacing
- Dynamic: min distance = rolling_stddev * spacing_multiplier
- Based on recent price volatility, not fixed percentage
- Prevents position clustering at similar price levels

All adaptation is gated by R^2 confidence threshold - no adjustment when the regression fit is weak.

## Session Lifecycle (24-hour cycle)

Binance auto-disconnects after 24 hours. The engine manages this proactively:

```
T+0:00:00   CONNECT + WARMUP (observe market, build rolling stats)
T+0:XX:XX   ACTIVE (buy gate enabled, positions tracked)
T+23:25:00  WIND DOWN (disable buy gate, exit gate still active)
T+23:30:00  CLOSE ALL + RECONNECT (airtight procedure below)
```

### Airtight Reconnect Procedure
1. Disable buy gate
2. Drain all remaining websocket frames, run exit gate on each
3. Force-close every remaining position at last known price
4. **VERIFY**: assert(portfolio.active_bitmap == 0) - halts if not zero
5. Clear order pool
6. SSL shutdown + close socket
7. Reconnect TCP + TLS + WebSocket
8. Reset controller to warmup

The hard gate at step 4 ensures the engine never reconnects with orphaned positions.

## Fixed-Point Arithmetic (FixedPoint/FixedPointN.hpp)

All hot-path math uses `FPN<F>` with F=64 fractional bits (~19 decimal digits precision). No floats on the hot path.

- Arbitrary width via template parameter
- All comparisons return int (0/1) for branchless mask generation
- Mul/Div use __uint128_t for intermediate carry
- Endian swaps use __builtin_bswap16/64 (single instruction)

## File Map

```
CoreFrameworks/
  OrderGates.hpp          - BuyGate, SellGate (branchless threshold checks)
  Portfolio.hpp           - bitmap-based position tracking, PositionExitGate
  PortfolioController.hpp - feedback loop, regression, adaptive filters

DataStream/
  BinanceCrypto.hpp       - websocket data stream (TCP/TLS/WS/JSON/FPN)
  EngineTUI.hpp           - ANSI terminal dashboard
  FauxFIX.hpp             - FIX protocol parser (for mock data)
  MockGenerator.hpp       - synthetic market data generator
  TradeLog.hpp            - CSV trade logger

FixedPoint/
  FixedPointN.hpp         - arbitrary-width fixed-point arithmetic

MemHeaders/
  PoolAllocator.hpp       - bitmap-based order pool
  BuddyAllocator.hpp      - buddy allocator (unused currently)

ML_Headers/
  LinearRegression3X.hpp  - 3-point OLS regression
  ROR_regressor.hpp       - slope-of-slopes (rate of return regression)
  RollingStats.hpp        - rolling price/volume statistics
  GateControlNetwork.hpp  - gate control network

tests/
  controller_test.cpp     - 80 assertions, portfolio + controller + integration
  binance_test.cpp        - live data integration test

main.cpp                  - engine entry point, event loop
engine.cfg                - unified configuration
```
