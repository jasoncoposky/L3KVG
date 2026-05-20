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

### 3. Foundational Unified Identity
L3KVG implements a **Security at the Lowest Layer** model. Identity and authorization are built directly into the foundational storage engine, ensuring that all graph operations are secure-by-design.

- **Principal Propagation**: Every ZMQ request carries a 4-byte `EffectiveUID` frame. Physical nodes establishment trusted pipes via `auth_secret` and are delegated authority to "forward" the identities of the users they act for.
- **Local-First Authorization**: Security metadata (users and ACLs) under the `sys:` prefix is automatically replicated to all nodes globally. Authorization checks happen locally in the storage hot-path (< 5ns), eliminating WAN bottlenecks for permission lookups.
- **Single Global Identity**: A user defined in the US cluster can execute a federated query spanning EU and Asia nodes using their single global UID.

### 4. High-Performance Compute Foundation
L3KVG uses the **citor** high-performance thread pool for sub-microsecond task dispatch and hardware-topology awareness.

- **Lock-Free Work Stealing**: Replaced central mutex contention with decentralized **Chase-Lev deques**, allowing the engine to scale linearly with CPU core counts (e.g. 64+ cores).
- **L3 Cache Locality**: Tasks are pinned to **CCD (Core Complex Die)** arenas, ensuring that graph traversals stay "hot" in the fastest cache hierarchy.
- **Parallel Exploration**: The `MATCH` engine parallelizes path exploration across all available cores, reducing latency for high fan-out queries.

## Performance Benchmarks

L3KVG is designed to meet strict SRE performance targets on persistent, sharded workloads.

| Operation | Performance | Target |
| :--- | :--- | :--- |
| **Write Throughput** | **~9,500 edges/sec** | > 5,000 |
| **Path Exploration** | **~30,000 paths/sec** | > 10,000 |
| **Traversal Latency (50 fan-out)** | **180 µs** | < 500 µs |
| **Traversal Latency (10k fan-out)** | **1.5 ms** | < 2.0 ms |

## Docker & Cloud Native

L3KVG is fully containerized and ready for cloud-native deployments. We provide a multi-stage `Dockerfile` and a `docker-compose.yml` for rapid cluster setup.

### 1. Build the Image
```bash
docker build -t l3kvg:latest -f l3kvg/Dockerfile .
```

### 2. Launch a 3-Node Cluster (Compose)
Simulate a sharded US-East cluster on your local machine:
```bash
cd l3kvg
docker-compose up -d
```
This spins up 3 nodes (`l3kvg-1`, `l3kvg-2`, `l3kvg-3`) with automatic ZMQ peering and persistent volumes.

### 3. Deploy to Swarm
For production-like orchestration across multiple hosts:
```bash
docker stack deploy -c l3kvg/docker-compose.yml l3kvg_mesh
```

### Environment Variables
The Docker entrypoint dynamically generates `config.json` from these variables:
- `NODE_ID`: Unique ID for the node.
- `CLUSTER_ID`: Identity of the regional cluster.
- `AUTH_SECRET`: Shared mesh secret.
- `PEERS`: Comma-separated list of `id:host:port`.

## ZMQ Replication Protocol (Internal)

All mesh requests follow a unified structure:
`[Identity, Delimiter, EffectiveUID (4b), Opcode (1b), ...Payload Frames]`

| Opcode | Description | Structure |
| :--- | :--- | :--- |
| **G** | Get Node | `[..., "G", HexID]` |
| **M** | Multi-Get | `[..., "M", Key1, Key2, ...]` |
| **N** | Get Neighbors | `[..., "N", HexID, Label, MinWeight]` |
| **R** | Resume Query | `[..., "R", NodeListJSON, QueryJSON]` |
| **P** | Put Node/Edge | `[..., "P", Key, Payload]` |
| **S** | Replication Sync | `[..., "S", Key, Payload, OriginID]` |
| **H** | Heartbeat | `[..., "H", Dummy]` |

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
