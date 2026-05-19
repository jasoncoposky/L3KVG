# L3KV Architectural Design: Credentials & ACL Manager (Secure Mesh)

## Overview
This document outlines the security architecture for the **L3KV** foundational layer and its integration with the **L3KVG** graph engine. The goal is to transition from a "trusted network" model to a **Zero-Trust Federated Mesh** where every operation is cryptographically authenticated and authorized.

## 1. Cryptographic Identity (Transport Security)
L3KV will leverage **ZeroMQ CURVE (Elliptic Curve 25519)** for all inter-node and client-server communication.

### 1.1 Mutual Authentication
- **Public/Private Keys**: Every node and client is assigned a unique 25519 key pair.
- **Server Identity**: The server presents its Public Key to clients.
- **Client Identity**: Clients must present a valid Public Key that exists in the server's `sys:users` table.
- **Encryption**: All ZMQ traffic is automatically encrypted by the CURVE mechanism, preventing eavesdropping on WAN links.

## 2. Identity Model (`sys:users`)
Permissions are tied to a unique `ClientUID` (uint32_t).

- **Identity Mapping**: Internal keys map Public Keys to UIDs.
  `sys:u:key:{public_key}` -> `{uid: 101, name: "US-Cluster-Node-1"}`
- **Persistence**: User definitions are stored in the L3KV WAL, making security persistent and distributable.

## 3. Authorization Model (Prefix-Based ACLs)
Authorization is enforced via prefix matching, providing a high-performance "Sandboxing" model for sharded data.

### 3.1 Permission Rules
Rules are stored as system keys:
`sys:acl:{uid}:{prefix}` -> `[READ | WRITE | ADMIN | DENY]`

**Standard L3KVG Mappings:**
- **Local Sovereignty**: Cluster `US` has `WRITE` for `n:{us:` and `e:{us:`.
- **Global Read**: Cluster `EU` has `READ` for `n:{us:`.
- **Administrative**: The `admin` UID has `ADMIN` for `*` (full access).

### 3.2 Evaluation Order
1.  **Direct Match**: Explicit rule for the exact key.
2.  **Longest Prefix Match**: The rule with the longest matching prefix wins.
3.  **Default Deny**: If no rules match, the operation is rejected.

## 4. Performance: Bitmask Caching
To maintain L3KV's microsecond performance, security checks must not involve string parsing on the hot path.

- **Session Bitmask**: When a CURVE connection is established, the server evaluates all ACLs for that `ClientUID` and builds a **Permission Bitmask** for each shard.
- **Fast-Path Hook**: `Engine::get` and `Engine::put` check the bitmask before execution.
  ```cpp
  if (!(shard.session_bitmasks[client_id] & PERM_WRITE)) throw Unauthorized();
  ```

## 5. Secure Replication (L3KVG Integration)
The graph engine leverages the ACL layer to ensure mesh integrity.

- **Origin Verification**: When an `"S"` (Sync) packet arrives, the server verifies that the `OriginClusterID` in the packet matches the `ClientUID` of the verified ZMQ session.
- **Anti-Spoofing**: Prevents a compromised Cluster B from claiming to send updates "on behalf" of Cluster A.

## 6. Implementation Roadmap

### Phase 1: L3KV Layer (Foundation)
- **Task 1: The `CredentialManager`**: Implements key verification and the `sys:users` store.
- **Task 2: ZMQ Secure Server**: Updates `l3kv::ZmqServer` to use CURVE and verify identities.
- **Task 3: ACL Engine**: Implements the prefix-based rule evaluation and bitmask cache.

### Phase 2: L3KVG Layer (Mesh)
- **Task 4: Managed Keys**: Adds key pair generation to `l3kvg_server` and cluster setup.
- **Task 5: Secure Protocol**: Adds the authentication handshake to all remote graph operations.

### Phase 3: Validation
- **Task 6: The "Rogue Node" Test**: A comprehensive simulation of an unauthorized node attempting to join the mesh and modify data.
