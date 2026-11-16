#include "L3KVG/MutationBatch.hpp"

namespace l3kvg {

using namespace lite3cpp::literals;

MutationBatch::MutationBatch() {
    clear();
}

void MutationBatch::clear() {
    buf_.clear();
    buf_.init_object();
    arr_ofs_ = buf_.set_arr(0, "m"_hk);
    count_ = 0;
}

void MutationBatch::put_raw(std::string_view key, std::string_view value) {
    size_t ofs = buf_.arr_append_obj(arr_ofs_);
    buf_.set_i64(ofs, "op"_hk, static_cast<int64_t>(MutationOp::PutRaw));
    buf_.set_str(ofs, "k"_hk, key);
    buf_.set_str(ofs, "v"_hk, value);
    count_++;
}

void MutationBatch::put_node(uint64_t node_id, std::string_view payload) {
    size_t ofs = buf_.arr_append_obj(arr_ofs_);
    buf_.set_i64(ofs, "op"_hk, static_cast<int64_t>(MutationOp::PutNode));
    buf_.set_i64(ofs, "s"_hk, static_cast<int64_t>(node_id));
    buf_.set_str(ofs, "v"_hk, payload);
    count_++;
}

void MutationBatch::put_node(uint64_t node_id, const lite3cpp::Buffer& payload) {
    put_node(node_id, std::string_view(reinterpret_cast<const char*>(payload.data()), payload.size()));
}

void MutationBatch::add_edge(uint64_t src, std::string_view label, double weight, uint64_t dst, std::string_view payload) {
    size_t ofs = buf_.arr_append_obj(arr_ofs_);
    buf_.set_i64(ofs, "op"_hk, static_cast<int64_t>(MutationOp::AddEdge));
    buf_.set_i64(ofs, "s"_hk, static_cast<int64_t>(src));
    buf_.set_i64(ofs, "d"_hk, static_cast<int64_t>(dst));
    buf_.set_str(ofs, "l"_hk, label);
    buf_.set_f64(ofs, "w"_hk, weight);
    if (!payload.empty()) {
        buf_.set_str(ofs, "v"_hk, payload);
    }
    count_++;
}

void MutationBatch::add_index(std::string_view key, std::string_view value_hex) {
    put_raw(key, value_hex);
}

void MutationBatch::inc_node_int(uint64_t node_id, std::string_view key, int64_t delta) {
    size_t ofs = buf_.arr_append_obj(arr_ofs_);
    buf_.set_i64(ofs, "op"_hk, static_cast<int64_t>(MutationOp::IncNodeInt));
    buf_.set_i64(ofs, "s"_hk, static_cast<int64_t>(node_id));
    buf_.set_str(ofs, "k"_hk, key);
    buf_.set_i64(ofs, "i"_hk, delta);
    count_++;
}

void MutationBatch::del_raw(std::string_view key) {
    size_t ofs = buf_.arr_append_obj(arr_ofs_);
    buf_.set_i64(ofs, "op"_hk, static_cast<int64_t>(MutationOp::DelRaw));
    buf_.set_str(ofs, "k"_hk, key);
    count_++;
}

void MutationBatch::del_node(uint64_t node_id) {
    size_t ofs = buf_.arr_append_obj(arr_ofs_);
    buf_.set_i64(ofs, "op"_hk, static_cast<int64_t>(MutationOp::DelNode));
    buf_.set_i64(ofs, "s"_hk, static_cast<int64_t>(node_id));
    count_++;
}

void MutationBatch::del_edge(uint64_t src, std::string_view label, double weight, uint64_t dst) {
    size_t ofs = buf_.arr_append_obj(arr_ofs_);
    buf_.set_i64(ofs, "op"_hk, static_cast<int64_t>(MutationOp::DelEdge));
    buf_.set_i64(ofs, "s"_hk, static_cast<int64_t>(src));
    buf_.set_i64(ofs, "d"_hk, static_cast<int64_t>(dst));
    buf_.set_str(ofs, "l"_hk, label);
    buf_.set_f64(ofs, "w"_hk, weight);
    count_++;
}

size_t MutationBatch::item_count(const lite3cpp::Buffer& buf) {
    if (buf.size() < sizeof(lite3cpp::PackedNodeLayout)) return 0;
    if (auto arr_ofs = buf.try_get_arr(0, "m"_hk)) {
        if (*arr_ofs + sizeof(lite3cpp::PackedNodeLayout) <= buf.size()) {
            lite3cpp::NodeView nv(reinterpret_cast<const lite3cpp::PackedNodeLayout*>(buf.data() + *arr_ofs));
            return nv.size();
        }
    }
    if (auto c = buf.try_get_i64(0, "c"_hk)) {
        return static_cast<size_t>(*c);
    }
    return 0;
}

MutationItem MutationBatch::read_item_at_offset(const lite3cpp::Buffer& buf, size_t ofs) noexcept {
    MutationItem item;
    if (auto op = buf.try_get_i64(ofs, "op"_hk)) {
        item.op = static_cast<MutationOp>(*op);
    }
    if (auto k = buf.try_get_str(ofs, "k"_hk)) {
        item.key = *k;
    }
    if (auto v = buf.try_get_str(ofs, "v"_hk)) {
        item.value = *v;
    }
    if (auto s = buf.try_get_i64(ofs, "s"_hk)) {
        item.src = static_cast<uint64_t>(*s);
    } else if (auto s_legacy = buf.try_get_i64(ofs, "src"_hk)) {
        item.src = static_cast<uint64_t>(*s_legacy);
    }
    if (auto d = buf.try_get_i64(ofs, "d"_hk)) {
        item.dst = static_cast<uint64_t>(*d);
    } else if (auto d_legacy = buf.try_get_i64(ofs, "dst"_hk)) {
        item.dst = static_cast<uint64_t>(*d_legacy);
    }
    if (auto l = buf.try_get_str(ofs, "l"_hk)) {
        item.label = *l;
    } else if (auto l_legacy = buf.try_get_str(ofs, "lbl"_hk)) {
        item.label = *l_legacy;
    }
    if (auto w = buf.try_get_f64(ofs, "w"_hk)) {
        item.weight = *w;
    }
    if (auto inc = buf.try_get_i64(ofs, "i"_hk)) {
        item.int_delta = *inc;
    }
    return item;
}

MutationItem MutationBatch::read_item(const lite3cpp::Buffer& buf, size_t index) {
    size_t arr_ofs = buf.get_arr(0, "m"_hk);
    size_t ofs = buf.arr_get_obj(arr_ofs, static_cast<uint32_t>(index));
    return read_item_at_offset(buf, ofs);
}

MutationItem MutationBatch::get(size_t index) const {
    return read_item(buf_, index);
}

} // namespace l3kvg
