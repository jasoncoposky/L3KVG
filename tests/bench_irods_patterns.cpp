#include "L3KVG/Engine.hpp"
#include "L3KVG/Node.hpp"
#include "L3KVG/Query.hpp"
#include <chrono>
#include <iostream>
#include <vector>
#include <random>
#include <filesystem>

using namespace l3kvg;

int main() {
    std::string db_path = "bench_irods.db";
    std::filesystem::remove_all(db_path);
    
    Settings settings;
    Engine engine(db_path, 1, nullptr, 4, settings);

    const int num_collections = 100;
    const int num_data_objects = 1000;
    
    std::cout << "Seeding " << num_collections << " collections and " << num_data_objects << " data objects...\n";
    
    std::vector<uint64_t> coll_ids;
    for (int i = 0; i < num_collections; ++i) {
        uint64_t cid = 0xC0000000 + i;
        lite3cpp::Buffer buf;
        buf.init_object();
        buf.set_str(0, "n", "/tempZone/home/coll" + std::to_string(i));
        buf.set_str(0, "t", "collection");
        engine.put_node(cid, buf.move_to_string());
        coll_ids.push_back(cid);
    }
    
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> coll_dist(0, num_collections - 1);
    
    std::vector<uint64_t> data_ids;
    for (int i = 0; i < num_data_objects; ++i) {
        uint64_t did = 0xD0000000 + i;
        lite3cpp::Buffer buf;
        buf.init_object();
        buf.set_str(0, "n", "file" + std::to_string(i));
        buf.set_str(0, "t", "data_object");
        engine.put_node(did, buf.move_to_string());
        data_ids.push_back(did);
        
        int cid_idx = coll_dist(rng);
        uint64_t cid = coll_ids[cid_idx];
        engine.add_edge(cid, "CONTAINS", 1.0, did);
        
        uint64_t rid = 0xE0000000 + i;
        lite3cpp::Buffer rbuf;
        rbuf.init_object();
        rbuf.set_str(0, "t", "replica");
        rbuf.set_i64(0, "rn", 0);
        engine.put_node(rid, rbuf.move_to_string());
        engine.add_edge(did, "HAS_REPLICA", 1.0, rid);
    }
    
    std::cout << "Seeding complete.\n";

    std::cout << "Benchmarking OBJ_STAT join (Branching: DataObject -> Replica AND DataObject -> Collection)...\n";
    
    const int num_iterations = 1000;
    auto start_bench = std::chrono::high_resolution_clock::now();
    
    int fail_count = 0;
    for (int i = 0; i < num_iterations; ++i) {
        int did_idx = i % num_data_objects;
        uint64_t did = data_ids[did_idx];
        
        Query q(&engine);
        q.match_id(did, "DataObject")
         .out("HAS_REPLICA").as("Replica")
         .from("DataObject").in("CONTAINS").as("Collection") // BRANCH HERE
         .return_("DataObject", "n")
         .return_("Replica", "rn")
         .return_("Collection", "n");
         
        auto results = q.execute();
        if (results.empty()) {
            fail_count++;
        }
    }
    
    auto end_bench = std::chrono::high_resolution_clock::now();
    double total_ms = std::chrono::duration<double, std::milli>(end_bench - start_bench).count();
    std::cout << "Executed " << num_iterations << " branching joins in " << total_ms << " ms\n";
    std::cout << "Avg Latency: " << (total_ms * 1000.0 / num_iterations) << " us\n";
    std::cout << "Failures: " << fail_count << "\n";

    std::filesystem::remove_all(db_path);
    return 0;
}
