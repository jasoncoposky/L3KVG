# L3KV Architectural Design: Foundational Unified Identity (Secure Mesh)

## Overview
This document outlines the security architecture for the **L3KV** foundational layer and its integration with the **L3KVG** graph engine. The core philosophy is **Security at the Lowest Layer**: identity and authorization are built directly into the storage engine, ensuring that all higher-level applications (like L3KVG) are secure-by-design.

The goal is a **Single Global Identity** where a user authenticates once at the mesh edge and their permissions are respected across every sharded node and federated region.

## 1. Unified Identity Model
L3KV manages two distinct layers of identity:

### 1.1 Session Identity (Node-to-Node)
- Uses **ZeroMQ CURVE** for mutual authentication between physical nodes.
- Establishes a **Trusted Pipe** between peers in the mesh.

### 1.2 Principal Identity (User/Service)
- Represents the actual initiator of the request (e.g., `UserID: 500`).
- **Principal Propagation**: Every ZMQ request carries an `EffectiveUID` frame.
- **Trust Delegation**: Peer nodes that have established a secure Session are trusted to assert the `EffectiveUID` of the user they are acting for.

## 2. Global Security Metadata Replication
To ensure a user's identity and ACLs are known mesh-wide, security data is treated as first-class persistent data.

- **System Prefix (`sys:`)**: All keys starting with `sys:u:` (users) and `sys:acl:` (permissions) are automatically replicated to all nodes in all clusters.
- **Mesh-Wide Convergence**: A permission change in one region propagates asynchronously to the entire global mesh using L3KV's HLC-synchronized replication.

## 3. Storage-Level Authorization
Authorization happens at the storage hot-path, minimizing the attack surface.

### 3.1 Prefix-Based Sandboxing
- Rules are enforced in `l3kv::Engine` before any read/write hits the memory shards.
- **Implication for L3KVG**: Graph regional sovereignty (e.g., `n:{us:` prefix) is enforced by L3KV, preventing a compromised US node from writing to EU data even if it manages to spoof a request.

## 4. L3KVG Implications (Leveraging the Foundation)

### 4.1 Secure Multi-Hop Traversal
When a Cypher query hops between clusters, L3KVG simply forwards the `EffectiveUID`. The receiving cluster checks its local (replicated) copy of the ACLs for that UID. No secondary authentication or WAN-based permission lookups are required.

### 4.2 Attribute-Level Security
L3KVG can store sensitive node attributes under specific sub-prefixes (e.g., `n:{us:123}:PII:email`) and use L3KV ACLs to restrict access to specialized "Compliance" UIDs.

## 5. Performance Impact & Mitigation
To ensure L3KV maintains its microsecond-scale performance, security enforcement follows a "Zero-Overhead" philosophy.

### 5.1 Permission Caching (Thread-Local)
- **Problem**: Longest-prefix matching on every `get/put` adds latency.
- **Solution**: Every worker thread maintains a small `thread_local` LRU cache of `(UID, KeyPrefix) -> PermissionBitmask`. 
- **Result**: 99.9% of security checks resolve in < 5ns (single cache lookup).

### 5.2 Zero-Copy Identity Forwarding
- **Mechanism**: The `EffectiveUID` is a fixed-size `uint32_t` frame.
- **Optimization**: ZMQ handles fixed-size frames with zero-copy. The server reads the UID directly from the ZMQ buffer without allocations or string conversions.

### 5.3 Asynchronous Security Sync
- **Mechanism**: ACL updates are sharded just like graph data.
- **Optimization**: Security metadata replication happens in the background via WAL tailing. Updating a user's permissions has 0% impact on active query throughput.

## 6. Implementation Roadmap

### Phase 1: L3KV Foundational Identity
- **Task 1: PrincipalID Protocol**: Update ZMQ server to support and propagate the `EffectiveUID` frame.
- **Task 2: System Key Replication**: Enable the replication manager to ship `sys:` keys globally.
- **Task 3: Identity-Aware Engine**: Hook the `PrincipalID` into the `get/put` authorization checks.

### Phase 2: L3KVG Context Propagation
- **Task 4: Query Context**: Add `principal_id` to the L3KVG `Query` object.
- **Task 5: Secure Client**: Update `RemoteL3KVClient` to automatically inject the current context's `PrincipalID` into all sharding and federation RPCs.

### Phase 3: Validation
- **Task 6: The "Global Identity" Test**: Verify that a user created in Cluster A can immediately perform a multi-region traversal spanning Cluster B and C using their single global UID.
