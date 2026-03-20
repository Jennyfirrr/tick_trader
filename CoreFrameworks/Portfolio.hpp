//======================================================================================================
// [PORTFOLIO MANAGER]
//======================================================================================================
// this is basically just gonna track positions and stuff and be the core portfolio managment system, im not sure if ill actually add the rebaalncing logic and stuff here, but it should eventually just serve as the API call to get position deltas and stuff, it will be more robust that just the simple pool allocator i was attempting earlier
//======================================================================================================
// [INCLUDE]
//======================================================================================================
#ifndef PORTFOLIO_HPP
#define PORTFOLIO_HPP

#include "../FixedPoint/FixedPointN.hpp"
//======================================================================================================
// [STRUCTS]
//======================================================================================================
// Im not sure how many positions i really want to track here but for now im just gonna leave it at like 16 i think, there will probably be more advanced logic added later to have a model that watches performace and dynamically updates or something like i attempted to do in FoxML core, but this is a deepr dive so i can actually learn and understand the logic behind stuff, and i just think its cool as shit, like why learn java when stuff lke this exists lmao, also i get to make my own library so im not functioning off blackbox implementations where the end of the documentation is lke "Trust me bro", and i hate reading documentation, so id rather build my own
//======================================================================================================
template <unsigned F> struct Position {
    FPN<F> quantity;          // positive for long, negative for short
    FPN<F> entry_price;
    FPN<F> take_profit_price; // computed at fill: entry_price * (1 + tp_pct)
    FPN<F> stop_loss_price;   // computed at fill: entry_price * (1 - sl_pct)
};
static_assert(sizeof(Position<64>) == 4 * sizeof(FPN<64>), "Position size mismatch");
//======================================================================================================
// [PORTFOLIO]
//======================================================================================================
// bitmap-based like OrderPool - same __builtin_ctz pattern, no array shifting on removal,
// hot-path exit gate only walks set bits so cleared positions are skipped automatically
//======================================================================================================
template <unsigned F> struct Portfolio {
    Position<F> positions[16];
    uint16_t active_bitmap; // one bit per slot, same pattern as OrderPool
};
// sizeof includes padding after uint16_t to align the struct - thats fine, the bitmap is what matters
//======================================================================================================
// [EXIT STRUCTS]
//======================================================================================================
// ExitRecord stores position index for O(1) lookup - the position data stays in the slot
// after the bit is cleared, so the controller can read entry_price/quantity directly
//======================================================================================================
template <unsigned F> struct ExitRecord {
    uint32_t position_index;
    FPN<F> exit_price;
    uint64_t tick;
    int reason; // 0 = take profit, 1 = stop loss
};

template <unsigned F> struct ExitBuffer {
    ExitRecord<F> records[16];
    uint32_t count;
};

template <unsigned F> inline void ExitBuffer_Init(ExitBuffer<F> *buf) {
    buf->count = 0;
}

template <unsigned F> inline void ExitBuffer_Clear(ExitBuffer<F> *buf) {
    buf->count = 0;
}
//======================================================================================================
// [FUNCTIONS]
//======================================================================================================
// similar to the pool allocator, will need more work and im not sure if i want the rebalancing adn stuff here or in another header, probably another header for the actual managment, because these are just the basic functions to add and manipulate the actual opsitions, tnd are dependent on the buy/sell gates
//======================================================================================================
template <unsigned F> inline void Portfolio_Init(Portfolio<F> *portfolio) {
    for (int i = 0; i < 16; i++) {
        portfolio->positions[i].quantity          = FPN_Zero<F>();
        portfolio->positions[i].entry_price       = FPN_Zero<F>();
        portfolio->positions[i].take_profit_price = FPN_Zero<F>();
        portfolio->positions[i].stop_loss_price   = FPN_Zero<F>();
    }
    portfolio->active_bitmap = 0;
}
//======================================================================================================
template <unsigned F> inline int Portfolio_IsFull(const Portfolio<F> *portfolio) {
    return portfolio->active_bitmap == 0xFFFF;
}
//======================================================================================================
template <unsigned F> inline int Portfolio_CountActive(const Portfolio<F> *portfolio) {
    return __builtin_popcount(portfolio->active_bitmap);
}
//======================================================================================================
// find position by entry price - walks active bits, returns index or -1
//======================================================================================================
template <unsigned F> inline int Portfolio_FindByPrice(const Portfolio<F> *portfolio, FPN<F> entry_price) {
    uint16_t active = portfolio->active_bitmap;
    while (active) {
        int idx = __builtin_ctz(active);
        if (FPN_Equal(portfolio->positions[idx].entry_price, entry_price)) {
            return idx;
        }
        active &= active - 1;
    }
    return -1;
}
//======================================================================================================
// add quantity to existing position at index (consolidation)
//======================================================================================================
template <unsigned F> inline void Portfolio_AddQuantity(Portfolio<F> *portfolio, int index, FPN<F> quantity) {
    portfolio->positions[index].quantity = FPN_AddSat(portfolio->positions[index].quantity, quantity);
}
//======================================================================================================
// add new position with pre-computed exit prices, returns slot index or -1 if full
//======================================================================================================
template <unsigned F>
inline int Portfolio_AddPositionWithExits(Portfolio<F> *portfolio, FPN<F> quantity, FPN<F> entry_price,
                                          FPN<F> take_profit_price, FPN<F> stop_loss_price) {
    if (portfolio->active_bitmap == 0xFFFF) return -1;
    int idx                                       = __builtin_ctz(~portfolio->active_bitmap);
    portfolio->positions[idx].quantity             = quantity;
    portfolio->positions[idx].entry_price          = entry_price;
    portfolio->positions[idx].take_profit_price    = take_profit_price;
    portfolio->positions[idx].stop_loss_price      = stop_loss_price;
    portfolio->active_bitmap |= (1 << idx);
    return idx;
}
//======================================================================================================
// legacy add - for backward compatibility with existing tests, no exit prices
//======================================================================================================
template <unsigned F> inline void Portfolio_AddPosition(Portfolio<F> *portfolio, FPN<F> quantity, FPN<F> entry_price) {
    if (portfolio->active_bitmap == 0xFFFF) return;
    int idx                                       = __builtin_ctz(~portfolio->active_bitmap);
    portfolio->positions[idx].quantity             = quantity;
    portfolio->positions[idx].entry_price          = entry_price;
    portfolio->positions[idx].take_profit_price    = FPN_Zero<F>();
    portfolio->positions[idx].stop_loss_price      = FPN_Zero<F>();
    portfolio->active_bitmap |= (1 << idx);
}
//======================================================================================================
// remove - just clears the bit, data stays in slot for controller to read
//======================================================================================================
template <unsigned F> inline void Portfolio_RemovePosition(Portfolio<F> *portfolio, int index) {
    portfolio->active_bitmap &= ~(1 << index);
}
//======================================================================================================
template <unsigned F> inline void Portfolio_ClearPositions(Portfolio<F> *portfolio) {
    for (int i = 0; i < 16; i++) {
        portfolio->positions[i].quantity          = FPN_Zero<F>();
        portfolio->positions[i].entry_price       = FPN_Zero<F>();
        portfolio->positions[i].take_profit_price = FPN_Zero<F>();
        portfolio->positions[i].stop_loss_price   = FPN_Zero<F>();
    }
    portfolio->active_bitmap = 0;
}
//======================================================================================================
template <unsigned F>
inline void Portfolio_UpdatePosition(Portfolio<F> *portfolio, int index, FPN<F> new_quantity, FPN<F> new_entry_price) {
    portfolio->positions[index].quantity    = new_quantity;
    portfolio->positions[index].entry_price = new_entry_price;
}
//======================================================================================================
// [P&L FUNCTIONS]
//======================================================================================================
// unrealized P&L: for each active position, (current_price - entry_price) * quantity
// this is the signal the controller feeds to regression - measures whether current
// gate conditions are producing positions that are making money
//======================================================================================================
template <unsigned F> inline FPN<F> Portfolio_ComputePnL(const Portfolio<F> *portfolio, FPN<F> current_price) {
    FPN<F> total = FPN_Zero<F>();
    uint16_t active  = portfolio->active_bitmap;
    while (active) {
        int idx        = __builtin_ctz(active);
        FPN<F> diff = FPN_Sub(current_price, portfolio->positions[idx].entry_price);
        FPN<F> pnl  = FPN_Mul(diff, portfolio->positions[idx].quantity);
        total           = FPN_AddSat(total, pnl);
        active &= active - 1;
    }
    return total;
}
//======================================================================================================
// total portfolio value: sum of current_price * quantity across active positions
//======================================================================================================
template <unsigned F> inline FPN<F> Portfolio_ComputeValue(const Portfolio<F> *portfolio, FPN<F> current_price) {
    FPN<F> total = FPN_Zero<F>();
    uint16_t active  = portfolio->active_bitmap;
    while (active) {
        int idx        = __builtin_ctz(active);
        FPN<F> val = FPN_Mul(current_price, portfolio->positions[idx].quantity);
        total          = FPN_AddSat(total, val);
        active &= active - 1;
    }
    return total;
}
//======================================================================================================
// [POSITION EXIT GATE - HOT PATH]
//======================================================================================================
// runs every tick - walks active bitmap, checks each position's TP/SL against current price
// branchless comparisons, writes to exit buffer using count += should_exit pattern
// clears position bit immediately on exit so next tick's gate skips it
//======================================================================================================
template <unsigned F>
inline void PositionExitGate(Portfolio<F> *portfolio, FPN<F> current_price, ExitBuffer<F> *exit_buf, uint64_t tick) {
    uint16_t active = portfolio->active_bitmap;
    while (active) {
        int idx = __builtin_ctz(active);

        int hit_tp = FPN_GreaterThanOrEqual(current_price, portfolio->positions[idx].take_profit_price);
        int hit_sl = FPN_LessThanOrEqual(current_price, portfolio->positions[idx].stop_loss_price);

        // skip positions with no exit prices set (legacy adds, zero TP/SL)
        int has_exits = !FPN_IsZero(portfolio->positions[idx].take_profit_price);
        int should_exit = (hit_tp | hit_sl) & has_exits;

        // always write to current buffer slot (branchless), increment count by 0 or 1
        exit_buf->records[exit_buf->count].position_index = idx;
        exit_buf->records[exit_buf->count].exit_price     = current_price;
        exit_buf->records[exit_buf->count].tick            = tick;
        exit_buf->records[exit_buf->count].reason          = hit_sl & (!hit_tp); // 0=TP, 1=SL (TP takes priority)
        exit_buf->count += should_exit;

        // clear portfolio bit if exiting
        uint16_t clear_mask = (uint16_t)(-(int16_t)should_exit) & (1 << idx);
        portfolio->active_bitmap &= ~clear_mask;

        active &= active - 1;
    }
}
//======================================================================================================
//======================================================================================================
//======================================================================================================
#endif
