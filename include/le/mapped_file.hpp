#pragma once
// Read-only memory-mapped файл: данные не копируются в user-space буфер,
// страницы подгружаются ОС по мере чтения.
#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace le {

class MappedFile {
public:
    MappedFile() = default;
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    MappedFile(MappedFile&& o) noexcept
        : data_(std::exchange(o.data_, nullptr)), size_(std::exchange(o.size_, 0)) {}
    MappedFile& operator=(MappedFile&& o) noexcept {
        if (this != &o) {
            close();
            data_ = std::exchange(o.data_, nullptr);
            size_ = std::exchange(o.size_, 0);
        }
        return *this;
    }
    ~MappedFile() { close(); }

    static MappedFile open(const std::filesystem::path& path) {
        MappedFile f;
#if defined(_WIN32)
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot open file: " + path.string());
        LARGE_INTEGER sz{};
        if (!GetFileSizeEx(file, &sz)) {
            CloseHandle(file);
            throw std::runtime_error("cannot get file size");
        }
        if (sz.QuadPart > 0) {
            HANDLE map = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
            if (!map) {
                CloseHandle(file);
                throw std::runtime_error("CreateFileMapping failed");
            }
            void* p = MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0);
            CloseHandle(map);  // view держит mapping живым
            if (!p) {
                CloseHandle(file);
                throw std::runtime_error("MapViewOfFile failed");
            }
            f.data_ = static_cast<const char*>(p);
            f.size_ = static_cast<size_t>(sz.QuadPart);
        }
        CloseHandle(file);
#else
        int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) throw std::runtime_error("cannot open file: " + path.string());
        struct stat st {};
        if (::fstat(fd, &st) != 0) {
            ::close(fd);
            throw std::runtime_error("fstat failed");
        }
        if (st.st_size > 0) {
            void* p = ::mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
            if (p == MAP_FAILED) {
                ::close(fd);
                throw std::runtime_error("mmap failed");
            }
            ::madvise(p, static_cast<size_t>(st.st_size), MADV_SEQUENTIAL);
            f.data_ = static_cast<const char*>(p);
            f.size_ = static_cast<size_t>(st.st_size);
        }
        ::close(fd);
#endif
        return f;
    }

    std::string_view view() const noexcept { return {data_, size_}; }
    size_t size() const noexcept { return size_; }

private:
    void close() noexcept {
        if (!data_) return;
#if defined(_WIN32)
        UnmapViewOfFile(data_);
#else
        ::munmap(const_cast<char*>(data_), size_);
#endif
        data_ = nullptr;
        size_ = 0;
    }

    const char* data_ = nullptr;
    size_t size_ = 0;
};

}  // namespace le
