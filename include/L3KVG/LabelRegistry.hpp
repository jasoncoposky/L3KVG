#pragma once
#include <string>
#include <string_view>
#include <unordered_map>
#include <deque>
#include <shared_mutex>
#include <mutex>
#include <cstdint>
#include <cstddef>

namespace l3kvg {

class LabelRegistry {
public:
    LabelRegistry() = default;

    uint16_t get_or_register(std::string_view label);
    [[nodiscard]] std::string_view get_label(uint16_t id) const noexcept;
    [[nodiscard]] size_t size() const noexcept;

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, uint16_t> label_to_id_;
    std::deque<std::string> id_to_label_{""}; // 1-indexed (0 reserved for NONE)
};

} // namespace l3kvg
