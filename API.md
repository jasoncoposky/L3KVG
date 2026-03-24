# L3KVG HTTP API Specification

L3KVG nodes expose a RESTful API for both dashboard visualization and internal cluster coordination.

## External API (Query & Metrics)

### POST `/api/query`
Executes a Cypher query against the graph.
- **Body**: `{"query": "MATCH (n) RETURN n"}`
- **Response**: JSON array of matched nodes and edges.

### GET `/api/metrics`
Returns real-time SRE metrics.
- **Response**:
```json
{
  "uptime": 3600,
  "nodes": 1024,
  "edges": 5000,
  "traversal_p99_us": 450,
  "cross_shard_hops": 12
}
```

## Internal API (Cluster Coordination)

### POST `/api/internal/neighbors`
Retrieves neighbors for a specific node (used for remote traversal).
- **Body**: `{"target": "uuid", "label": "string", "min_weight": 0.5}`
- **Response**: `["neighbor_uuid_1", "neighbor_uuid_2"]`

### POST `/api/internal/put_node`
Upserts a node into the local shard.
- **Body**: `{"target": "uuid", "payload": {...}}`

### POST `/api/internal/put_edge`
Upserts an edge into the local shard.
- **Body**: `{"key": "e:out:...", "payload": {...}}`

### GET `/api/internal/node/{id}`
Retrieves raw node BSON buffer.
- **Response**: Binary BSON data.

## Security Warning
> [!WARNING]
> The current HTTP implementation does **NOT** include Authentication or TLS. It is intended for use within a trusted private network or behind a reverse proxy (e.g., Nginx) that handles security.
