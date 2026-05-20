#!/bin/bash
set -e

# Configuration Template
CONFIG_FILE=${1:-config.json}

# Default values
NODE_ID=${NODE_ID:-1}
CLUSTER_ID=${CLUSTER_ID:-1}
CLUSTER_NAME=${CLUSTER_NAME:-"local-cluster"}
ZMQ_PORT=${ZMQ_PORT:-8081}
HTTP_PORT=${HTTP_PORT:-8080}
AUTH_SECRET=${AUTH_SECRET:-""}
DB_PATH=${DB_PATH:-"/app/data/l3kvg_db_${NODE_ID}"}

# PEERS format: "id:host:port,id:host:port"
# Example: "102:l3kvg-2:8081,103:l3kvg-3:8081"
IFS=',' read -ra PEER_LIST <<< "$PEERS"
PEER_JSON="[]"

for peer in "${PEER_LIST[@]}"; do
    IFS=':' read -ra ADDR <<< "$peer"
    PEER_ID=${ADDR[0]}
    PEER_HOST=${ADDR[1]}
    PEER_PORT=${ADDR[2]:-8081}
    
    PEER_ENTRY=$(printf '{"id":%d, "host":"%s", "port":%d}' "$PEER_ID" "$PEER_HOST" "$PEER_PORT")
    PEER_JSON=$(echo "$PEER_JSON" | jq ". += [$PEER_ENTRY]")
done

# Performance Defaults
NODE_CACHE_SHARDS=${NODE_CACHE_SHARDS:-8}
EDGE_WRITE_SHARDS=${EDGE_WRITE_SHARDS:-8}
THREAD_POOL_SIZE=${THREAD_POOL_SIZE:-256}
PREFIX_SCAN_LIMIT=${PREFIX_SCAN_LIMIT:-1000}

# Generate config.json
cat <<EOF > "$CONFIG_FILE"
{
  "node_id": $NODE_ID,
  "cluster_id": $CLUSTER_ID,
  "cluster_name": "$CLUSTER_NAME",
  "port": $HTTP_PORT,
  "zmq_port": $ZMQ_PORT,
  "db_path": "$DB_PATH",
  "auth_secret": "$AUTH_SECRET",
  "thread_pool_size": $THREAD_POOL_SIZE,
  "node_cache_shards": $NODE_CACHE_SHARDS,
  "edge_write_shards": $EDGE_WRITE_SHARDS,
  "prefix_scan_limit": $PREFIX_SCAN_LIMIT,
  "peers": $PEER_JSON,
  "federations": []
}
EOF

echo "[Docker] Generated config.json for Node $NODE_ID in Cluster $CLUSTER_NAME"
exec "$@"
