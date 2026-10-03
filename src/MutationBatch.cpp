#include "L3KVG/MutationBatch.hpp"

namespace l3kvg {

MutationBatch::MutationBatch() {
    buf_.init_array();
}

void MutationBatch::clear() {
    buf_ = lite3cpp::Buffer();
    buf_.init_array();
    count_ = 0;
}

void MutationBatch::put_raw(std::string_view key, std::string_view value) {
    size_t ofs = buf_.arr_append_obj(0);
    buf_.set_i64(ofs, "op", static_cast<int64_t>(MutationOp::PutRaw));
    buf_.set_str(ofs, "k", key);
    buf_.set_str(ofs, "v", value);
    count_++;
}

void MutationBatch::put_node(uint64_t node_id, std::string_view payload) {
    size_t ofs = buf_.arr_append_obj(0);
    buf_.set_i64(ofs, "op", static_cast<int64_t>(MutationOp::PutNode));
    buf_.set_i64(ofs, "src", static_cast<int64_t>(node_id));
    buf_.set_str(ofs, "v", payload);
    count_++;
}

void MutationBatch::add_edge(uint64_t src, std::string_view label, double weight, uint64_t dst, std::string_view payload) {
    size_t ofs = buf_.arr_append_obj(0);
    buf_.set_i64(ofs, "op", static_cast<int64_t>(MutationOp::AddEdge));
    buf_.set_i64(ofs, "src", static_cast<int64_t>(src));
    buf_.set_i64(ofs, "dst", static_cast<int64_t>(dst));
    buf_.set_str(ofs, "lbl", label);
    buf_.set_f64(ofs, "w", weight);
    buf_.set_str(ofs, "v", payload);
    count_++;
}

void MutationBatch::add_index(std::string_view key, std::string_view value_hex) {
    put_raw(key, value_hex);
}

void MutationBatch::del_raw(std::string_view key) {
    size_t ofs = buf_.arr_append_obj(0);
    buf_.set_i64(ofs, "op", static_cast<int64_t>(MutationOp::DelRaw));
    buf_.set_str(ofs, "k", key);
    count_++;
}

void MutationBatch::del_node(uint64_t node_id) {
    size_t ofs = buf_.arr_append_obj(0);
    buf_.set_i64(ofs, "op", static_cast<int64_t>(MutationOp::DelNode));
    buf_.set_i64(ofs, "src", static_cast<int64_t>(node_id));
    count_++;
}

void MutationBatch::del_edge(uint64_t src, std::string_view label, double weight, uint64_t dst) {
    size_t ofs = buf_.arr_append_obj(0);
    buf_.set_i64(ofs, "op", static_cast<int64_t>(MutationOp::DelEdge));
    buf_.set_i64(ofs, "src", static_cast<int64_t>(src));
    buf_.set_i64(ofs, "dst", static_cast<int64_t>(dst));
    buf_.set_str(ofs, "lbl", label);
    buf_.set_f64(ofs, "w", weight);
    count_++;
}

MutationItem MutationBatch::read_item(const lite3cpp::Buffer& buf, size_t index) {
    size_t ofs = buf.arr_get_obj(0, static_cast<uint32_t>(index));
    MutationItem item;
    item.op = static_cast<MutationOp>(buf.get_i64(ofs, "op"));
    if (buf.get_type(ofs, "k") == lite3cpp::Type::String) {
        item.key = buf.get_str(ofs, "k");
    }
    if (buf.get_type(ofs, "v") == lite3cpp::Type::String) {
        item.value = buf.get_str(ofs, "v");
    }
    if (buf.get_type(ofs, "src") == lite3cpp::Type::Int64) {
        item.src = static_cast<uint64_t>(buf.get_i64(ofs, "src"));
    }
    if (buf.get_type(ofs, "dst") == lite3cpp::Type::Int64) {
        item.dst = static_cast<uint64_t>(buf.get_i64(ofs, "dst"));
    }
    if (buf.get_type(ofs, "lbl") == lite3cpp::Type::String) {
        item.label = buf.get_str(ofs, "lbl");
    }
    if (buf.get_type(ofs, "w") == lite3cpp::Type::Float64) {
        item.weight = buf.get_f64(ofs, "w");
    }
    return item;
}

MutationItem MutationBatch::get(size_t index) const {
    return read_item(buf_, index);
}

} // namespace l3kvg
