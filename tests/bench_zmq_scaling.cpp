#include "L3KVG/MutationBatch.hpp"
#include "L3KVG/RemoteL3KVClient.hpp"
#include "L3KVG/Settings.hpp"
#include "buffer.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <spawn.h>
#include <string>
#include <thread>
#include <vector>

extern char** environ;

#ifndef L3KVG_SERVER_BIN
#define L3KVG_SERVER_BIN "./l3kvg_server"
#endif

namespace {

uint16_t pick_free_port() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) throw std::runtime_error("socket() failed");
    int opt = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        throw std::runtime_error("bind() failed");
    }
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        ::close(fd);
        throw std::runtime_error("getsockname() failed");
    }
    uint16_t port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

class ServerProcess {
public:
    ServerProcess(uint16_t zmq_port, uint16_t http_port, const std::string& db_dir, uint32_t workers)
        : db_dir_(db_dir), cfg_path_(db_dir + "/server_cfg.json") {
        std::filesystem::remove_all(db_dir_);
        std::filesystem::create_directories(db_dir_);

        std::ofstream cfg(cfg_path_);
        cfg << "{\n"
            << "  \"db_path\": \"" << db_dir_ << "/data\",\n"
            << "  \"node_id\": 1,\n"
            << "  \"zmq_port\": " << zmq_port << ",\n"
            << "  \"http_port\": " << http_port << ",\n"
            << "  \"num_workers\": " << workers << "\n"
            << "}\n";
        cfg.close();

        std::string bin = L3KVG_SERVER_BIN;
        char* argv[] = {const_cast<char*>(bin.c_str()), const_cast<char*>(cfg_path_.c_str()), nullptr};
        if (::posix_spawnp(&pid_, bin.c_str(), nullptr, nullptr, argv, environ) != 0) {
            throw std::runtime_error("posix_spawnp(l3kvg_server) failed");
        }
    }

    ~ServerProcess() {
        stop();
        std::error_code ec;
        std::filesystem::remove_all(db_dir_, ec);
    }

    void stop() {
        if (pid_ > 0) {
            ::kill(pid_, SIGTERM);
            int status = 0;
            ::waitpid(pid_, &status, 0);
            pid_ = -1;
        }
    }

private:
    std::string db_dir_;
    std::string cfg_path_;
    pid_t pid_{-1};
};

template <typename Fn>
void run_bench_case(const std::string& name, int threads, int ops_per_thread, Fn&& fn, int items_per_op = 1) {
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&, t]() {
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            fn(t, ops_per_thread);
        });
    }

    while (ready.load(std::memory_order_acquire) < threads) {
        std::this_thread::yield();
    }

    auto t0 = std::chrono::steady_clock::now();
    go.store(true, std::memory_order_release);
    for (auto& w : workers) w.join();
    auto t1 = std::chrono::steady_clock::now();

    double sec = std::chrono::duration<double>(t1 - t0).count();
    double rpcs = static_cast<double>(threads) * ops_per_thread;
    double rpcs_per_sec = rpcs / sec;
    double items_per_sec = (rpcs * items_per_op) / sec;
    double avg_us = (sec * 1e6) / ops_per_thread;

    std::cout << std::left << std::setw(34) << name
              << " | threads=" << std::setw(2) << threads
              << " | " << std::right << std::setw(10) << std::fixed << std::setprecision(0) << rpcs_per_sec << " rpc/s"
              << " (" << std::setw(10) << items_per_sec << " items/s)"
              << " | " << std::setw(7) << std::setprecision(2) << avg_us << " us/rpc\n";
}

} // namespace

int main(int argc, char** argv) {
    uint32_t server_workers = 8;
    if (argc > 1) {
        server_workers = static_cast<uint32_t>(std::max(1, std::atoi(argv[1])));
    }

    uint16_t zmq_port = pick_free_port();
    uint16_t http_port = pick_free_port();
    std::string db_dir = "/tmp/bench_zmq_scaling_" + std::to_string(::getpid());

    ServerProcess server(zmq_port, http_port, db_dir, server_workers);

    l3kvg::Settings settings;
    settings.fed_timeout_ms = 5000;
    l3kvg::RemoteL3KVClient client(settings);
    client.add_peer(1, "tcp://127.0.0.1:" + std::to_string(zmq_port));

    // Wait for server readiness via ping / empty get
    for (int attempt = 0; attempt < 100; ++attempt) {
        try {
            (void)client.get_raw_key(1, "__ready_probe");
            break;
        } catch (...) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    // Seed 2,000 nodes and 2,000 edges in batches of 100
    constexpr uint64_t NUM_NODES = 2000;
    {
        l3kvg::MutationBatch batch;
        for (uint64_t i = 1; i <= NUM_NODES; ++i) {
            lite3cpp::Buffer nb;
            nb.init_object();
            nb.set_i64(0, "id", static_cast<int64_t>(i));
            nb.set_str(0, "name", "node_" + std::to_string(i));
            batch.put_node(i, nb);
            batch.add_edge(i, "CONTAINS", 1.0, (i % NUM_NODES) + 1);
            if (i % 100 == 0) {
                if (!client.execute_batch(1, batch)) {
                    std::cerr << "Seed batch failed!\n";
                    return 1;
                }
                batch.clear();
            }
        }
    }

    std::cout << "=================================================================================\n";
    std::cout << "  L3KVG ZMQ Server + RemoteL3KVClient Scaling Benchmark (server_workers=" << server_workers << ")\n";
    std::cout << "=================================================================================\n";

    for (int t : {1, 4, 8, 16}) {
        const int rpcs = (t == 1) ? 4000 : 3000;
        run_bench_case("get_node_payload (1 op)", t, rpcs, [&](int tid, int n) {
            for (int i = 0; i < n; ++i) {
                uint64_t id = ((tid * n + i) % NUM_NODES) + 1;
                auto p = client.get_node_payload(1, id);
                if (p.empty()) abort();
            }
        });
    }
    std::cout << "---------------------------------------------------------------------------------\n";

    for (int t : {1, 4, 8, 16}) {
        const int rpcs = (t == 1) ? 4000 : 3000;
        run_bench_case("get_neighbors (1 hop)", t, rpcs, [&](int tid, int n) {
            for (int i = 0; i < n; ++i) {
                uint64_t id = ((tid * n + i) % NUM_NODES) + 1;
                auto nb = client.get_neighbors(1, id, "CONTAINS");
                if (nb.empty()) abort();
            }
        });
    }
    std::cout << "---------------------------------------------------------------------------------\n";

    for (int t : {1, 4, 8, 16}) {
        const int rpcs = (t == 1) ? 1500 : 1000;
        run_bench_case("execute_multi_read ('G' x16)", t, rpcs, [&](int tid, int n) {
            std::vector<l3kvg::GraphReadOp> ops(16);
            for (int i = 0; i < n; ++i) {
                uint64_t base = ((tid * n + i) % (NUM_NODES - 16)) + 1;
                for (int k = 0; k < 16; ++k) {
                    ops[k].kind = (k % 2 == 0) ? l3kvg::GraphReadKind::NodePayload : l3kvg::GraphReadKind::OutNeighbors;
                    ops[k].node_id = base + k;
                    ops[k].label_or_key = (k % 2 == 0) ? "" : "CONTAINS";
                }
                auto res = client.execute_multi_read(1, ops);
                if (res.size() != 16) abort();
            }
        }, 16);
    }
    std::cout << "---------------------------------------------------------------------------------\n";

    for (int t : {1, 4, 8, 16}) {
        const int rpcs = (t == 1) ? 1500 : 1000;
        run_bench_case("execute_batch (16 edges/RPC)", t, rpcs, [&](int tid, int n) {
            l3kvg::MutationBatch batch;
            for (int i = 0; i < n; ++i) {
                batch.clear();
                uint64_t base = 100000ULL + static_cast<uint64_t>(tid) * 100000ULL + i;
                for (int k = 0; k < 16; ++k) {
                    batch.add_edge(base, "CONTAINS", 1.0, base + 1 + k);
                }
                if (!client.execute_batch(1, batch)) abort();
            }
        }, 16);
    }

    return 0;
}
