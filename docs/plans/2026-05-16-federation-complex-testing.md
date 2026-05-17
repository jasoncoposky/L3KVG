# Complex Federation Testing Implementation Plan

> **For Gemini:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Bulletproof the federation feature set by testing batching, complex projections, and recursive (multi-hop) traversals.

**Architecture:**
- Use multiple mock ZeroMQ servers within a single test file to simulate different clusters.
- Leverage the `resume` query logic to verify that sub-queries can trigger further suspensions.

---

### Task 1: Multi-Node Batching Test

**Files:**
- Create: `l3kvg/tests/test_federation_complex.cpp`

**Step 1: Implement Batch Traversal Test.**
- Setup: 1 Local Cluster, 1 Remote Cluster (Mock).
- Data: Local Node `A` has edges to Remote Nodes `B1` through `B10`.
- Query: `MATCH (a) OUT('link') AS b RETURN b.id`.
- Verification: Ensure only ONE ZeroMQ request is sent (batching) and 10 results are gathered.

**Step 2: Commit.**
`git add l3kvg/tests/test_federation_complex.cpp && git commit -m "test: add multi-node batching federation test"`

---

### Task 2: Multi-Projection Consistency Test

**Files:**
- Modify: `l3kvg/tests/test_federation_complex.cpp`

**Step 1: Implement Multi-Projection Test.**
- Query: `MATCH (a) OUT('link') AS b RETURN b.name, b.age, b.status`.
- Mock: Remote cluster returns rows with all three fields.
- Verification: Ensure the local engine correctly maps `0, 1, 2` aliases to the correct properties in the unified result set.

**Step 2: Commit.**

---

### Task 3: Recursive Federation (Multi-Hop) Test

**Files:**
- Modify: `l3kvg/tests/test_federation_complex.cpp`

**Step 1: Implement 2-Hop Traversal Test.**
- Setup: Cluster 1 (Local), Cluster 2 (Remote Mock), Cluster 3 (Remote Mock).
- Path: A (C1) -> B (C2) -> C (C3).
- Query: `MATCH (a) OUT('link') OUT('link') AS target RETURN target.val`.
- Flow: 
    1. C1 traverses to C2 and suspends.
    2. C2 resumes, traverses to C3 and suspends *again*.
    3. C3 resumes, returns result to C2.
    4. C2 returns result to C1.
- Verification: Final results at C1 contain C3's data.

**Step 2: Commit.**

---

### Task 4: Integration and Build

**Files:**
- Modify: `l3kvg/CMakeLists.txt`

**Step 1: Add `test_federation_complex` to build.**
**Step 2: Run all tests and verify PASS.**
**Step 3: Commit.**
