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

} // namespace l3kvg
