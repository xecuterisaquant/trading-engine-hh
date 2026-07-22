// Platform implementation of MemoryMappedFile. The <windows.h> / <sys/mman.h>
// includes are confined to THIS translation unit so no consumer of the header pays
// for them (see the header's platform note).
#include "trading_sim/memory_mapped_file.hpp"

#include <stdexcept>
#include <string>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN  // trim the Win32 header surface
#  define NOMINMAX             // keep min/max as std functions, not macros (/WX)
#  include <windows.h>
#else
#  include <fcntl.h>     // ::open, O_RDONLY
#  include <sys/mman.h>  // ::mmap, ::munmap, PROT_READ, MAP_PRIVATE, MAP_FAILED
#  include <sys/stat.h>  // ::fstat, struct stat
#  include <unistd.h>    // ::close
#endif

namespace trading_sim {

#ifdef _WIN32

MemoryMappedFile::MemoryMappedFile(const std::filesystem::path& path) {
    // path.c_str() is wchar_t* on Windows, so the wide CreateFileW is the natural fit.
    const HANDLE file =
        CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("MemoryMappedFile: cannot open " + path.string());
    }

    LARGE_INTEGER file_size{};
    if (GetFileSizeEx(file, &file_size) == 0) {
        CloseHandle(file);
        throw std::runtime_error("MemoryMappedFile: cannot size " + path.string());
    }
    if (file_size.QuadPart == 0) {
        CloseHandle(file);
        return;  // empty file -> empty view {nullptr, 0}
    }

    const HANDLE mapping =
        CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mapping == nullptr) {
        CloseHandle(file);
        throw std::runtime_error("MemoryMappedFile: cannot map " + path.string());
    }

    void* const view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    // The view keeps the pages valid on its own; closing the file and mapping handles
    // now is documented-safe and leaves only the view to release in release_().
    CloseHandle(mapping);
    CloseHandle(file);
    if (view == nullptr) {
        throw std::runtime_error("MemoryMappedFile: cannot view " + path.string());
    }

    data_ = static_cast<const std::byte*>(view);
    size_ = static_cast<std::size_t>(file_size.QuadPart);
}

void MemoryMappedFile::release_() noexcept {
    if (data_ != nullptr) {
        UnmapViewOfFile(data_);
        data_ = nullptr;
        size_ = 0;
    }
}

#else  // POSIX

MemoryMappedFile::MemoryMappedFile(const std::filesystem::path& path) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        throw std::runtime_error("MemoryMappedFile: cannot open " + path.string());
    }

    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        throw std::runtime_error("MemoryMappedFile: cannot stat " + path.string());
    }
    if (st.st_size == 0) {
        ::close(fd);
        return;  // empty file -> empty view {nullptr, 0}
    }

    void* const addr = ::mmap(nullptr, static_cast<std::size_t>(st.st_size),
                              PROT_READ, MAP_PRIVATE, fd, 0);
    // On POSIX the fd may be closed immediately: the mapping holds its own reference,
    // so the view outlives the descriptor. Symmetric with the Windows path above.
    ::close(fd);
    if (addr == MAP_FAILED) {
        throw std::runtime_error("MemoryMappedFile: cannot mmap " + path.string());
    }

    data_ = static_cast<const std::byte*>(addr);
    size_ = static_cast<std::size_t>(st.st_size);
}

void MemoryMappedFile::release_() noexcept {
    if (data_ != nullptr) {
        // munmap wants void*; our view is read-only (const), so cast away const only
        // to hand the address back to the OS — we never write through it.
        ::munmap(const_cast<void*>(static_cast<const void*>(data_)), size_);
        data_ = nullptr;
        size_ = 0;
    }
}

#endif

// --- Platform-independent special members (release_ hides the platform difference) --

MemoryMappedFile::~MemoryMappedFile() { release_(); }

MemoryMappedFile::MemoryMappedFile(MemoryMappedFile&& other) noexcept
    : data_(other.data_), size_(other.size_) {
    other.data_ = nullptr;  // leave the source empty so its dtor unmaps nothing
    other.size_ = 0;
}

MemoryMappedFile& MemoryMappedFile::operator=(MemoryMappedFile&& other) noexcept {
    if (this != &other) {
        release_();             // free what we currently hold before taking theirs
        data_ = other.data_;
        size_ = other.size_;
        other.data_ = nullptr;
        other.size_ = 0;
    }
    return *this;
}

}  // namespace trading_sim
