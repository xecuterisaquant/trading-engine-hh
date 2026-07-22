// CTest for the DBN record framer (dbn_reader.hpp: frame_one).
//
// Same conventions as the other C++ tests (ADR 0008): no framework, no assert()
// (compiled out under NDEBUG so it would silently pass in Release), each check
// returns a DISTINCT non-zero exit code so a red CTest names exactly which case
// broke. 0 == every case passed.
//
// frame_one answers ONE question about the FRONT of a byte buffer: "is a whole
// record here, and if so how big is it?" — without copying or advancing a cursor.
// The hard case it exists for is the STRADDLE: a record split across a chunk/read
// boundary must report NeedMore (wait for more bytes), never be read from a short
// buffer (which would pull 'the rest' out of unrelated memory).
#include <array>
#include <cstddef>   // std::byte
#include <cstdint>
#include <filesystem>    // temp-file path for the FileByteSource test
#include <fstream>       // write the known temp file
#include <span>
#include <stdexcept>     // catch the open-failure throw
#include <system_error>  // std::error_code for non-throwing temp-file cleanup
#include <vector>        // synthetic byte streams for the DbnReader test

#include "trading_sim/dbn_reader.hpp"

namespace ts = trading_sim;

int main() {
    using std::byte;

    // Framing prefix convention (see dbn.hpp): byte 0 = length (record bytes / 4),
    // byte 1 = rtype. frame_one reads only these two to decide; the rest is payload.

    // --- Case 1: empty buffer -> NeedMore (nothing to read) ------------------
    {
        std::span<const byte> empty{};
        if (ts::frame_one(empty).status != ts::FrameStatus::NeedMore) return 1;
    }

    // --- Case 2: a single byte -> NeedMore (have length, but not rtype) ------
    {
        std::array<byte, 1> one{ byte{14} };
        if (ts::frame_one(one).status != ts::FrameStatus::NeedMore) return 2;
    }

    // --- Case 3: a full 56-byte MBO record -> Ok, rtype 0xA0, size 56 --------
    {
        std::array<byte, 56> rec{};
        rec[0] = byte{14};     // length: 56 / 4
        rec[1] = byte{0xA0};   // rtype: MBO (representative — frame_one stays agnostic)
        const auto r = ts::frame_one(rec);
        if (r.status != ts::FrameStatus::Ok) return 3;
        if (r.rtype != 0xA0)                 return 4;
        if (r.record_size != 56)             return 5;
    }

    // --- Case 4: first 44 bytes of a 56-byte record -> NeedMore (THE STRADDLE)
    // Record advertises 56 bytes (length 14) but only 44 have arrived. The entire
    // reason this function exists: report NeedMore; never read past the 44 present.
    {
        std::array<byte, 56> rec{};
        rec[0] = byte{14};
        rec[1] = byte{0xA0};
        std::span<const byte> partial{rec.data(), 44};
        if (ts::frame_one(partial).status != ts::FrameStatus::NeedMore) return 6;
    }

    // --- Case 5: size known (56) but exactly one byte short (55) -> NeedMore -
    {
        std::array<byte, 56> rec{};
        rec[0] = byte{14};
        rec[1] = byte{0xA0};
        std::span<const byte> partial{rec.data(), 55};
        if (ts::frame_one(partial).status != ts::FrameStatus::NeedMore) return 7;
    }

    // --- Case 6: length == 0 -> Corrupt (a zero-size record would loop forever)
    {
        std::array<byte, 8> rec{};
        rec[0] = byte{0};      // length 0 -> record_size 0
        rec[1] = byte{0xA0};
        if (ts::frame_one(rec).status != ts::FrameStatus::Corrupt) return 8;
    }

    // --- Case 7: a complete NON-MBO record -> Ok with ITS own rtype/size -----
    // Proves frame_one frames by the record's OWN length, not by assuming 56 or
    // MBO. Here length 4 -> a 16-byte record with a different rtype.
    {
        std::array<byte, 16> rec{};
        rec[0] = byte{4};      // length: 16 / 4
        rec[1] = byte{0x15};   // some non-MBO rtype
        const auto r = ts::frame_one(rec);
        if (r.status != ts::FrameStatus::Ok) return 9;
        if (r.rtype != 0x15)                 return 10;
        if (r.record_size != 16)             return 11;
    }

    // --- Case 8 (added): a full record FOLLOWED by a partial next one --------
    // frame_one must frame only the FRONT record (size 56) and ignore the 44
    // trailing bytes of the next. This is what lets the caller loop: take 56,
    // advance the cursor, call again, get NeedMore on the 44-byte remainder.
    {
        std::array<byte, 100> buf{};   // one 56-byte record + 44 bytes of the next
        buf[0] = byte{14};
        buf[1] = byte{0xA0};
        const auto r = ts::frame_one(buf);
        if (r.status != ts::FrameStatus::Ok) return 12;
        if (r.record_size != 56)             return 13;
    }

    // =======================================================================
    // RecordAssembler: carries the straddled tail across appends and yields
    // complete record views. Exit codes 20+ so a failure here is unmistakable.
    // (`asm` is a keyword, so the instance is named `asm_`.)
    // =======================================================================

    // --- Case A: one full record in a single append -> Record, then NeedMore -
    {
        ts::RecordAssembler asm_;
        std::array<byte, 56> rec{};
        rec[0] = byte{14};
        rec[1] = byte{0xA0};
        asm_.append(rec);
        const auto p1 = asm_.next();
        if (p1.status != ts::PullStatus::Record)            return 20;
        if (p1.record.size() != 56)                         return 21;
        if (asm_.next().status != ts::PullStatus::NeedMore) return 22;
    }

    // --- Case B: THE STRADDLE across two appends, with a sentinel past byte 44
    // The sentinel sits at offset 50 -> in the SECOND half, so it only arrives in
    // the second append. If the stitched record carries it, reassembly is real and
    // not an accident of the first chunk.
    {
        ts::RecordAssembler asm_;
        std::array<byte, 56> rec{};
        rec[0]  = byte{14};
        rec[1]  = byte{0xA0};
        rec[50] = byte{0x7E};   // sentinel, lives in the second half

        asm_.append(std::span<const byte>{rec.data(), 44});     // first 44 bytes
        if (asm_.next().status != ts::PullStatus::NeedMore) return 23;
        if (asm_.buffered() != 44)                          return 24;

        asm_.append(std::span<const byte>{rec.data() + 44, 12}); // remaining 12
        const auto p = asm_.next();
        if (p.status != ts::PullStatus::Record) return 25;
        if (p.record.size() != 56)              return 26;
        if (p.record[50] != byte{0x7E})         return 27;  // sentinel survived the stitch
        if (asm_.buffered() != 0)               return 28;
    }

    // --- Case C: two records back-to-back in one append ----------------------
    {
        ts::RecordAssembler asm_;
        std::array<byte, 112> two{};
        two[0]  = byte{14}; two[1]  = byte{0xA0};   // record 0
        two[56] = byte{14}; two[57] = byte{0xA0};   // record 1
        asm_.append(two);
        if (asm_.next().status != ts::PullStatus::Record)   return 29;
        if (asm_.next().status != ts::PullStatus::Record)   return 30;
        if (asm_.next().status != ts::PullStatus::NeedMore) return 31;
    }

    // --- Case D: length == 0 surfaces as Corrupt (not NeedMore, not Record) --
    {
        ts::RecordAssembler asm_;
        std::array<byte, 8> zero{};
        zero[0] = byte{0};
        zero[1] = byte{0xA0};
        asm_.append(zero);
        if (asm_.next().status != ts::PullStatus::Corrupt) return 32;
    }

    // =======================================================================
    // FileByteSource: the byte pump (Unit 3). We write a known 100-byte temp
    // file, then read it back in 40-byte chunks (SMALLER than the file) to
    // exercise multi-read, the short read at EOF, and the 0-return past EOF.
    // Payload byte i == i, so we can also confirm content and continuity.
    // Exit codes 40+.
    // =======================================================================
    {
        namespace fs = std::filesystem;
        const fs::path tmp = fs::temp_directory_path() / "ts_filesource_test.bin";

        {   // write 100 bytes, then close the stream before reading it back
            std::ofstream out(tmp, std::ios::binary);
            std::array<char, 100> payload{};
            for (std::size_t i = 0; i < payload.size(); ++i) {
                payload[i] = static_cast<char>(i);   // 0..99, all fit in a char
            }
            out.write(payload.data(),
                      static_cast<std::streamsize>(payload.size()));
        }

        {   // scope the source so its file handle is CLOSED before we remove the
            // file below — on Windows an open file cannot be deleted (no POSIX
            // unlink-while-open semantics), and a throwing remove would abort().
            ts::FileByteSource src(tmp);
            std::array<byte, 40> chunk{};

            const std::size_t n1 = src.read(chunk);   // full 40 (bytes 0..39)
            if (n1 != 40)              return 40;
            if (chunk[0]  != byte{0})  return 41;      // content: first byte
            if (chunk[39] != byte{39}) return 42;

            const std::size_t n2 = src.read(chunk);   // full 40 (bytes 40..79)
            if (n2 != 40)              return 43;
            if (chunk[0]  != byte{40}) return 44;      // CONTINUED, not restarted

            const std::size_t n3 = src.read(chunk);   // short read: only 20 left
            if (n3 != 20)              return 45;
            if (chunk[19] != byte{99}) return 46;      // last byte of the file

            const std::size_t n4 = src.read(chunk);   // past EOF -> 0
            if (n4 != 0)               return 47;
        }

        std::error_code ec;
        fs::remove(tmp, ec);   // best-effort cleanup; never throw from a test teardown
    }

    // --- Case F: opening a missing file throws (a one-time, fatal failure) ---
    {
        namespace fs = std::filesystem;
        const fs::path missing =
            fs::temp_directory_path() / "ts_filesource_does_not_exist.bin";
        fs::remove(missing);   // make sure it is absent

        bool threw = false;
        try {
            ts::FileByteSource src(missing);
            (void)src;
        } catch (const std::runtime_error&) {
            threw = true;
        }
        if (!threw) return 48;
    }

    // =======================================================================
    // DbnReader: the full pull-iterator (Unit 4). Synthesize a byte stream with
    // a good MBO, a non-MBO record, a bad-action MBO, and another good MBO;
    // write it to a temp file; confirm next() emits ONLY the two good MBOs
    // (skipping the non-MBO per ADR 0003 and the bad record per ADR 0002) and
    // then reports a CLEAN eof. A second stream ending in a length-0 record
    // confirms the corrupt-frame STOP + failed() flag. Exit codes 50+.
    // =======================================================================

    // Build a 56-byte MBO wire record with the given identifying fields. Braced
    // integer inits avoid /W4 narrowing warnings on the small fields.
    auto make_mbo = [](char action, char side, std::uint64_t order_id,
                       std::int64_t price) {
        ts::WireMbo w{};
        w.length   = std::uint8_t{14};    // 56 / 4
        w.rtype    = std::uint8_t{0xA0};   // MBO
        w.action   = action;
        w.side     = side;
        w.order_id = order_id;
        w.price    = price;
        w.size     = std::uint32_t{5};
        return w;
    };
    auto append_bytes = [](std::vector<byte>& out, const void* src,
                           std::size_t n) {
        const auto* p = static_cast<const byte*>(src);
        out.insert(out.end(), p, p + n);
    };
    // Prepend a minimal DBN prologue: "DBN" + version 1 + u32 metadata length,
    // then that many zero metadata bytes. DbnReader's ctor consumes exactly this
    // before any records, so every synthetic stream must carry one.
    auto append_prologue = [](std::vector<byte>& out, std::uint32_t metadata_len) {
        std::array<byte, 8> hdr{};
        hdr[0] = byte{'D'};
        hdr[1] = byte{'B'};
        hdr[2] = byte{'N'};
        hdr[3] = byte{1};   // version
        hdr[4] = static_cast<byte>(metadata_len & 0xFFu);          // u32 LE
        hdr[5] = static_cast<byte>((metadata_len >> 8) & 0xFFu);
        hdr[6] = static_cast<byte>((metadata_len >> 16) & 0xFFu);
        hdr[7] = static_cast<byte>((metadata_len >> 24) & 0xFFu);
        out.insert(out.end(), hdr.begin(), hdr.end());
        out.insert(out.end(), metadata_len, byte{0});
    };

    // --- Case G: skip non-MBO + bad record, emit the two good ones, clean EOF -
    {
        namespace fs = std::filesystem;
        const fs::path tmp = fs::temp_directory_path() / "ts_dbnreader_g.bin";

        std::vector<byte> stream;
        append_prologue(stream, 4u);          // header + 4 metadata bytes to skip
        const ts::WireMbo m0 = make_mbo('A', 'B', 111u, 1000);
        append_bytes(stream, &m0, sizeof(m0));

        std::array<byte, 16> nonmbo{};        // length 4 -> 16 bytes, non-MBO rtype
        nonmbo[0] = byte{4};
        nonmbo[1] = byte{0x15};
        append_bytes(stream, nonmbo.data(), nonmbo.size());

        const ts::WireMbo bad = make_mbo('Z', 'B', 999u, 9999);  // 'Z' bad action
        append_bytes(stream, &bad, sizeof(bad));

        const ts::WireMbo m1 = make_mbo('C', 'A', 222u, 2000);
        append_bytes(stream, &m1, sizeof(m1));

        {
            std::ofstream out(tmp, std::ios::binary);
            out.write(reinterpret_cast<const char*>(stream.data()),
                      static_cast<std::streamsize>(stream.size()));
        }

        {   // reader scoped so its file handle closes before remove (Windows lock)
            ts::DbnReader reader(tmp);

            const auto e0 = reader.next();
            if (!e0)                  return 50;
            if (e0->order_id != 111u) return 51;   // first good MBO
            if (e0->price    != 1000) return 52;

            const auto e1 = reader.next();          // skips non-MBO AND bad-action
            if (!e1)                  return 53;
            if (e1->order_id != 222u) return 54;   // jumped to the 2nd good record

            const auto e2 = reader.next();
            if (e2)                   return 55;    // stream exhausted
            if (reader.failed())      return 56;    // ...cleanly, not a corruption
        }

        std::error_code ec;
        fs::remove(tmp, ec);
    }

    // --- Case H: a good MBO then a length-0 record -> emit one, then STOP+fail -
    {
        namespace fs = std::filesystem;
        const fs::path tmp = fs::temp_directory_path() / "ts_dbnreader_h.bin";

        std::vector<byte> stream;
        append_prologue(stream, 0u);     // header with no metadata to skip
        const ts::WireMbo m0 = make_mbo('A', 'B', 77u, 700);
        append_bytes(stream, &m0, sizeof(m0));
        std::array<byte, 8> corrupt{};   // length byte 0 -> record_size 0 -> Corrupt
        append_bytes(stream, corrupt.data(), corrupt.size());

        {
            std::ofstream out(tmp, std::ios::binary);
            out.write(reinterpret_cast<const char*>(stream.data()),
                      static_cast<std::streamsize>(stream.size()));
        }

        {
            ts::DbnReader reader(tmp);

            const auto e0 = reader.next();
            if (!e0)                return 60;
            if (e0->order_id != 77u) return 61;

            const auto e1 = reader.next();
            if (e1)                 return 62;      // stopped
            if (!reader.failed())   return 63;      // ...because of corruption
        }

        std::error_code ec;
        fs::remove(tmp, ec);
    }

    // --- Case I: an unsupported DBN version is REFUSED at construction --------
    // Our WireMbo layout is validated for v1 only. A v2/v3 file could place fields
    // at different offsets, so the reader must reject it LOUDLY (throw) at ctor time
    // rather than decode through the wrong layout and silently forge garbage Events.
    // We hand-build the header here (append_prologue hardcodes v1) to set version 2.
    {
        namespace fs = std::filesystem;
        const fs::path tmp = fs::temp_directory_path() / "ts_dbnreader_i.bin";

        std::vector<byte> stream;
        std::array<byte, 8> hdr{};
        hdr[0] = byte{'D'};
        hdr[1] = byte{'B'};
        hdr[2] = byte{'N'};
        hdr[3] = byte{2};   // version 2: a record layout we have NOT validated
        // metadata_len (bytes 4..7) left 0 — the throw must fire before it matters.
        stream.insert(stream.end(), hdr.begin(), hdr.end());
        const ts::WireMbo m0 = make_mbo('A', 'B', 1u, 100);  // a would-be valid record
        append_bytes(stream, &m0, sizeof(m0));

        {
            std::ofstream out(tmp, std::ios::binary);
            out.write(reinterpret_cast<const char*>(stream.data()),
                      static_cast<std::streamsize>(stream.size()));
        }

        // The ctor runs skip_prologue_, which must throw on the bad version. The
        // already-constructed FileByteSource member is destroyed during unwinding,
        // closing the handle before remove (Windows lock lesson still applies).
        bool threw = false;
        try {
            ts::DbnReader reader(tmp);
            (void)reader;
        } catch (const std::runtime_error&) {
            threw = true;
        }
        if (!threw) return 64;

        std::error_code ec;
        fs::remove(tmp, ec);
    }

    // =======================================================================
    // MmapDbnReader: same contract as DbnReader (routing, corrupt-stop, version
    // guard) but over a whole-file mmap. We reuse the same synthetic streams to
    // prove BOTH readers agree byte-for-byte. Exit codes 70+.
    // =======================================================================

    // --- Case J: mmap reader skips non-MBO + bad record, emits two good, clean EOF
    {
        namespace fs = std::filesystem;
        const fs::path tmp = fs::temp_directory_path() / "ts_mmapreader_j.bin";

        std::vector<byte> stream;
        append_prologue(stream, 4u);
        const ts::WireMbo m0 = make_mbo('A', 'B', 111u, 1000);
        append_bytes(stream, &m0, sizeof(m0));
        std::array<byte, 16> nonmbo{};
        nonmbo[0] = byte{4};
        nonmbo[1] = byte{0x15};
        append_bytes(stream, nonmbo.data(), nonmbo.size());
        const ts::WireMbo bad = make_mbo('Z', 'B', 999u, 9999);
        append_bytes(stream, &bad, sizeof(bad));
        const ts::WireMbo m1 = make_mbo('C', 'A', 222u, 2000);
        append_bytes(stream, &m1, sizeof(m1));

        {
            std::ofstream out(tmp, std::ios::binary);
            out.write(reinterpret_cast<const char*>(stream.data()),
                      static_cast<std::streamsize>(stream.size()));
        }

        {   // reader scoped so the mapping is released before remove (Windows lock)
            ts::MmapDbnReader reader(tmp);

            const auto e0 = reader.next();
            if (!e0)                  return 70;
            if (e0->order_id != 111u) return 71;
            if (e0->price    != 1000) return 72;

            const auto e1 = reader.next();
            if (!e1)                  return 73;
            if (e1->order_id != 222u) return 74;

            const auto e2 = reader.next();
            if (e2)                   return 75;
            if (reader.failed())      return 76;
        }

        std::error_code ec;
        fs::remove(tmp, ec);
    }

    // --- Case K: mmap reader emits one, then STOPS + fails on a length-0 record --
    {
        namespace fs = std::filesystem;
        const fs::path tmp = fs::temp_directory_path() / "ts_mmapreader_k.bin";

        std::vector<byte> stream;
        append_prologue(stream, 0u);
        const ts::WireMbo m0 = make_mbo('A', 'B', 77u, 700);
        append_bytes(stream, &m0, sizeof(m0));
        std::array<byte, 8> corrupt{};   // length 0 -> Corrupt
        append_bytes(stream, corrupt.data(), corrupt.size());

        {
            std::ofstream out(tmp, std::ios::binary);
            out.write(reinterpret_cast<const char*>(stream.data()),
                      static_cast<std::streamsize>(stream.size()));
        }

        {
            ts::MmapDbnReader reader(tmp);

            const auto e0 = reader.next();
            if (!e0)                 return 80;
            if (e0->order_id != 77u) return 81;

            const auto e1 = reader.next();
            if (e1)                  return 82;
            if (!reader.failed())    return 83;
        }

        std::error_code ec;
        fs::remove(tmp, ec);
    }

    // --- Case L: mmap reader also REFUSES an unsupported version at construction -
    {
        namespace fs = std::filesystem;
        const fs::path tmp = fs::temp_directory_path() / "ts_mmapreader_l.bin";

        std::vector<byte> stream;
        std::array<byte, 8> hdr{};
        hdr[0] = byte{'D'};
        hdr[1] = byte{'B'};
        hdr[2] = byte{'N'};
        hdr[3] = byte{2};   // unsupported version
        stream.insert(stream.end(), hdr.begin(), hdr.end());
        const ts::WireMbo m0 = make_mbo('A', 'B', 1u, 100);
        append_bytes(stream, &m0, sizeof(m0));

        {
            std::ofstream out(tmp, std::ios::binary);
            out.write(reinterpret_cast<const char*>(stream.data()),
                      static_cast<std::streamsize>(stream.size()));
        }

        bool threw = false;
        try {
            ts::MmapDbnReader reader(tmp);
            (void)reader;
        } catch (const std::runtime_error&) {
            threw = true;
        }
        if (!threw) return 90;

        std::error_code ec;
        fs::remove(tmp, ec);
    }

    return 0;
}
