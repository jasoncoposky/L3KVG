#!/bin/sh
FAILED=0
IFS=";"
FILES="../../../include/httplib.h;../../../src/Cypher.cpp;../../../src/EdgeCoordinator.cpp;../../../src/Engine.cpp;../../../src/FederationResolver.cpp;../../../src/Node.cpp;../../../src/Query.cpp;../../../src/RemoteL3KVClient.cpp;../../../src/server/dbg_server.cpp;../../../src/server/main.cpp;../../../tests/bench_distributed.cpp;../../../tests/bench_l3kvg.cpp;../../../tests/bench_unified.cpp;../../../tests/test_async_traversal.cpp;../../../tests/test_circuit_breaker.cpp;../../../tests/test_client.cpp;../../../tests/test_cluster_resolver.cpp;../../../tests/test_cypher.cpp;../../../tests/test_edge_coordinator.cpp;../../../tests/test_edge_properties.cpp;../../../tests/test_engine.cpp;../../../tests/test_federation_complex.cpp;../../../tests/test_federation_id.cpp;../../../tests/test_federation_integration.cpp;../../../tests/test_federation_query.cpp;../../../tests/test_federation_resolver.cpp;../../../tests/test_fluent_api.cpp;../../../tests/test_remote_client.cpp;../../../tests/test_replication_broadcast.cpp;../../../tests/test_replication_conflict.cpp;../../../tests/test_replication_loop.cpp;../../../tests/test_replication_routing.cpp;../../../tests/test_secure_mesh.cpp;../../../tests/test_ultimate_mesh.cpp"
IDS=$(echo -en "\n\b")
for FILE in $FILES
do
	clang-format -style=file -output-replacements-xml "$FILE" | grep "<replacement " >/dev/null &&
    {
      echo "$FILE is not correctly formatted"
	  FAILED=1
	}
done
if [ "$FAILED" -eq "1" ] ; then exit 1 ; fi
