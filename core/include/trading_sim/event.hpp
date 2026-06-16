#pragma once

#include <cstdint>
#include <type_traits>

// Typed in-memory representation of one market-by-order (MBO) event.
//
// DESIGN RULE (same as version.hpp): pure C++, no Python. This header defines
// the *internal* event the engine consumes — it is NOT a mirror of the DBN wire
// record. The parser (separate unit) reads the wire layout, validates it, and
// converts into this struct. Wire concerns (rtype, publisher_id, instrument_id,
// channel_id, ts_in_delta) are validated at parse time and deliberately not
// stored: every byte here is paid for in cache bandwidth on every replayed
// event, so dead fields would slow the book for nothing.
namespace trading_sim {

// ---------------------------------------------------------------------------
// Action / Side: char-valued enums.
//
// The enumerator values ARE the DBN wire bytes ('A' == 0x41, ...), so wire ->
// enum conversion is a static_cast: a compile-time relabeling that emits zero
// instructions. We still get scoped-enum type safety and switch-exhaustiveness
// warnings. The cast does NOT validate, so the parser must reject any byte
// outside the valid set (is_valid_* below) before trusting the value.
// ---------------------------------------------------------------------------

enum class Action : char {
    Add    = 'A',  // new order enters the book
    Cancel = 'C',  // order fully cancelled (or remaining size cancelled)
    Modify = 'M',  // order changed (price/size); may lose queue priority
    Trade  = 'T',  // trade occurred (aggressor side reported; no book change)
    Fill   = 'F',  // resting order filled (book-affecting execution)
    Clear  = 'R',  // wipe the entire book (session reset / failover)
};

enum class Side : char {
    Bid  = 'B',
    Ask  = 'A',
    None = 'N',  // side not applicable (e.g. some Trade / Clear events)
};

// Boundary validation: the single choke point where raw wire bytes are checked
// before being trusted as enums. constexpr so the compiler can fold checks on
// known values and the tests can run at compile time too.
[[nodiscard]] constexpr bool is_valid_action(char c) noexcept {
    switch (c) {
        case 'A': case 'C': case 'M': case 'T': case 'F': case 'R': return true;
        default: return false;
    }
}

[[nodiscard]] constexpr bool is_valid_side(char c) noexcept {
    switch (c) {
        case 'B': case 'A': case 'N': return true;
        default: return false;
    }
}

// ---------------------------------------------------------------------------
// Event: 48 bytes, fields ordered largest-first so the compiler inserts no
// padding between fields (43 bytes used; 5 tail-padding to 8-byte alignment).
//
// price is INTEGER NANODOLLARS (1e-9 USD), exactly as on the wire — lossless.
// Tick quantization happens at the book boundary, where tick size is defined
// and where alignment is assertable: resting-order prices (A/C/M) must divide
// exactly by the tick; trade prints (T/F) may be sub-penny (e.g. midpoint
// fills) and never index the book's price array, so dividing here would
// silently destroy information that can never be recovered downstream.
// ---------------------------------------------------------------------------
struct Event {
    std::uint64_t ts_event;  // exchange/matching-engine timestamp, ns since epoch — book ordering
    std::uint64_t ts_recv;   // consumer receive timestamp, ns — latency realism later
    std::uint64_t order_id;  // exchange order id — the book's key
    std::int64_t  price;     // nanodollars (1e-9 USD); see block comment above
    std::uint32_t size;      // shares
    std::uint32_t sequence;  // per-feed sequence number — gap detection in replay
    Action        action;
    Side          side;
    std::uint8_t  flags;     // wire flag bits (e.g. end-of-event-group marker)
};

// Pin the layout at compile time: if a field is added/reordered and the size
// changes, the build fails here instead of silently bloating replay bandwidth.
static_assert(sizeof(Event) == 48, "Event grew past one-and-a-half cache lines");
static_assert(alignof(Event) == 8);
// Trivially copyable => safe to memcpy in/out of raw buffers (parser fast path).
static_assert(std::is_trivially_copyable_v<Event>);

}  // namespace trading_sim
