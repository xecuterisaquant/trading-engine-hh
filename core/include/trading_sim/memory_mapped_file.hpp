#pragma once

#include <cstddef>     // std::byte, std::size_t
#include <filesystem>  // std::filesystem::path
#include <span>        // std::span — the whole-file view we hand out

// MemoryMappedFile — an RAII owner of a read-only, whole-file memory mapping.
//
// It maps the ENTIRE file into the process address space so its bytes can be read in
// place, with NO copy into a user buffer (contrast FileByteSource, whose read()
// copies into a caller buffer — that copy, "Copy 1", is ~85% of ingest cost per the
// baseline benchmark, ADR 0019). Mapping the whole file also erases the straddle: the
// file is one contiguous region, so records never split across a boundary and the
// RecordAssembler is unnecessary on this path.
//
// Ownership (RAII, move-only): a mapping is a UNIQUE OS resource. Copying the object
// would let two instances release the same region in their destructors — a
// double-unmap (undefined behavior). So copy is deleted and move transfers the view
// and nulls the source. The destructor releases the mapping; nothing is ever unmapped
// by hand, and cleanup is exception-safe on any scope exit.
//
// Platform: the mapping is created with POSIX mmap or Win32 MapViewOfFile behind an
// #ifdef in the .cpp. That code lives in the .cpp ON PURPOSE — <windows.h> must not
// leak into every translation unit that includes this header (it drags in macros like
// min/max that would break our /W4 /WX build). The header therefore stays free of any
// platform handle: on both OSes we close the file/mapping handles right after
// establishing the view (the OS keeps the pages valid), so only {data_, size_} remain.
//
// Hazard (SIGBUS): if the file is truncated by another process while mapped, touching
// the vanished pages raises SIGBUS (POSIX) / EXCEPTION_IN_PAGE_ERROR (Windows) and by
// default kills the process — unlike read(), which fails synchronously with a return
// value. We map read-only historical files we own, so this is low-risk, but it is why
// mmap is a file-specialized fast path, not a replacement for the streaming source
// (which a socket still needs).
namespace trading_sim {

class MemoryMappedFile {
public:
    // Map the whole file read-only. Throws std::runtime_error on open/size/map
    // failure (a one-time, fatal, off-hot-path failure — same policy as
    // FileByteSource's open). A zero-byte file is VALID and maps to an empty view:
    // both OSes refuse to map a 0-length region, so we represent it as {nullptr, 0}
    // and callers simply see no bytes.
    explicit MemoryMappedFile(const std::filesystem::path& path);
    ~MemoryMappedFile();

    // Move-only: transfer the view, leave the source empty. See the ownership note.
    MemoryMappedFile(MemoryMappedFile&& other) noexcept;
    MemoryMappedFile& operator=(MemoryMappedFile&& other) noexcept;
    MemoryMappedFile(const MemoryMappedFile&) = delete;
    MemoryMappedFile& operator=(const MemoryMappedFile&) = delete;

    // The whole file as a read-only span, valid until this object is destroyed or
    // moved-from. Empty (data()==nullptr, size()==0) for a zero-byte file.
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return {data_, size_};
    }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

private:
    // Unmap (if mapped) and reset to the empty state. Idempotent, noexcept — called
    // by the destructor and by move-assignment before it overwrites this object.
    void release_() noexcept;

    const std::byte* data_ = nullptr;  // start of the mapped view (nullptr == empty)
    std::size_t      size_ = 0;        // mapped length in bytes
};

}  // namespace trading_sim
