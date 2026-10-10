#include "L3KVG/BinaryEdgeWAL.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#define close _close
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#endif

namespace l3kvg {

BinaryEdgeWAL::BinaryEdgeWAL(const std::string& path)
    : path_(path), fd_(-1) {
    std::filesystem::path p(path_);
    if (p.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
    }

#if defined(_WIN32)
    fd_ = ::_open(path_.c_str(), _O_RDWR | _O_CREAT | _O_APPEND | _O_BINARY, _S_IREAD | _S_IWRITE);
#elif defined(O_CLOEXEC)
    fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
#else
    fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT | O_APPEND, 0644);
#endif

    if (fd_ < 0) {
        throw std::runtime_error("Failed to open WAL file " + path_ + ": " + std::string(std::strerror(errno)));
    }
    buffer_.resize(65536);
    buffer_len_ = 0;
}

BinaryEdgeWAL::~BinaryEdgeWAL() {
    try {
        close();
    } catch (...) {
        // Suppress any errors during destruction
    }
}

BinaryEdgeWAL::BinaryEdgeWAL(BinaryEdgeWAL&& other) noexcept {
    std::lock_guard<std::mutex> lock(other.write_mutex_);
    path_ = std::move(other.path_);
    fd_ = other.fd_;
    buffer_ = std::move(other.buffer_);
    buffer_len_ = other.buffer_len_;
    other.buffer_len_ = 0;
    other.fd_ = -1;
}

BinaryEdgeWAL& BinaryEdgeWAL::operator=(BinaryEdgeWAL&& other) noexcept {
    if (this != &other) {
        std::scoped_lock lock(write_mutex_, other.write_mutex_);
        close_unlocked();
        path_ = std::move(other.path_);
        fd_ = other.fd_;
        buffer_ = std::move(other.buffer_);
        buffer_len_ = other.buffer_len_;
        other.buffer_len_ = 0;
        other.fd_ = -1;
    }
    return *this;
}

void BinaryEdgeWAL::write_all_unlocked(const char* data, size_t total_bytes) {
    size_t written = 0;
    while (written < total_bytes) {
#if defined(_WIN32)
        unsigned int chunk = static_cast<unsigned int>(std::min<size_t>(total_bytes - written, static_cast<size_t>(UINT_MAX)));
        int res = ::_write(fd_, data + written, chunk);
#else
        ssize_t res = ::write(fd_, data + written, total_bytes - written);
#endif
        if (res < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error("Write to WAL failed: " + std::string(std::strerror(errno)));
        }
        if (res == 0) {
            throw std::runtime_error("Write to WAL returned 0 bytes");
        }
        written += static_cast<size_t>(res);
    }
}

void BinaryEdgeWAL::flush_unlocked() {
    if (fd_ >= 0) {
        if (buffer_len_ > 0) {
            write_all_unlocked(buffer_.data(), buffer_len_);
            buffer_len_ = 0;
        }
#if defined(__linux__)
        if (::fdatasync(fd_) != 0) {
            throw std::runtime_error("fdatasync failed: " + std::string(std::strerror(errno)));
        }
#elif defined(_WIN32)
        if (::_commit(fd_) != 0) {
            throw std::runtime_error("_commit failed");
        }
#else
        if (::fsync(fd_) != 0) {
            throw std::runtime_error("fsync failed: " + std::string(std::strerror(errno)));
        }
#endif
    }
}

void BinaryEdgeWAL::close_unlocked() {
    if (fd_ >= 0) {
        if (buffer_len_ > 0) {
            try {
                write_all_unlocked(buffer_.data(), buffer_len_);
                buffer_len_ = 0;
            } catch (...) {}
        }
#if defined(__linux__)
        ::fdatasync(fd_);
#elif defined(_WIN32)
        ::_commit(fd_);
#else
        ::fsync(fd_);
#endif
        ::close(fd_);
        fd_ = -1;
    }
}

void BinaryEdgeWAL::append(const EdgeMutationOp& op) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (fd_ < 0) {
        throw std::runtime_error("Cannot append: WAL is closed");
    }
    if (buffer_len_ + sizeof(EdgeMutationOp) > 65536) {
        write_all_unlocked(buffer_.data(), buffer_len_);
        buffer_len_ = 0;
    }
    std::memcpy(buffer_.data() + buffer_len_, &op, sizeof(EdgeMutationOp));
    buffer_len_ += sizeof(EdgeMutationOp);
}

void BinaryEdgeWAL::append_batch(std::span<const EdgeMutationOp> ops) {
    if (ops.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (fd_ < 0) {
        throw std::runtime_error("Cannot append batch: WAL is closed");
    }
    const char* ptr = reinterpret_cast<const char*>(ops.data());
    size_t total_bytes = ops.size_bytes();
    if (total_bytes >= 65536) {
        if (buffer_len_ > 0) {
            write_all_unlocked(buffer_.data(), buffer_len_);
            buffer_len_ = 0;
        }
        write_all_unlocked(ptr, total_bytes);
    } else {
        if (buffer_len_ + total_bytes > 65536) {
            write_all_unlocked(buffer_.data(), buffer_len_);
            buffer_len_ = 0;
        }
        std::memcpy(buffer_.data() + buffer_len_, ptr, total_bytes);
        buffer_len_ += total_bytes;
    }
}

void BinaryEdgeWAL::flush() {
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (fd_ < 0) {
        throw std::runtime_error("Cannot flush: WAL is closed");
    }
    flush_unlocked();
}

void BinaryEdgeWAL::close() {
    std::lock_guard<std::mutex> lock(write_mutex_);
    close_unlocked();
}

bool BinaryEdgeWAL::is_open() const noexcept {
    std::lock_guard<std::mutex> lock(write_mutex_);
    return fd_ >= 0;
}

size_t BinaryEdgeWAL::recover(AdjacencyStore& store) {
    std::lock_guard<std::mutex> lock(write_mutex_);

    if (fd_ >= 0 && !buffer_.empty()) {
        write_all_unlocked(buffer_.data(), buffer_.size());
        buffer_.clear();
    }

    int read_fd = fd_;
    bool close_read_fd = false;

    if (read_fd < 0) {
        if (path_.empty()) {
            return 0;
        }
#if defined(_WIN32)
        read_fd = ::_open(path_.c_str(), _O_RDONLY | _O_BINARY);
#elif defined(O_CLOEXEC)
        read_fd = ::open(path_.c_str(), O_RDONLY | O_CLOEXEC);
#else
        read_fd = ::open(path_.c_str(), O_RDONLY);
#endif
        if (read_fd < 0) {
            return 0;
        }
        close_read_fd = true;
    } else {
        flush_unlocked();
    }

    struct FdGuard {
        int fd;
        bool should_close;
        ~FdGuard() {
            if (should_close && fd >= 0) {
                ::close(fd);
            }
        }
    } guard{read_fd, close_read_fd};

    size_t recovered_count = 0;
    constexpr size_t CHUNK_SIZE = 64 * 1024; // 65,536 bytes (2048 * sizeof(EdgeMutationOp))
    static_assert(CHUNK_SIZE % sizeof(EdgeMutationOp) == 0, "CHUNK_SIZE must be a multiple of EdgeMutationOp size");
    std::vector<char> buffer(CHUNK_SIZE);

    uint64_t file_offset = 0;

    while (true) {
        size_t bytes_in_chunk = 0;
        while (bytes_in_chunk < CHUNK_SIZE) {
#if defined(_WIN32)
            __int64 seek_res = _lseeki64(read_fd, static_cast<__int64>(file_offset + bytes_in_chunk), SEEK_SET);
            if (seek_res == -1) {
                if (errno == EINTR) {
                    continue;
                }
                throw std::runtime_error("Failed to seek in binary edge WAL during recovery: " + std::string(std::strerror(errno)));
            }
            int bytes_read = ::_read(read_fd, buffer.data() + bytes_in_chunk, static_cast<unsigned int>(CHUNK_SIZE - bytes_in_chunk));
#else
            ssize_t bytes_read = ::pread(read_fd, buffer.data() + bytes_in_chunk, CHUNK_SIZE - bytes_in_chunk, static_cast<off_t>(file_offset + bytes_in_chunk));
#endif
            if (bytes_read == 0) {
                break; // Clean EOF
            }
            if (bytes_read < 0) {
                if (errno == EINTR) {
                    continue; // Interrupted by signal, retry
                }
                throw std::runtime_error("Failed to read from binary edge WAL during recovery: " + std::string(std::strerror(errno)));
            }
            bytes_in_chunk += static_cast<size_t>(bytes_read);
        }

        if (bytes_in_chunk == 0) {
            break;
        }

        size_t full_records = bytes_in_chunk / sizeof(EdgeMutationOp);
        const auto* ops = reinterpret_cast<const EdgeMutationOp*>(buffer.data());

        for (size_t i = 0; i < full_records; ++i) {
            const auto& op = ops[i];
            if (op.is_delete()) {
                store.del_edge(op.src_id, op.dst_id, op.label_id, op.weight <= 0.0f ? -1.0f : op.weight);
            } else {
                store.add_edge(op.src_id, op.dst_id, op.label_id, op.weight, op.flags, 0, static_cast<uint32_t>(op.hlc_timestamp));
            }
            recovered_count++;
        }

        // If less than a full chunk was read, we reached EOF (including any truncated partial record < 32 bytes)
        if (bytes_in_chunk < CHUNK_SIZE) {
            break;
        }

        file_offset += bytes_in_chunk;
    }

    return recovered_count;
}

} // namespace l3kvg
