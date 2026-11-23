// Read-only memory mapping of a whole file (POSIX).
//
// Why mmap instead of reading into a buffer: the ITCH day is ~10 GB, more than
// this machine's RAM. A mapping lets the parser see the file as one contiguous
// byte span while the kernel pages it in on demand and drops pages it no longer
// needs, with no copy into user-space buffers. madvise(SEQUENTIAL) tells the
// kernel to read ahead aggressively and free pages behind us.
#pragma once

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <span>
#include <string>
#include <system_error>
#include <utility>

namespace orderflow {

class MappedFile {
public:
    explicit MappedFile(const std::string& path) {
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) throw std::system_error(errno, std::generic_category(), "open " + path);
        struct stat st {};
        if (::fstat(fd, &st) != 0) {
            const int err = errno;
            ::close(fd);
            throw std::system_error(err, std::generic_category(), "fstat " + path);
        }
        size_ = static_cast<std::size_t>(st.st_size);
        if (size_ > 0) {
            data_ = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
            if (data_ == MAP_FAILED) {
                const int err = errno;
                ::close(fd);
                throw std::system_error(err, std::generic_category(), "mmap " + path);
            }
            ::madvise(data_, size_, MADV_SEQUENTIAL);
        }
        ::close(fd);  // the mapping keeps the file alive
    }

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    MappedFile(MappedFile&& other) noexcept
        : data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0)) {}
    MappedFile& operator=(MappedFile&& other) noexcept {
        if (this != &other) {
            unmap();
            data_ = std::exchange(other.data_, nullptr);
            size_ = std::exchange(other.size_, 0);
        }
        return *this;
    }
    ~MappedFile() { unmap(); }

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return {static_cast<const std::byte*>(data_), size_};
    }

private:
    void unmap() noexcept {
        if (data_ != nullptr) ::munmap(data_, size_);
        data_ = nullptr;
    }
    void* data_ = nullptr;
    std::size_t size_ = 0;
};

}  // namespace orderflow
