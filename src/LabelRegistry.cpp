#include "L3KVG/LabelRegistry.hpp"
#include <limits>
#include <stdexcept>

namespace l3kvg {

uint16_t LabelRegistry::get_or_register(std::string_view label) {
    if (label.empty()) {
        return 0;
    }

    {
        std::shared_lock read_lock(mutex_);
        if (auto it = label_to_id_.find(label); it != label_to_id_.end()) {
            return it->second;
        }
    }

    std::unique_lock write_lock(mutex_);
    if (auto it = label_to_id_.find(label); it != label_to_id_.end()) {
        return it->second;
    }

    if (id_to_label_.size() > std::numeric_limits<uint16_t>::max()) {
        throw std::overflow_error("LabelRegistry capacity exceeded (maximum 65535 labels supported)");
    }

    uint16_t new_id = static_cast<uint16_t>(id_to_label_.size());
    id_to_label_.emplace_back(label);
    std::string_view stored_view = id_to_label_.back();
    label_to_id_[stored_view] = new_id;
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
