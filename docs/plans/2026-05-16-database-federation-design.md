# L3KVG Architectural Design: Database Federation

## Overview
This document outlines the architectural design for federating multiple independent L3KVG graph database clusters. The primary goal is to create a **Virtual Unified Graph**, enabling queries to seamlessly traverse edges spanning across distinct databases without requiring the physical merging of data. The design prioritizes high mechanical sympathy (performance) and resilience to high-latency WAN environments.

## Core Architecture: Snowflake-style Integer Routing

To achieve maximum routing performance, L3KVG will adopt a "Snowflake-style" 64-bit integer ID system internally. The routing logic will transition away from string-based keys to purely bitwise operations.

### ID Structure (64-bit `uint64_t`)
The 64 bits of every Node ID are partitioned as follows:
- **Top 16 bits:** Cluster ID (Supports up to 65,536 federated clusters).
- **Bottom 48 bits:** Local Node Hash (Deterministic `xxHash` of the local UUID string).

### Two-Tier Resolution
The existing `ClusterResolver` is extended into a `FederationResolver`.
1.  **Tier 1 (Federation):** The engine reads the top 16 bits (`id >> 48`). If this matches the local cluster ID, execution proceeds locally. If not, it looks up the associated ZeroMQ endpoint in the `FederationRegistry` to yield a `RemoteTarget`.
2.  **Tier 2 (Sharding):** If local, the engine hashes the bottom 48 bits against the existing `ConsistentHash` ring to locate the precise intra-cluster shard.

## Developer Experience (Edge-Hashed Strings)

While the engine uses 64-bit integers internally, the developer experience remains strictly string-based to preserve ergonomics and Cypher compatibility. The API boundary automatically hashes strings into the bitwise representation.

### Node and Edge Interaction
Developers identify nodes using a URN-style format: `[cluster_name]:[local_uuid]`.
- If no prefix is provided, the engine defaults to the local cluster's ID.
- The `[cluster_name]` must be registered with the engine upon startup.

```cpp
#include "L3KVG/Engine.hpp"

// 1. Setup & Registration
auto engine = std::make_unique<l3kvg::Engine>("db_path", 1);
// Register the local cluster mapping to ID 101
engine->get_resolver().register_local_cluster("us-east", 101);
// Register foreign federation peers mapping to their respective IDs
engine->get_resolver().register_federation("eu-west", 100, "tcp://eu-west.l3kvg.local:8080"); 

// 2. Adding Nodes
// The string "eu-west:user_1" is intercepted. 
// "eu-west" becomes 100. "user_1" is hashed to 0x1A2B3C4D.
// Internally stored as: (100ULL << 48) | (0x1A2B3C4D & 0xFFFFFFFFFFFF)
engine->put_node("eu-west:user_1", R"({"name": "Alice"})");
engine->put_node("us-east:server_A", R"({"status": "active"})");

// 3. Adding Cross-Cluster Edges
// The engine automatically handles the dual-shard HLC write across the WAN via ZeroMQ.
engine->add_edge("eu-west:user_1", "connects_to", 1.0, "us-east:server_A");
```

### Cypher Query Execution
Cross-cluster traversals are transparent to the user. The engine handles the network hops automatically.

```cpp
// 4. Querying a Unified Graph
auto results = engine->query()
    .match("eu-west:user_1")
    .out("connects_to") 
    .return_({"status"})
    .execute();
    
// Cypher Alternative
auto rows = parser.execute("MATCH (n {id: 'eu-west:user_1'})-[e:connects_to]->(tgt) RETURN tgt.status");
```

## Cross-Cluster Traversal: The Suspended Branch Approach

To prevent high-latency network saturation during cross-cluster traversals, L3KVG implements a deferred "Suspended Branch" batching strategy.

### Execution Flow
1.  **Local Execution First:** The `Query::execute()` pipeline begins normally on the originating cluster.
2.  **Branch Suspension:** When traversing an edge (`node->get_neighbors()`), the engine inspects the top 16 bits of each neighbor ID. If the cluster ID does not match the local cluster, that specific traversal path is "suspended". The engine records the foreign `uint64_t` node ID and the remainder of the AST steps.
3.  **Batch Dispatch:** Once all local branches are exhausted, the engine groups all suspended paths by their target cluster. It serializes the remaining AST steps and fires a single, asynchronous ZeroMQ batch request via `RemoteL3KVClient` to the foreign cluster.
    - *Payload Concept:* `{"resume_ast": [...steps...], "starting_nodes": [ID_1, ID_2, ... ID_500]}`
4.  **Remote Resumption:** The foreign cluster receives the payload, initializes its own `Query` pipeline, seeds the frontier with the starting nodes, and executes the remaining steps entirely locally on its own fast SSDs.
5.  **Scatter-Gather:** The originating cluster waits for the futures, concatenates the `ResultRows` returned from the foreign clusters with its own local results, and returns the final, unified dataset to the user.

## Configuration & Persistence

The Federation Registry is configured statically at startup to ensure routing occurs with zero disk-I/O overhead.

### Configuration Format (`config.json`)
The node's `config.json` is extended to define both the local cluster identity and foreign federation peers.

```json
{
  "local_cluster": {
    "name": "us-east",
    "id": 101
  },
  "federation": [
    {
      "name": "eu-west",
      "id": 100,
      "gateway_endpoints": [
        "tcp://eu-west-gw1.l3kvg.internal:8080"
      ]
    }
  ]
}
```

### In-Memory Routing
Upon startup, the server parses these values and populates the `FederationResolver`'s in-memory registry. Because bitwise routing is executed millions of times per second, the registry must remain strictly in memory. Dynamic additions can be supported via internal HTTP endpoints, with operators responsible for updating the static `config.json` for persistence across reboots.

## Migration & Rollback
- **Backward Compatibility:** Existing un-prefixed string UUIDs will be treated as belonging to the local cluster, maintaining compatibility.
- **Rollback:** The federation registry can be deactivated via configuration, reverting the engine to isolated, single-cluster operation. Cross-cluster edges will simply fail to resolve their remote targets.
