# Changelog

## [0.4.3] - 2026-03-20 (branch: feature/risk-and-sizing)

### Fixed
- **Stale buy gate clamp** - `buy_conds_initial` was set once during warmup and never updated.
  `AdjustBuyGate`'s max_shift clamp was dragging the gate back toward the original warmup
  price as the market moved, causing the gate to sit $40+ below where it should be after
  8+ minutes. Fix: `buy_conds_initial` now updates from rolling stats each slow-path tick.
  The clamp window moves with the market.
- **Config hot-reload** - `r` key now reloads all fields including fee_rate, risk_pct,
  volume_multiplier, entry_offset_pct, spacing_multiplier, all clamps, and risk limits.
  Also resets live adaptive filters to new values on reload.
- **Dead code** - removed unused `slope_positive` variable from idle squeeze.

### Known Limitations
- Circuit breaker uses `portfolio_delta` from the last slow-path cycle, which could be up to
  `poll_interval` ticks stale. In a flash crash, one fill could slip through before the
  breaker updates. Acceptable tradeoff vs adding Portfolio_ComputePnL to every tick.

## [0.4.2] - 2026-03-20 (branch: feature/risk-and-sizing)

### Fixed
- **Regression optimizes on net P&L** - was using gross unrealized P&L (before fees), causing
  adaptive filters to tune based on illusory profits. Now subtracts estimated exit fees from
  open positions before feeding to regression.
- **Config hot-reload** - pressing `r` now reloads all fields (fee_rate, risk_pct, volume_multiplier,
  entry_offset_pct, spacing_multiplier, all clamps, risk limits). Previously only reloaded
  the original 6 fields. Also resets live adaptive filters to new config values.
- **TUI "TRADES" label** - was showing total ticks, now shows actual buy count and exit count.

### Added
- **Win/loss tracking** - counts TP exits vs SL exits. TUI shows wins, losses, and win rate %.
- **Buy counter** - tracks total entries for the session.
- **Position net P&L** - each position shows both gross and net P&L (after round-trip fees).
- **Position dollar value** - shows `val:$200.04` per position.
- **Disconnect resilience** - snapshot saved before every reconnect attempt.

## [0.4.1] - 2026-03-20 (branch: feature/risk-and-sizing)

### Fixed
- **Balance accounting** - positions were accumulating raw stream quantity via consolidation,
  inflating sizes to 0.7+ BTC instead of the sized 0.003 BTC. Disabled consolidation since
  position sizing means each fill is an independent entry. Entry spacing prevents duplicates.
- **Realized P&L** - now includes entry fee in cost basis. Previously only deducted exit fee,
  understating losses.
- **Position persistence on quit** - pressing `q` now saves snapshot WITH positions intact
  instead of force-closing everything first. Positions resume on restart.
- **Duplicate FEES PAID line** removed from TUI.
- **Snapshot saved before reconnect** - both unplanned disconnects and 24-hour boundary now
  save state before any reconnect attempt.

### Added
- **Position dollar value** in TUI - shows `val:$200.04` (current price * qty) so you can
  see how much you actually hold in each position.
- **Net P&L per position** - shows both gross and net P&L accounting for round-trip fees
  (0.2% at default rate). A position showing +0.04% gross might be -0.16% net.
- **Disconnect resilience** - laptop sleep, WiFi drops, and process crashes all preserve
  positions via snapshot. Engine reconnects and resumes monitoring on wake.

### Notes
- Engine does NOT run while laptop is asleep. Process freezes completely. For 24/7 operation,
  run on a VPS or always-on machine in a tmux session with `tui_enabled=0`.
- Positions that would have hit TP/SL while offline will exit on the first tick after wake,
  which may be at a very different price (gap risk).

## [0.4.0] - 2026-03-20 (branch: feature/risk-and-sizing)

### Added
- **Trading fees** - 0.1% per trade (configurable `fee_rate`). Deducted from balance on
  both buy and sell. Realized P&L is now net of fees. Cumulative fees tracked and displayed
  in TUI. All fee math is branchless FPN operations in the fill/exit paths.

- **Position sizing** - quantity computed as `(balance * risk_pct) / price` instead of using
  the stream's micro-fill volume. Default 2% risk per position ($200 at $10k balance =
  0.00286 BTC at $70k). Configurable via `risk_pct` in engine.cfg.

- **TP fee floor** - minimum take profit set at `entry * fee_rate * 3` (round-trip fees +
  safety margin). Prevents the scenario where a volatility-based TP exit actually loses money
  after fees. At $70k with 0.1% fees, TP floor is $210 above entry.

- **Circuit breaker** - halts all new entries when total P&L (realized + unrealized) drops
  below `max_drawdown_pct` of starting balance. Default 10% = halt at -$1,000 on a $10k
  account. Exit gates keep running to protect existing positions. Branchless check ANDed
  into the fill mask.

- **Exposure limit** - caps total deployed capital at `max_exposure_pct` of starting balance.
  Default 50% = max $5,000 in open positions. Ensures cash reserves for new entries on
  deeper dips. Branchless check ANDed into the fill mask.

- **TUI risk display** - shows fees paid, risk per position, exposure percentage with limit,
  circuit breaker status (OK/TRIPPED).

### Changed
- Fill consumption now checks 6 conditions (all branchless, ANDed together):
  not-duplicate, room-in-portfolio, entry-spacing, can-afford, not-blown, under-exposure
- Exit buffer drain deducts exit fees from proceeds before returning to balance
- Realized P&L is now net of both entry and exit fees

## [0.3.0] - 2026-03-20

### Added
- **Idle squeeze** - when portfolio is empty and price is above buy gate, the entry offset
  shrinks 5% toward minimum each slow-path tick. Solves the trailing-rising-price problem
  where the engine sits idle missing an uptrend. Once a position is entered, squeeze stops
  and P&L-based adaptation takes over. All branchless (word-level mask-select).

- **Paper trading balance** - configurable starting balance (default $10k in engine.cfg).
  Deducts `price * qty` on buy, adds `exit_price * qty` on sell. Balance check is branchless
  (one FPN_GreaterThanOrEqual ANDed into the existing fill mask). Persists in snapshot.

- **Portfolio persistence** - binary snapshot (`portfolio.snapshot`) saves/loads portfolio state,
  realized P&L, adaptive filter values, and balance. Written every slow-path cycle (~1/sec).
  On restart, engine resumes with all positions and state intact. Magic number + version check
  prevents loading stale or corrupt snapshots.

- **TUI balance display** - shows current balance, starting amount, return %, realized P&L,
  unrealized P&L, and total P&L with percentage.

### Changed
- Snapshot format bumped to version 2 (added balance field)
- Default starting_balance in code is 1M (so tests aren't balance-limited), engine.cfg sets 10k

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
