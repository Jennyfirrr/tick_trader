//======================================================================================================
// [CONTROLLER CONFIG]
//======================================================================================================
// configuration for the portfolio controller - all tunable parameters in one place
// parsed from a simple key=value text file, no JSON, no external libs
//======================================================================================================
#ifndef CONTROLLER_CONFIG_HPP
#define CONTROLLER_CONFIG_HPP

#include "../FixedPoint/FixedPointN.hpp"
#include "../ML_Headers/LinearRegression3X.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
//======================================================================================================
// [CONFIG]
//======================================================================================================
template <unsigned F> struct ControllerConfig {
  uint32_t poll_interval;  // ticks between slow-path runs
  uint32_t warmup_ticks;   // ticks to observe before trading
  FPN<F> r2_threshold;     // min R^2 to trust regression
  FPN<F> slope_scale_buy;  // how much slope shifts buy price threshold
  FPN<F> max_shift;        // max drift from initial buy conditions
  FPN<F> take_profit_pct;  // per-position take profit (e.g. 0.03 = 3%)
  FPN<F> stop_loss_pct;    // per-position stop loss (e.g. 0.015 = 1.5%)
  FPN<F> starting_balance; // paper trading starting balance (e.g. 10000.0)
  FPN<F> fee_rate;         // per-trade fee rate (e.g. 0.001 = 0.1% for Binance)
  FPN<F> risk_pct; // fraction of balance to risk per position (e.g. 0.02 = 2%)
  // market microstructure filters (initial values - adapted at runtime by P&L
  // regression)
  FPN<F> volume_multiplier; // buy only when tick volume >= this * rolling_avg
                            // (e.g. 3.0)
  FPN<F> entry_offset_pct;  // buy gate offset below rolling mean (e.g. 0.0015 =
                            // 0.15%)
  FPN<F> spacing_multiplier; // min entry spacing = stddev * this (e.g. 2.0)
  // adaptation clamps - how far the filters can drift from their initial values
  FPN<F>
      offset_min; // min entry_offset_pct (most aggressive, e.g. 0.0005 = 0.05%)
  FPN<F> offset_max; // max entry_offset_pct (most defensive, e.g. 0.005 = 0.5%)
  FPN<F> vol_mult_min; // min volume_multiplier (most aggressive, e.g. 1.5)
  FPN<F> vol_mult_max; // max volume_multiplier (most defensive, e.g. 6.0)
  FPN<F> filter_scale; // how much P&L slope shifts the filters (e.g. 0.50)
  // risk management
  FPN<F> max_drawdown_pct; // halt trading if total P&L drops below this % of
                           // starting balance (e.g. 0.10 = 10%)
  FPN<F> max_exposure_pct; // max fraction of balance deployed in positions
                           // (e.g. 0.50 = 50%)
  // enhanced buy signal (disabled by default = backward compatible)
  FPN<F> offset_stddev_mult;  // stddev-scaled offset multiplier (0 = use percentage mode)
  FPN<F> offset_stddev_min;   // adaptation lower bound for stddev mode (e.g. 0.5)
  FPN<F> offset_stddev_max;   // adaptation upper bound for stddev mode (e.g. 4.0)
  FPN<F> min_long_slope;      // min long-window price slope to allow buys (0 = disabled)
};
//======================================================================================================
template <unsigned F> inline ControllerConfig<F> ControllerConfig_Default() {
  ControllerConfig<F> cfg;
  cfg.poll_interval = 100;
  cfg.warmup_ticks = MAX_WINDOW * 8; // 64 ticks, enough for one full ROR cycle
  cfg.r2_threshold = FPN_FromDouble<F>(0.30);
  cfg.slope_scale_buy = FPN_FromDouble<F>(0.50);
  cfg.max_shift = FPN_FromDouble<F>(5.00);
  cfg.take_profit_pct = FPN_FromDouble<F>(0.03);
  cfg.stop_loss_pct = FPN_FromDouble<F>(0.015);
  cfg.starting_balance =
      FPN_FromDouble<F>(1000000.0); // 1M default so tests arent balance-limited
  cfg.fee_rate = FPN_FromDouble<F>(0.001); // 0.1% per trade (Binance default)
  cfg.risk_pct = FPN_FromDouble<F>(0.02);  // risk 2% of balance per position
  cfg.volume_multiplier = FPN_FromDouble<F>(3.0);
  cfg.entry_offset_pct = FPN_FromDouble<F>(0.0015);
  cfg.spacing_multiplier = FPN_FromDouble<F>(2.0);
  cfg.offset_min = FPN_FromDouble<F>(0.0005);     // 0.05% - most aggressive
  cfg.offset_max = FPN_FromDouble<F>(0.005);      // 0.5%  - most defensive
  cfg.vol_mult_min = FPN_FromDouble<F>(1.5);      // 1.5x  - most aggressive
  cfg.vol_mult_max = FPN_FromDouble<F>(6.0);      // 6.0x  - most defensive
  cfg.filter_scale = FPN_FromDouble<F>(0.50);     // how fast filters adapt
  cfg.max_drawdown_pct = FPN_FromDouble<F>(0.10); // halt at 10% drawdown
  cfg.max_exposure_pct =
      FPN_FromDouble<F>(0.50); // max 50% of balance in positions
  cfg.offset_stddev_mult = FPN_Zero<F>();         // 0 = disabled, use percentage mode
  cfg.offset_stddev_min = FPN_FromDouble<F>(0.5); // 0.5 stddev - most aggressive
  cfg.offset_stddev_max = FPN_FromDouble<F>(4.0); // 4.0 stddev - most defensive
  cfg.min_long_slope = FPN_Zero<F>();             // 0 = disabled
  return cfg;
}
//======================================================================================================
// [CONFIG PARSER]
//======================================================================================================
// simple key=value text file parser, no JSON, no external libs
// returns defaults if file is missing or unreadable
//======================================================================================================
template <unsigned F>
inline ControllerConfig<F> ControllerConfig_Load(const char *filepath) {
  ControllerConfig<F> cfg = ControllerConfig_Default<F>();

  FILE *f = fopen(filepath, "r");
  if (!f)
    return cfg;

  char line[256];
  while (fgets(line, sizeof(line), f)) {
    // strip \r\n
    int len = 0;
    while (line[len] && line[len] != '\n' && line[len] != '\r')
      len++;
    line[len] = '\0';

    // skip empty lines and comments
    if (len == 0 || line[0] == '#')
      continue;

    // find '='
    int eq_pos = -1;
    for (int i = 0; i < len; i++) {
      if (line[i] == '=') {
        eq_pos = i;
        break;
      }
    }
    if (eq_pos < 0)
      continue;

    // null-terminate key, value starts after '='
    line[eq_pos] = '\0';
    char *key = line;
    char *val = &line[eq_pos + 1];

    if (strcmp(key, "poll_interval") == 0)
      cfg.poll_interval = (uint32_t)atol(val);
    else if (strcmp(key, "warmup_ticks") == 0)
      cfg.warmup_ticks = (uint32_t)atol(val);
    else if (strcmp(key, "r2_threshold") == 0)
      cfg.r2_threshold = FPN_FromDouble<F>(atof(val));
    else if (strcmp(key, "slope_scale_buy") == 0)
      cfg.slope_scale_buy = FPN_FromDouble<F>(atof(val));
    else if (strcmp(key, "max_shift") == 0)
      cfg.max_shift = FPN_FromDouble<F>(atof(val));
    else if (strcmp(key, "take_profit_pct") == 0)
      cfg.take_profit_pct = FPN_FromDouble<F>(atof(val) / 100.0);
    else if (strcmp(key, "stop_loss_pct") == 0)
      cfg.stop_loss_pct = FPN_FromDouble<F>(atof(val) / 100.0);
    else if (strcmp(key, "starting_balance") == 0)
      cfg.starting_balance = FPN_FromDouble<F>(atof(val));
    else if (strcmp(key, "fee_rate") == 0)
      cfg.fee_rate = FPN_FromDouble<F>(atof(val) / 100.0);
    else if (strcmp(key, "risk_pct") == 0)
      cfg.risk_pct = FPN_FromDouble<F>(
          atof(val) / 100.0); // needs to be current portfolio value, or total
                              // equity holding it goes 1000 -> 900 -> 800
    else if (strcmp(key, "volume_multiplier") == 0)
      cfg.volume_multiplier = FPN_FromDouble<F>(atof(val));
    else if (strcmp(key, "entry_offset_pct") == 0)
      cfg.entry_offset_pct = FPN_FromDouble<F>(atof(val) / 100.0);
    else if (strcmp(key, "spacing_multiplier") == 0)
      cfg.spacing_multiplier = FPN_FromDouble<F>(atof(val));
    else if (strcmp(key, "offset_min") == 0)
      cfg.offset_min = FPN_FromDouble<F>(atof(val) / 100.0);
    else if (strcmp(key, "offset_max") == 0)
      cfg.offset_max = FPN_FromDouble<F>(atof(val) / 100.0);
    else if (strcmp(key, "vol_mult_min") == 0)
      cfg.vol_mult_min = FPN_FromDouble<F>(atof(val));
    else if (strcmp(key, "vol_mult_max") == 0)
      cfg.vol_mult_max = FPN_FromDouble<F>(atof(val));
    else if (strcmp(key, "filter_scale") == 0)
      cfg.filter_scale = FPN_FromDouble<F>(atof(val));
    else if (strcmp(key, "max_drawdown_pct") == 0)
      cfg.max_drawdown_pct = FPN_FromDouble<F>(atof(val) / 100.0);
    else if (strcmp(key, "max_exposure_pct") == 0)
      cfg.max_exposure_pct = FPN_FromDouble<F>(atof(val) / 100.0);
    else if (strcmp(key, "offset_stddev_mult") == 0) {
      double v = atof(val); if (v < 0) v = 0;
      cfg.offset_stddev_mult = FPN_FromDouble<F>(v);
    }
    else if (strcmp(key, "offset_stddev_min") == 0) {
      double v = atof(val); if (v < 0) v = 0;
      cfg.offset_stddev_min = FPN_FromDouble<F>(v);
    }
    else if (strcmp(key, "offset_stddev_max") == 0) {
      double v = atof(val); if (v < 0) v = 0;
      cfg.offset_stddev_max = FPN_FromDouble<F>(v);
    }
    else if (strcmp(key, "min_long_slope") == 0)
      cfg.min_long_slope = FPN_FromDouble<F>(atof(val));
  }

  fclose(f);
  return cfg;
}
//======================================================================================================
//======================================================================================================
#endif // CONTROLLER_CONFIG_HPP
