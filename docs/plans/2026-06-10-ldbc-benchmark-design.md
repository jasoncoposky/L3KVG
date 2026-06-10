# Design: LDBC SNB v2 Benchmark Bridge for L3KVG

## 1. JNI Bridge & Native Integration

The core of the performance strategy is a "Fat JNI" bridge. Instead of multiple small JNI calls, we will expose high-level query entry points that map 1:1 to the LDBC Interactive operations.

### Native Engine Interface (`NativeEngine.java`)
This class acts as the gateway. It will load `libl3kvg_jni.so` and define the 29 required operations.

```java
package com.l3kvg.ldbc;

public class NativeEngine {
    static {
        System.loadLibrary("l3kvg_jni");
    }

    // Example: Complex Query 1 (Friend of Friend)
    public native long[] executeComplexQuery1(long personId, String firstName);
    
    // Lifecycle
    public native void init(String configPath);
    public native void shutdown();
}
```

### Zero-Copy Result Sets
To achieve the sub-millisecond latencies L3KVG is capable of, we will avoid standard Java object instantiation inside the JNI loop. 

- **Read Path**: The C++ layer will return `long[]` arrays (for IDs) and `byte[]` buffers containing `lite3-cpp` BSON for complex attributes.
- **Buffer Recycling**: We will explore using `DirectByteBuffer` if profiling shows GC pressure on the array allocations.

## 2. Core Engine Optimizations

To reach peak performance, we will implement the following "Fast-Paths" in the L3KVG core:

### A. Pre-Compiled Match Pipelines
Bypass the regex-based Cypher-Lite parser for the 14 fixed LDBC Complex Reads.
- **Mechanism**: Implement a `QueryRegistry` where pre-constructed `ExecutionPlan` objects are stored.
- **JNI Interface**: `NativeEngine.executeComplexQuery(int queryId, params...)`.

### B. Multi-Node Batch Neighbors
Optimize the high fan-out explorations common in LDBC.
- **Mechanism**: New `l3kvg::get_neighbors_batch` method that leverages `citor` to parallelize L3KV sharding lookups.
- **Impact**: Reduces JNI transition frequency by 5x-10x for neighborhood scans.

### C. ZMQ/RPC Bypass ("Local-Direct" Mode)
Eliminate serialization overhead for single-node benchmark runs.
- **Mechanism**: A `use_direct_pointer_access` flag in the configuration.
- **Impact**: Removes the 500µs RPC overhead for local-only traversals, reducing latencies to the sub-100µs range.

## 3. Testing & Validation

The qualification of the benchmark bridge will follow a four-stage verification process:

### Stage 1: Functional Unit Tests (Native)
- **Goal**: Verify each of the 29 query handlers returns correct results against a small, deterministic "Mini-Graph" (mesh_db_101).
- **Tool**: C++ `GTest` suite in `l3kvg/benchmark/tests/`.

### Stage 2: JNI Integrity Tests (Java)
- **Goal**: Ensure the `DirectByteBuffer` and `long[]` exchanges are memory-safe and leak-free.
- **Tool**: JUnit 5 with `LDBC Mock Driver`.

### Stage 3: Scale Factor 1 (SF-1) Baseline
- **Goal**: Run a complete end-to-end benchmark on a 1GB dataset.
- **Success Criteria**: 0% error rate and response times within the target SRE windows defined in `README.md`.

### Stage 4: Scale Factor 100 (SF-100) Performance Validation
- **Goal**: Stress test the `ConsistentHash` and `EdgeCoordinator` in a multi-node sharded environment (3 nodes).
- **Tool**: Full `ldbc_snb_interactive_v2_driver` execution.

---
**Status**: Design Complete.
**Next Step**: Ready to set up for implementation?
