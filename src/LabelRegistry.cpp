#include "L3KVG/LabelRegistry.hpp"

namespace l3kvg {

uint16_t LabelRegistry::get_or_register(std::string_view label) {
    if (label.empty()) {
        return 0;
    }

    {
        std::shared_lock read_lock(mutex_);
        if (auto it = label_to_id_.find(std::string(label)); it != label_to_id_.end()) {
            return it->second;
        }
    }

    std::unique_lock write_lock(mutex_);
    if (auto it = label_to_id_.find(std::string(label)); it != label_to_id_.end()) {
        return it->second;
    }

    uint16_t new_id = static_cast<uint16_t>(id_to_label_.size());
    std::string str(label);
    id_to_label_.push_back(str);
    label_to_id_[std::move(str)] = new_id;
    return new_id;
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
