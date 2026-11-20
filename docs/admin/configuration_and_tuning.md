# Configuration, Runtime Knobs & Tuning

`l3kvg` runs either as an **embedded C++20 library** (configured via `l3kvg::Settings` in [Settings.hpp](file:///home/darkfell/dev/l3kvg/include/L3KVG/Settings.hpp)) or as a **standalone server daemon** (configured via JSON passed to `l3kvg_server <config.json>` in [main.cpp](file:///home/darkfell/dev/l3kvg/src/server/main.cpp)).

---

## 1. Standalone Server Configuration (`config.json`)

The server binary reads a JSON config file at startup (`Usage: l3kvg_server <config.json>`):

```json
{
  "db_path": "/var/lib/l3kvg/node1",
  "node_id": 1,
  "zmq_port": 5556,
  "http_port": 8080,
  "num_workers": 8,
  "auth_secret": "change-me-in-production",
  "local_cluster_name": "us-east",
  "local_cluster_id": 1,
  "federations": [
    {
      "name": "eu-west",
      "id": 2,
      "endpoints": ["tcp://10.20.1.11:5556", "tcp://10.20.1.12:5556"]
    }
  ]
}
```

### Server Config Fields

| Key | Type | Default | Description |
| :--- | :---: | :---: | :--- |
| `db_path` | `string` | *(required)* | Directory or monolithic file path for `kv.wal`, `edges.wal`, and `labels.dict`. |
| `node_id` | `uint16` | `1` | Unique node identifier on the consistent-hash ring and in HLC tie-breaking. |
| `zmq_port` | `uint32` | `5556` | ZeroMQ `ROUTER` port for graph queries, batch mutations, and cluster replication. |
| `http_port` | `uint32` | `8080` | HTTP port bound to `0.0.0.0` (exposes `GET /api/health` in [main.cpp](file:///home/darkfell/dev/l3kvg/src/server/main.cpp#L916)). |
| `num_workers` | `uint32` | `clamp(cores, 4, 32)` | Worker thread pool size for ZMQ request handling and query execution. |
| `auth_secret` | `string` | `""` | Shared secret required by ZeroMQ peers/clients when authentication is enabled. |
| `local_cluster_name` | `string` | `""` | Human-readable name for the local cluster. |
| `local_cluster_id` | `uint16` | `0` | Numeric cluster ID stamped onto replicated mutations (`origin_cluster_id`). |
| `federations` | `array` | `[]` | Remote cluster definitions (`name`, `id`, `endpoints[]`). |

---

## 2. Engine Runtime Knobs (`l3kvg::Settings`)

All engine-level tuning parameters live in `l3kvg::Settings` ([Settings.hpp](file:///home/darkfell/dev/l3kvg/include/L3KVG/Settings.hpp)):

| Field | Default | Unit | Operational Impact & Tuning Guidance |
| :--- | :---: | :---: | :--- |
| `node_id` | `1` | ID | Local node identifier passed to `Engine` constructor. |
| `default_principal_id` | `INTERNAL_UID` (`0xFFFFFFFF`) | UID | Default ACL principal when caller does not supply one. |
| `node_cache_shards` | `8` | shards | Number of independent LRU lock shards for `Node` objects. Increase on >16-core hosts to reduce mutex contention. |
| `node_cache_size_per_shard` | `2000` | nodes | Max cached `Node` objects per shard (`8 * 2000 = 16,000` total by default). Increase if `SREMetrics::get_hit_ratio()` drops on read-heavy workloads. |
| `edge_write_shards` | `8` | shards | Partition count for `EdgeCoordinator` async write batching. |
| `edge_flush_interval_ms` | `2` | ms | Micro-batch flush interval in `EdgeCoordinator`. Lower = lower tail latency; higher = better write coalescing. |
| `prefix_scan_limit` | `100000` | keys | Safety cap on `scan_kv_prefix` / `get_nodes_by_prefix` scans to prevent OOM on unbounded queries. |
| `node_initial_buffer_size` | `1024` | bytes | Initial `lite3cpp::Buffer` capacity allocated for new node payloads. |
| `default_min_weight` | `-999999.0` | float | Default minimum edge weight filter when unspecified. |
| `fallback_to_full_scan_on_index_miss` | `false` | bool | Keep `false` in production; enabling `true` allows full KV scans when a secondary index entry is missing, trading latency for completeness. |
| `zmq_sndhwm` | `1000` | msgs | ZeroMQ send high-water mark per `ZMQ_DEALER` socket. Increase under heavy cross-shard burst traffic. |
| `fed_timeout_ms` | `30000` | ms | RPC timeout for remote shard/federation calls before throwing `FederationTimeoutException`. |
| `breaker_failure_threshold` | `3` | failures | Consecutive failures before a peer's circuit breaker transitions `CLOSED -> OPEN`. |
| `breaker_reset_timeout_ms` | `5000` | ms | Cooldown before an `OPEN` breaker transitions to `HALF_OPEN` for probing. |
| `health_check_interval_ms` | `1000` | ms | Background peer health-check cadence in `RemoteL3KVClient`. |
| `cluster_endpoints` | `{}` | URLs | Initial peer ZeroMQ endpoints. |

---

## 3. Container & Mesh Deployment

`l3kvg` includes a [Dockerfile](file:///home/darkfell/dev/l3kvg/Dockerfile) and a 3-node [docker-compose.yml](file:///home/darkfell/dev/l3kvg/docker-compose.yml) template (`l3kvg-1`, `l3kvg-2`, `l3kvg-3`):
* **Persistent Volumes**: Mount `/app/data` to durable host storage so `kv.wal`, `edges.wal`, and `labels.dict` survive container restarts.
* **Restart Policy**: `restart_policy: on-failure` ensures automatic WAL replay if a node terminates abnormally.
* **Network Isolation**: Keep `zmq_port` (`5556`) and `http_port` (`8080`) on a private mesh network (`l3kvg-mesh`) and terminate TLS/auth at a reverse proxy if exposing HTTP externally.
