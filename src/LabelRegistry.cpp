#include "L3KVG/LabelRegistry.hpp"
#include <limits>
#include <stdexcept>
#include <atomic>

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
    tl_last_registry = this;
    tl_last_instance_id = instance_id_;
    tl_last_label = label;
    tl_last_id = new_id;
    return new_id;
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
