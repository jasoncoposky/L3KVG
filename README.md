# L3KVG: High-Performance Embedded Graph Engine

L3KVG is a C++20 embedded property graph engine built directly on top of the **L3KV** actor-model key-value store, leveraging **lite3-cpp** for zero-copy native BSON-like JSON document resolution over PMR (Polymorphic Memory Resources).

## Features

- **Embedded & Network-Free:** No serialized network round-trips. Direct pointer-local traversals isolated completely to the host process.
- **Fluent C++ API:** A Cypher-inspired fluent builder API natively available directly in C++.
- **Zero-Copy Attributes:** Node and Edge properties are represented as `lite3::Buffer` views, enforcing lazy evaluation. Attributes are only dynamically allocated and resolved exactly when projected.
- **Lock-Free Concurrency:** Fully concurrent multi-threaded architecture leveraging L3KV's internal 64-way sharded actor-model message queues, bypassing global serialization locks.

## Architecture & Performance

To meet strict SRE guidelines (sub-500µs single hop traversal, >10,000 ops/sec writing), L3KVG implements the following foundational patterns:

- **Redis-style Hash Tagging (`{id}`)**: By embedding routing tags directly in edge keys (e.g. `e:out:{uuid}:label:weight:dst`), all outbound and inbound edges form contiguous edge blocks residing strictly on the **same hardware thread/shard** as their parent graph node. 
- **Actor-Model Routines**: `add_edge` and adjacency traversals encapsulate execution closures and pass them to bounded underlying core routines. This removes the necessity of thread-unsafe maps, achieving safe parallel traversal isolation.
- **Spin-Lock De-jitter**: By inserting spin-cycles preceding task yielding upon empty queues, L3KVG bypasses OS-level thread rescheduling penalties (~1-15ms Windows Jitter), accelerating node-expansion 50-edge fan-outs down to **~140µs**.

## API Example

```cpp
#include "L3KVG/Engine.hpp"

// Initialize Graph Engine bounds
auto engine = std::make_unique<l3kvg::Engine>("db_path_test", 1);

// Add Nodes
engine->put_node("npc_1", R"({"name": "Thief", "type": "npc"})");
engine->put_node("npc_2", R"({"name": "Guard", "type": "npc"})");

// Link Edges
engine->add_edge("npc_1", "knows", 1.0, "npc_2");

// Fluent Traversal Pipeline
auto results = engine->query()
    .match("npc_1")
    .where_eq("type", "npc")
    .out("knows")
    .return_({"name"})
    .execute();
```
