// CTest for MemoryMappedFile (memory_mapped_file.hpp/.cpp).
//
// Conventions (ADR 0008): no framework, no assert(), each check returns a DISTINCT
// non-zero exit code so a red CTest names exactly which case broke. 0 == all passed.
//
// What we prove: a whole file maps to a byte-accurate view; the type is move-only and
// a moved-from mapping is safely empty (its dtor must unmap nothing); a zero-byte file
// maps to an empty view rather than throwing; a missing file throws. The mapping is
// always scoped CLOSED before we remove the temp file — while a view is open the OS
// can hold the file, the same Windows-lock lesson the FileByteSource test learned.
#include <array>
#include <cstddef>       // std::byte
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <system_error>  // std::error_code — non-throwing temp cleanup
#include <utility>       // std::move

#include "trading_sim/memory_mapped_file.hpp"

namespace ts = trading_sim;
namespace fs = std::filesystem;

namespace {
// Write `n` bytes with value == index (0,1,2,... mod 256) and return the path.
fs::path write_ramp(const char* name, std::size_t n) {
    const fs::path p = fs::temp_directory_path() / name;
    std::ofstream out(p, std::ios::binary);
    for (std::size_t i = 0; i < n; ++i) {
        const auto b = static_cast<char>(i & 0xFF);
        out.write(&b, 1);
    }
    return p;
}
}  // namespace

int main() {
    using std::byte;

    // --- Case 1: a 100-byte file maps to a byte-accurate whole-file view ------
    {
        const fs::path tmp = write_ramp("ts_mmap_ramp.bin", 100);
        {
            ts::MemoryMappedFile m(tmp);              // scope closes it before remove
            if (m.size() != 100)                    return 1;
            const std::span<const byte> b = m.bytes();
            if (b.size() != 100)                    return 2;
            if (b[0]  != byte{0})                   return 3;   // first byte
            if (b[99] != byte{99})                  return 4;   // last byte
            if (b[50] != byte{50})                  return 5;   // interior byte
        }
        std::error_code ec;
        fs::remove(tmp, ec);
    }

    // --- Case 2: move-construct transfers the view; source becomes empty ------
    {
        const fs::path tmp = write_ramp("ts_mmap_move.bin", 64);
        {
            ts::MemoryMappedFile src(tmp);
            const std::byte* const data_before = src.bytes().data();

            ts::MemoryMappedFile dst(std::move(src));
            if (dst.size() != 64)                   return 10;
            if (dst.bytes().data() != data_before)  return 11;  // same view, not a copy
            if (dst.bytes()[63] != byte{63})        return 12;

            // Moved-from source must be empty so ITS destructor unmaps nothing.
            if (src.size() != 0)                    return 13;   // NOLINT(bugprone-use-after-move)
            if (src.bytes().data() != nullptr)      return 14;
        }
        std::error_code ec;
        fs::remove(tmp, ec);
    }

    // --- Case 3: move-assign releases the old mapping and takes the new one ---
    {
        const fs::path a = write_ramp("ts_mmap_a.bin", 40);
        const fs::path b = write_ramp("ts_mmap_b.bin", 80);
        {
            ts::MemoryMappedFile m(a);
            m = ts::MemoryMappedFile(b);            // old view of `a` released here
            if (m.size() != 80)                     return 20;
            if (m.bytes()[79] != byte{79})          return 21;
        }
        std::error_code ec;
        fs::remove(a, ec);
        fs::remove(b, ec);
    }

    // --- Case 4: a zero-byte file is valid and maps to an empty view ----------
    {
        const fs::path tmp = write_ramp("ts_mmap_empty.bin", 0);
        {
            ts::MemoryMappedFile m(tmp);
            if (m.size() != 0)                      return 30;
            if (!m.bytes().empty())                 return 31;
        }
        std::error_code ec;
        fs::remove(tmp, ec);
    }

    // --- Case 5: opening a missing file throws (one-time fatal, off hot path) -
    {
        const fs::path missing =
            fs::temp_directory_path() / "ts_mmap_does_not_exist.bin";
        std::error_code ec;
        fs::remove(missing, ec);   // ensure absent

        bool threw = false;
        try {
            ts::MemoryMappedFile m(missing);
            (void)m;
        } catch (const std::runtime_error&) {
            threw = true;
        }
        if (!threw)                                 return 40;
    }

    return 0;
}
