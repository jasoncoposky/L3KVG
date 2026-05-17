# L3KVG Architectural Design: Circuit Breakers & Health Checks

## Overview
As part of Phase 3 (Operational Maturity), L3KVG requires a robust failure management system for its federated queries. To prevent a single degraded cluster from causing cascading network saturation and query hangs, we will implement a Hybrid Circuit Breaker and Health Check system. This guarantees a "Fail-Fast" strict consistency model while ensuring safe, automated recovery without risking live user traffic.

## 1. ZeroMQ Transport Timeouts
Currently, ZeroMQ routing sockets can block indefinitely. We will enforce strict SLAs:
*   **`ZMQ_RCVTIMEO`**: Configured on all `RemoteL3KVClient` sockets (default: `500ms`). 
*   If a remote cluster fails to respond within this window, the socket operation aborts, throwing a `std::runtime_error`.
*   **Strict Consistency**: The `Query::execute()` engine will catch this exception and immediately abort the entire user query, ensuring clients receive a definitive failure rather than silently missing remote data.

## 2. Circuit Breaker State Machine
Each federated peer `Session` in `RemoteL3KVClient` will manage an atomic state machine:
*   **CLOSED (Healthy)**: Requests pass through normally. The client tracks `consecutive_failures`. If this exceeds `breaker_failure_threshold` (e.g., 3 consecutive timeouts/errors), the state transitions to OPEN.
*   **OPEN (Failing)**: The cluster is marked as degraded. All incoming user queries bound for this cluster are immediately rejected *before* network I/O, throwing a `CircuitBreakerOpenException`. This "fails fast" and sheds load. The circuit remains OPEN for a `breaker_reset_timeout_ms` (e.g., 5000ms).
*   **HALF-OPEN (Testing Recovery)**: Once the reset timeout expires, the state transitions to HALF-OPEN. The system is ready to test if the remote cluster has recovered.

## 3. Active Recovery via Background Heartbeats
To prevent live user queries from being used as "test dummies" during the HALF-OPEN state, recovery is handled by a background Health Check thread.

### The Dedicated Ping Opcode (`"H"`)
*   The `main.cpp` ZMQ listener will be extended with a new lightweight opcode: `"H"` (Heartbeat). 
*   When it receives `"H"`, it immediately replies with an empty success frame. This tests network connectivity and ZMQ router health with minimal CPU/Memory overhead.

### The Health Check Loop
*   A background thread in `RemoteL3KVClient` wakes up periodically (e.g., every 1000ms).
*   It iterates through all peer `Session`s.
*   If a session is in the **HALF-OPEN** state, it sends an `"H"` ping to that specific peer.
    *   **Success**: The peer responds within the timeout. The state transitions to **CLOSED**, `consecutive_failures` is reset to 0, and user traffic resumes.
    *   **Failure**: The peer times out. The state transitions back to **OPEN**, and the reset timer restarts.

## Configuration Updates
The `Settings` struct and `config.json` will be extended with the following parameters:
- `fed_timeout_ms` (default: 500)
- `breaker_failure_threshold` (default: 3)
- `breaker_reset_timeout_ms` (default: 5000)
- `health_check_interval_ms` (default: 1000)

## Summary of Operation
1. Normal user queries flow until Cluster B dies.
2. 3 queries timeout sequentially. Client opens circuit.
3. Next 10,000 user queries fail instantly without waiting 500ms each.
4. After 5 seconds, background thread pings Cluster B. Ping fails. Wait 5 more seconds.
5. Cluster B reboots.
6. Background ping succeeds. Circuit closes.
7. Next user query succeeds.