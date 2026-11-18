#pragma once

#include <string_view>
#include <string>
#include <cstdint>
#include "buffer.hpp"

namespace l3kvg {

enum class MutationOp : uint8_t {
    PutRaw = 0,
    PutNode = 1,
    AddEdge = 2,
    DelRaw = 3,
    DelNode = 4,
    DelEdge = 5,
    IncNodeInt = 6,
    CasRaw = 7
};

struct MutationItem {
    MutationOp op = MutationOp::PutRaw;
    std::string_view key;
    std::string_view value;
    uint64_t src = 0;
    uint64_t dst = 0;
    std::string_view label;
    double weight = 1.0;
    int64_t int_delta = 0;
    uint64_t expected_hash = 0;
    int64_t ttl_ms = 0;
};

class MutationBatch {
public:
    MutationBatch();

    void put_raw(std::string_view key, std::string_view value);
    void cas_raw(std::string_view key, std::string_view value, uint64_t expected_hash, int64_t ttl_ms = 0);
    void cas_raw(std::string_view key, const lite3cpp::Buffer& value, uint64_t expected_hash, int64_t ttl_ms = 0);
    void put_node(uint64_t node_id, std::string_view payload);
    void put_node(uint64_t node_id, const lite3cpp::Buffer& payload);
    void add_edge(uint64_t src, std::string_view label, double weight, uint64_t dst, std::string_view payload = "");
    void add_index(std::string_view key, std::string_view value_hex);
    void inc_node_int(uint64_t node_id, std::string_view key, int64_t delta);
    
    void del_raw(std::string_view key);
    void del_node(uint64_t node_id);
    void del_edge(uint64_t src, std::string_view label, double weight, uint64_t dst);

    [[nodiscard]] MutationItem get(size_t index) const;
    static MutationItem read_item(const lite3cpp::Buffer& buf, size_t index);
    static MutationItem read_item_at_offset(const lite3cpp::Buffer& buf, size_t item_ofs) noexcept;
    static size_t item_count(const lite3cpp::Buffer& buf);

    [[nodiscard]] const lite3cpp::Buffer& get_buffer() const noexcept { return buf_; }
    [[nodiscard]] size_t size() const noexcept { return count_; }
    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
    void clear();

private:
    lite3cpp::Buffer buf_;
    size_t arr_ofs_ = 0;
    size_t count_ = 0;
};

} // namespace l3kvg
