// CTest for the DBN wire-mirror record (dbn.hpp). Same conventions as the other
// C++ tests: no framework, no assert() (compiled out under NDEBUG), each failure
// returns a distinct exit code so a red CTest names the broken check.
//
// The layout guarantees (sizeof == 56, every offsetof == N, little-endian host,
// trivial copyability) are static_asserts inside dbn.hpp itself — *including this
// header compiles them*, so this TU is what makes those checks run in CI.
#include <cstdint>
#include <cstring>

#include "trading_sim/dbn.hpp"

namespace ts = trading_sim;

int main() {
    // Build a 56-byte record as raw little-endian bytes, exactly as it would sit
    // in a DBN stream, then memcpy it into a WireMbo and confirm each multi-byte
    // field reassembles correctly. This is the property the parser relies on:
    // bytes in == typed fields out, with no swapping on a little-endian host.
    unsigned char buf[sizeof(ts::WireMbo)] = {};

    buf[0] = 14;    // length: 56 / 4
    buf[1] = 0xA0;  // rtype (MBO); not validated here, just carried through

    // price = 500'000'000'000 (== $500.00 in nanodollars) at offset 24, LE.
    const std::int64_t price = 500'000'000'000LL;
    std::memcpy(buf + 24, &price, sizeof price);

    // action 'A' at offset 38, side 'B' at offset 39.
    buf[38] = 'A';
    buf[39] = 'B';

    ts::WireMbo rec{};
    std::memcpy(&rec, buf, sizeof rec);

    if (rec.length != 14)    return 1;
    if (rec.rtype != 0xA0)   return 2;
    if (rec.price != price)  return 3;
    if (rec.action != 'A')   return 4;
    if (rec.side != 'B')     return 5;

    // --- to_event: a fully-populated record converts and maps every field ----
    ts::WireMbo good{};
    good.ts_event = 1'748'955'600'000'000'000ULL;
    good.ts_recv  = 1'748'955'600'000'000'123ULL;
    good.order_id = 123'456'789ULL;
    good.price    = price;
    good.size     = 100;
    good.sequence = 42;
    good.flags    = 0x80;
    good.action   = 'A';
    good.side     = 'B';
    // Fields the converter must NOT carry into Event (validated/dropped elsewhere):
    good.rtype = 0xA0; good.publisher_id = 2; good.instrument_id = 7;
    good.channel_id = 1; good.ts_in_delta = 50;

    const auto ev = ts::to_event(good);
    if (!ev)                                     return 6;   // should have succeeded
    if (ev->ts_event != good.ts_event)           return 7;
    if (ev->ts_recv  != good.ts_recv)            return 8;
    if (ev->order_id != good.order_id)           return 9;
    if (ev->price    != good.price)              return 10;
    if (ev->size     != good.size)               return 11;
    if (ev->sequence != good.sequence)           return 12;
    if (ev->flags    != good.flags)              return 13;
    if (ev->action   != ts::Action::Add)         return 14;
    if (ev->side     != ts::Side::Bid)           return 15;

    // --- failure paths: bad action / bad side return the typed reason --------
    ts::WireMbo bad_action = good;
    bad_action.action = 'Z';  // not in the wire alphabet
    const auto r1 = ts::to_event(bad_action);
    if (r1)                                       return 16;  // must fail
    if (r1.error() != ts::ParseError::BadAction)  return 17;

    ts::WireMbo bad_side = good;
    bad_side.side = 'X';
    const auto r2 = ts::to_event(bad_side);
    if (r2)                                       return 18;
    if (r2.error() != ts::ParseError::BadSide)    return 19;

    return 0;
}
