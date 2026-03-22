# CLAUDE.md

## Overview

Tick-level crypto trading engine in C++. Branchless fixed-point arithmetic, bitmap-based portfolio management, regression-driven gate adjustment with regime detection. Single-symbol, single-threaded hot path with multicore TUI.

## Build

CMake with zero-dependency ANSI TUI (default):
```bash
cmake -B build && cmake --build build         # production (ANSI TUI, no deps)
cmake -B build -DUSE_FTXUI=ON && cmake --build build      # FTXUI TUI (auto-fetched)
cmake -B build -DUSE_NOTCURSES=ON && cmake --build build  # notcurses TUI (experimental)
./build/controller_test                        # run tests (128 assertions)
cd build && ./engine                           # run engine (needs engine.cfg symlink)
```

Build options: `-DLATENCY_PROFILING=ON`, `-DLATENCY_LITE=ON`, `-DLATENCY_BENCH=ON`, `-DBUSY_POLL=ON`, `-DUSE_NATIVE_128=ON`. See README.md for details.

## Architecture

```
HOT PATH (every tick):
  BuyGate (branchless) -> OrderPool
  PositionExitGate (branchless bitmap walk) -> ExitBuffer

EVERY TICK (PortfolioController_Tick):
  - consume new fills from pool into portfolio (branchless consolidation)
  - clear consumed pool slots

EVERY N TICKS (slow path):
  - RollingStats_Push (least-squares regression: slope, R², variance)
  - regime detection: RegimeSignals → score-based Regime_Classify
  - strategy dispatch: MR or Momentum adapt + buy signal
  - drain exit buffer, log sells to CSV
  - push rolling.price_slope to ROR (trend acceleration)
```

## Data Flow: Regime Detection

```
RollingStats (128-tick window):
  price_slope (least-squares), price_r_squared, price_variance
  volume_avg, volume_slope, price_avg, price_stddev (range/4), min, max

RollingStats (512-tick window):
  same fields, broader timeframe

ROR Regressor:
  fed rolling.price_slope each slow-path cycle → slope-of-slopes (acceleration)

        ↓ all feed into ↓

RegimeSignals struct (Strategies/RegimeDetector.hpp):
  short_slope, short_r2, short_variance  (128-tick)
  long_slope, long_r2, long_variance     (512-tick)
  vol_ratio (short/long variance)        (self-adapting volatility)
  ror_slope                              (trend acceleration)
  volume_slope                           (volume confirmation)
  [future: model_score, order_flow]      (extensibility hooks)

        ↓

Regime_Classify (score-based):
  trending_score (5 signals: slope×2, R², acceleration, volume)
  volatile_score (2 signals: variance spike, no direction)
  → RANGING / TRENDING / VOLATILE with hysteresis

        ↓

Strategy dispatch → position adjustment on regime switch
```

## Directory Structure

- **CoreFrameworks/** - OrderGates (buy gate), Portfolio (bitmap positions, exit gate), PortfolioController (feedback loop, regime wiring)
- **Strategies/** - RegimeDetector (RegimeSignals, score-based classify, position adjustment), MeanReversion, Momentum, StrategyInterface
- **DataStream/** - FauxFIX, MockGenerator, TradeLog, BinanceCrypto (websocket), EngineTUI (snapshot, thread), TUIAnsi (default, zero-dep), TUIWidgets/TUILayout (FTXUI opt-in), TUINotcurses (notcurses, experimental)
- **FixedPoint/** - FPN arbitrary-width fixed-point arithmetic library
- **MemHeaders/** - PoolAllocator (bitmap order pool), BuddyAllocator
- **ML_Headers/** - RollingStats (regression + R²), LinearRegression3X, ROR_regressor (slope-of-slopes), GateControlNetwork
- **tests/** - controller_test.cpp (101 assertions)
- **plans/** - implementation plans (gitignored)

## Code Conventions

- `using namespace std;` used throughout
- C-style with templates, no classes
- Functions follow Pattern_FunctionName convention (e.g. Portfolio_Init, BuyGate)
- All hot-path math uses FPN<F> fixed point, no floats (F=64 words = 4096-bit precision)
- Branchless patterns: mask tricks with -(uint64_t)pass, word-level mask-select
- Inline comments explain reasoning and learning insights
- User's voice/comments must be preserved exactly when editing existing files

## Current State

- Portfolio controller: COMPLETE (128/128 tests passing)
- Post-SL cooldown: pauses buying for N cycles after stop loss (anti-falling-knife)
- Regime detection: score-based with 7 signals, extensible RegimeSignals struct
- Volume spike detection: current/max ratio, spacing relaxation on 5x+ spikes
- RollingStats: real least-squares regression (slope, R², variance)
- Snapshot persistence: v7 (entry_time + session stats survive restarts)
- Binance websocket: WORKING (live market data)
- TUI: ANSI TUI is default (zero deps, diff-based rendering, foxml palette), FTXUI/notcurses opt-in
- Momentum TP/SL: adaptive (R²-scaled + ROR acceleration bonus at fill time)

## Key Design Decisions

1. Portfolio uses uint16_t bitmap (not sequential count) - same pattern as OrderPool
2. Per-position TP/SL exits on hot path, portfolio management on slow path
3. Fill consumption happens EVERY tick (zero unprotected exposure)
4. Sell logic is per-position percentage-based, buy gate is regression-driven
5. Warmup phase observes market before trading (gates on slow-path sample count, not raw ticks)
6. 24-hour session lifecycle: warmup -> trade -> wind down -> close all -> reconnect
7. TUI is independent of engine (engine runs headless, TUI only reads state)
8. No API key needed for market data websocket (public endpoint)
9. RollingStats computes regression inline (no separate feeder for regime R²)
10. RegimeSignals struct is the extensibility point — new signal = new field + one comparison

## Extensibility Hooks

- **New regime signal**: add field to RegimeSignals + one comparison in Regime_Classify
- **FoxML model output**: add `model_score` to RegimeSignals, feed from bridge
- **New strategy**: new header file + one case in dispatch switch + mapping in Regime_ToStrategy
- **New regime**: new constant + new mapping + optional position adjustment case
- **Lookup table**: RegimeSignals fields map to table indices naturally
