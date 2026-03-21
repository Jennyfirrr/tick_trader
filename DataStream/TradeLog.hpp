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
// [BUFFERED TRADE LOG]
//======================================================================================================
// hot-path version: accumulates trade records in a ring buffer (~10ns per push).
// slow-path drains the buffer to the CSV file (fprintf happens off the hot path).
// NOTE: if engine crashes between fill and next slow-path drain, CSV entry is lost
// (but position is preserved in binary snapshot).
//======================================================================================================
#define TRADE_LOG_BUF_SIZE 64

struct TradeLogRecord {
    uint64_t tick;
    double price, quantity, entry_price, delta_pct;
    double tp, sl, buy_cond_p, buy_cond_v;
    int is_buy;          // 1=BUY, 0=SELL
    char reason[16];     // "TP", "SL", "TIME", "SESSION_CLOSE"
};

struct TradeLogBuffer {
    TradeLogRecord records[TRADE_LOG_BUF_SIZE];
    uint32_t head;
    uint32_t count;
};

static inline void TradeLogBuffer_Init(TradeLogBuffer *buf) {
    buf->head = 0;
    buf->count = 0;
}

// hot path: push a record to the ring buffer (~10ns, no file I/O)
static inline void TradeLogBuffer_PushBuy(TradeLogBuffer *buf, uint64_t tick,
                                           double price, double qty,
                                           double tp, double sl,
                                           double bc_p, double bc_v) {
    if (buf->count >= TRADE_LOG_BUF_SIZE) return; // overflow safety
    TradeLogRecord *r = &buf->records[buf->head];
    r->tick = tick;
    r->price = price;
    r->quantity = qty;
    r->tp = tp;
    r->sl = sl;
    r->buy_cond_p = bc_p;
    r->buy_cond_v = bc_v;
    r->is_buy = 1;
    r->entry_price = 0;
    r->delta_pct = 0;
    r->reason[0] = '\0';
    buf->head = (buf->head + 1) & (TRADE_LOG_BUF_SIZE - 1);
    buf->count++;
}

static inline void TradeLogBuffer_PushSell(TradeLogBuffer *buf, uint64_t tick,
                                            double price, double qty,
                                            double entry_price, double delta_pct,
                                            const char *reason) {
    if (buf->count >= TRADE_LOG_BUF_SIZE) return;
    TradeLogRecord *r = &buf->records[buf->head];
    r->tick = tick;
    r->price = price;
    r->quantity = qty;
    r->entry_price = entry_price;
    r->delta_pct = delta_pct;
    r->is_buy = 0;
    r->tp = 0; r->sl = 0; r->buy_cond_p = 0; r->buy_cond_v = 0;
    // copy reason string
    int i = 0;
    while (reason[i] && i < 15) { r->reason[i] = reason[i]; i++; }
    r->reason[i] = '\0';
    buf->head = (buf->head + 1) & (TRADE_LOG_BUF_SIZE - 1);
    buf->count++;
}

// slow path: drain all buffered records to the CSV file
static inline void TradeLogBuffer_Drain(TradeLogBuffer *buf, TradeLog *log) {
    if (buf->count == 0 || !log->file) return;
    uint32_t start = (buf->head - buf->count + TRADE_LOG_BUF_SIZE) & (TRADE_LOG_BUF_SIZE - 1);
    for (uint32_t i = 0; i < buf->count; i++) {
        uint32_t idx = (start + i) & (TRADE_LOG_BUF_SIZE - 1);
        TradeLogRecord *r = &buf->records[idx];
        if (r->is_buy) {
            TradeLog_Buy(log, r->tick, r->price, r->quantity, r->tp, r->sl, r->buy_cond_p, r->buy_cond_v);
        } else {
            TradeLog_Sell(log, r->tick, r->price, r->quantity, r->entry_price, r->delta_pct, r->reason);
        }
    }
    buf->count = 0;
}

//======================================================================================================
//======================================================================================================
#endif // TRADE_LOG_HPP
