# Tick Trader

Tick-level crypto trading engine in C++. Branchless fixed-point arithmetic, bitmap-based portfolio management, regression-driven strategy adaptation. Single-symbol, single-threaded hot path with optional multicore TUI.

## Quick Start

```bash
# build
g++ -std=c++17 -O2 -lssl -lcrypto -o engine main.cpp

# run (connects to Binance, paper trades BTC)
./engine

# custom config
./engine /path/to/config.cfg
```

## Architecture

```
HOT PATH (every tick, p50 ~950ns, min ~60ns):
  BuyGate          branchless price+volume check          (~80ns avg)
  PositionExitGate branchless bitmap walk, TP/SL per pos  (~130ns/pos)
  FillConsumption  consume fills, position sizing, risk   (~750ns avg, ~4us on fill)

SLOW PATH (every 100 ticks):
  RollingStats     128-tick + 512-tick market statistics
  Strategy         MeanReversion adapt + buy signal + exit adjust
  TradeLog         buffered CSV drain
  Snapshot         binary portfolio persistence
```

## Strategy

Mean reversion with adaptive filters:
- **Entry:** buy when price dips below rolling average (stddev-scaled or percentage offset)
- **Exit:** per-position TP/SL, trailing TP (SNR×R² gated), time-based exit
- **Adaptation:** P&L regression loosens/tightens entry filters
- **Multi-timeframe gate:** blocks buys when 512-tick trend is negative

All strategy parameters in `engine.cfg` with inline documentation. Hot-reload via `r` key.

## Build Modes

```bash
# production — zero measurement overhead
g++ -std=c++17 -O2 -lssl -lcrypto -o engine main.cpp

# multicore TUI — engine on core 0, TUI on core 1 (no L1 cache pollution)
g++ -std=c++17 -O2 -DMULTICORE_TUI -lssl -lcrypto -lpthread -o engine main.cpp

# latency profiling — per-component RDTSCP breakdown in TUI (4 rdtscp/tick)
g++ -std=c++17 -O2 -DLATENCY_PROFILING -DMULTICORE_TUI -lssl -lcrypto -lpthread -o engine main.cpp

# latency lite — total-only measurement, lower overhead (2 rdtscp/tick)
g++ -std=c++17 -O2 -DLATENCY_PROFILING -DLATENCY_LITE -DMULTICORE_TUI -lssl -lcrypto -lpthread -o engine main.cpp

# bench — profiling with TUI disabled, stats to stderr
g++ -std=c++17 -O2 -DLATENCY_PROFILING -DLATENCY_BENCH -lssl -lcrypto -o engine main.cpp
```

### Compile Flags

| Flag | Effect |
|------|--------|
| `MULTICORE_TUI` | Engine on core 0, TUI on core 1. Requires `-lpthread` |
| `LATENCY_PROFILING` | RDTSCP timing with per-component breakdown, p50/p95 histogram |
| `LATENCY_LITE` | Total-only timing (2 rdtscp vs 4), ~70ns less measurement overhead |
| `LATENCY_BENCH` | Profiling with TUI disabled, dumps stats to stderr |
| `USE_NATIVE_128` | FPN<64> ops forward to FP64 `__uint128_t` (experimental, adds overhead) |
| `BUSY_POLL` | Spin-poll instead of sleeping in poll() (experimental, wastes CPU) |

## TUI Controls

| Key | Action |
|-----|--------|
| `q` | Quit (saves positions to snapshot) |
| `p` | Pause/unpause buying (exit gate keeps running) |
| `r` | Hot-reload engine.cfg |

## Tests

```bash
cd tests
g++ -std=c++17 -O2 -I.. -o controller_test controller_test.cpp
./controller_test   # 101 tests
```

## Docs

- `DOCS/CHANGELOG.md` — version history
- `DOCS/PERFORMANCE.md` — hot-path breakdown, optimization guide
- `DOCS/LATENCY_PROFILING.md` — measurement guide and build modes
- `DOCS/CONFIGURATION.md` — all config keys
- `DOCS/ARCHITECTURE.md` — system design
- `engine.cfg` — annotated config with all parameters explained
