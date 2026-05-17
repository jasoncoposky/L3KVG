# Magic Number Elimination Implementation Plan

> **For Gemini:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Eliminate hardcoded "magic numbers" from the L3KVG codebase by introducing a centralized, JSON-configurable `Settings` system.

**Architecture:**
- **Centralized Settings:** A new `Settings` struct will store all tunable parameters.
- **Dependency Injection:** The `Engine` and its sub-components (`EdgeCoordinator`, `RemoteL3KVClient`) will receive and store a reference or copy of these settings.
- **Server Integration:** The `config.json` will be extended to include these parameters, which will be mapped to the `Settings` struct at startup.

**Tunable Parameters:**
- `node_cache_size_per_shard` (default: 2000)
- `node_cache_shards` (default: 8)
- `edge_write_shards` (default: 8)
- `prefix_scan_limit` (default: 1000)
- `zmq_sndhwm` (default: 1000)
- `edge_flush_interval_ms` (default: 2)

---

### Task 1: Define Settings Struct

**Files:**
- Create: `l3kvg/include/L3KVG/Settings.hpp`

**Step 1: Implement the struct with defaults.**
```cpp
#pragma once
#include <cstddef>

namespace l3kvg {
struct Settings {
    size_t node_cache_size_per_shard = 2000;
    size_t node_cache_shards = 8;
    size_t edge_write_shards = 8;
    size_t prefix_scan_limit = 1000;
    int zmq_sndhwm = 1000;
    int edge_flush_interval_ms = 2;
};
}
```

**Step 2: Commit.**
`git add l3kvg/include/L3KVG/Settings.hpp && git commit -m "feat: add centralized Settings struct"`

---

### Task 2: Integrate Settings into Engine and Cache

**Files:**
- Modify: `l3kvg/include/L3KVG/Engine.hpp`
- Modify: `l3kvg/src/Engine.cpp`

**Step 1: Update `Engine` to store `Settings`.**
Add `Settings settings_;` member. Update constructor to accept `Settings`.

**Step 2: Update Node Cache initialization.**
Use `settings_.node_cache_shards` and `settings_.node_cache_size_per_shard`.

**Step 3: Update `get_nodes_by_prefix`.**
Use `settings_.prefix_scan_limit`.

**Step 4: Verify existing tests pass.**

**Step 5: Commit.**

---

### Task 3: Propagate Settings to Sub-components

**Files:**
- Modify: `l3kvg/include/L3KVG/EdgeCoordinator.hpp`
- Modify: `l3kvg/src/EdgeCoordinator.cpp`
- Modify: `l3kvg/include/L3KVG/RemoteL3KVClient.hpp`
- Modify: `l3kvg/src/RemoteL3KVClient.cpp`

**Step 1: Update `EdgeCoordinator` to use `Settings`.**
Use `settings_.edge_write_shards` for shard count and `settings_.edge_flush_interval_ms` for the loop wait.

**Step 2: Update `RemoteL3KVClient` to use `Settings`.**
Use `settings_.zmq_sndhwm` when initializing ZMQ sockets.

**Step 3: Verify with `test_edge_coordinator` and `test_remote`.**

**Step 4: Commit.**

---

### Task 4: Update Node and Query Logic

**Files:**
- Modify: `l3kvg/include/L3KVG/Node.hpp`
- Modify: `l3kvg/src/Node.cpp`
- Modify: `l3kvg/src/Query.cpp`

**Step 1: Access Settings from Engine.**
`engine_->get_settings()`

**Step 2: Replace `1000` with `settings.prefix_scan_limit`.**

**Step 3: Verify with `test_federation_query`.**

**Step 4: Commit.**

---

### Task 5: Server Configuration Mapping

**Files:**
- Modify: `l3kvg/src/server/main.cpp`

**Step 1: Extend `Config` struct and `load_config`.**
Map new JSON keys to `Config` members.

**Step 2: Initialize `Settings` from `Config` and pass to `Engine`.**

**Step 3: End-to-end verification with a custom `config.json`.**

**Step 4: Commit.**
