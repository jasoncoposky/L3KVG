#include "L3KVG/LabelRegistry.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <filesystem>
#include <limits>
#include <stdexcept>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#define close _close
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace l3kvg {

static std::atomic<uint64_t> s_next_registry_instance_id{1};

LabelRegistry::LabelRegistry()
    : instance_id_(s_next_registry_instance_id.fetch_add(1, std::memory_order_relaxed)) {}

uint16_t LabelRegistry::get_or_register(std::string_view label) {
    if (label.empty()) {
        return 0;
    }

    thread_local const LabelRegistry* tl_last_registry = nullptr;
    thread_local uint64_t tl_last_instance_id = 0;
    thread_local std::string tl_last_label;
    thread_local uint16_t tl_last_id = 0;
    if (tl_last_registry == this && tl_last_instance_id == instance_id_ && tl_last_id != 0 && tl_last_label == label) {
        return tl_last_id;
    }

    {
        std::shared_lock read_lock(mutex_);
        if (auto it = label_to_id_.find(label); it != label_to_id_.end()) {
            tl_last_registry = this;
            tl_last_instance_id = instance_id_;
            tl_last_label = label;
            tl_last_id = it->second;
            return it->second;
        }
    }

    std::unique_lock write_lock(mutex_);
    if (auto it = label_to_id_.find(label); it != label_to_id_.end()) {
        tl_last_registry = this;
        tl_last_instance_id = instance_id_;
        tl_last_label = label;
        tl_last_id = it->second;
        return it->second;
    }

    if (id_to_label_.size() > std::numeric_limits<uint16_t>::max()) {
        throw std::overflow_error("LabelRegistry capacity exceeded (maximum 65535 labels supported)");
    }

    uint16_t new_id = static_cast<uint16_t>(id_to_label_.size());
    id_to_label_.emplace_back(label);
    std::string_view stored_view = id_to_label_.back();
    label_to_id_[stored_view] = new_id;
    if (!dict_path_.empty()) {
        persist_unlocked();
    }
    tl_last_registry = this;
    tl_last_instance_id = instance_id_;
    tl_last_label = label;
    tl_last_id = new_id;
    return new_id;
}

void LabelRegistry::set_dict_path(std::string path) {
    std::unique_lock write_lock(mutex_);
    dict_path_ = std::move(path);
}

bool LabelRegistry::persist() const noexcept {
    std::unique_lock write_lock(mutex_);
    return persist_unlocked();
}

bool LabelRegistry::persist_unlocked() const noexcept {
    if (dict_path_.empty()) {
        return true;
    }

    std::string content;
    content.reserve(id_to_label_.size() * 16);
    for (size_t i = 1; i < id_to_label_.size(); ++i) {
        content.append(id_to_label_[i]);
        content.push_back('\n');
    }

    std::filesystem::path p(dict_path_);
    if (p.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
    }

    const std::string tmp_path = dict_path_ + ".tmp";
#if defined(_WIN32)
    int fd = ::_open(tmp_path.c_str(), _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, _S_IREAD | _S_IWRITE);
#elif defined(O_CLOEXEC)
    int fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
#else
    int fd = ::open(tmp_path.c_str(), O_WRONLY | _O_CREAT | _O_TRUNC, 0644);
#endif
    if (fd < 0) {
        return false;
    }

    bool ok = true;
    size_t written = 0;
    while (written < content.size()) {
#if defined(_WIN32)
        unsigned int chunk = static_cast<unsigned int>(std::min<size_t>(content.size() - written, static_cast<size_t>(UINT_MAX)));
        int res = ::_write(fd, content.data() + written, chunk);
#else
        ssize_t res = ::write(fd, content.data() + written, content.size() - written);
#endif
        if (res < 0) {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
        if (res == 0) {
            ok = false;
            break;
        }
        written += static_cast<size_t>(res);
    }

    if (ok) {
#if defined(_WIN32)
        if (::_commit(fd) != 0) {
            ok = false;
        }
#else
        while (::fsync(fd) != 0) {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
#endif
    }
    if (::close(fd) != 0 && errno != EINTR) {
        ok = false;
    }

    std::error_code ec;
    if (!ok) {
        std::filesystem::remove(tmp_path, ec);
        return false;
    }

    std::filesystem::rename(tmp_path, dict_path_, ec);
    if (ec) {
        std::filesystem::remove(tmp_path, ec);
        return false;
    }

#if !defined(_WIN32)
    auto parent = p.parent_path();
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

    return true;
}

uint16_t LabelRegistry::lookup(std::string_view label) const noexcept {
    if (label.empty()) {
        return 0;
    }
    thread_local const LabelRegistry* tl_last_registry = nullptr;
    thread_local uint64_t tl_last_instance_id = 0;
    thread_local std::string tl_last_label;
    thread_local uint16_t tl_last_id = 0;
    if (tl_last_registry == this && tl_last_instance_id == instance_id_ && tl_last_id != 0 && tl_last_label == label) {
        return tl_last_id;
    }

    std::shared_lock read_lock(mutex_);
    if (auto it = label_to_id_.find(label); it != label_to_id_.end()) {
        tl_last_registry = this;
        tl_last_instance_id = instance_id_;
        tl_last_label = label;
        tl_last_id = it->second;
        return it->second;
    }
    return 0;
}

std::string_view LabelRegistry::get_label(uint16_t id) const noexcept {
    std::shared_lock read_lock(mutex_);
    if (id == 0 || id >= id_to_label_.size()) {
        return "";
    }
    return id_to_label_[id];
}

size_t LabelRegistry::size() const noexcept {
    std::shared_lock read_lock(mutex_);
    return id_to_label_.empty() ? 0 : (id_to_label_.size() - 1);
}

} // namespace l3kvg
