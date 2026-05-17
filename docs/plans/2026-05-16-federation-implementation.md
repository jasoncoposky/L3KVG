# L3KVG Federation Implementation Plan

> **For Gemini:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Implement a high-performance database federation system for L3KVG using Snowflake-style bitwise routing and a "Suspended Branch" traversal strategy.

**Architecture:** 
- **Internal Routing:** 64-bit `uint64_t` IDs (16-bit Cluster ID, 48-bit Node Hash).
- **Resolver:** `FederationResolver` for Tier 1 (Cross-Cluster) and Tier 2 (Intra-Cluster) routing.
- **Query Engine:** Deferred batching of remote traversals to minimize network roundtrips.

**Tech Stack:** C++20, ZeroMQ, lite3-cpp, L3KV.

---

### Task 1: FederationID Utility (TDD)

**Files:**
- Create: `l3kvg/include/L3KVG/FederationID.hpp`
- Test: `l3kvg/tests/test_federation_id.cpp`

**Step 1: Write the failing test**
Create `l3kvg/tests/test_federation_id.cpp` with tests for packing and unpacking IDs.
```cpp
#include <gtest/gtest.h>
#include "L3KVG/FederationID.hpp"

TEST(FederationIDTest, PackUnpack) {
    uint16_t cluster_id = 100;
    uint64_t node_hash = 0x1A2B3C4D5E6FULL;
    uint64_t id = l3kvg::FederationID::pack(cluster_id, node_hash);
    
    EXPECT_EQ(l3kvg::FederationID::get_cluster(id), cluster_id);
    EXPECT_EQ(l3kvg::FederationID::get_local_hash(id), node_hash & 0xFFFFFFFFFFFFULL);
}
```

**Step 2: Run test to verify it fails**
Add the test to `l3kvg/CMakeLists.txt` and run it.
Expected: Compilation error (FederationID not defined).

**Step 3: Write minimal implementation**
Implement `FederationID.hpp`.
```cpp
#pragma once
#include <cstdint>

namespace l3kvg {
class FederationID {
public:
    static constexpr uint64_t LOCAL_HASH_MASK = 0xFFFFFFFFFFFFULL;
    static uint64_t pack(uint16_t cluster_id, uint64_t node_hash) {
        return (static_cast<uint64_t>(cluster_id) << 48) | (node_hash & LOCAL_HASH_MASK);
    }
    static uint16_t get_cluster(uint64_t id) { return static_cast<uint16_t>(id >> 48); }
    static uint64_t get_local_hash(uint64_t id) { return id & LOCAL_HASH_MASK; }
};
}
```

**Step 4: Run test to verify it passes**
Run the test again.
Expected: PASS.

**Step 5: Commit**
`git add l3kvg/include/L3KVG/FederationID.hpp l3kvg/tests/test_federation_id.cpp l3kvg/CMakeLists.txt && git commit -m "feat: add FederationID utility"`

---

### Task 2: FederationResolver (TDD)

**Files:**
- Modify: `l3kvg/include/L3KVG/ClusterResolver.hpp` -> Rename to `FederationResolver.hpp` or create new.
- Modify: `l3kvg/src/ClusterResolver.cpp` -> Rename to `FederationResolver.cpp` or create new.
- Test: `l3kvg/tests/test_federation_resolver.cpp`

**Step 1: Write the failing test**
Test registration of clusters and routing logic.
```cpp
TEST(FederationResolverTest, Routing) {
    auto ring = std::make_shared<lite3::ConsistentHash>();
    l3kvg::FederationResolver resolver(ring, 1);
    resolver.register_local_cluster("us-east", 101);
    resolver.register_federation("eu-west", 100, {"tcp://eu-west:8080"});
    
    EXPECT_TRUE(resolver.is_local_cluster(101));
    EXPECT_FALSE(resolver.is_local_cluster(100));
}
```

**Step 2: Run test to verify it fails**

**Step 3: Write minimal implementation**
Update `FederationResolver` to support `register_local_cluster` and `register_federation`.

**Step 4: Run test to verify it passes**

**Step 5: Commit**

---

### Task 3: Edge-Hashed String Mapping (TDD)

**Files:**
- Modify: `l3kvg/include/L3KVG/Engine.hpp`
- Modify: `l3kvg/src/Engine.cpp`

**Step 1: Write the failing test**
Verify that `eu-west:user_1` results in the correct internal `uint64_t`.

**Step 2: Run test to verify it fails**

**Step 3: Write minimal implementation**
Add `parse_uuid(std::string_view uuid_str)` to `Engine` or `FederationResolver`.
Use `xxHash` for the local part.

**Step 4: Run test to verify it passes**

**Step 5: Commit**

---

### Task 3.5: Internal ID Transition (Refactor)

**Goal:** Transition `Node`, `Engine`, and `Query` to use `uint64_t` for internal identification instead of `std::string`.

**Files:**
- Modify: `l3kvg/include/L3KVG/Node.hpp`
- Modify: `l3kvg/src/Node.cpp`
- Modify: `l3kvg/include/L3KVG/Engine.hpp`
- Modify: `l3kvg/src/Engine.cpp`
- Modify: `l3kvg/src/Query.cpp`
- Modify: `l3kvg/include/L3KVG/KeyBuilder.hpp`

**Step 1: Update `Node` and `Engine` signatures.**
Change `uuid` from `std::string` to `uint64_t`.

**Step 2: Update `KeyBuilder` to stringify `uint64_t`.**
Use hex or decimal representation for the key in L3KV.

**Step 3: Update `Query` frontier.**
Change `std::vector<std::string> frontier` to `std::vector<uint64_t> frontier`.

**Step 4: Verify existing tests pass.**
Existing tests should be updated to use the new `uint64_t` (via `engine->parse_uuid` helper).

**Step 5: Commit**

---

### Task 4: Suspended Branch Traversal (TDD)

**Files:**
- Modify: `l3kvg/src/Query.cpp`

**Step 1: Write the failing test**
Mock a query that traverses to a foreign cluster and verify it creates a "Suspended Branch".

**Step 2: Run test to verify it fails**

**Step 3: Write minimal implementation**

**Step 4: Run test to verify it passes**

**Step 5: Commit**

---

### Task 5: Integration and Server Support

**Files:**
- Modify: `l3kvg/src/server/main.cpp`
- Modify: `l3kvg/src/RemoteL3KVClient.cpp`

**Step 1: Implement `resume_ast` endpoint in server.**
**Step 2: Implement batch dispatch in `RemoteL3KVClient`.**
**Step 3: End-to-end integration test with two mock servers.**

**Step 4: Commit**
