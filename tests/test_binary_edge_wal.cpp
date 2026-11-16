#include <gtest/gtest.h>
#include "L3KVG/BinaryEdgeWAL.hpp"
#include "L3KVG/AdjacencyStore.hpp"
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>
#include <atomic>

namespace {

class TempWALFile {
public:
    explicit TempWALFile(std::string path) : path_(std::move(path)) {
        cleanup();
    }

    ~TempWALFile() {
        cleanup();
    }

    [[nodiscard]] const std::string& path() const noexcept {
        return path_;
    }

    void cleanup() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

private:
    std::string path_;
};

} // namespace

TEST(BinaryEdgeWALTest, StructLayoutAndTraits) {
    EXPECT_EQ(sizeof(l3kvg::EdgeMutationOp), 32);
    EXPECT_EQ(alignof(l3kvg::EdgeMutationOp), 8);
    EXPECT_TRUE(std::is_trivially_copyable_v<l3kvg::EdgeMutationOp>);
    EXPECT_TRUE(std::is_standard_layout_v<l3kvg::EdgeMutationOp>);

    l3kvg::EdgeMutationOp op{};
    EXPECT_EQ(op.src_id, 0);
    EXPECT_EQ(op.dst_id, 0);
    EXPECT_EQ(op.hlc_timestamp, 0);
    EXPECT_EQ(op.label_id, 0);
    EXPECT_EQ(op.flags, 0);
    EXPECT_FLOAT_EQ(op.weight, 1.0f);
    EXPECT_FALSE(op.is_delete());
    EXPECT_FALSE(op.is_active());

    op.set_active(true);
    EXPECT_TRUE(op.is_active());
    EXPECT_FALSE(op.is_delete());

    op.set_delete(true);
    EXPECT_TRUE(op.is_delete());

    op.set_active(false);
    EXPECT_FALSE(op.is_active());
    EXPECT_TRUE(op.is_delete());

    op.set_delete(false);
    EXPECT_FALSE(op.is_delete());

    l3kvg::EdgeMutationOp op1{
        .src_id = 10,
        .dst_id = 20,
        .hlc_timestamp = 100,
        .label_id = 1,
        .flags = l3kvg::EdgeMutationOp::FLAG_ACTIVE,
        .weight = 2.5f
    };
    l3kvg::EdgeMutationOp op2 = op1;
    EXPECT_EQ(op1, op2);

    op2.dst_id = 21;
    EXPECT_NE(op1, op2);
}

TEST(BinaryEdgeWALTest, AppendAndRecover) {
    TempWALFile temp_file("test_wal_append_recover.bin");

    {
        l3kvg::BinaryEdgeWAL wal(temp_file.path());
        EXPECT_TRUE(wal.is_open());

        l3kvg::EdgeMutationOp op1{
            .src_id = 100,
            .dst_id = 200,
            .hlc_timestamp = 1000,
            .label_id = 1,
            .flags = l3kvg::EdgeMutationOp::FLAG_ACTIVE,
            .weight = 1.5f
        };
        l3kvg::EdgeMutationOp op2{
            .src_id = 100,
            .dst_id = 300,
            .hlc_timestamp = 1001,
            .label_id = 2,
            .flags = l3kvg::EdgeMutationOp::FLAG_ACTIVE,
            .weight = 2.5f
        };
        l3kvg::EdgeMutationOp op3{
            .src_id = 200,
            .dst_id = 300,
            .hlc_timestamp = 1002,
            .label_id = 3,
            .flags = l3kvg::EdgeMutationOp::FLAG_ACTIVE,
            .weight = 3.5f
        };

        wal.append(op1);
        wal.append(op2);
        wal.append(op3);
        wal.flush();
        wal.close();
        EXPECT_FALSE(wal.is_open());
    }

    l3kvg::AdjacencyStore store;
    {
        l3kvg::BinaryEdgeWAL wal(temp_file.path());
        size_t recovered = wal.recover(store);
        EXPECT_EQ(recovered, 3);
    }

    // Verify topology in recovered store
    EXPECT_EQ(store.get_out_degree(100), 2);
    EXPECT_EQ(store.get_in_degree(200), 1);
    EXPECT_EQ(store.get_out_degree(200), 1);
    EXPECT_EQ(store.get_in_degree(300), 2);

    auto edges = store.get_outgoing_edges_snapshot(100);
    ASSERT_EQ(edges.size(), 2);
    EXPECT_EQ(edges[0].target_id, 200);
    EXPECT_EQ(edges[0].label_id, 1);
    EXPECT_FLOAT_EQ(edges[0].weight, 1.5f);
    EXPECT_EQ(edges[0].hlc_timestamp, 1000u);
    EXPECT_EQ(edges[1].target_id, 300);
    EXPECT_EQ(edges[1].label_id, 2);
    EXPECT_FLOAT_EQ(edges[1].weight, 2.5f);
    EXPECT_EQ(edges[1].hlc_timestamp, 1001u);

    auto out_200 = store.get_outgoing_edges_snapshot(200);
    ASSERT_EQ(out_200.size(), 1);
    EXPECT_EQ(out_200[0].target_id, 300);
    EXPECT_EQ(out_200[0].label_id, 3);
    EXPECT_FLOAT_EQ(out_200[0].weight, 3.5f);
}

TEST(BinaryEdgeWALTest, BatchAppendAndRecovery) {
    TempWALFile temp_file("test_wal_batch_append.bin");
    constexpr size_t NUM_OPS = 5000;

    std::vector<l3kvg::EdgeMutationOp> ops;
    ops.reserve(NUM_OPS);

    for (size_t i = 0; i < NUM_OPS; ++i) {
        l3kvg::EdgeMutationOp op{
            .src_id = static_cast<uint64_t>(i + 1),
            .dst_id = static_cast<uint64_t>((i + 1) * 10),
            .hlc_timestamp = 10000 + static_cast<uint64_t>(i),
            .label_id = static_cast<uint16_t>((i % 16) + 1),
            .flags = l3kvg::EdgeMutationOp::FLAG_ACTIVE,
            .weight = static_cast<float>(i + 1) * 0.5f
        };
        ops.push_back(op);
    }

    {
        l3kvg::BinaryEdgeWAL wal(temp_file.path());
        wal.append_batch(ops);
        wal.flush();
        wal.close();
    }

    l3kvg::AdjacencyStore store;
    {
        l3kvg::BinaryEdgeWAL wal(temp_file.path());
        size_t recovered = wal.recover(store);
        EXPECT_EQ(recovered, NUM_OPS);
    }

    // Verify edge samples across the entire 5000 ops range
    EXPECT_EQ(store.get_out_degree(1), 1);
    EXPECT_EQ(store.get_in_degree(10), 1);
    auto e1 = store.get_outgoing_edges_snapshot(1);
    ASSERT_EQ(e1.size(), 1);
    EXPECT_EQ(e1[0].target_id, 10);
    EXPECT_EQ(e1[0].label_id, 1);
    EXPECT_FLOAT_EQ(e1[0].weight, 0.5f);

    EXPECT_EQ(store.get_out_degree(2500), 1);
    EXPECT_EQ(store.get_in_degree(25000), 1);

    EXPECT_EQ(store.get_out_degree(5000), 1);
    EXPECT_EQ(store.get_in_degree(50000), 1);
}

TEST(BinaryEdgeWALTest, DeletionTombstones) {
    TempWALFile temp_file("test_wal_deletion_tombstones.bin");

    {
        l3kvg::BinaryEdgeWAL wal(temp_file.path());

        // Add 3 edges for src 10
        wal.append(l3kvg::EdgeMutationOp{
            .src_id = 10,
            .dst_id = 20,
            .hlc_timestamp = 100,
            .label_id = 1,
            .flags = l3kvg::EdgeMutationOp::FLAG_ACTIVE,
            .weight = 1.0f
        });
        wal.append(l3kvg::EdgeMutationOp{
            .src_id = 10,
            .dst_id = 30,
            .hlc_timestamp = 101,
            .label_id = 2,
            .flags = l3kvg::EdgeMutationOp::FLAG_ACTIVE,
            .weight = 2.0f
        });
        wal.append(l3kvg::EdgeMutationOp{
            .src_id = 10,
            .dst_id = 40,
            .hlc_timestamp = 102,
            .label_id = 3,
            .flags = l3kvg::EdgeMutationOp::FLAG_ACTIVE,
            .weight = 3.0f
        });

        // Delete edge 10 -> 20 (label 1) using wildcard weight (-1.0f)
        l3kvg::EdgeMutationOp del_op{
            .src_id = 10,
            .dst_id = 20,
            .hlc_timestamp = 103,
            .label_id = 1,
            .flags = l3kvg::EdgeMutationOp::OP_DELETE,
            .weight = -1.0f
        };
        EXPECT_TRUE(del_op.is_delete());
        wal.append(del_op);

        wal.flush();
        wal.close();
    }

    l3kvg::AdjacencyStore store;
    {
        l3kvg::BinaryEdgeWAL wal(temp_file.path());
        size_t recovered = wal.recover(store);
        EXPECT_EQ(recovered, 4); // 3 adds + 1 delete
    }

    // Verify that edge (10 -> 20) was tombstoned
    EXPECT_EQ(store.get_out_degree(10), 2);
    EXPECT_EQ(store.get_in_degree(20), 0);
    EXPECT_EQ(store.get_in_degree(30), 1);
    EXPECT_EQ(store.get_in_degree(40), 1);

    auto out_edges = store.get_outgoing_edges_snapshot(10);
    ASSERT_EQ(out_edges.size(), 2);
    for (const auto& e : out_edges) {
        EXPECT_NE(e.target_id, 20);
        EXPECT_TRUE(e.is_active());
    }
}

TEST(BinaryEdgeWALTest, TruncatedTailRecovery) {
    TempWALFile temp_file("test_wal_truncated_tail.bin");

    constexpr size_t NUM_VALID = 25;
    {
        l3kvg::BinaryEdgeWAL wal(temp_file.path());
        for (size_t i = 0; i < NUM_VALID; ++i) {
            wal.append(l3kvg::EdgeMutationOp{
                .src_id = static_cast<uint64_t>(100 + i),
                .dst_id = static_cast<uint64_t>(200 + i),
                .hlc_timestamp = static_cast<uint64_t>(500 + i),
                .label_id = 1,
                .flags = l3kvg::EdgeMutationOp::FLAG_ACTIVE,
                .weight = 1.0f
            });
        }
        wal.flush();
        wal.close();
    }

    // Append 10 bytes of truncated garbage tail
    {
        std::ofstream stream(temp_file.path(), std::ios::binary | std::ios::app);
        const char garbage[10] = {'\xDE', '\xAD', '\xBE', '\xEF', 0x1, 0x2, 0x3, 0x4, 0x5, 0x6};
        stream.write(garbage, 10);
        stream.flush();
    }

    // File size should be 25 * 32 + 10 = 810 bytes
    EXPECT_EQ(std::filesystem::file_size(temp_file.path()), NUM_VALID * sizeof(l3kvg::EdgeMutationOp) + 10);

    l3kvg::AdjacencyStore store;
    {
        l3kvg::BinaryEdgeWAL wal(temp_file.path());
        size_t recovered = wal.recover(store);
        EXPECT_EQ(recovered, NUM_VALID);
    }

    for (size_t i = 0; i < NUM_VALID; ++i) {
        EXPECT_EQ(store.get_out_degree(100 + i), 1);
        EXPECT_EQ(store.get_in_degree(200 + i), 1);
    }
}

TEST(BinaryEdgeWALTest, MultiThreadedAppends) {
    TempWALFile temp_file("test_wal_multithreaded.bin");

    constexpr size_t NUM_THREADS = 4;
    constexpr size_t OPS_PER_THREAD = 250;
    constexpr size_t TOTAL_OPS = NUM_THREADS * OPS_PER_THREAD;

    {
        l3kvg::BinaryEdgeWAL wal(temp_file.path());

        std::vector<std::thread> threads;
        threads.reserve(NUM_THREADS);

        for (size_t t = 0; t < NUM_THREADS; ++t) {
            threads.emplace_back([&wal, t]() {
                for (size_t i = 0; i < OPS_PER_THREAD; ++i) {
                    uint64_t src = (t + 1) * 100000 + i;
                    uint64_t dst = src + 1;
                    l3kvg::EdgeMutationOp op{
                        .src_id = src,
                        .dst_id = dst,
                        .hlc_timestamp = static_cast<uint64_t>(t * 1000 + i),
                        .label_id = static_cast<uint16_t>(t + 1),
                        .flags = l3kvg::EdgeMutationOp::FLAG_ACTIVE,
                        .weight = static_cast<float>(t + 1)
                    };
                    wal.append(op);
                }
            });
        }

        for (auto& th : threads) {
            th.join();
        }

        wal.flush();
        wal.close();
    }

    l3kvg::AdjacencyStore store;
    {
        l3kvg::BinaryEdgeWAL wal(temp_file.path());
        size_t recovered = wal.recover(store);
        EXPECT_EQ(recovered, TOTAL_OPS);
    }

    // Verify all 1,000 ops are present in store
    for (size_t t = 0; t < NUM_THREADS; ++t) {
        for (size_t i = 0; i < OPS_PER_THREAD; ++i) {
            uint64_t src = (t + 1) * 100000 + i;
            uint64_t dst = src + 1;
            EXPECT_EQ(store.get_out_degree(src), 1);
            EXPECT_EQ(store.get_in_degree(dst), 1);
        }
    }
}

TEST(BinaryEdgeWALTest, MultiChunkRecoveryWithTail) {
    TempWALFile temp_file("test_wal_multichunk_tail.bin");

    // 2048 ops = 64KB (exact chunk boundary). Let's write 2500 ops (crosses 64KB boundary)
    constexpr size_t NUM_OPS = 2500;
    {
        l3kvg::BinaryEdgeWAL wal(temp_file.path());
        std::vector<l3kvg::EdgeMutationOp> batch;
        batch.reserve(NUM_OPS);

        for (size_t i = 0; i < NUM_OPS; ++i) {
            batch.push_back(l3kvg::EdgeMutationOp{
                .src_id = static_cast<uint64_t>(i + 1),
                .dst_id = static_cast<uint64_t>((i + 1) * 2),
                .hlc_timestamp = static_cast<uint64_t>(i),
                .label_id = 1,
                .flags = l3kvg::EdgeMutationOp::FLAG_ACTIVE,
                .weight = 1.0f
            });
        }
        wal.append_batch(batch);
        wal.flush();
        wal.close();
    }

    // Append 17 bytes of corrupted trailing bytes
    {
        std::ofstream stream(temp_file.path(), std::ios::binary | std::ios::app);
        std::string garbage(17, '\xAB');
        stream.write(garbage.data(), garbage.size());
        stream.flush();
    }

    l3kvg::AdjacencyStore store;
    {
        l3kvg::BinaryEdgeWAL wal(temp_file.path());
        size_t recovered = wal.recover(store);
        EXPECT_EQ(recovered, NUM_OPS);
    }

    EXPECT_EQ(store.get_out_degree(1), 1);
    EXPECT_EQ(store.get_out_degree(2048), 1);
    EXPECT_EQ(store.get_out_degree(2500), 1);
}

TEST(BinaryEdgeWALTest, MoveSemantics) {
    TempWALFile temp_file("test_wal_move.bin");

    l3kvg::BinaryEdgeWAL wal1(temp_file.path());
    EXPECT_TRUE(wal1.is_open());

    wal1.append(l3kvg::EdgeMutationOp{
        .src_id = 1,
        .dst_id = 2,
        .hlc_timestamp = 10,
        .label_id = 1,
        .flags = l3kvg::EdgeMutationOp::FLAG_ACTIVE,
        .weight = 1.0f
    });

    // Move construct
    l3kvg::BinaryEdgeWAL wal2(std::move(wal1));
    EXPECT_FALSE(wal1.is_open());
    EXPECT_TRUE(wal2.is_open());

    wal2.append(l3kvg::EdgeMutationOp{
        .src_id = 2,
        .dst_id = 3,
        .hlc_timestamp = 20,
        .label_id = 1,
        .flags = l3kvg::EdgeMutationOp::FLAG_ACTIVE,
        .weight = 1.0f
    });

    // Move assign
    TempWALFile temp_file2("test_wal_move2.bin");
    l3kvg::BinaryEdgeWAL wal3(temp_file2.path());
    wal3 = std::move(wal2);
    EXPECT_FALSE(wal2.is_open());
    EXPECT_TRUE(wal3.is_open());

    wal3.append(l3kvg::EdgeMutationOp{
        .src_id = 3,
        .dst_id = 4,
        .hlc_timestamp = 30,
        .label_id = 1,
        .flags = l3kvg::EdgeMutationOp::FLAG_ACTIVE,
        .weight = 1.0f
    });
    wal3.flush();
    wal3.close();

    l3kvg::AdjacencyStore store;
    l3kvg::BinaryEdgeWAL wal_read(temp_file.path());
    size_t recovered = wal_read.recover(store);
    EXPECT_EQ(recovered, 3);
    EXPECT_EQ(store.get_out_degree(1), 1);
    EXPECT_EQ(store.get_out_degree(2), 1);
    EXPECT_EQ(store.get_out_degree(3), 1);
}

TEST(BinaryEdgeWALTest, OperationsAfterClose) {
    TempWALFile temp_file("test_wal_closed.bin");

    l3kvg::BinaryEdgeWAL wal(temp_file.path());
    wal.close();
    EXPECT_FALSE(wal.is_open());

    l3kvg::EdgeMutationOp op{
        .src_id = 1,
        .dst_id = 2,
        .hlc_timestamp = 1,
        .label_id = 1,
        .flags = l3kvg::EdgeMutationOp::FLAG_ACTIVE,
        .weight = 1.0f
    };

    EXPECT_THROW(wal.append(op), std::runtime_error);
    std::vector<l3kvg::EdgeMutationOp> batch = {op};
    EXPECT_THROW(wal.append_batch(batch), std::runtime_error);
    EXPECT_THROW(wal.flush(), std::runtime_error);

    // Calling close multiple times should be safe no-op
    EXPECT_NO_THROW(wal.close());
}

TEST(BinaryEdgeWALTest, RecoverWhileOpen) {
    TempWALFile temp_file("test_wal_open_recover.bin");

    l3kvg::BinaryEdgeWAL wal(temp_file.path());
    wal.append(l3kvg::EdgeMutationOp{
        .src_id = 50,
        .dst_id = 60,
        .hlc_timestamp = 1,
        .label_id = 1,
        .flags = l3kvg::EdgeMutationOp::FLAG_ACTIVE,
        .weight = 1.0f
    });

    l3kvg::AdjacencyStore store1;
    size_t rec1 = wal.recover(store1);
    EXPECT_EQ(rec1, 1);
    EXPECT_EQ(store1.get_out_degree(50), 1);

    // WAL remains open and functional
    EXPECT_TRUE(wal.is_open());
    wal.append(l3kvg::EdgeMutationOp{
        .src_id = 50,
        .dst_id = 70,
        .hlc_timestamp = 2,
        .label_id = 2,
        .flags = l3kvg::EdgeMutationOp::FLAG_ACTIVE,
        .weight = 2.0f
    });

    l3kvg::AdjacencyStore store2;
    size_t rec2 = wal.recover(store2);
    EXPECT_EQ(rec2, 2);
    EXPECT_EQ(store2.get_out_degree(50), 2);
}

TEST(BinaryEdgeWALTest, EmptyWALRecovery) {
    TempWALFile temp_file("test_wal_empty.bin");

    {
        l3kvg::BinaryEdgeWAL wal(temp_file.path());
        wal.close();
    }

    l3kvg::AdjacencyStore store;
    l3kvg::BinaryEdgeWAL wal(temp_file.path());
    size_t rec = wal.recover(store);
    EXPECT_EQ(rec, 0);
}

TEST(BinaryEdgeWALTest, RecoveryDoesNotWritePhantomRecordsOrZeroBuffer) {
    TempWALFile temp_file("test_wal_no_phantom_write.bin");

    {
        l3kvg::BinaryEdgeWAL wal(temp_file.path());
        wal.append(l3kvg::EdgeMutationOp{
            .src_id = 10,
            .dst_id = 20,
            .hlc_timestamp = 100,
            .label_id = 1,
            .flags = l3kvg::EdgeMutationOp::FLAG_ACTIVE,
            .weight = 1.0f
        });

        l3kvg::AdjacencyStore store1;
        size_t recovered = wal.recover(store1);
        EXPECT_EQ(recovered, 1);
        EXPECT_EQ(store1.get_out_degree(10), 1);

        // Verify file size on disk is exactly 32 bytes (1 record), NOT 64KB (65536 bytes)
        auto file_size = std::filesystem::file_size(temp_file.path());
        EXPECT_EQ(file_size, sizeof(l3kvg::EdgeMutationOp));

        // Subsequent append must work without heap corruption or buffer capacity issues
        wal.append(l3kvg::EdgeMutationOp{
            .src_id = 10,
            .dst_id = 30,
            .hlc_timestamp = 200,
            .label_id = 1,
            .flags = l3kvg::EdgeMutationOp::FLAG_ACTIVE,
            .weight = 2.0f
        });
        wal.flush();

        file_size = std::filesystem::file_size(temp_file.path());
        EXPECT_EQ(file_size, 2 * sizeof(l3kvg::EdgeMutationOp));

        l3kvg::AdjacencyStore store2;
        size_t rec2 = wal.recover(store2);
        EXPECT_EQ(rec2, 2);
        EXPECT_EQ(store2.get_out_degree(10), 2);
    }
}
