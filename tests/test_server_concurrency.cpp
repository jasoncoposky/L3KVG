#include <gtest/gtest.h>
#include "L3KVG/Engine.hpp"
#include "L3KVG/Node.hpp"
#include "L3KVG/RemoteL3KVClient.hpp"
#include "L3KVG/MutationBatch.hpp"
#include "L3KVG/Query.hpp"
#include "engine/store.hpp"
#include "buffer.hpp"
#include <zmq.hpp>
#include <zmq_addon.hpp>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <future>
#include <algorithm>

namespace {

static std::atomic<uint16_t> g_test_port{9850};

class ConcurrentServer {
public:
    ConcurrentServer(uint16_t port, const std::string& db_path, uint32_t num_workers = 4)
        : port_(port), db_path_(db_path), num_workers_(num_workers) {}

    ~ConcurrentServer() {
        stop();
    }

    void start() {
        std::filesystem::remove_all(db_path_);
        l3kvg::Settings settings;
        settings.node_id = 1;
        engine_ = std::make_unique<l3kvg::Engine>(db_path_, 1, nullptr, 4, settings);

        ctx_ = std::make_unique<zmq::context_t>(1);

        frontend_ = std::make_unique<zmq::socket_t>(*ctx_, zmq::socket_type::router);
        frontend_->set(zmq::sockopt::linger, 0);
        frontend_->bind("tcp://127.0.0.1:" + std::to_string(port_));

        inproc_addr_ = "inproc://workers_" + std::to_string(port_);
        backend_ = std::make_unique<zmq::socket_t>(*ctx_, zmq::socket_type::dealer);
        backend_->set(zmq::sockopt::linger, 0);
        backend_->bind(inproc_addr_);

        running_.store(true, std::memory_order_relaxed);

        workers_.reserve(num_workers_);
        for (uint32_t i = 0; i < num_workers_; ++i) {
            workers_.emplace_back([this]() {
                worker_loop();
            });
        }

        proxy_thread_ = std::thread([this]() {
            try {
                zmq::proxy(*frontend_, *backend_);
            } catch (...) {}
        });

        flusher_thread_ = std::thread([this]() {
            while (running_.load(std::memory_order_relaxed)) {
                std::unique_lock<std::mutex> lock(flusher_mu_);
                flusher_cv_.wait_for(lock, std::chrono::milliseconds(50), [this] {
                    return !running_.load(std::memory_order_relaxed);
                });
                if (!running_.load(std::memory_order_relaxed)) break;
                try {
                    if (engine_ && engine_->get_store()) {
                        engine_->get_store()->flush();
                    }
                } catch (...) {}
            }
        });
    }

    void stop() {
        if (!running_.exchange(false)) return;
        flusher_cv_.notify_all();

        try {
            if (ctx_) {
                ctx_->shutdown();
            }
        } catch (...) {}

        if (proxy_thread_.joinable()) proxy_thread_.join();
        for (auto& w : workers_) {
            if (w.joinable()) w.join();
        }
        workers_.clear();
        if (flusher_thread_.joinable()) flusher_thread_.join();

        try {
            if (frontend_) { frontend_->close(); frontend_.reset(); }
            if (backend_) { backend_->close(); backend_.reset(); }
            if (ctx_) { ctx_->close(); ctx_.reset(); }
        } catch (...) {}

        if (engine_) {
            try { engine_->flush(); } catch (...) {}
        }
        std::filesystem::remove_all(db_path_);
    }

    l3kvg::Engine* engine() { return engine_.get(); }
    uint16_t port() const { return port_; }

private:
    void worker_loop() {
        try {
            zmq::socket_t worker_sock(*ctx_, zmq::socket_type::dealer);
            worker_sock.set(zmq::sockopt::rcvtimeo, 250);
            worker_sock.set(zmq::sockopt::linger, 0);
            worker_sock.connect(inproc_addr_);

            while (running_.load(std::memory_order_relaxed)) {
                std::vector<zmq::message_t> recv_msgs;
                try {
                    auto res = zmq::recv_multipart(worker_sock, std::back_inserter(recv_msgs));
                    if (!res || recv_msgs.empty()) continue;
                } catch (const zmq::error_t& e) {
                    if (e.num() == ETERM) break;
                    continue;
                } catch (...) {
                    break;
                }
                handle_request(worker_sock, recv_msgs);
            }
        } catch (...) {}
    }

    void handle_request(zmq::socket_t& sock, std::vector<zmq::message_t>& recv_msgs) {
        if (recv_msgs.size() < 4) return;

        auto identity = std::move(recv_msgs[0]);
        size_t data_idx = 1;
        if (recv_msgs[data_idx].size() == 0) data_idx++; // Skip delimiter

        uint32_t principal_id = 0;
        if (data_idx < recv_msgs.size() && recv_msgs[data_idx].size() == 4) {
            std::memcpy(&principal_id, recv_msgs[data_idx].data(), 4);
            data_idx++;
        }

        if (data_idx >= recv_msgs.size()) return;

        std::string_view opcode(static_cast<const char*>(recv_msgs[data_idx].data()), recv_msgs[data_idx].size());
        data_idx++;

        if (opcode == "+") {
            if (data_idx + 1 > recv_msgs.size()) {
                sock.send(identity, zmq::send_flags::sndmore);
                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                sock.send(zmq::message_t("ERR", 3), zmq::send_flags::none);
                return;
            }
            std::string key = recv_msgs[data_idx].to_string(); data_idx++;
            int64_t delta = 1;
            if (data_idx < recv_msgs.size()) {
                delta = std::stoll(recv_msgs[data_idx].to_string());
                data_idx++;
            }

            try {
                static std::mutex inc_locks[64];
                size_t lidx = std::hash<std::string>{}(key) % 64;
                std::lock_guard<std::mutex> lock(inc_locks[lidx]);

                auto buf = engine_->get_store()->get(key, principal_id);
                int64_t val = 0;
                if (buf.size() > 0) {
                    val = buf.get_i64(0, "v");
                }
                val += delta;

                lite3cpp::Buffer nbuf; nbuf.init_object(); nbuf.set_i64(0, "v", val);
                engine_->get_store()->put(key, nbuf.move_to_string(), principal_id);

                sock.send(identity, zmq::send_flags::sndmore);
                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                sock.send(zmq::message_t(std::to_string(val)), zmq::send_flags::none);
            } catch (...) {
                sock.send(identity, zmq::send_flags::sndmore);
                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                sock.send(zmq::message_t("ERR", 3), zmq::send_flags::none);
            }
            return;
        }

        if (opcode == "P") {
            if (data_idx + 2 > recv_msgs.size()) {
                sock.send(identity, zmq::send_flags::sndmore);
                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                sock.send(zmq::message_t("ERR", 3), zmq::send_flags::none);
                return;
            }
            try {
                std::string key = recv_msgs[data_idx].to_string(); data_idx++;
                std::string payload = recv_msgs[data_idx].to_string();

                if (key.starts_with("e:out:{")) {
                    uint64_t src = 0, dst = 0; double weight = 0; char label_buf[256] = {0};
                    int parsed = std::sscanf(key.c_str(), "e:out:{%llx\x7d:%255[^:]:%lf:{%llx\x7d", (unsigned long long*)&src, label_buf, &weight, (unsigned long long*)&dst);
                    if (parsed == 4) {
                        engine_->add_edge(src, label_buf, weight, dst, std::move(payload));
                    } else {
                        engine_->get_store()->put(key, std::move(payload), principal_id);
                    }
                } else if (key.starts_with("n:{")) {
                    uint64_t id = 0;
                    int parsed = std::sscanf(key.c_str(), "n:{%llx\x7d", (unsigned long long*)&id);
                    if (parsed == 1) {
                        engine_->put_node(id, std::move(payload));
                    } else {
                        engine_->get_store()->put(key, std::move(payload), principal_id);
                    }
                } else {
                    engine_->get_store()->put(key, std::move(payload), principal_id);
                }

                sock.send(identity, zmq::send_flags::sndmore);
                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                sock.send(zmq::message_t("OK", 2), zmq::send_flags::none);
            } catch (...) {
                sock.send(identity, zmq::send_flags::sndmore);
                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                sock.send(zmq::message_t("ERR", 3), zmq::send_flags::none);
            }
            return;
        }

        if (opcode == "G") {
            if (data_idx + 1 > recv_msgs.size()) {
                sock.send(identity, zmq::send_flags::sndmore);
                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                sock.send(zmq::message_t("", 0), zmq::send_flags::none);
                return;
            }
            try {
                std::string key = recv_msgs[data_idx].to_string();
                auto buf = engine_->get_store()->get(key, principal_id);
                sock.send(identity, zmq::send_flags::sndmore);
                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                sock.send(zmq::message_t(buf.data(), buf.size()), zmq::send_flags::none);
            } catch (...) {
                sock.send(identity, zmq::send_flags::sndmore);
                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                sock.send(zmq::message_t("", 0), zmq::send_flags::none);
            }
            return;
        }

        if (opcode == "B") {
            if (data_idx + 1 > recv_msgs.size()) {
                sock.send(identity, zmq::send_flags::sndmore);
                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                sock.send(zmq::message_t("ERR", 3), zmq::send_flags::none);
                return;
            }
            try {
                const auto& msg = recv_msgs[data_idx];
                lite3cpp::Buffer batch_buf(static_cast<const uint8_t*>(msg.data()), msg.size());
                bool ok = engine_->apply_batch(batch_buf, principal_id);
                sock.send(identity, zmq::send_flags::sndmore);
                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                sock.send(zmq::message_t(ok ? "OK" : "ERR", ok ? 2 : 3), zmq::send_flags::none);
            } catch (...) {
                sock.send(identity, zmq::send_flags::sndmore);
                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                sock.send(zmq::message_t("ERR", 3), zmq::send_flags::none);
            }
            return;
        }

        if (opcode == "E") {
            if (data_idx + 4 > recv_msgs.size()) {
                sock.send(identity, zmq::send_flags::sndmore);
                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                sock.send(zmq::message_t("ERR", 3), zmq::send_flags::none);
                return;
            }
            try {
                uint64_t src = std::stoull(recv_msgs[data_idx].to_string(), nullptr, 16); data_idx++;
                std::string label = recv_msgs[data_idx].to_string(); data_idx++;
                double weight = std::stod(recv_msgs[data_idx].to_string()); data_idx++;
                uint64_t dst = std::stoull(recv_msgs[data_idx].to_string(), nullptr, 16);
                engine_->add_edge(src, label, weight, dst);
                sock.send(identity, zmq::send_flags::sndmore);
                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                sock.send(zmq::message_t("OK", 2), zmq::send_flags::none);
            } catch (...) {
                sock.send(identity, zmq::send_flags::sndmore);
                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                sock.send(zmq::message_t("ERR", 3), zmq::send_flags::none);
            }
            return;
        }

        if (opcode == "N") {
            if (data_idx + 3 > recv_msgs.size()) {
                sock.send(identity, zmq::send_flags::sndmore);
                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                sock.send(zmq::message_t("ERR", 3), zmq::send_flags::none);
                return;
            }
            try {
                std::string id_str = recv_msgs[data_idx].to_string(); data_idx++;
                std::string label = recv_msgs[data_idx].to_string(); data_idx++;
                std::string w_str = recv_msgs[data_idx].to_string();
                uint64_t target_node_id = std::stoull(id_str, nullptr, 16);
                double min_weight = std::stod(w_str);

                auto node = engine_->get_node(target_node_id);
                std::vector<uint64_t> neighs;
                if (node) neighs = node->get_neighbors(label, min_weight, principal_id);

                sock.send(identity, zmq::send_flags::sndmore);
                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                sock.send(zmq::message_t(neighs.data(), neighs.size() * sizeof(uint64_t)), zmq::send_flags::none);
            } catch (...) {
                sock.send(identity, zmq::send_flags::sndmore);
                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                sock.send(zmq::message_t("ERR", 3), zmq::send_flags::none);
            }
            return;
        }

        if (opcode == "K") {
            if (data_idx + 1 > recv_msgs.size()) {
                sock.send(identity, zmq::send_flags::sndmore);
                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                sock.send(zmq::message_t("ERR", 3), zmq::send_flags::none);
                return;
            }
            try {
                std::string prefix = recv_msgs[data_idx].to_string(); data_idx++;
                auto perm = engine_->get_store()->credentials().check_permission(principal_id, prefix);
                if (!(perm & l3kv::Permission::READ) && !(perm & l3kv::Permission::ADMIN)) {
                    sock.send(identity, zmq::send_flags::sndmore);
                    sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                    sock.send(zmq::message_t("ERR_AUTH", 8), zmq::send_flags::none);
                    return;
                }
                size_t limit = 100000;
                auto entries = engine_->get_store()->get_prefix_entries_all_shards(prefix, "", limit);
                lite3cpp::Buffer kbuf;
                kbuf.init_array();
                for (const auto& [k, v] : entries) {
                    size_t e = kbuf.arr_append_obj(0);
                    kbuf.set_str(e, "k", k);
                    kbuf.set_str(e, "v", v);
                }
                sock.send(identity, zmq::send_flags::sndmore);
                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                sock.send(zmq::message_t(kbuf.data(), kbuf.size()), zmq::send_flags::none);
            } catch (...) {
                sock.send(identity, zmq::send_flags::sndmore);
                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                sock.send(zmq::message_t("ERR", 3), zmq::send_flags::none);
            }
            return;
        }
    }

    uint16_t port_;
    std::string db_path_;
    uint32_t num_workers_;
    std::string inproc_addr_;
    std::unique_ptr<l3kvg::Engine> engine_;
    std::unique_ptr<zmq::context_t> ctx_;
    std::unique_ptr<zmq::socket_t> frontend_;
    std::unique_ptr<zmq::socket_t> backend_;
    std::vector<std::thread> workers_;
    std::thread proxy_thread_;
    std::thread flusher_thread_;
    std::mutex flusher_mu_;
    std::condition_variable flusher_cv_;
    std::atomic<bool> running_{false};
};

} // namespace

TEST(ServerConcurrencyTest, ParallelAtomicIncrements) {
    uint16_t port = g_test_port.fetch_add(1);
    std::string db = "test_conc_inc_" + std::to_string(port);
    ConcurrentServer server(port, db, 8);
    server.start();

    const int num_clients = 10;
    const int incs_per_client = 50;
    std::atomic<int> successful_responses{0};

    std::vector<std::thread> clients;
    clients.reserve(num_clients);

    for (int c = 0; c < num_clients; ++c) {
        clients.emplace_back([port, incs_per_client, &successful_responses]() {
            zmq::context_t client_ctx(1);
            zmq::socket_t sock(client_ctx, zmq::socket_type::dealer);
            sock.set(zmq::sockopt::rcvtimeo, 3000);
            sock.set(zmq::sockopt::linger, 0);
            sock.connect("tcp://127.0.0.1:" + std::to_string(port));

            for (int i = 0; i < incs_per_client; ++i) {
                // DEALER wire format: delimiter, pid, opcode, key, delta
                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                uint32_t pid = 0;
                sock.send(zmq::message_t(&pid, 4), zmq::send_flags::sndmore);
                sock.send(zmq::message_t("+", 1), zmq::send_flags::sndmore);
                sock.send(zmq::message_t("shared_counter", 14), zmq::send_flags::sndmore);
                sock.send(zmq::message_t("1", 1), zmq::send_flags::none);

                std::vector<zmq::message_t> resp;
                auto res = zmq::recv_multipart(sock, std::back_inserter(resp));
                if (res && resp.size() >= 2) {
                    std::string val_str = resp[1].to_string();
                    if (!val_str.empty() && val_str != "ERR") {
                        successful_responses.fetch_add(1);
                    }
                }
            }
            sock.close();
            client_ctx.close();
        });
    }

    for (auto& c : clients) {
        c.join();
    }

    EXPECT_EQ(successful_responses.load(), num_clients * incs_per_client);

    // Verify counter in engine
    auto buf = server.engine()->get_store()->get("shared_counter");
    ASSERT_GT(buf.size(), 0u);
    lite3cpp::Buffer lbuf(std::vector<uint8_t>(buf.data(), buf.data() + buf.size()));
    int64_t final_val = lbuf.get_i64(0, "v");
    EXPECT_EQ(final_val, num_clients * incs_per_client);

    server.stop();
}

TEST(ServerConcurrencyTest, ParallelPutAndGet) {
    uint16_t port = g_test_port.fetch_add(1);
    std::string db = "test_conc_pg_" + std::to_string(port);
    ConcurrentServer server(port, db, 8);
    server.start();

    const int num_clients = 8;
    const int keys_per_client = 30;
    std::atomic<int> puts_succeeded{0};
    std::atomic<int> gets_matched{0};

    // Phase 1: Parallel Puts
    std::vector<std::thread> put_clients;
    for (int c = 0; c < num_clients; ++c) {
        put_clients.emplace_back([port, c, keys_per_client, &puts_succeeded]() {
            zmq::context_t ctx(1);
            zmq::socket_t sock(ctx, zmq::socket_type::dealer);
            sock.set(zmq::sockopt::rcvtimeo, 3000);
            sock.set(zmq::sockopt::linger, 0);
            sock.connect("tcp://127.0.0.1:" + std::to_string(port));

            for (int k = 0; k < keys_per_client; ++k) {
                std::string key = "key_" + std::to_string(c) + "_" + std::to_string(k);
                std::string val = "payload_content_" + std::to_string(c) + "_" + std::to_string(k);

                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                uint32_t pid = 0;
                sock.send(zmq::message_t(&pid, 4), zmq::send_flags::sndmore);
                sock.send(zmq::message_t("P", 1), zmq::send_flags::sndmore);
                sock.send(zmq::message_t(key), zmq::send_flags::sndmore);
                sock.send(zmq::message_t(val), zmq::send_flags::none);

                std::vector<zmq::message_t> resp;
                auto res = zmq::recv_multipart(sock, std::back_inserter(resp));
                if (res && resp.size() >= 2 && resp[1].to_string() == "OK") {
                    puts_succeeded.fetch_add(1);
                }
            }
            sock.close();
            ctx.close();
        });
    }

    for (auto& t : put_clients) t.join();
    EXPECT_EQ(puts_succeeded.load(), num_clients * keys_per_client);

    // Phase 2: Parallel Gets
    std::vector<std::thread> get_clients;
    for (int c = 0; c < num_clients; ++c) {
        get_clients.emplace_back([port, c, keys_per_client, &gets_matched]() {
            zmq::context_t ctx(1);
            zmq::socket_t sock(ctx, zmq::socket_type::dealer);
            sock.set(zmq::sockopt::rcvtimeo, 3000);
            sock.set(zmq::sockopt::linger, 0);
            sock.connect("tcp://127.0.0.1:" + std::to_string(port));

            for (int k = 0; k < keys_per_client; ++k) {
                std::string key = "key_" + std::to_string(c) + "_" + std::to_string(k);
                std::string expected_val = "payload_content_" + std::to_string(c) + "_" + std::to_string(k);

                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                uint32_t pid = 0;
                sock.send(zmq::message_t(&pid, 4), zmq::send_flags::sndmore);
                sock.send(zmq::message_t("G", 1), zmq::send_flags::sndmore);
                sock.send(zmq::message_t(key), zmq::send_flags::none);

                std::vector<zmq::message_t> resp;
                auto res = zmq::recv_multipart(sock, std::back_inserter(resp));
                if (res && resp.size() >= 2 && resp[1].to_string() == expected_val) {
                    gets_matched.fetch_add(1);
                }
            }
            sock.close();
            ctx.close();
        });
    }

    for (auto& t : get_clients) t.join();
    EXPECT_EQ(gets_matched.load(), num_clients * keys_per_client);

    server.stop();
}

TEST(ServerConcurrencyTest, RemoteClientBatchConcurrency) {
    uint16_t port = g_test_port.fetch_add(1);
    std::string db = "test_conc_batch_" + std::to_string(port);
    ConcurrentServer server(port, db, 6);
    server.start();

    const int num_clients = 6;
    const int batches_per_client = 15;
    std::atomic<int> batches_ok{0};

    std::vector<std::thread> client_threads;
    for (int c = 0; c < num_clients; ++c) {
        client_threads.emplace_back([port, c, batches_per_client, &batches_ok]() {
            l3kvg::Settings settings;
            settings.fed_timeout_ms = 3000;
            l3kvg::RemoteL3KVClient client(settings);
            auto pool = std::make_shared<l3kvg::ThreadPool>(2);
            client.set_thread_pool(pool);
            client.add_peer(100 + c, "tcp://127.0.0.1:" + std::to_string(port));

            for (int b = 0; b < batches_per_client; ++b) {
                l3kvg::MutationBatch batch;
                uint64_t node_id = static_cast<uint64_t>(c) * 10000 + b;
                batch.put_node(node_id, "{\"label\":\"conc_node\"}");
                batch.put_raw("batch_key_" + std::to_string(c) + "_" + std::to_string(b), "data");

                auto fut = client.execute_batch_async(100 + c, batch);
                if (fut.get()) {
                    batches_ok.fetch_add(1);
                }
            }
        });
    }

    for (auto& t : client_threads) t.join();
    EXPECT_EQ(batches_ok.load(), num_clients * batches_per_client);

    server.stop();
}

TEST(ServerConcurrencyTest, ParallelGraphTraversals) {
    uint16_t port = g_test_port.fetch_add(1);
    std::string db = "test_conc_graph_" + std::to_string(port);
    ConcurrentServer server(port, db, 8);
    server.start();

    // Populate a root node with multiple edges
    uint64_t root_id = 0xAA00;
    server.engine()->put_node(root_id, "{\"type\":\"root\"}");
    for (uint64_t i = 1; i <= 20; ++i) {
        uint64_t child_id = 0xAA00 + i;
        server.engine()->put_node(child_id, "{\"type\":\"child\"}");
        server.engine()->add_edge(root_id, "knows", 1.0, child_id);
    }

    const int num_clients = 8;
    const int queries_per_client = 25;
    std::atomic<int> valid_neighbors_count{0};

    std::vector<std::thread> clients;
    for (int c = 0; c < num_clients; ++c) {
        clients.emplace_back([port, root_id, queries_per_client, &valid_neighbors_count]() {
            zmq::context_t ctx(1);
            zmq::socket_t sock(ctx, zmq::socket_type::dealer);
            sock.set(zmq::sockopt::rcvtimeo, 3000);
            sock.set(zmq::sockopt::linger, 0);
            sock.connect("tcp://127.0.0.1:" + std::to_string(port));

            for (int q = 0; q < queries_per_client; ++q) {
                // Opcode N: id_str, label, w_str
                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                uint32_t pid = 0;
                sock.send(zmq::message_t(&pid, 4), zmq::send_flags::sndmore);
                sock.send(zmq::message_t("N", 1), zmq::send_flags::sndmore);
                char id_hex[32];
                std::snprintf(id_hex, sizeof(id_hex), "%llx", (unsigned long long)root_id);
                sock.send(zmq::message_t(id_hex, std::strlen(id_hex)), zmq::send_flags::sndmore);
                sock.send(zmq::message_t("knows", 5), zmq::send_flags::sndmore);
                sock.send(zmq::message_t("0.0", 3), zmq::send_flags::none);

                std::vector<zmq::message_t> resp;
                auto res = zmq::recv_multipart(sock, std::back_inserter(resp));
                if (res && resp.size() >= 2) {
                    size_t count = resp[1].size() / sizeof(uint64_t);
                    if (count == 20) {
                        valid_neighbors_count.fetch_add(1);
                    }
                }
            }
            sock.close();
            ctx.close();
        });
    }

    for (auto& t : clients) t.join();
    EXPECT_EQ(valid_neighbors_count.load(), num_clients * queries_per_client);

    server.stop();
}

TEST(ServerConcurrencyTest, GracefulShutdownUnderLoad) {
    uint16_t port = g_test_port.fetch_add(1);
    std::string db = "test_conc_shutdown_" + std::to_string(port);
    auto server = std::make_unique<ConcurrentServer>(port, db, 4);
    server->start();

    std::atomic<bool> client_stop{false};
    std::vector<std::thread> traffic_threads;
    for (int i = 0; i < 4; ++i) {
        traffic_threads.emplace_back([port, &client_stop]() {
            try {
                zmq::context_t ctx(1);
                zmq::socket_t sock(ctx, zmq::socket_type::dealer);
                sock.set(zmq::sockopt::rcvtimeo, 200);
                sock.set(zmq::sockopt::linger, 0);
                sock.connect("tcp://127.0.0.1:" + std::to_string(port));

                while (!client_stop.load(std::memory_order_relaxed)) {
                    sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                    uint32_t pid = 0;
                    sock.send(zmq::message_t(&pid, 4), zmq::send_flags::sndmore);
                    sock.send(zmq::message_t("G", 1), zmq::send_flags::sndmore);
                    sock.send(zmq::message_t("some_key", 8), zmq::send_flags::none);

                    std::vector<zmq::message_t> resp;
                    try {
                        (void)zmq::recv_multipart(sock, std::back_inserter(resp));
                    } catch (...) {}
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                sock.close();
                ctx.close();
            } catch (...) {}
        });
    }

    // Allow some traffic to flow
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Stop server while traffic is running - should not deadlock or hang
    auto start_time = std::chrono::steady_clock::now();
    server->stop();
    auto stop_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_time
    ).count();

    // Verify stop took less than 3 seconds
    EXPECT_LT(stop_duration, 3000);

    // Stop clients
    client_stop.store(true, std::memory_order_relaxed);
    for (auto& t : traffic_threads) t.join();
}

TEST(ServerConcurrencyTest, ParallelPrefixScans) {
    uint16_t port = g_test_port.fetch_add(1);
    std::string db = "test_conc_prefix_" + std::to_string(port);
    ConcurrentServer server(port, db, 4);
    server.start();

    // Populate prefix entries and noise entries
    const int num_entries = 20;
    const std::string prefix = "idx:Collection:n:/zone/home/coll/";
    for (int i = 0; i < num_entries; ++i) {
        std::string k = prefix + "sub" + std::to_string(i);
        std::string v = std::to_string(1000 + i);
        server.engine()->get_store()->put(k, v);
    }
    // Noise entries that should not match prefix
    server.engine()->get_store()->put("idx:Collection:n:/zone/home/other", "2000");
    server.engine()->get_store()->put("idx:DataObject:n:/zone/home/coll/file.txt", "3000");

    l3kvg::Settings client_settings;
    client_settings.node_id = 2;
    client_settings.fed_timeout_ms = 5000;
    l3kvg::RemoteL3KVClient client(client_settings);
    auto pool = std::make_shared<l3kvg::ThreadPool>(8);
    client.set_thread_pool(pool);
    client.add_peer(1, "tcp://127.0.0.1:" + std::to_string(port));

    const int num_threads = 6;
    const int iterations_per_thread = 15;
    std::atomic<int> successful_scans{0};

    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&client, prefix, num_entries, iterations_per_thread, &successful_scans]() {
            for (int it = 0; it < iterations_per_thread; ++it) {
                auto entries = client.get_prefix_entries_async(1, prefix).get();
                if (entries.size() == static_cast<size_t>(num_entries)) {
                    bool all_matched = true;
                    for (const auto& [k, v] : entries) {
                        if (!k.starts_with(prefix)) {
                            all_matched = false;
                            break;
                        }
                    }
                    if (all_matched) {
                        auto keys = client.get_prefix_keys_async(1, prefix).get();
                        if (keys.size() == static_cast<size_t>(num_entries)) {
                            successful_scans.fetch_add(1);
                        }
                    }
                }
            }
        });
    }

    for (auto& th : threads) th.join();
    EXPECT_EQ(successful_scans.load(), num_threads * iterations_per_thread);

    server.stop();
}

