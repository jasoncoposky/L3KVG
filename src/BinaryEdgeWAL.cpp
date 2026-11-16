#include "L3KVG/BinaryEdgeWAL.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
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

void BinaryEdgeWAL::append_unlocked(const EdgeMutationOp& op) {
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

void BinaryEdgeWAL::append(const EdgeMutationOp& op) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    append_unlocked(op);
}

void BinaryEdgeWAL::append_batch_unlocked(std::span<const EdgeMutationOp> ops) {
    if (ops.empty()) {
        return;
    }
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

void BinaryEdgeWAL::append_batch(std::span<const EdgeMutationOp> ops) {
    if (ops.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(write_mutex_);
    append_batch_unlocked(ops);
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
                store.add_edge(op.src_id, op.dst_id, op.label_id, op.weight, op.flags, 0, op.hlc_timestamp);
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

uint64_t BinaryEdgeWAL::file_size() const noexcept {
    std::lock_guard<std::mutex> lock(write_mutex_);
#if defined(_WIN32)
    if (fd_ >= 0) {
        struct _stat64 st;
        if (::_fstat64(fd_, &st) == 0) {
            return static_cast<uint64_t>(st.st_size) + buffer_len_;
        }
    }
    std::error_code ec;
    auto sz = std::filesystem::file_size(path_, ec);
    return (ec ? 0 : static_cast<uint64_t>(sz)) + buffer_len_;
#else
    if (fd_ >= 0) {
        struct stat st;
        if (::fstat(fd_, &st) == 0) {
            return static_cast<uint64_t>(st.st_size) + buffer_len_;
        }
    }
    std::error_code ec;
    auto sz = std::filesystem::file_size(path_, ec);
    return (ec ? 0 : static_cast<uint64_t>(sz)) + buffer_len_;
#endif
}

bool BinaryEdgeWAL::compact(const AdjacencyStore& store) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (path_.empty() || path_ == ":memory:") {
        return false;
    }

    if (fd_ >= 0) {
        try {
            flush_unlocked();
        } catch (...) {
            return false;
        }
    }

    uint64_t old_bytes = 0;
#if defined(_WIN32)
    if (fd_ >= 0) {
        struct _stat64 st;
        if (::_fstat64(fd_, &st) == 0) {
            old_bytes = static_cast<uint64_t>(st.st_size) + buffer_len_;
        }
    }
#else
    if (fd_ >= 0) {
        struct stat st;
        if (::fstat(fd_, &st) == 0) {
            old_bytes = static_cast<uint64_t>(st.st_size) + buffer_len_;
        }
    }
#endif

    // Capture tear-free point-in-time snapshot in memory and release all shard locks BEFORE disk I/O
    std::vector<EdgeMutationOp> snapshot;
    store.snapshot_active_edges(
        [&](size_t total_active) {
            snapshot.reserve(total_active);
        },
        [&](uint64_t src_id, const PackedEdge& pe) {
            snapshot.push_back(EdgeMutationOp{
                .src_id = src_id,
                .dst_id = pe.target_id,
                .hlc_timestamp = pe.hlc_timestamp,
                .label_id = pe.label_id,
                .flags = pe.flags,
                .weight = pe.weight
            });
        }
    );

    std::string tmp_path = path_ + ".compact.tmp";
#if defined(_WIN32)
    int tmp_fd = ::_open(tmp_path.c_str(), _O_RDWR | _O_CREAT | _O_TRUNC | _O_BINARY, _S_IREAD | _S_IWRITE);
#elif defined(O_CLOEXEC)
    int tmp_fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
#else
    int tmp_fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
#endif
    if (tmp_fd < 0) {
        return false;
    }

    bool write_ok = true;
    const char* ptr = reinterpret_cast<const char*>(snapshot.data());
    const size_t total_bytes = snapshot.size() * sizeof(EdgeMutationOp);
    size_t written = 0;
    while (written < total_bytes) {
#if defined(_WIN32)
        unsigned int chunk = static_cast<unsigned int>(std::min<size_t>(total_bytes - written, static_cast<size_t>(UINT_MAX)));
        int res = ::_write(tmp_fd, ptr + written, chunk);
#else
        ssize_t res = ::write(tmp_fd, ptr + written, total_bytes - written);
#endif
        if (res < 0) {
            if (errno == EINTR) continue;
            write_ok = false;
            break;
        }
        if (res == 0) {
            write_ok = false;
            break;
        }
        written += static_cast<size_t>(res);
    }

    if (write_ok) {
#if defined(_WIN32)
        if (::_commit(tmp_fd) != 0) {
            write_ok = false;
        }
#else
        while (::fsync(tmp_fd) != 0) {
            if (errno == EINTR) continue;
            write_ok = false;
            break;
        }
#endif
    }
    if (::close(tmp_fd) != 0 && errno != EINTR) {
        write_ok = false;
    }

    std::error_code ec;
    if (!write_ok) {
        std::filesystem::remove(tmp_path, ec);
        return false;
    }

    std::filesystem::rename(tmp_path, path_, ec);
    if (ec) {
        std::filesystem::remove(tmp_path, ec);
        return false;
    }

#if defined(_WIN32)
    int new_fd = ::_open(path_.c_str(), _O_RDWR | _O_CREAT | _O_APPEND | _O_BINARY, _S_IREAD | _S_IWRITE);
#elif defined(O_CLOEXEC)
    int new_fd = ::open(path_.c_str(), O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
#else
    int new_fd = ::open(path_.c_str(), O_RDWR | O_CREAT | O_APPEND, 0644);
#endif
    if (new_fd < 0) {
        return false;
    }

#if !defined(_WIN32)
    auto parent = std::filesystem::path(path_).parent_path();
    if (parent.empty()) parent = ".";
#if defined(O_DIRECTORY) && defined(O_CLOEXEC)
    int dir_fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
#elif defined(O_DIRECTORY)
    int dir_fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY);
#else
    int dir_fd = ::open(parent.c_str(), O_RDONLY);
#endif
    if (dir_fd >= 0) {
        while (::fsync(dir_fd) != 0 && errno == EINTR) {}
        ::close(dir_fd);
    }
#endif

    if (fd_ >= 0) {
        ::close(fd_);
    }
    fd_ = new_fd;
    buffer_len_ = 0;

    (void)old_bytes;
    return true;
}

} // namespace l3kvg
