# Configuration

All settings are in `engine.cfg` (key=value format). Lines starting with `#` are comments. The controller and binance configs are parsed from the same file.

## Data Source

| Key | Default | Description |
|---|---|---|
| `symbol` | `btcusdt` | Trading pair (lowercase) |
| `use_testnet` | `0` | 1 = testnet endpoint, 0 = production |
| `use_binance_us` | `0` | 1 = binance.us endpoint |
| `poll_timeout_ms` | `100` | poll() timeout in milliseconds |
| `reconnect_delay` | `5` | seconds to wait before reconnect |

## Session Lifecycle

| Key | Default | Description |
|---|---|---|
| `wind_down_minutes` | `5` | stop buys X minutes before reconnect |

## Warmup

| Key | Default | Description |
|---|---|---|
| `warmup_ticks` | `64` | ticks to observe before trading |

## Regression / Gate Adjustment

| Key | Default | Description |
|---|---|---|
| `poll_interval` | `100` | ticks between slow-path runs |
| `r2_threshold` | `0.30` | min R^2 to trust regression |
| `slope_scale_buy` | `0.50` | how much P&L slope shifts buy price |
| `max_shift` | `5.00` | max price drift from initial conditions ($) |

## Position Exit Conditions

| Key | Default | Description |
|---|---|---|
| `take_profit_pct` | `3.00` | TP as % (also used as stddev multiplier for volatility-based exits) |
| `stop_loss_pct` | `1.50` | SL as % (also used as stddev multiplier for volatility-based exits) |

When rolling stats are populated, TP/SL are computed as `entry +/- (stddev * pct * 100)`. Falls back to percentage-based when stats aren't ready.

## Market Microstructure Filters

These are initial values that adapt at runtime based on P&L regression.

| Key | Default | Description |
|---|---|---|
| `volume_multiplier` | `3.00` | require tick volume >= this * rolling avg |
| `entry_offset_pct` | `0.15` | buy gate offset below rolling mean (%) |
| `spacing_multiplier` | `2.00` | min entry spacing = stddev * this |

## Adaptive Filter Clamps

Control how far the adaptive filters can drift from initial values.

| Key | Default | Description |
|---|---|---|
| `offset_min` | `0.05` | most aggressive offset (%) |
| `offset_max` | `0.50` | most defensive offset (%) |
| `vol_mult_min` | `1.50` | most aggressive volume multiplier |
| `vol_mult_max` | `6.00` | most defensive volume multiplier |
| `filter_scale` | `0.50` | how fast filters adapt to P&L slope |

## TUI

| Key | Default | Description |
|---|---|---|
| `tui_enabled` | `1` | 0 = headless mode (no terminal output) |
| `tui_render_interval` | `10` | render every N ticks |

## Hot-Swappable vs Restart-Required

**Hot-swappable** (press `r` in TUI to reload):
- poll_interval, r2_threshold, slope_scale_buy, max_shift
- take_profit_pct, stop_loss_pct (only affects NEW positions)
- volume_multiplier, entry_offset_pct, spacing_multiplier
- All adaptive filter clamps

**Requires restart:**
- symbol (needs websocket reconnect)
- use_testnet, use_binance_us (needs reconnect)
- warmup_ticks (already completed)

## Example Config

```
# data source
symbol=btcusdt
use_testnet=0
use_binance_us=0
poll_timeout_ms=100
reconnect_delay=5

# session lifecycle
wind_down_minutes=5

# warmup phase
warmup_ticks=64

# portfolio-level
poll_interval=100
r2_threshold=0.30
slope_scale_buy=0.50
max_shift=5.00

# per-position exit conditions
take_profit_pct=3.00
stop_loss_pct=1.50

# market microstructure filters
volume_multiplier=3.00
entry_offset_pct=0.15
spacing_multiplier=2.00

# adaptive filter clamps
offset_min=0.05
offset_max=0.50
vol_mult_min=1.50
vol_mult_max=6.00
filter_scale=0.50

# TUI
tui_enabled=1
tui_render_interval=10
```
