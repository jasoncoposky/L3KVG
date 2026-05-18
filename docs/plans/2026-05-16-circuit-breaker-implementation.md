# Circuit Breaker & Health Check Implementation Plan

> **For Gemini:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Implement a robust circuit breaker and background health check system for L3KVG federation.

**Architecture:**
- **State Machine**: Each ZeroMQ session tracks its health state (CLOSED, OPEN, HALF-OPEN).
- **Timeouts**: Enforce `ZMQ_RCVTIMEO` on all remote operations.
- **Background recovery**: A dedicated thread in `RemoteL3KVClient` pings degraded peers to facilitate recovery.

---

### Task 1: Circuit State and Settings (TDD)

**Files:**
- Modify: `l3kvg/include/L3KVG/Settings.hpp`
- Modify: `l3kvg/include/L3KVG/RemoteL3KVClient.hpp`

**Step 1: Update Settings struct.**
Add `fed_timeout_ms`, `breaker_failure_threshold`, `breaker_reset_timeout_ms`, and `health_check_interval_ms`.

**Step 2: Implement CircuitBreaker class/struct.**
Add it to `RemoteL3KVClient.hpp` inside `Session`.

**Step 3: Test State Transitions.**
Create `l3kvg/tests/test_circuit_breaker.cpp` and test transition logic.

**Step 4: Commit.**
`git add . && git commit -m "feat: add circuit breaker state machine and settings"`

---

### Task 2: ZeroMQ Timeouts and Tripping (TDD)

**Files:**
- Modify: `l3kvg/src/RemoteL3KVClient.cpp`

**Step 1: Set `ZMQ_RCVTIMEO` in `add_peer`.**
**Step 2: Track failures in RPC methods.**
Increment error count on timeout. Trip to OPEN if threshold hit.
**Step 3: Check circuit state before sending.**
Throw exception if OPEN.
**Step 4: Verify with `test_federation_integration`.**
Mock a timeout and verify the circuit opens.

**Step 5: Commit.**

---

### Task 3: Server Heartbeat Handler (TDD)

**Files:**
- Modify: `l3kvg/src/server/main.cpp`

**Step 1: Implement `"H"` opcode.**
Respond with an empty payload.
**Step 2: Test ping logic.**
Update `test_federation_integration` to send an `"H"` and expect a response.

**Step 3: Commit.**

---

### Task 4: Background Health Checker (TDD)

**Files:**
- Modify: `l3kvg/src/RemoteL3KVClient.cpp`
- Modify: `l3kvg/include/L3KVG/RemoteL3KVClient.hpp`

**Step 1: Implement background thread loop.**
**Step 2: Implement logic to ping HALF-OPEN sessions.**
**Step 3: Transition to CLOSED on success.**
**Step 4: Functional Test.**
Trip a circuit, wait, then verify it closes after the mock server comes back up.

**Step 5: Commit.**

---

### Task 5: Server Configuration Integration

**Files:**
- Modify: `l3kvg/src/server/main.cpp`

**Step 1: Map new Settings from `config.json`.**
**Step 2: Final End-to-End verification.**

**Step 3: Commit.**
