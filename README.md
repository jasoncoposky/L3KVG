<p align="center">
  <img src="l3kvg_logo.jpg" alt="L3KVG Logo" width="100%" />
</p>

# L3KVG: High-Performance Federated Sharded Mesh

L3KVG is a C++20 embedded property graph engine built directly on top of the **L3KV** high-performance key-value store. It provides horizontal scalability via consistent hashing, geographic resilience via N-way replication, and production-grade failure management via built-in circuit breakers.

## Key Features

- **Embedded & Network-Free**: Zero-copy pointer-local traversals. All graph logic resides in-process for sub-microsecond local access.
- **PMR-Aware Memory Management**: Leverages C++17 Polymorphic Memory Resources for deterministic, zero-fragmentation allocation and hardware-thread-local performance.
- **Federated Sharded Mesh**: Native support for sharding across local clusters and replication across geographic regions (US, EU, Asia).
- **Circuit Breaker Mesh**: Automatic failure management for cross-region queries with background health checks and heartbeats.
- **Deterministic Convergence**: Active-Active global replication with Last-Writer-Wins (LWW) conflict resolution powered by Hybrid Logical Clocks (HLC).
- **Cypher-Lite Parser**: High-performance regex-assisted query engine supporting multi-hop federated MATCH/WHERE/RETURN traversals.
- **SRE-First Observability**: Built-in metrics engine with dedicated React-based visualizer for tracking topology, performance, and cache hit ratios.

## Architecture & Performance

L3KVG uses a tiered storage and access model optimized for high-throughput workloads:

1.  **L1 Cache (PMR)**: Hot nodes are pinned in hardware-thread-local memory for zero-serialized access.
2.  **L2 Shard (ZMQ)**: Warm nodes are sharded across the local cluster using consistent hashing, accessible via sub-500µs transparent RPC.
3.  **L3 Disk (L3KV)**: Cold nodes reside in the high-performance WAL-backed store with zero-data-loss durability.

### Distribution & Global Mesh
The engine has evolved from local clusters into a globally interconnected graph fabric:

- **Cluster Mapping**: Utilizes `ConsistentHash` to deterministically shard nodes across physical peers.
- **Atomic Edge Coordination**: The `EdgeCoordinator` implements a dual-shard write protocol with HLC synchronization, ensuring causal consistency for edges spanning multiple physical nodes.
- **Inter-Cluster Replication**: Updates are asynchronously broadcast to all other N regional clusters. Every packet carries an `OriginClusterID` to prevent broadcast loops.
- **Resilient Federation**: Remote regions are accessed via a "Local-First" routing strategy. If a local replicated copy is unavailable, the engine falls back to remote federation, protected by a state-machine circuit breaker.

## Configuration Guide

L3KVG nodes are configured via a `config.json` file passed at startup.

### 1. Local Cluster (Sharding)
To set up a local 3-node cluster, each node must list its local peers. This populates the `ConsistentHash` ring and enables intra-cluster RPC.

```json
{
  "node_id": 101,
  "cluster_id": 1,
  "cluster_name": "us-east",
  "zmq_port": 9001,
  "peers": [
    { "id": 102, "host": "10.0.0.2", "port": 9001 },
    { "id": 103, "host": "10.0.0.3", "port": 9001 }
  ]
}
```

### 2. Global Federation (N-Way Replication)
To interconnect regional clusters, add the `federations` section. Any cluster listed here will receive asynchronous replication updates (Active-Active) and participate in federated queries.

```json
{
  "node_id": 101,
  "cluster_id": 1,
  "federations": [
    {
      "id": 2,
      "name": "eu-west",
      "endpoints": ["tcp://52.1.2.3:9001"]
    },
    {
      "id": 3,
      "name": "asia-pacific",
      "endpoints": ["tcp://13.4.5.6:9001"]
    }
  ]
}
```

### 3. Security & ACLs
L3KVG enforces Zero-Trust security via a shared `auth_secret` and prefix-based Access Control Lists (ACLs). This ensures regional sovereignty and protects against unauthorized data access.

```json
{
  "node_id": 101,
  "auth_secret": "your-mesh-shared-secret",
  "cluster_id": 1,
  "cluster_name": "us-east"
}
```
*Note: In the current version, all regional peers must share the same `auth_secret` to participate in the mesh.*

### 4. Operational Parameters
Tune the circuit breaker and replication performance based on your network environment.

```json
{
  "fed_timeout_ms": 500,
  "breaker_failure_threshold": 3,
  "breaker_reset_timeout_ms": 10000,
  "health_check_interval_ms": 2000,
  "node_cache_shards": 16,
  "edge_write_shards": 16
}
```

## ZMQ Replication Protocol (Internal)

| Opcode | Description | Structure |
| :--- | :--- | :--- |
| **G** | Get Node | `[Identity, Delimiter, "G", HexID]` |
| **N** | Get Neighbors | `[Identity, Delimiter, "N", HexID, Label, MinWeight]` |
| **R** | Resume Query | `[Identity, Delimiter, "R", NodeListJSON, QueryJSON]` |
| **P** | Put Node/Edge | `[Identity, Delimiter, "P", Key, Payload]` |
| **S** | Replication Sync | `[Identity, Delimiter, "S", Key, Payload, OriginID]` |
| **H** | Heartbeat | `[Identity, Delimiter, "H", Dummy]` |

## Getting Started

### 1. Build
```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### 2. Run
```bash
./l3kvg_server config.json
```

Detailed API documentation is available in [API.md](API.md).
