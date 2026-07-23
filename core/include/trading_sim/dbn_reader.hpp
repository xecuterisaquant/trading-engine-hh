#pragma once

#include <algorithm>   // std::min — chunked discard of the metadata prologue
#include <array>       // std::array — the fixed 8-byte DBN header
#include <cstddef>  // std::byte, std::size_t, std::to_integer
#include <cstdint>
#include <cstring>     // std::memcpy — raw record bytes into a WireMbo
#include <filesystem>  // std::filesystem::path — the byte source opens a file
#include <fstream>     // std::ifstream — the file byte pump
#include <optional>    // std::optional<Event> — the pull-iterator return
#include <span>
#include <stdexcept>   // std::runtime_error — thrown once on a failed open
#include <string>      // std::string — path.string() for the error message
#include <vector>

#include "trading_sim/dbn.hpp"  // WireMbo framing convention (length == bytes/4)
#include "trading_sim/memory_mapped_file.hpp"  // whole-file mapping for MmapDbnReader

// DBN streaming reader — the layer that turns raw bytes off disk (or, later, off a
// socket) into a sequence of validated Events. It is built bottom-up:
//
//   frame_one   <-- THIS UNIT: pure per-record framing decision, no I/O, no state
//   (next) carry-tail buffer that stitches records across chunk boundaries
//   (next) a byte source (plain .dbn file first; zstd/socket later)
//   (next) a pull iterator: reader.next() -> std::optional<Event>
//
// Splitting framing out from I/O is deliberate: the hard, bug-prone logic (the
// straddle) lives in one small pure function that unit-tests with hand-built byte
// arrays and has zero dependencies on files, compression, or the network.
namespace trading_sim {

// The outcome of trying to frame ONE record at the FRONT of a byte buffer.
enum class FrameStatus {
    Ok,        // a whole record is present; rtype + record_size are set
    NeedMore,  // fewer bytes than the record needs — carry the tail, wait for more
    Corrupt,   // impossible framing (length == 0); more bytes cannot fix it
};

struct FrameResult {
    FrameStatus  status;
    std::uint8_t rtype;        // meaningful ONLY when status == Ok
    std::size_t  record_size;  // bytes the record occupies; meaningful ONLY when Ok
};

// Look at the front of `buf` and decide whether a complete DBN record sits there.
//
// Reads only the 2-byte framing prefix — length @ offset 0, rtype @ offset 1 (see
// dbn.hpp) — then copies nothing and advances no cursor: the CALLER owns buffer
// management and routing. `length` is the record size in 4-byte words, so the record
// is length*4 bytes. Because length is a u8 (max 255), record_size is bounded to
// [0, 1020] and length*4 cannot overflow.
//
// Three checks, on mutually-exclusive conditions (so their ORDER is immaterial —
// what matters is that each one EXISTS):
//   1. < 2 bytes            -> NeedMore  (can't even read length+rtype yet)
//   2. record_size == 0     -> Corrupt   (the zero-size guard. A u8 length of 0 is a
//                                          0-byte record; WITHOUT this check it falls
//                                          through to Ok and the caller's advance-by-
//                                          size loop never moves. Note the size check
//                                          below cannot catch it: buf.size() is
//                                          unsigned, so `buf.size() < 0` is never true.)
//   3. buf.size < record    -> NeedMore  (the straddle: self-describing but partial)
// Only past all three does a whole record exist.
[[nodiscard]] inline FrameResult
frame_one(std::span<const std::byte> buf) noexcept {
    if (buf.size() < 2) {
        return {FrameStatus::NeedMore, 0, 0};
    }

    const auto length = std::to_integer<std::uint8_t>(buf[0]);
    const std::size_t record_size = static_cast<std::size_t>(length) * 4;

    if (record_size == 0) {
        return {FrameStatus::Corrupt, 0, 0};
    }

    if (buf.size() < record_size) {
        return {FrameStatus::NeedMore, 0, 0};
    }

    const auto rtype = std::to_integer<std::uint8_t>(buf[1]);
    return {FrameStatus::Ok, rtype, record_size};
}

// The outcome of pulling a record from the assembler. Mirrors FrameResult one
// layer up; three states for the same reason frame_one has three — collapsing
// NeedMore and Corrupt into "nothing" (a std::optional) would lose the very
// distinction the framer works to preserve.
enum class PullStatus { Record, NeedMore, Corrupt };

struct Pull {
    PullStatus                 status;
    std::span<const std::byte> record;  // valid ONLY when status == Record
};

// RecordAssembler: the memory frame_one lacks. It holds bytes across chunk
// boundaries so a record split by a read (the straddle) is stitched back together
// before it is handed out.
//
// It is deliberately ignorant of where bytes come from — a file, a zstd stream, a
// live socket all look identical to it. That ignorance is the point: the same
// assembler serves every source. When it runs dry it returns NeedMore, and the
// CALLER (which alone knows the source) feeds the next chunk. Consumer drives,
// buffer holds — the pull model.
class RecordAssembler {
public:
    // Add a chunk from the source. Reclaims already-consumed bytes first
    // (compact-on-append) so the buffer holds only unconsumed data plus the new
    // chunk — otherwise it would grow without bound. The reclaim slides the small
    // leftover tail (< one record) to the front; the dominant cost here is copying
    // the chunk itself, not that memmove.
    void append(std::span<const std::byte> chunk) {
        if (read_pos_ != 0) {
            buf_.erase(buf_.begin(),
                       buf_.begin() + static_cast<std::ptrdiff_t>(read_pos_));
            read_pos_ = 0;
        }
        buf_.insert(buf_.end(), chunk.begin(), chunk.end());
    }

    // Pull the next complete record from the front. On Record, the returned span
    // VIEWS the internal buffer in place — consume it (e.g. memcpy into WireMbo)
    // BEFORE the next append(), which may move or reallocate the buffer and leave
    // the view dangling.
    [[nodiscard]] Pull next() {
        const std::span<const std::byte> view{buf_.data() + read_pos_,
                                               buf_.size() - read_pos_};
        const FrameResult r = frame_one(view);

        // No `default:` on purpose — an added FrameStatus should fail -Wswitch
        // (ADR 0007), not be silently swallowed. The trailing return then keeps
        // MSVC's "not all paths return" (C4715) quiet.
        switch (r.status) {
            case FrameStatus::Ok: {
                const std::span<const std::byte> rec = view.first(r.record_size);
                read_pos_ += r.record_size;
                return {PullStatus::Record, rec};
            }
            case FrameStatus::NeedMore:
                return {PullStatus::NeedMore, {}};
            case FrameStatus::Corrupt:
                return {PullStatus::Corrupt, {}};
        }
        return {PullStatus::Corrupt, {}};
    }

    // Unconsumed bytes currently held (the carry-tail size). Note: this proves the
    // read cursor drains, but NOT that memory is reclaimed — reclamation is a
    // construction guarantee of append()'s erase, not observable through here.
    [[nodiscard]] std::size_t buffered() const noexcept {
        return buf_.size() - read_pos_;
    }

private:
    std::vector<std::byte> buf_;
    std::size_t            read_pos_ = 0;  // advanced by next(), reset by append()
};

// FileByteSource: a dumb byte pump over a local file. On each read() it fills the
// caller's buffer with up to dst.size() bytes and returns how many it actually got
// (0 == end of file). It knows NOTHING about DBN — no records, no framing, no
// prologue — it is pure I/O. That ignorance is deliberate: the same
// read(span)->size_t shape could later wrap a zstd stream or a socket, and the
// RecordAssembler above cannot tell the difference.
//
// This is the B1 ownership model: the CALLER owns the destination buffer. The
// source writes into your memory and never hands back a view, so there is no
// borrowed span whose lifetime you must track (contrast the assembler, which DOES
// hand out views and so carries the "consume before next append" contract).
//
// Note: this type is logically source-agnostic I/O, not DBN-specific — it lives
// here only so the whole ingest stack reads top-to-bottom in one file.
class FileByteSource {
public:
    // Open `path` for binary reading. Throws std::runtime_error if it cannot be
    // opened. A missing file is a one-time, fatal, off-hot-path failure, so it
    // throws — unlike a malformed record, which is routine on the parse path and
    // so returns a value (see to_event's std::expected). If the constructor
    // returns, the source is always in a valid, open state.
    explicit FileByteSource(const std::filesystem::path& path)
        : file_(path, std::ios::binary) {
        if (!file_) {
            throw std::runtime_error("FileByteSource: cannot open " + path.string());
        }
    }

    // Fill `dst` with up to dst.size() bytes; return the number actually read.
    // A return < dst.size() is a "short read" — EOF was reached mid-request; the
    // NEXT call then returns 0 (the eof/failbit set here makes the next read a
    // no-op with gcount() == 0). std::byte and char may legally alias any object's
    // bytes, so the reinterpret_cast to char* is well-defined, not a hack — it is
    // the standard bridge from a byte buffer to istream::read, which speaks char*.
    // istream::read does not itself report a count; gcount() does, so we return it.
    [[nodiscard]] std::size_t read(std::span<std::byte> dst) {
        file_.read(reinterpret_cast<char*>(dst.data()),
                   static_cast<std::streamsize>(dst.size()));
        return static_cast<std::size_t>(file_.gcount());
    }

private:
    std::ifstream file_;
};

// ---------------------------------------------------------------------------
// Shared record routing + protocol constants — used by BOTH readers (streaming
// DbnReader and MmapDbnReader), so the routing/version policy lives in ONE place
// instead of being copy-pasted per reader.
// ---------------------------------------------------------------------------

// DBN market-by-order rtype. A protocol constant at namespace scope so every
// consumer of a framed record routes by the same value.
inline constexpr std::uint8_t kMboRtype = 0xA0;

// The one DBN version whose MBO record layout we have validated byte-for-byte
// (dbn.hpp's offsetof static_asserts). Both readers REFUSE every other version
// rather than memcpy into offsets we have not verified: a mislayout emits garbage
// Events with no crash — the silent-corruption failure this project fears most.
// Correctness over reach; widen only after validating the new layout, not before.
// (Our real XNAS-ITCH MSFT slice is v1, so this rejects nothing we use.)
inline constexpr std::uint8_t kSupportedDbnVersion = 1;

// The outcome of routing ONE framed record. Emit -> hand back the Event; the two
// Skip variants record WHY a record was dropped, which is what makes ingest
// observable (see IngestStats). Collapsing them (as an optional<Event> did) throws
// away a real distinction: an expected other-record-type is normal, a malformed MBO
// is a data-quality signal you might alarm on — different meanings, different counts.
enum class RouteOutcome { Emit, SkipNonMbo, SkipMalformed };

struct Routed {
    RouteOutcome outcome;
    Event        event;  // meaningful ONLY when outcome == Emit
};

// Per-reader ingest counters. Silent skips are an observability hole; these turn
// "how many records did we drop, and why?" into a queryable number. u64 because a
// full trading day is tens of millions of records and this must never wrap.
struct IngestStats {
    std::uint64_t records           = 0;  // records framed (frame_one Ok)
    std::uint64_t events            = 0;  // Events emitted (routed Emit)
    std::uint64_t skipped_non_mbo   = 0;  // non-MBO rtype or wrong size (ADR 0003)
    std::uint64_t skipped_malformed = 0;  // MBO frame, bad action/side  (ADR 0002)
};

// Route + decode ONE already-framed record (a frame_one/assembler "Ok" span). PURE:
// it returns WHAT to do and never touches state — counting is tally()'s job, so this
// stays trivially testable and the benchmark can call it without a reader.
//
// The size guard is not paranoia: memcpy'ing sizeof(WireMbo) out of a shorter record
// would read into the FOLLOWING record's bytes and forge a garbage Event.
[[nodiscard]] inline Routed
route_record(std::span<const std::byte> record) noexcept {
    const auto rtype = std::to_integer<std::uint8_t>(record[1]);
    if (rtype != kMboRtype || record.size() != sizeof(WireMbo)) {
        return {RouteOutcome::SkipNonMbo, {}};  // non-MBO rtype or wrong size (ADR 0003)
    }
    WireMbo w;
    std::memcpy(&w, record.data(), sizeof(WireMbo));
    const std::expected<Event, ParseError> e = to_event(w);
    if (e) {
        return {RouteOutcome::Emit, *e};  // raw bytes are now a trusted Event
    }
    return {RouteOutcome::SkipMalformed, {}};  // valid frame, bad action/side (ADR 0002)
}

// Apply a routed record to a reader's stats and yield the Event to emit (or nullopt
// to skip). Shared by BOTH readers so the counting policy lives in exactly one place.
[[nodiscard]] inline std::optional<Event>
tally(IngestStats& stats, const Routed& routed) noexcept {
    ++stats.records;
    switch (routed.outcome) {
        case RouteOutcome::Emit:
            ++stats.events;
            return routed.event;
        case RouteOutcome::SkipNonMbo:
            ++stats.skipped_non_mbo;
            return std::nullopt;
        case RouteOutcome::SkipMalformed:
            ++stats.skipped_malformed;
            return std::nullopt;
    }
    return std::nullopt;  // -Wswitch: no default, so a new RouteOutcome fails to build
}

// DbnReader: the top of the ingest stack — the one object a consumer holds. It owns
// {FileByteSource, RecordAssembler, a scratch buffer} and, on each next(), drives
// them until it can hand back one validated Event (or signal end-of-stream).
//
// This is the pull-iterator: the CONSUMER drives (calls next() when it wants the
// next event); the reader pulls bytes only as needed. next() returns:
//   - Event      : a well-formed MBO record, validated through to_event.
//   - nullopt    : the stream is exhausted. Either a clean EOF (failed() == false)
//                  or it stopped on a corrupt frame (failed() == true) — check
//                  failed() to tell which.
//
// Routing/skip policy is split exactly as the ADRs prescribe:
//   - non-MBO record        -> skip and continue        (ADR 0003: routing here)
//   - MBO with bad action/side -> skip and continue     (ADR 0002: malformed==routine)
//   - corrupt frame (length 0) -> STOP, set failed()    (ADR 0018: framing lost, a
//                                 desynced stream cannot be safely resynced)
class DbnReader {
public:
    // Default read chunk: 64 KiB. Large enough that per-chunk syscall overhead is
    // amortized over many records, small enough to stay off the "slurp the whole
    // multi-hundred-MB file" path that motivated streaming in the first place.
    explicit DbnReader(const std::filesystem::path& path,
                       std::size_t chunk_size = std::size_t{64} * 1024)
        : source_(path), scratch_(chunk_size) {
        skip_prologue_();  // consume the DBN header so next() sees only records
    }

    // DBN format version from the file header. Exposed for inspection, but the
    // header value is also ENFORCED at construction: our WireMbo byte layout
    // (dbn.hpp) is the v1 MBO record, validated field-by-field with offsetof
    // static_asserts. A v2/v3 record could move fields, so decoding it THROUGH the
    // v1 layout would memcpy into the wrong offsets and silently forge garbage
    // Events — no crash, the worst failure. skip_prologue_ therefore REFUSES any
    // version we have not validated (see kSupportedDbnVersion); if this getter
    // returns, the stream is a version we can parse correctly.
    [[nodiscard]] std::uint8_t version() const noexcept { return version_; }

    [[nodiscard]] std::optional<Event> next() {
        for (;;) {
            const Pull p = assembler_.next();
            switch (p.status) {
                case PullStatus::Record: {
                    // Route + decode + tally one framed record (shared policy). A real
                    // Event returns immediately; nullopt means skip (counted by
                    // category in stats_) — loop for the next record.
                    if (const std::optional<Event> e =
                            tally(stats_, route_record(p.record))) {
                        return *e;
                    }
                    continue;
                }
                case PullStatus::NeedMore: {
                    // The assembler ran dry; refill it from the source. A 0-byte
                    // read means EOF — the stream is cleanly exhausted.
                    const std::size_t n = source_.read(std::span<std::byte>{scratch_});
                    if (n == 0) {
                        return std::nullopt;  // clean EOF; failed_ stays false
                    }
                    assembler_.append(
                        std::span<const std::byte>{scratch_.data(), n});
                    continue;
                }
                case PullStatus::Corrupt:
                    // Framing is lost; nothing after this is trustworthy. Stop and
                    // let the caller distinguish this from a clean EOF via failed().
                    failed_ = true;
                    return std::nullopt;
            }
            // No default above keeps -Wswitch honest (ADR 0007). This loop only
            // ever exits through the returns inside the switch, so there is no
            // fall-through path that needs a value returned here.
        }
    }

    // True iff next() stopped because the stream corrupted (as opposed to a clean
    // end of file). Lets the consumer treat "ran out cleanly" and "gave up on bad
    // framing" differently, instead of collapsing both into a silent nullopt.
    [[nodiscard]] bool failed() const noexcept { return failed_; }

    // Ingest counters accumulated so far: records framed, events emitted, and the two
    // skip categories. Turns silent drops into a queryable metric (observability).
    [[nodiscard]] const IngestStats& stats() const noexcept { return stats_; }

private:
    // Read until `dst` is full or the source hits EOF; return bytes obtained. Loops
    // because a single read() may come up short (Unit 3's lesson) — a header/skip
    // must not trust one read to deliver everything.
    [[nodiscard]] std::size_t read_fully_(std::span<std::byte> dst) {
        std::size_t total = 0;
        while (total < dst.size()) {
            const std::size_t n = source_.read(dst.subspan(total));
            if (n == 0) {
                break;  // EOF before dst filled
            }
            total += n;
        }
        return total;
    }

    // Consume the DBN prologue: a fixed 8-byte header (magic "DBN" + version byte +
    // u32 metadata length) followed by that many metadata bytes. We validate the
    // magic and DISCARD the metadata by reading past it — no seek, so this same
    // logic would work over a non-seekable source (a socket). After this returns,
    // the source is positioned at the first record and next() can begin framing.
    void skip_prologue_() {
        std::array<std::byte, 8> header{};
        if (read_fully_(header) != header.size()) {
            throw std::runtime_error("DbnReader: truncated DBN header");
        }
        if (header[0] != std::byte{'D'} || header[1] != std::byte{'B'} ||
            header[2] != std::byte{'N'}) {
            throw std::runtime_error("DbnReader: not a DBN stream (bad magic)");
        }
        version_ = std::to_integer<std::uint8_t>(header[3]);
        // Enforce the version BEFORE we skip the metadata or frame a single record:
        // a version we have not validated must not reach the memcpy in next(). Throw
        // (not a failed() flag) — a wrong-version file is unusable from byte one, so
        // fail fast at construction, consistent with the bad-magic throw above.
        if (version_ != kSupportedDbnVersion) {
            throw std::runtime_error(
                "DbnReader: unsupported DBN version " + std::to_string(version_) +
                " (only v" + std::to_string(kSupportedDbnVersion) +
                " record layout is validated)");
        }

        std::uint32_t metadata_len = 0;  // u32 LE; host is little-endian (dbn.hpp)
        std::memcpy(&metadata_len, header.data() + 4, sizeof(metadata_len));

        std::size_t remaining = metadata_len;
        while (remaining > 0) {
            const std::size_t want = std::min(remaining, scratch_.size());
            const std::size_t got =
                read_fully_(std::span<std::byte>{scratch_.data(), want});
            if (got != want) {
                throw std::runtime_error("DbnReader: truncated DBN metadata");
            }
            remaining -= got;
        }
    }

    FileByteSource         source_;
    RecordAssembler        assembler_;
    std::vector<std::byte> scratch_;
    std::uint8_t           version_ = 0;
    bool                   failed_  = false;
    IngestStats            stats_{};
};

// MmapDbnReader: the same ingest contract as DbnReader — same next()/version()/
// failed() surface, same routing (route_record) and version guard — but over a
// whole-file memory mapping instead of a streaming byte source.
//
// Because the entire file is one contiguous region (MemoryMappedFile), records never
// straddle a boundary. So this reader has NO RecordAssembler, NO scratch buffer, and
// NO dangling-view lifetime contract — an entire buffering layer and its class of
// use-after-free/overwrite bugs are gone. next() is just a cursor walking frame_one
// across the mapped span. (Baseline benchmark, ADR 0019: the copy this deletes was
// ~85% of ingest cost; the assembler it deletes was the other ~15%'s memmove.)
//
// End-of-stream difference from DbnReader: with the whole file already mapped, a
// frame_one NeedMore at the cursor means the trailing bytes are a partial/empty
// record and nothing more is coming — i.e. a CLEAN EOF, not "go read more" (there is
// no source to read from). Corrupt still stops and sets failed() (ADR 0018); an
// unsupported version still throws at construction (ADR 0020).
//
// Caveat (SIGBUS): the mapping is only valid for a stable, untruncated file — see
// MemoryMappedFile. For our read-only historical files that holds.
class MmapDbnReader {
public:
    explicit MmapDbnReader(const std::filesystem::path& path) : map_(path) {
        skip_prologue_();  // validate header + version, position cursor_ at record 0
    }

    // See DbnReader::version(): exposed AND enforced (unsupported versions throw in
    // skip_prologue_ before a single record is framed).
    [[nodiscard]] std::uint8_t version() const noexcept { return version_; }

    [[nodiscard]] std::optional<Event> next() {
        for (;;) {
            const std::span<const std::byte> rest = map_.bytes().subspan(cursor_);
            const FrameResult r = frame_one(rest);
            switch (r.status) {
                case FrameStatus::Ok: {
                    const std::span<const std::byte> rec = rest.first(r.record_size);
                    cursor_ += r.record_size;  // advance BEFORE routing (route may skip)
                    if (const std::optional<Event> e =
                            tally(stats_, route_record(rec))) {
                        return *e;
                    }
                    continue;  // skipped record; frame the next
                }
                case FrameStatus::NeedMore:
                    // Whole file mapped: no more bytes are coming, so a partial or
                    // empty trailing record is a clean EOF (parity with DbnReader's
                    // short-final-read -> nullopt, failed_ stays false).
                    return std::nullopt;
                case FrameStatus::Corrupt:
                    failed_ = true;
                    return std::nullopt;
            }
            // No default: an added FrameStatus must fail -Wswitch (ADR 0007).
        }
    }

    // See DbnReader::failed(): true iff next() stopped on a corrupt frame rather than
    // a clean end of file.
    [[nodiscard]] bool failed() const noexcept { return failed_; }

    // See DbnReader::stats(): same ingest counters, so both readers report identically.
    [[nodiscard]] const IngestStats& stats() const noexcept { return stats_; }

private:
    // Validate the prologue over the mapped span and set cursor_ to the first record.
    // Mirrors DbnReader::skip_prologue_ but INDEXES the span instead of reading through
    // a source: with the whole file present there is no seek and no short-read loop —
    // the metadata is skipped by advancing the cursor past it.
    void skip_prologue_() {
        const std::span<const std::byte> f = map_.bytes();
        if (f.size() < 8) {
            throw std::runtime_error("MmapDbnReader: truncated DBN header");
        }
        if (f[0] != std::byte{'D'} || f[1] != std::byte{'B'} ||
            f[2] != std::byte{'N'}) {
            throw std::runtime_error("MmapDbnReader: not a DBN stream (bad magic)");
        }
        version_ = std::to_integer<std::uint8_t>(f[3]);
        if (version_ != kSupportedDbnVersion) {  // enforce BEFORE framing any record
            throw std::runtime_error(
                "MmapDbnReader: unsupported DBN version " + std::to_string(version_) +
                " (only v" + std::to_string(kSupportedDbnVersion) +
                " record layout is validated)");
        }
        std::uint32_t metadata_len = 0;  // u32 LE; host is little-endian (dbn.hpp)
        std::memcpy(&metadata_len, f.data() + 4, sizeof(metadata_len));
        cursor_ = std::size_t{8} + metadata_len;  // u32 + 8 cannot overflow size_t
        if (cursor_ > f.size()) {
            throw std::runtime_error("MmapDbnReader: truncated DBN metadata");
        }
    }

    MemoryMappedFile map_;
    std::size_t      cursor_  = 0;  // index of the next unframed byte in map_.bytes()
    std::uint8_t     version_ = 0;
    bool             failed_  = false;
    IngestStats      stats_{};
};

}  // namespace trading_sim
