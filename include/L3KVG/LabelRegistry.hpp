#pragma once
#include <string>
#include <string_view>
#include <unordered_map>
#include <deque>
#include <shared_mutex>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <cstddef>

namespace l3kvg {

class LabelRegistry {
public:
    LabelRegistry();
    ~LabelRegistry() = default;
    LabelRegistry(const LabelRegistry&) = delete;
    LabelRegistry& operator=(const LabelRegistry&) = delete;
    LabelRegistry(LabelRegistry&&) = delete;
    LabelRegistry& operator=(LabelRegistry&&) = delete;

    uint16_t get_or_register(std::string_view label);
    [[nodiscard]] uint16_t lookup(std::string_view label) const noexcept;
    [[nodiscard]] std::string_view get_label(uint16_t id) const noexcept;
    [[nodiscard]] size_t size() const noexcept;
    [[nodiscard]] uint64_t instance_id() const noexcept { return instance_id_; }

private:
    uint64_t instance_id_{0};
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string_view, uint16_t> label_to_id_;
    std::deque<std::string> id_to_label_{""}; // 1-indexed (0 reserved for NONE)
};

} // namespace l3kvg
