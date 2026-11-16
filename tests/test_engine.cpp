#include "L3KVG/Engine.hpp"
#include "L3KVG/Node.hpp"
#include "L3KVG/Query.hpp"
#include "L3KVG/MutationBatch.hpp"
#include "L3KVG/KeyBuilder.hpp"
#include "engine/store.hpp"
#include "buffer.hpp"
#include <gtest/gtest.h>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

namespace {
struct ScopedTestDir {
  std::string path;
  explicit ScopedTestDir(std::string p) : path(std::move(p)) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
  ~ScopedTestDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};
} // namespace

TEST(EngineTest, FormatWeight) {
  std::string w1 = l3kvg::Engine::format_weight(0.85);
  std::string w2 = l3kvg::Engine::format_weight(1.0);
  std::string w3 = l3kvg::Engine::format_weight(0.05);

  EXPECT_EQ(w1, "0000000.8500");
  EXPECT_EQ(w2, "0000001.0000");
  EXPECT_EQ(w3, "0000000.0500");

  // Lexicographical sorting check
  EXPECT_TRUE(w3 < w1);
  EXPECT_TRUE(w1 < w2);
}

TEST(EngineTest, NodeZeroCopyAttrAndConcurrentAccessAndNoZeroDegreeFallback) {
  using namespace lite3cpp::literals;
  ScopedTestDir dir("test_wave2b_node_zerocopy_db");
  l3kvg::Settings settings;
  l3kvg::Engine engine(dir.path, 1, nullptr, 4, settings);

  // Build nested _binary Lite3 payload
  lite3cpp::Buffer nested_buf;
  nested_buf.init_object();
  nested_buf.set_str(0, "nested_str"_hk, "irods_nested_value");
  nested_buf.set_i64(0, "nested_i64"_hk, 9876543210LL);
  nested_buf.set_f64(0, "nested_f64"_hk, 3.14159);
  nested_buf.set_bool(0, "nested_bool"_hk, true);

  lite3cpp::Buffer root_buf;
  root_buf.init_object();
  root_buf.set_str(0, "root_str"_hk, "root_hello");
  root_buf.set_i64(0, "root_i64"_hk, 42);
  root_buf.set_f64(0, "root_f64"_hk, 2.71828);
  root_buf.set_bool(0, "root_bool"_hk, false);
  root_buf.set_bytes(0, "_binary"_hk,
                     std::span<const std::byte>(
                         reinterpret_cast<const std::byte*>(nested_buf.data()),
                         nested_buf.size()));
  size_t props_ofs = root_buf.set_obj(0, "properties"_hk);
  root_buf.set_str(props_ofs, "prop_str"_hk, "from_properties_obj");
  root_buf.set_i64(props_ofs, "prop_i64"_hk, -777);

  const uint64_t hub_id = 0x1001;
  const uint64_t out_n1 = 0x1002;
  const uint64_t out_n2 = 0x1003;
  const uint64_t in_n1 = 0x1004;
  const uint64_t isolated_id = 0x1099;

  engine.put_node(hub_id, std::string(reinterpret_cast<const char*>(root_buf.data()), root_buf.size()));
  engine.put_node(isolated_id, R"({"name":"isolated"})");
  engine.add_edge(hub_id, "CONTAINS", 1.0, out_n1);
  engine.add_edge(hub_id, "CONTAINS", 2.0, out_n2);
  engine.add_edge(in_n1, "PARENT_OF", 1.0, hub_id);

  // Plant raw KV edge keys bypassing Engine (simulating phantom KV scan trap):
  // In pure native mode (!engine.has_legacy_kv_edges()), 0-degree get_in_neighbors / get_neighbors
  // MUST NOT scan store_->get_prefix_chunk("e:...")!
  EXPECT_FALSE(engine.has_legacy_kv_edges());
  const std::string trap_in_key(l3kvg::KeyBuilder::edge_in_key(isolated_id, "CONTAINS", hub_id));
  const std::string trap_out_key(l3kvg::KeyBuilder::edge_out_key(isolated_id, "CONTAINS", 1.0, hub_id));
  engine.get_store()->apply_put(trap_in_key, "{}");
  engine.get_store()->apply_put(trap_out_key, "{}");

  auto iso_node = engine.get_node(isolated_id);
  ASSERT_NE(iso_node, nullptr);
  EXPECT_TRUE(iso_node->get_in_neighbors("CONTAINS").empty());
  EXPECT_TRUE(iso_node->get_in_neighbors("NON_EXISTENT_OR_EMPTY").empty());
  EXPECT_TRUE(iso_node->get_neighbors("CONTAINS").empty());
  EXPECT_TRUE(iso_node->get_neighbors("NON_EXISTENT_OR_EMPTY").empty());

  auto cached_node = engine.get_node(hub_id);
  ASSERT_NE(cached_node, nullptr);

  // Verify zero-copy pointer identity against underlying L3KV Blob view
  auto blob = engine.get_store()->get_blob(l3kvg::KeyBuilder::node_key(hub_id));
  ASSERT_NE(blob, nullptr);
  const char* blob_begin = reinterpret_cast<const char*>(blob->view().data());
  const char* blob_end = blob_begin + blob->view().size();

  auto root_sv_var = cached_node->try_get_attr("root_str"_hk);
  ASSERT_TRUE(std::holds_alternative<std::string_view>(root_sv_var));
  std::string_view root_sv = std::get<std::string_view>(root_sv_var);
  EXPECT_EQ(root_sv, "root_hello");
  EXPECT_GE(root_sv.data(), blob_begin);
  EXPECT_LT(root_sv.data(), blob_end);

  auto nested_sv_var = cached_node->try_get_attr("nested_str"_hk);
  ASSERT_TRUE(std::holds_alternative<std::string_view>(nested_sv_var));
  std::string_view nested_sv = std::get<std::string_view>(nested_sv_var);
  EXPECT_EQ(nested_sv, "irods_nested_value");
  EXPECT_GE(nested_sv.data(), blob_begin);
  EXPECT_LT(nested_sv.data(), blob_end);

  // 8 threads hammering the SAME cached std::shared_ptr<Node>
  std::atomic<int> failures{0};
  std::vector<std::thread> workers;
  workers.reserve(8);
  for (int t = 0; t < 8; ++t) {
    workers.emplace_back([&cached_node, &failures, out_n1, out_n2, in_n1]() {
      for (int iter = 0; iter < 300; ++iter) {
        auto v_i64 = cached_node->try_get_attr("root_i64"_hk);
        if (!std::holds_alternative<int64_t>(v_i64) || std::get<int64_t>(v_i64) != 42) {
          failures.fetch_add(1, std::memory_order_relaxed);
        }
        auto v_f64 = cached_node->try_get_attr("nested_f64"_hk);
        if (!std::holds_alternative<double>(v_f64) || std::abs(std::get<double>(v_f64) - 3.14159) > 1e-9) {
          failures.fetch_add(1, std::memory_order_relaxed);
        }
        auto v_bool = cached_node->try_get_attr("nested_bool"_hk);
        if (!std::holds_alternative<bool>(v_bool) || !std::get<bool>(v_bool)) {
          failures.fetch_add(1, std::memory_order_relaxed);
        }
        auto v_prop = cached_node->try_get_attr(std::string_view("prop_str"));
        if (!std::holds_alternative<std::string_view>(v_prop) ||
            std::get<std::string_view>(v_prop) != "from_properties_obj") {
          failures.fetch_add(1, std::memory_order_relaxed);
        }
        auto v_prop_i64 = cached_node->try_get_attr("prop_i64"_hk);
        if (!std::holds_alternative<int64_t>(v_prop_i64) || std::get<int64_t>(v_prop_i64) != -777) {
          failures.fetch_add(1, std::memory_order_relaxed);
        }
        auto v_miss = cached_node->try_get_attr("does_not_exist"_hk);
        if (!std::holds_alternative<std::monostate>(v_miss)) {
          failures.fetch_add(1, std::memory_order_relaxed);
        }
        if (!cached_node->has_attribute("nested_str") ||
            cached_node->get_attribute_as_string("nested_i64") != "9876543210") {
          failures.fetch_add(1, std::memory_order_relaxed);
        }
        auto outs = cached_node->get_neighbors("CONTAINS", 0.5);
        if (outs.size() != 2 ||
            !((outs[0] == out_n1 && outs[1] == out_n2) || (outs[0] == out_n2 && outs[1] == out_n1))) {
          failures.fetch_add(1, std::memory_order_relaxed);
        }
        auto ins = cached_node->get_in_neighbors("PARENT_OF");
        if (ins.size() != 1 || ins[0] != in_n1) {
          failures.fetch_add(1, std::memory_order_relaxed);
        }
        if (!cached_node->get_in_neighbors("EMPTY_LABEL").empty()) {
          failures.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  for (auto& w : workers) w.join();
  EXPECT_EQ(failures.load(), 0);
}

TEST(EngineTest, QueryStartIndexPrefixFlatRowsAndNumericFilters) {
  using namespace lite3cpp::literals;
  ScopedTestDir dir("test_wave2b_query_flat_db");
  l3kvg::Settings settings;
  l3kvg::Engine engine(dir.path, 1, nullptr, 4, settings);

  auto format_hex = [](uint64_t id) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(id));
    return std::string(buf);
  };

  // Seed 3 DataObject nodes under /tempZone/home/rods and 1 under /tempZone/home/other
  const uint64_t id0 = 0x2000;
  const uint64_t id1 = 0x2001;
  const uint64_t id2 = 0x2002;
  const uint64_t id_other = 0x2099;

  engine.put_node(id0, R"({"entity_type":"data_object","n":"obj0","s":100,"tag":"alpha"})");
  engine.put_node(id1, R"({"entity_type":"data_object","n":"obj1","s":200,"tag":"beta"})");
  engine.put_node(id2, R"({"entity_type":"data_object","n":"obj2","s":50,"tag":"gamma"})");
  engine.put_node(id_other, R"({"entity_type":"data_object","n":"other_obj","s":999,"tag":"delta"})");

  l3kvg::MutationBatch batch;
  batch.put_raw("idx:DataObject:pn:/tempZone/home/rods:obj0", format_hex(id0));
  batch.put_raw("idx:DataObject:pn:/tempZone/home/rods:obj1", format_hex(id1));
  batch.put_raw("idx:DataObject:pn:/tempZone/home/rods:obj2", format_hex(id2));
  batch.put_raw("idx:DataObject:pn:/tempZone/home/other:other_obj", format_hex(id_other));
  batch.put_raw("idx:DataObject:id:" + format_hex(id_other), format_hex(id_other));
  ASSERT_TRUE(engine.apply_batch(batch.get_buffer()));

  // Build query_buf with start_index_prefix + flat_rows + numeric Ge filter (s >= 100)
  lite3cpp::Buffer qbuf;
  qbuf.init_object();
  qbuf.set_str(0, "root_alias"_hk, "DataObject");
  qbuf.set_str(0, "start_index_prefix"_hk, "idx:DataObject:pn:/tempZone/home/rods:");
  qbuf.set_bool(0, "flat_rows"_hk, true);

  size_t projs_ofs = qbuf.set_arr(0, "projections"_hk);
  {
    size_t p0 = qbuf.arr_append_obj(projs_ofs);
    qbuf.set_str(p0, "alias"_hk, "DataObject");
    qbuf.set_str(p0, "property"_hk, "n");
    qbuf.set_i64(p0, "agg"_hk, 0);
    qbuf.set_bool(p0, "distinct"_hk, false);

    size_t p1 = qbuf.arr_append_obj(projs_ofs);
    qbuf.set_str(p1, "alias"_hk, "DataObject");
    qbuf.set_str(p1, "property"_hk, "s");
    qbuf.set_i64(p1, "agg"_hk, 0);
    qbuf.set_bool(p1, "distinct"_hk, false);
  }
  size_t filters_ofs = qbuf.set_arr(0, "filters"_hk);
  {
    size_t f0 = qbuf.arr_append_obj(filters_ofs);
    qbuf.set_str(f0, "prepended_op"_hk, "and");
    qbuf.set_str(f0, "alias"_hk, "DataObject");
    qbuf.set_str(f0, "key"_hk, "s");
    qbuf.set_i64(f0, "op"_hk, static_cast<int64_t>(l3kvg::Query::Op::Ge));
    qbuf.set_str(f0, "value"_hk, "100");
  }

  lite3cpp::Buffer flat_buf = engine.query()
      .set_populate_fields(false)
      .resume({}, qbuf)
      .execute_to_buffer();

  ASSERT_GE(flat_buf.size(), sizeof(lite3cpp::PackedNodeLayout));
  lite3cpp::NodeView root_nv(reinterpret_cast<const lite3cpp::PackedNodeLayout*>(flat_buf.data()));
  ASSERT_EQ(root_nv.type(), lite3cpp::Type::Array);
  ASSERT_EQ(root_nv.size(), 2u);

  // Deterministic ID order (id0 = 0x2000, id1 = 0x2001): [["obj0", "100"], ["obj1", "200"]]
  ASSERT_EQ(flat_buf.arr_get_type(0, 0), lite3cpp::Type::Array);
  size_t r0 = flat_buf.arr_get_arr(0, 0);
  EXPECT_EQ(flat_buf.arr_get_str(r0, 0), "obj0");
  EXPECT_EQ(flat_buf.arr_get_str(r0, 1), "100");

  ASSERT_EQ(flat_buf.arr_get_type(0, 1), lite3cpp::Type::Array);
  size_t r1 = flat_buf.arr_get_arr(0, 1);
  EXPECT_EQ(flat_buf.arr_get_str(r1, 0), "obj1");
  EXPECT_EQ(flat_buf.arr_get_str(r1, 1), "200");

  // Empty start_index_prefix match MUST return 0 rows immediately without falling back to idx:DataObject:id:*
  lite3cpp::Buffer empty_qbuf;
  empty_qbuf.init_object();
  empty_qbuf.set_str(0, "root_alias"_hk, "DataObject");
  empty_qbuf.set_str(0, "start_index_prefix"_hk, "idx:DataObject:pn:/tempZone/home/nonexistent:");
  empty_qbuf.set_bool(0, "flat_rows"_hk, true);
  size_t ep_ofs = empty_qbuf.set_arr(0, "projections"_hk);
  size_t ep0 = empty_qbuf.arr_append_obj(ep_ofs);
  empty_qbuf.set_str(ep0, "alias"_hk, "DataObject");
  empty_qbuf.set_str(ep0, "property"_hk, "n");

  lite3cpp::Buffer empty_flat = engine.query().set_populate_fields(false).resume({}, empty_qbuf).execute_to_buffer();
  ASSERT_GE(empty_flat.size(), sizeof(lite3cpp::PackedNodeLayout));
  lite3cpp::NodeView empty_nv(reinterpret_cast<const lite3cpp::PackedNodeLayout*>(empty_flat.data()));
  EXPECT_EQ(empty_nv.type(), lite3cpp::Type::Array);
  EXPECT_EQ(empty_nv.size(), 0u);

  // Verify Gt / Lt / Le numeric comparisons (note: "50" < "100" numerically even though "50" > "100" lexicographically!)
  auto lt_rows = engine.query()
      .set_start_index_prefix("idx:DataObject:pn:/tempZone/home/rods:")
      .match("DataObject")
      .where("DataObject", "s", l3kvg::Query::Op::Lt, "100")
      .return_("DataObject", "n")
      .execute();
  ASSERT_EQ(lt_rows.size(), 1u);
  EXPECT_EQ(lt_rows[0].projected_values[0], "obj2");

  // Non-numeric Gt/Lt comparison does not throw exceptions
  auto str_gt_rows = engine.query()
      .set_start_index_prefix("idx:DataObject:pn:/tempZone/home/rods:")
      .match("DataObject")
      .where("DataObject", "tag", l3kvg::Query::Op::Gt, "beta")
      .return_("DataObject", "n")
      .execute();
  ASSERT_EQ(str_gt_rows.size(), 1u);
  EXPECT_EQ(str_gt_rows[0].projected_values[0], "obj2");
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
