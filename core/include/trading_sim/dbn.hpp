#pragma once

#include <bit>       // std::endian — little-endian hygiene guard
#include <cstddef>   // offsetof — wire-offset compile-time proofs
#include <cstdint>
#include <expected>  // std::expected — value-or-typed-error return of to_event
#include <type_traits>

#include "trading_sim/event.hpp"  // Event + is_valid_action/is_valid_side

// DBN (Databento Binary Encoding) wire layer.
//
// DESIGN RULE (same as event.hpp): pure C++, no Python. This header defines the
// EXTERNAL on-wire record exactly as Databento writes it. It is deliberately a
// separate type from Event (the internal representation): the parser reads bytes
// into a WireMbo, validates them, and CONVERTS into an Event. Keeping the wire
// mirror and the engine type distinct means a wire-format change touches only
// this file and the conversion — never the book, never replay.
namespace trading_sim {

// ---------------------------------------------------------------------------
// Byte-order contract.
//
// The parser fast path memcpy's raw bytes straight into the multi-byte integer
// fields below. That is only correct if this machine's byte order matches the
// wire's. DBN is little-endian; x86-64 and ARM64 (our targets) are little-endian
// too, so the bytes line up with no swapping. This assert turns that hidden
// assumption into a LOUD compile-time failure: build on a big-endian machine and
// you get an error here, instead of a binary that silently scrambles every
// timestamp and price with no crash to warn you.
// ---------------------------------------------------------------------------
static_assert(std::endian::native == std::endian::little,
              "DBN is little-endian; this code assumes a little-endian host");

// ---------------------------------------------------------------------------
// WireMbo: the 56-byte market-by-order record, byte-for-byte as it sits in the
// DBN stream. Fields are in WIRE ORDER (not largest-first like Event) because
// this struct's job is to MATCH the wire, not to be cache-optimal.
//
// Natural layout, NOT #pragma pack(1): the DBN record was designed so every
// field already lands on its natural alignment boundary (see the offsetof proofs
// below — none of them is misaligned), so there is no padding for pack to
// remove. Packing would buy zero bytes here while stripping the alignment
// guarantee and inviting misaligned-pointer UB. Instead we keep natural
// alignment and PROVE the wire match with static_assert(offsetof == N): verified
// at compile time, zero runtime cost.
//
// length is the record length in units of 4 bytes (56/4 == 14). The parser reads
// it to advance to the true next-record boundary rather than hardcoding 56, so a
// record of a different type/size cannot desync the rest of the stream.
// ---------------------------------------------------------------------------
struct WireMbo {
    std::uint8_t  length;         // record length / 4  (== 14 for a 56-byte MBO)
    std::uint8_t  rtype;          // record type tag (validated, not stored in Event)
    std::uint16_t publisher_id;   // dataset/venue id (validated, not stored)
    std::uint32_t instrument_id;  // exchange symbol id (validated, not stored)
    std::uint64_t ts_event;       // exchange/matching-engine ts, ns since epoch
    std::uint64_t order_id;       // exchange order id
    std::int64_t  price;          // nanodollars (1e-9 USD)
    std::uint32_t size;           // shares
    std::uint8_t  flags;          // wire flag bits
    std::uint8_t  channel_id;     // feed channel (validated, not stored)
    char          action;         // 'A' 'C' 'M' 'T' 'F' 'R' — validated at convert
    char          side;           // 'B' 'A' 'N' — validated at convert
    std::uint64_t ts_recv;        // consumer receive ts, ns since epoch
    std::int32_t  ts_in_delta;    // matching-to-send delta, ns (validated, not stored)
    std::uint32_t sequence;       // per-feed sequence number
};

// Pin the wire match at compile time. If any of these fail, the struct no longer
// mirrors the DBN record and the memcpy fast path would read garbage — the build
// stops here instead of letting it reach real data.
static_assert(sizeof(WireMbo) == 56, "DBN MBO record is exactly 56 bytes");
static_assert(alignof(WireMbo) == 8);
static_assert(std::is_trivially_copyable_v<WireMbo>,
              "memcpy in/out of the byte buffer requires trivial copyability");

// Every field sits at its documented wire offset (and, since none is misaligned,
// proves the natural layout needs no padding to match the wire).
static_assert(offsetof(WireMbo, length)        ==  0);
static_assert(offsetof(WireMbo, rtype)         ==  1);
static_assert(offsetof(WireMbo, publisher_id)  ==  2);
static_assert(offsetof(WireMbo, instrument_id) ==  4);
static_assert(offsetof(WireMbo, ts_event)      ==  8);
static_assert(offsetof(WireMbo, order_id)      == 16);
static_assert(offsetof(WireMbo, price)         == 24);
static_assert(offsetof(WireMbo, size)          == 32);
static_assert(offsetof(WireMbo, flags)         == 36);
static_assert(offsetof(WireMbo, channel_id)    == 37);
static_assert(offsetof(WireMbo, action)        == 38);
static_assert(offsetof(WireMbo, side)          == 39);
static_assert(offsetof(WireMbo, ts_recv)       == 40);
static_assert(offsetof(WireMbo, ts_in_delta)   == 48);
static_assert(offsetof(WireMbo, sequence)      == 52);

// ---------------------------------------------------------------------------
// Parse failures the converter can report. Only the fields to_event() actually
// CONSUMES can fail here: a bad action or side byte would be cast into a bogus
// enum and corrupt the book. Wire-record ROUTING (is this even an MBO record?
// rtype/length) is the streaming loop's job — it sees the raw framing and skips
// non-MBO records before they ever reach to_event(); see dbn.hpp's WireMbo notes.
// ---------------------------------------------------------------------------
enum class ParseError {
    BadAction,  // action byte not in {'A','C','M','T','F','R'}
    BadSide,    // side byte not in {'B','A','N'}
};

// Validate-and-convert: the single boundary where untrusted wire bytes become a
// trusted Event. By construction the only way to get an Event from the wire is
// through here, so the book can never see an unvalidated action/side. On success
// returns the Event; on a bad byte returns the typed reason (no throw — malformed
// records are routine on this path, so failure is a return value, not an
// exception). The field copies are plain assignments; action/side are zero-cost
// static_casts because the enumerator values ARE the wire bytes (see event.hpp).
[[nodiscard]] inline std::expected<Event, ParseError>
to_event(const WireMbo& w) noexcept {
    if (!is_valid_action(w.action)) return std::unexpected(ParseError::BadAction);
    if (!is_valid_side(w.side))     return std::unexpected(ParseError::BadSide);

    Event e{};
    e.ts_event = w.ts_event;
    e.ts_recv  = w.ts_recv;
    e.order_id = w.order_id;
    e.price    = w.price;
    e.size     = w.size;
    e.sequence = w.sequence;
    e.action   = static_cast<Action>(w.action);
    e.side     = static_cast<Side>(w.side);
    e.flags    = w.flags;
    return e;
}

}  // namespace trading_sim
