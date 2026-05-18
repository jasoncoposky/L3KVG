# L3KVG Architectural Design: The Ultimate Mesh Test

## Overview
This document outlines the design for the **Ultimate Mesh Test**, a comprehensive end-to-end functional verification of the L3KVG Graph Engine. The test validates the integration of **Intra-Cluster Sharding**, **Inter-Cluster Replication**, **Remote Federation**, and **Resilient Circuit Breaking** in a single multi-node scenario.

## 1. Topology Simulation
The test simulates a global infrastructure consisting of 5 nodes across 3 distinct regions:

### 1.1 Cluster US-EAST (ID: 1, Namespace: "us")
- **Architecture**: 2 Nodes (Node 101, Node 102).
- **Sharding**: Shared `ConsistentHash` ring.
- **Replication**: Active-Active with Cluster EU.

### 1.2 Cluster EU-WEST (ID: 2, Namespace: "eu")
- **Architecture**: 2 Nodes (Node 201, Node 202).
- **Sharding**: Shared `ConsistentHash` ring.
- **Replication**: Active-Active with Cluster US.

### 1.3 Cluster CLOUD (ID: 3, Namespace: "cloud")
- **Architecture**: 1 Standalone Node (Node 301).
- **Role**: Purely Federated (Not replicated). Data here must be fetched over the wire.

## 2. Test Phases

### Phase 1: Sharding & Local Consistency
- **Step 1**: Write Node `A` to Cluster US. Verify it lands on Node 101 (based on sharding).
- **Step 2**: Write Node `B` to Cluster US. Verify it lands on Node 102.
- **Step 3**: Create Edge `A -> B`. Verify `EdgeCoordinator` handles the cross-node dual-write within the cluster.

### Phase 2: Active-Active Replication & HLC
- **Step 1**: Verify Node `A` automatically appears in Cluster EU via background replication.
- **Step 2 (Conflict)**: Simultaneously update Node `A` in US with `v2` and Node `A` in EU with `v3` (higher HLC).
- **Step 3 (Convergence)**: Verify that after replication settle time, BOTH clusters hold `v3` (Last-Writer-Wins).

### Phase 3: Global Federation Traversal
- **Step 1**: Seed Node `C` (Global Truth) in Cluster CLOUD.
- **Step 2**: Create Federated Edge `B (US) -> C (CLOUD)`.
- **Step 3 (Query)**: Execute a Cypher traversal starting at `A (US)`:
  `MATCH (a)-[:knows]->(b)-[:depends]->(c) RETURN c.name`
- **Success Criteria**: The engine must use local memory for `A`, local RPC for `B`, and remote Federation for `C`.

### Phase 4: Resilience & Circuit Breaking
- **Step 1**: Kill Node 301 (CLOUD).
- **Step 2**: Execute the same Cypher query from Phase 3.
- **Step 3**: Verify the US Cluster circuit for "CLOUD" trips to OPEN and the query fails fast (< 50ms) instead of hanging.
- **Step 4**: Restart Node 301.
- **Step 5**: Verify background Heartbeats restore the connection and the query succeeds again.

## 3. Implementation Plan

### Task 1: Multi-Region Mock Harness
Implement a `GlobalMeshHarness` class in C++ that can spawn N independent L3KVG Engines, each with its own thread pool, ZMQ context, and virtualized network port.

### Task 2: Scenario Implementation
Implement the 4 phases as a single large GTest in `l3kvg/tests/test_ultimate_mesh.cpp`.

### Task 3: Performance Validation
Measure and log:
- **Local Read Latency** (PMR + Swizzling).
- **Cross-Cluster Replication Latency** (Async WAL Tailing).
- **Federated Fetch Latency** (WAN Simulation).

## 4. Verification Requirements
- [ ] 0 Deadlocks during simultaneous N-way sync.
- [ ] 100% Data Convergence across sharded replicas.
- [ ] Instant fail-fast when a remote region is degraded.
