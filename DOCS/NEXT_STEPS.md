# Next Steps

Prioritized list of improvements beyond the current paper trading engine. Items are ordered by impact — what will teach you the most about whether the strategy works before moving to real money.

---

## Priority 1: Understand current performance

These help you interpret what the engine is already doing.

### 1.1 Fill rejection diagnostics
Right now when a fill is rejected you can't see why. Was it spacing? Balance? Exposure? Circuit breaker? Add a rejection reason to stderr (slow path only, not every tick). Helps you tune config without guessing.

### 1.2 Session analytics on quit
Beyond the current summary, dump on exit:
- Average hold time (ticks between entry and exit)
- Average win size ($) vs average loss size ($)
- Largest winner / largest loser
- Max consecutive losses
- Profit factor (gross wins / gross losses)
All derivable from the CSV but useful to see immediately.

### 1.3 Longer paper trading runs
Let it run for hours/days on a machine that doesn't sleep. Collect enough data to see patterns in the CSV. The first few minutes tell you nothing — you need hundreds of trades to evaluate win rate, average P&L per trade, and whether fees eat all the profit.

---

## Priority 2: Stop losing money on bad entries

These address the most common failure modes visible in paper trading.

### 2.1 Cooldown after stop loss
If a position hits SL, the engine immediately looks for the next entry. In a falling market, this means repeated SL hits (catching falling knives). Add a configurable cooldown: skip N slow-path cycles after a loss before allowing new buys. Branchless — decrement a counter each slow-path tick, AND the counter-is-zero check into the fill mask.

### 2.2 Multiple timeframe confirmation
The 128-tick rolling window is ~30 seconds. Add a second, longer window (1000+ ticks, ~5-10 minutes) that tracks the broader trend. Only allow buys when both windows agree — short-term dip within a longer-term uptrend or range. Prevents buying dips that are actually the start of a crash.

### 2.3 Trailing stop loss
Current SL is fixed at entry time. A trailing stop moves the SL up as price rises, locking in gains. If you enter at $70,000 and it goes to $71,000, the SL should move from $68,950 to $69,950. Implementation: on each tick in PositionExitGate, compute `new_sl = current_price - sl_offset`. If `new_sl > current_sl`, update it. Branchless via FPN_Max.

---

## Priority 3: Strategy improvements

These make the entry/exit logic smarter.

### 3.1 Volume-weighted entries
Instead of just "volume >= N * average", look for volume spikes relative to recent history. A sudden 10x volume spike on a dip is a stronger signal than a gradual 3x above average. The rolling stats already have the data — just need a spike detection metric (current tick volume / rolling max volume).

### 3.2 Momentum detection
The current strategy is pure mean-reversion (buy below average). Add a momentum mode: when price slope is strongly positive AND volume is increasing (both from rolling stats), buy INTO the trend instead of waiting for a dip. The idle squeeze is a crude version of this — a proper momentum entry would be cleaner.

### 3.3 Dynamic TP/SL ratio
Currently TP and SL use the same stddev multiplier pattern. But optimal risk/reward isn't symmetric. In trending markets, TP should be wider (let winners run). In ranging markets, TP should be tighter (take quick profits). The rolling price slope could inform this: strong trend → asymmetric TP:SL ratio.

### 3.4 Time-based exits
Some positions sit at neither TP nor SL for extended periods. A time-based exit (close if position held for more than N ticks with less than X% gain) frees up capital for better opportunities. Track entry tick per position (could use an unused field or add one).

---

## Priority 4: Infrastructure for real money

Only after paper trading proves profitable over multiple days.

### 4.1 Slippage simulation
Current fills are at exact price. Real market orders have slippage — you'll get filled slightly worse than the price you saw. Add configurable slippage (e.g. 0.01% worse on both buy and sell) to make paper trading more realistic before going live.

### 4.2 Binance REST API order execution
`DataStream/BinanceOrderAPI.hpp` — HTTPS POST to Binance order endpoint with HMAC-SHA256 signing. API key + secret in a separate `secrets.cfg` (gitignored, chmod 600). Testnet first. This is the bridge from paper trading to real money.

### 4.3 Order status tracking
Real orders can partially fill, get rejected, or timeout. Need to track order state (pending, filled, partial, rejected) and handle each case. The current instant-fill model is a simplification.

### 4.4 Notifications
Send a message (webhook, email, or push notification) on: position opened, TP/SL hit, circuit breaker tripped, disconnect. Important when running headless on a VPS.

---

## Priority 5: Scaling (future)

### 5.1 Multiple symbols
Open multiple websocket connections (one per symbol, each with its own controller). Or use the combined stream endpoint and demultiplex by symbol field.

### 5.2 Correlation awareness
If trading multiple symbols, track correlation between positions. Don't let the portfolio become concentrated in correlated assets (e.g. BTC and ETH often move together).

### 5.3 Separate strategy core
Move strategy logic to a separate process/server. The execution engine stays fast (just executes conditions), while the strategy server does heavier analysis and sends condition updates over the network. This is the architecture mentioned in the OrderGates.hpp comments.

---

## What to watch in paper trading

Before implementing anything above, collect data and look for:

1. **Win rate** — below 40% means entries are poorly timed
2. **Average win vs average loss** — if avg loss > avg win, even 60% win rate loses money
3. **Profit factor** — gross wins / gross losses. Below 1.0 = losing strategy. Need > 1.2 after fees.
4. **Max consecutive losses** — tells you how much drawdown to expect
5. **Hold time distribution** — are positions closing in minutes or hours? Very short holds mean fees dominate.
6. **Time of day patterns** — does the strategy work better during high-volume hours (US market open)?

The CSV has all this data. A simple Python script or spreadsheet analysis after a few days of trading will answer these questions.
