# Next Steps

Prioritized improvements beyond the current engine. Updated 2026-03-21 (v3.0.1).

Items marked ~~strikethrough~~ are DONE. Remaining items ordered by impact.

---

## Priority 1: Validate current system

### 1.1 Fill rejection diagnostics
Add a rejection reason to stderr on slow path when fills are rejected (spacing? balance?
exposure? circuit breaker?). Helps tune config without guessing.

### ~~1.2 Session analytics on quit~~ DONE (v0.4.2+)
Win/loss count, win rate, profit factor, avg win/loss, avg hold time — all in TUI and
session summary.

### 1.3 Longer paper trading runs
Run for hours/days. Collect enough data (hundreds of trades) to evaluate win rate, average
P&L per trade, and whether fees eat the profit. First few minutes tell you nothing.

### 1.4 Regime detector tuning
Paper trade with regime switching enabled. Log every regime transition with timestamp and
subsequent P&L. After 2+ weeks, analyze which transitions were profitable. See
DOCS/FUTURE_AUTOTUNE.md.

---

## Priority 2: Entry quality

### 2.1 Cooldown after stop loss
After SL hit, skip N slow-path cycles before allowing new buys. Prevents repeated SL hits
in falling markets (catching falling knives). Branchless counter decrement.

### ~~2.2 Multiple timeframe confirmation~~ DONE (v0.6.0)
512-tick long window + `min_long_slope` gate.

### ~~2.3 Trailing take-profit~~ DONE (v0.7.0)
SNR × R² gated trailing TP/SL.

---

## Priority 3: Strategy improvements

### ~~3.1 Volume-weighted entries~~ DONE (v3.0.4)
Volume spike detection: `current_volume / rolling.volume_max`. Spike ratio ≥ 5x triggers
spacing relaxation (0.5x) for tighter position clustering on high-conviction dips.

### ~~3.2 Momentum strategy~~ DONE (v1.0.0)
Momentum_BuySignal buys breakouts above rolling avg. Regime detector switches between
MR and momentum based on market state.

### 3.3 Dynamic TP/SL ratio
TP/SL ratio informed by regime — trending markets get asymmetric (wider TP, tighter SL).
Partially done via strategy-aware fill path (momentum uses different multipliers).

### ~~3.4 Time-based exits~~ DONE (v0.7.1)
`max_hold_ticks` + `min_hold_gain_pct`.

### 3.5 Volatile regime strategy
Replace "pause buying" with active management. See DOCS/FUTURE_VOLATILE_STRATEGY.md.
Options: exit-only management (v1), reduced exposure (v2), wide straddle (v3).

---

## Priority 4: Infrastructure for real money

Only after paper trading proves profitable over multiple days.

### 4.1 Slippage simulation
Add configurable slippage (e.g. 0.01% worse on both buy and sell) to make paper trading
more realistic.

### 4.2 Binance REST API order execution
HTTPS POST with HMAC-SHA256 signing. API key + secret in `secrets.cfg` (gitignored).
Testnet first.

### 4.3 Order status tracking
Handle partial fills, rejections, timeouts. Current instant-fill model is a simplification.

### 4.4 Notifications
Webhook/email on: position opened, TP/SL hit, circuit breaker tripped, disconnect.

---

## Priority 5: Scaling

### 5.1 Multi-currency with shared portfolio
One core per currency, each with its own hot path, rolling stats, and strategy dispatch.
Shared portfolio managed by a central Portfolio Manager on a dedicated core.

**Architecture:**
```
Core 0: BTC engine (own hot path, own strategy, own regime detector)
Core 1: ETH engine (own hot path, own strategy, own regime detector)
Core 2: Portfolio Manager (shared balance, exposure limits, fill/exit requests)
Core 3: TUI (reads from all engines via per-engine snapshots)
```

**Key design decisions:**
- Per-currency position slots: positions 0-7 for BTC, 8-15 for ETH (partition bitmap)
- Balance access via lock-free message queue to Portfolio Manager (no hot-path locking)
- Exposure limit is cross-currency: Portfolio Manager enforces total deployed < max
- Each engine has local position tracking, central manager has the source of truth

**What changes:** Portfolio.hpp (partitioned bitmap), new PortfolioManager.hpp (message
queue + balance allocation), main.cpp (multi-thread spawn), TUI (multi-engine snapshots)

**What stays the same:** BuyGate, ExitGate, PortfolioController_Tick, strategies,
regime detection — all per-currency, no changes needed

### 5.2 Additional data streams
Currently using `@aggTrade` (individual trades). Other free Binance streams:
- `@depth` — orderbook bid/ask levels (~10 updates/sec even in calm markets).
  Orderbook imbalance (more bids = buying pressure) is a strong regime signal.
- `@kline_1s` — 1-second OHLCV candles, guaranteed 1/sec minimum. Useful for
  consistent tick rate instead of variable trade frequency.
- Multiple symbols simultaneously — subscribe to BTC + ETH + SOL on separate
  websocket connections for more total data. See 5.1 multi-currency architecture.

### 5.3 Correlation awareness
Track correlation between positions across symbols. Prevent concentrated exposure in
correlated assets (BTC + ETH move together ~70% of the time). Portfolio Manager should
reduce position size when entering a correlated asset that already has exposure.

### ~~5.3 Strategy library~~ DONE (v0.5.0, v1.0.0)
StrategyInterface.hpp contract, MeanReversion.hpp, Momentum.hpp, RegimeDetector.hpp.
Runtime dispatch via strategy_id. See README for extensibility guide.

### 5.4 Strategy dispatch refactor (when adding strategy #3)
- Replace switch statements with macro-generated dispatch or vtable
- Split ControllerConfig into per-strategy config files
- See README Build Modes for current architecture

### 5.5 Separate strategy server
Move strategy logic to separate process. Engine stays fast (executes conditions),
strategy server does analysis and sends updates over IPC.
