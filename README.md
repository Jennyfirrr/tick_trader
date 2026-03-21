# Tick Trader

Tick-level crypto trading engine in C++. Branchless fixed-point arithmetic, bitmap-based portfolio management, regime-adaptive strategy switching. Single-symbol, single-threaded hot path with optional multicore TUI.

## Quick Start

```bash
make          # build (multicore TUI, production)
./engine      # connects to Binance, paper trades BTC
make test     # run 101 tests
```

## Architecture

```
HOT PATH (every tick, p50 ~950ns, min ~60ns):
  BuyGate          direction-aware price+volume check     (~80ns avg)
  PositionExitGate branchless bitmap walk, TP/SL per pos  (~130ns/pos)
  FillConsumption  strategy-aware sizing, risk checks      (~750ns avg, ~4us on fill)

SLOW PATH (every 100 ticks):
  RegimeDetector   classify RANGING/TRENDING/VOLATILE
  StrategyDispatch adapt + buy signal (MR or momentum)
  RollingStats     128-tick + 512-tick market statistics
  TradeLog         buffered CSV drain
  Snapshot         binary state persistence (v5)
```

## Strategies

### Mean Reversion (RANGING regime)
- **Entry:** buy when price dips below rolling average (stddev-scaled offset)
- **Exit:** per-position TP/SL, trailing TP (SNR×R² gated), time-based exit
- **Adaptation:** P&L regression loosens/tightens entry filters

### Momentum (TRENDING regime)
- **Entry:** buy when price breaks above rolling average + stddev offset
- **Exit:** wider TP (let trends run), tighter SL (cut false breakouts)
- **Adaptation:** P&L regression adjusts breakout threshold

### Regime Detection
- **RANGING** → mean reversion (buy dips)
- **TRENDING** → momentum (buy breakouts)
- **VOLATILE** → pause buying (existing positions keep TP/SL)
- Classification: slope magnitude + R² consistency + stddev level
- Hysteresis prevents rapid switching (configurable cycles)
- Position TP/SL adjusted on regime transition

## Adding a New Strategy

1. Create `Strategies/NewStrategy.hpp` — implement Init, Adapt, BuySignal, ExitAdjust
2. Add `STRATEGY_NEW = 2` to `StrategyInterface.hpp`
3. Add case to `Strategy_Dispatch` switch in `PortfolioController.hpp`
4. Add case to `PortfolioController_Unpause` switch
5. Add config fields + defaults to `ControllerConfig.hpp`
6. Map regime → strategy in `Regime_ToStrategy` (if applicable)

No duplicate code to sync — shared functions handle config reload, unpause, and regime cycling.

## Build

Requires: g++ (C++17), OpenSSL, CMake 3.14+. FTXUI is fetched automatically.

```bash
make              # build (cmake + FTXUI, multicore TUI)
make test         # run 101 tests
make profile      # with per-component latency profiling
make profile-lite # total-only latency (lower overhead)
make bench        # headless profiling to stderr
make clean        # remove build directory
```

### CMake Options

| Option | Default | Effect |
|--------|---------|--------|
| `LATENCY_PROFILING` | OFF | RDTSCP timing with per-component breakdown |
| `LATENCY_LITE` | OFF | Total-only timing (2 rdtscp vs 4) |
| `LATENCY_BENCH` | OFF | Headless profiling to stderr |

## TUI

FTXUI-based terminal dashboard with FoxML color theme. Engine runs on core 0,
TUI renders on core 1 from a double-buffered snapshot (zero engine contention).

Features:
- 3 preset layouts: Standard, Positions Focus, Compact (cycle with `l`)
- Live price + P&L sparkline graphs (120-point ring buffer)
- Scrollable position list with hold time, trailing status
- Exposure gauge, regime status with duration
- Auto-resizes to terminal dimensions

## TUI Controls

| Key | Action |
|-----|--------|
| `q` | Quit (saves positions to snapshot) |
| `p` | Pause/unpause buying (exit gate keeps running) |
| `r` | Hot-reload engine.cfg |
| `s` | Cycle regime (RANGING→TRENDING→VOLATILE) for testing |
| `l` | Cycle layout (Standard→Positions→Compact) |

## Project Structure

```
CoreFrameworks/
  OrderGates.hpp          BuyGate (direction-aware), SellGate
  Portfolio.hpp            Position storage, ExitGate, bitmap ops
  PortfolioController.hpp  Tick function, dispatch, shared functions, snapshot v5
  ControllerConfig.hpp     Config struct, parser, defaults

Strategies/
  StrategyInterface.hpp    Strategy contract + enum definitions
  MeanReversion.hpp        Buy dips, stddev-scaled offset, P&L adaptation
  Momentum.hpp             Buy breakouts, trend-following, tighter SL
  RegimeDetector.hpp       Market classification + position adjustment

DataStream/
  BinanceCrypto.hpp        WebSocket stream (TCP/TLS/WS/JSON)
  EngineTUI.hpp            Terminal dashboard, multicore snapshot
  TradeLog.hpp             CSV logger + ring buffer

FixedPoint/
  FixedPointN.hpp          Arbitrary-width fixed-point arithmetic
  FixedPoint64.hpp         Static 128-bit FP (experimental)

ML_Headers/
  RollingStats.hpp         Price/volume moving avg, stddev, slope
  LinearRegression3X.hpp   3-sample rolling regression
  ROR_regressor.hpp        Slope-of-slopes (second derivative)

DOCS/
  CHANGELOG.md             Version history
  NEXT_STEPS.md            Roadmap (prioritized)
  ARCHITECTURE.md          System design
  CONFIGURATION.md         All config keys
  PERFORMANCE.md           Hot-path optimization guide
  LATENCY_PROFILING.md     Measurement guide
  FUTURE_AUTOTUNE.md       Regime threshold auto-tuning (planned)
  FUTURE_VOLATILE_STRATEGY.md  Volatile regime strategy (planned)
```

## Docs

- `DOCS/CONTRIBUTING.md` — guide for adding strategies and extending the engine
- `DOCS/CHANGELOG.md` — version history
- `DOCS/NEXT_STEPS.md` — roadmap with completed/remaining items
- `DOCS/ARCHITECTURE.md` — system design and data flow
- `DOCS/CONFIGURATION.md` — all config keys
- `DOCS/PERFORMANCE.md` — hot-path breakdown, optimization guide
- `DOCS/LATENCY_PROFILING.md` — measurement guide and build modes
- `engine.cfg` — annotated config with all parameters
