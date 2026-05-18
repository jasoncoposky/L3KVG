# L3KVG Architectural Design: N-Way Multi-Cluster Replication

## Overview
This document outlines how the **L3KVG Graph Engine** leverages the underlying **L3KV Geo-Replication** layer to achieve high-performance horizontal scaling across an arbitrary number (**N**) of geographic regions.

The target architecture is a **Full-Mesh Federated Sharded Mesh**. Every cluster in the mesh (e.g., US-East, EU-West, Asia-Pacific) consists of its own set of horizontally scaled nodes and replicates its local sharded data to all other clusters in the mesh.

## 1. N-Way Replication Topology
Instead of simple pair-wise mirroring, L3KVG supports arbitrary mesh topologies:

### 1.1 Cluster Identification (Namespaces)
- Every cluster is assigned a unique `ClusterID` (uint16_t) and a string `Namespace` (e.g., `"london"`).
- The `FederationResolver` maintains a map of all clusters in the mesh and their gateway endpoints.

### 1.2 Replication Streams
- **Broadcast Mesh**: Each node, upon committing a local write, identifies all *remote* clusters in its configuration.
- **Async Delivery**: Updates are pushed to a "Gateway" node in each remote cluster.
- **Loop Prevention**: Every replication packet carries its `OriginClusterID`. A cluster will never re-replicate a packet if it didn't originate locally, preventing infinite broadcast loops.

## 2. Graph Consistency in an N-Way Mesh

### 2.1 Consistent Shard Mapping
- **Global Shard Identity**: All clusters in the N-way mesh MUST use the identical `ConsistentHash` configuration (same seed and shard count).
- This ensures that Node `X` is always assigned to "Logical Shard 10", and every cluster has exactly one node (or set of threads) responsible for Shard 10.

### 2.2 N-Way Convergence (HLC + LWW)
Hybrid Logical Clocks (HLC) naturally scale to N-way replication:
- **Total Ordering**: Even if updates happen simultaneously in London, Tokyo, and New York, the HLC provides a deterministic global order.
- **Convergence**: All N clusters will eventually apply the update with the highest HLC timestamp, ensuring the graph converges to a single state globally.

## 3. Multi-Cluster Routing & Read-Your-Writes
The `Query` engine uses a tiered routing strategy to minimize WAN latency.

### 3.1 Tiered Data Access (N-Way)
1.  **Local Memory**: Hardware-thread local owner.
2.  **Local Cluster (RPC)**: Local ZMQ peer (same region).
3.  **Local Mirror (Replicated)**: Any data replicated from the other N-1 clusters.
4.  **Remote Mesh (Federation Fallback)**: If a local mirror is stale or the circuit breaker for a specific regional copy is open, fetch from the "source of truth" regional cluster via WAN.

## 4. Implementation Tasks (Phase 4)

### Task 1: Replication-Aware Shard Routing
Update the server's replication handler to use the `ConsistentHash` ring for incoming remote writes, ensuring replicated graph data is stored on the correct local shard.

### Task 2: Multi-Peer Replication Manager
Implement logic in `RemoteL3KVClient` to manage outgoing replication streams to N configured peer clusters simultaneously.

### Task 3: Mesh Loop Prevention
Add `OriginClusterID` to the internal ZMQ replication protocol and implement the "drop if not local" filter to prevent broadcast storms.

## 5. Performance Goals
- **Global Read Availability**: > 99.99% query success rate by falling back to any cluster in the mesh if the primary or local copy is unavailable.
- **Mesh Scalability**: Support for up to 16 global clusters with < 10% overhead on local write performance.
- **Eventual Consistency SLA**: Global convergence within 2x the highest round-trip time (RTT) between any two clusters in the mesh.
