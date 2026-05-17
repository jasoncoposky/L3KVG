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
  int fed_timeout_ms = 500;
  int breaker_failure_threshold = 3;
  int breaker_reset_timeout_ms = 5000;
  int health_check_interval_ms = 1000;
};

} // namespace l3kvg
