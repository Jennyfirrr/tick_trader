//======================================================================================================
// [TRADE LOG]
//======================================================================================================
// simple CSV append logger for recording buy/sell activity with gate conditions at time of fill
// filename is {symbol}_order_history.csv so the symbol is implicit, no symbol column needed
// fflush after every write so nothing is lost if the program crashes
//======================================================================================================
#ifndef TRADE_LOG_HPP
#define TRADE_LOG_HPP

#include <stdio.h>
#include <stdint.h>

//======================================================================================================
// [STRUCT]
//======================================================================================================
struct TradeLog {
    FILE *file;
    uint64_t trade_count;
};
//======================================================================================================
// [FUNCTIONS]
//======================================================================================================
static inline int TradeLog_Init(TradeLog *log, const char *symbol) {
    char filename[128];
    int pos = 0;
    while (*symbol && pos < 119) {
        filename[pos++] = *symbol++;
    }
    const char *suffix = "_order_history.csv";
    while (*suffix && pos < 127) {
        filename[pos++] = *suffix++;
    }
    filename[pos] = '\0';

    // check if file already has content (so we don't double-write the header)
    log->file = fopen(filename, "r");
    int has_content = 0;
    if (log->file) {
        has_content = (fgetc(log->file) != EOF);
        fclose(log->file);
    }

    log->file = fopen(filename, "a");
    if (!log->file) return 0;

    log->trade_count = 0;

    if (!has_content) {
        fprintf(log->file, "tick,side,price,quantity,entry_price,delta_pct,exit_reason,buy_price_cond,buy_vol_cond,take_profit,stop_loss\n");
        fflush(log->file);
    }

    return 1;
}
//======================================================================================================
static inline void TradeLog_Buy(TradeLog *log, uint64_t tick, double price, double quantity, double take_profit,
                                double stop_loss, double buy_price_cond, double buy_vol_cond) {
    if (!log->file) return;
    fprintf(log->file, "%lu,BUY,%.4f,%.4f,,,,,%.4f,%.4f,%.4f,%.4f\n", tick, price, quantity, buy_price_cond, buy_vol_cond,
            take_profit, stop_loss);
    fflush(log->file);
    log->trade_count++;
}
//======================================================================================================
static inline void TradeLog_Sell(TradeLog *log, uint64_t tick, double price, double quantity, double entry_price,
                                 double delta_pct, const char *reason) {
    if (!log->file) return;
    fprintf(log->file, "%lu,SELL,%.4f,%.4f,%.4f,%.4f,%s,,,,\n", tick, price, quantity, entry_price, delta_pct, reason);
    fflush(log->file);
    log->trade_count++;
}
//======================================================================================================
static inline void TradeLog_Close(TradeLog *log) {
    if (log->file) {
        fclose(log->file);
        log->file = 0;
    }
}
//======================================================================================================
//======================================================================================================
#endif // TRADE_LOG_HPP
