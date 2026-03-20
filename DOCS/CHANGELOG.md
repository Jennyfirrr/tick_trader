# Changelog

## [0.2.0] - 2026-03-20

### Added
- **Live Binance data stream** (`DataStream/BinanceCrypto.hpp`)
  - Full network stack: TCP -> TLS (OpenSSL) -> WebSocket -> JSON -> FPN
  - SSL_pending check before poll() to avoid SSL-internal-buffer mismatch
  - Burst frame draining: reads all buffered frames per poll cycle, runs exit gates on each
  - Ping/pong handled transparently inside frame reader
  - 24-hour session lifecycle with airtight reconnect procedure
  - Supports production (`data-stream.binance.vision`), Binance US, and testnet endpoints
  - No API key needed (public market data endpoint)

- **Terminal UI** (`DataStream/EngineTUI.hpp`)
  - ANSI escape code dashboard, no framework dependencies
  - Displays: price, volume, rolling market structure, buy gate state with adaptive filter values, all open positions with TP/SL distances, realized + unrealized + total P&L
  - Single-char commands: [q]uit, [p]ause, [r]eload config
  - Signal handler restores terminal on crash (SIGINT, SIGTERM, SIGSEGV)
  - Render interval configurable to avoid terminal thrash

- **Rolling market statistics** (`ML_Headers/RollingStats.hpp`)
  - 128-tick rolling window for price and volume
  - Outputs: price average, price stddev (range/4 approximation), price min/max, volume average, volume slope
  - Same ring buffer pattern as RegressionFeederX (branchless power-of-2 wrap)

- **Market microstructure filters**
  - Volume filter: require tick volume >= N * rolling average (filters micro-fills)
  - Entry offset: buy gate set below rolling mean by configurable percentage (buy on dips, not at market)
  - Entry spacing: minimum price distance between positions based on rolling volatility (prevents clustering)
  - Branchless absolute value in spacing check (word-level mask-select)

- **Adaptive filter system**
  - Entry offset and volume multiplier adapt based on P&L regression slope
  - Positive P&L trend -> loosen filters (more aggressive entries)
  - Negative P&L trend -> tighten filters (more defensive)
  - Gated by R^2 confidence threshold (no adjustment on weak fits)
  - Clamped to configurable min/max ranges
  - All adaptation logic is branchless (same mask pattern as gate adjustment)

- **Volatility-based TP/SL**
  - Take profit and stop loss computed from rolling price stddev instead of fixed percentages
  - Calm market: exits tighten (smaller, faster profits)
  - Volatile market: exits widen (positions have room to breathe)
  - Branchless fallback to percentage-based when rolling stats not yet populated

- **Realized P&L tracking**
  - Cumulative realized P&L accumulated when positions close via TP or SL
  - Displayed in TUI alongside unrealized and total P&L

- **Engine binary** (`main.cpp`)
  - Poll-based event loop wiring all components
  - Burst drain with exit gate on every intermediate price
  - Airtight reconnect: force-close all positions, verify bitmap zero, halt on failure
  - Session summary on exit (total ticks, trades, final P&L)
  - `has_data` guard prevents engine tick on zero-price before first real data

- **Integration test** (`tests/binance_test.cpp`)
  - Connects to live Binance data stream, runs full pipeline for 200 ticks
  - Verifies warmup completion, gate condition computation, data parsing

- **Unified config** (`engine.cfg`)
  - Single file for all binance, controller, filter, and TUI settings
  - Key=value format, comments with #

- **Documentation** (`DOCS/`)
  - Architecture overview with data flow diagrams
  - Configuration reference with all parameters
  - This changelog

### Changed
- `PortfolioController` now includes `RollingStats` and live adaptive filter state
- Warmup transition uses rolling stats for initial buy gate (offset from mean, volume filter)
- Slow-path buy gate update uses live adaptive values instead of fixed config
- Controller test updated for new filter behavior (adjusted thresholds and mock data params)
- Fixed filename typo: `BinanceCrytpo.hpp` -> `BinanceCrypto.hpp`

### Fixed
- Engine tick no longer runs on zero-price `DataStream` before first real data arrives
- Tests adjusted for rolling stats + volume filter (80/80 passing)

## [0.1.0] - 2026-03-19

### Initial Release
- Portfolio controller with 80/80 tests passing
- Branchless fixed-point arithmetic library (FPN arbitrary-width)
- Bitmap-based portfolio and order pool management
- Per-position TP/SL exit gates on hot path
- Regression-driven buy gate adjustment (R^2 gated, slope-driven, max_shift clamped)
- Fill consumption every tick (zero unprotected exposure)
- Rate-of-return (slope-of-slopes) regression
- FIX protocol parser and mock data generator
- CSV trade logger
- Pool allocator and buddy allocator
