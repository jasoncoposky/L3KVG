cmake_minimum_required(VERSION 3.20)
project(L3KVG VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Include L3KV, which transitively includes lite3-cpp and now ZeroMQ via FetchContent
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../L3KV ${CMAKE_CURRENT_BINARY_DIR}/L3KV)

include(FetchContent)
FetchContent_Declare(
  citor
  GIT_REPOSITORY https://github.com/Lallapallooza/citor.git
  GIT_TAG        main
)
FetchContent_MakeAvailable(citor)



add_library(l3kvg_engine STATIC
    src/Engine.cpp
    src/Node.cpp
    src/Query.cpp
    src/Cypher.cpp
    src/FederationResolver.cpp
    src/RemoteL3KVClient.cpp
    src/EdgeCoordinator.cpp
    src/ThreadPool.cpp
    src/MutationBatch.cpp
    src/LabelRegistry.cpp
    src/AdjacencyStore.cpp
    src/SIMDFilter.cpp
    src/BinaryEdgeWAL.cpp
)

include(CheckCXXCompilerFlag)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64|i[3-6]86")
    if(MSVC)
        check_cxx_compiler_flag("/arch:AVX2" COMPILER_SUPPORTS_AVX2)
        if(COMPILER_SUPPORTS_AVX2)
            target_compile_options(l3kvg_engine PUBLIC /arch:AVX2)
        endif()
    else()
        check_cxx_compiler_flag("-mavx2" COMPILER_SUPPORTS_AVX2)
        if(COMPILER_SUPPORTS_AVX2)
            target_compile_options(l3kvg_engine PUBLIC -mavx2)
        endif()
    endif()
endif()

target_include_directories(l3kvg_engine PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    "${CMAKE_CURRENT_SOURCE_DIR}/../lib/boost_1_89_0"
    "${CMAKE_CURRENT_SOURCE_DIR}/../L3KV/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../L3KV/lib/concurrentqueue"
    "${CMAKE_CURRENT_SOURCE_DIR}/../lib/liblite3client/external"
    ${citor_SOURCE_DIR}/single_include
    ${citor_SOURCE_DIR}/include
)

if(MSVC)
    target_compile_options(l3kvg_engine PRIVATE /bigobj)
endif()

target_link_libraries(l3kvg_engine PUBLIC
    l3kv_engine
    lite3-cpp
    libzmq
    cppzmq
    
    $<$<BOOL:${WIN32}>:ws2_32>
    $<$<BOOL:${WIN32}>:iphlpapi>
)


if(MSVC)
    add_compile_definitions(_WIN32_WINNT=0x0A00 NOMINMAX)
    add_compile_options(/bigobj)
endif()

# Tests
enable_testing()
find_package(Threads REQUIRED)
add_executable(test_l3kvg tests/test_engine.cpp)
target_link_libraries(test_l3kvg PRIVATE l3kvg_engine gtest_main Threads::Threads)

add_executable(test_fluent_api tests/test_fluent_api.cpp)
target_link_libraries(test_fluent_api PRIVATE l3kvg_engine gtest_main Threads::Threads)

add_executable(test_cypher tests/test_cypher.cpp)
target_link_libraries(test_cypher PRIVATE l3kvg_engine gtest_main Threads::Threads)

add_executable(test_federation_resolver tests/test_federation_resolver.cpp)
target_link_libraries(test_federation_resolver PRIVATE l3kvg_engine gtest_main Threads::Threads)

add_executable(test_remote_client tests/test_remote_client.cpp)
target_link_libraries(test_remote_client PRIVATE l3kvg_engine gtest_main Threads::Threads $<$<BOOL:${WIN32}>:ws2_32>)

add_executable(test_edge_coordinator tests/test_edge_coordinator.cpp)
target_link_libraries(test_edge_coordinator PRIVATE l3kvg_engine gtest_main Threads::Threads $<$<BOOL:${WIN32}>:ws2_32>)

add_executable(test_async_traversal tests/test_async_traversal.cpp)
target_link_libraries(test_async_traversal PRIVATE l3kvg_engine gtest_main Threads::Threads $<$<BOOL:${WIN32}>:ws2_32>)

add_executable(test_edge_properties tests/test_edge_properties.cpp)
target_link_libraries(test_edge_properties PRIVATE l3kvg_engine gtest_main Threads::Threads $<$<BOOL:${WIN32}>:ws2_32>)

add_executable(test_federation_id tests/test_federation_id.cpp)
target_link_libraries(test_federation_id PRIVATE l3kvg_engine gtest_main Threads::Threads)

add_executable(test_federation_query tests/test_federation_query.cpp)
target_link_libraries(test_federation_query PRIVATE l3kvg_engine gtest_main Threads::Threads $<$<BOOL:${WIN32}>:ws2_32>)

add_test(NAME test_engine COMMAND test_l3kvg)
add_test(NAME test_fluent COMMAND test_fluent_api)
add_test(NAME test_cypher_ast COMMAND test_cypher)
add_test(NAME test_federation_resolver COMMAND test_federation_resolver)
add_test(NAME test_remote COMMAND test_remote_client)
add_test(NAME test_coordinator COMMAND test_edge_coordinator)
add_test(NAME test_async COMMAND test_async_traversal)
add_test(NAME test_edge_props COMMAND test_edge_properties)
add_test(NAME test_federation COMMAND test_federation_id)
add_test(NAME test_federation_query COMMAND test_federation_query)

add_executable(test_mutation_batch tests/test_mutation_batch.cpp)
target_link_libraries(test_mutation_batch PRIVATE l3kvg_engine gtest_main Threads::Threads)
add_test(NAME test_mutation_batch COMMAND test_mutation_batch)

add_executable(test_packed_edge tests/test_packed_edge.cpp)
target_link_libraries(test_packed_edge PRIVATE l3kvg_engine gtest_main Threads::Threads)
add_test(NAME test_packed_edge COMMAND test_packed_edge)

add_executable(test_adjacency_store tests/test_adjacency_store.cpp)
target_link_libraries(test_adjacency_store PRIVATE l3kvg_engine gtest_main Threads::Threads)
add_test(NAME test_adjacency_store COMMAND test_adjacency_store)

add_executable(test_simd_filter tests/test_simd_filter.cpp)
target_link_libraries(test_simd_filter PRIVATE l3kvg_engine gtest_main Threads::Threads)
add_test(NAME test_simd_filter COMMAND test_simd_filter)

add_executable(test_binary_edge_wal tests/test_binary_edge_wal.cpp)
target_link_libraries(test_binary_edge_wal PRIVATE l3kvg_engine gtest_main Threads::Threads)
add_test(NAME test_binary_edge_wal COMMAND test_binary_edge_wal)

add_executable(test_native_engine_integration tests/test_native_engine_integration.cpp)
target_link_libraries(test_native_engine_integration PRIVATE l3kvg_engine gtest_main Threads::Threads)
add_test(NAME test_native_engine_integration COMMAND test_native_engine_integration)

add_executable(test_federation_integration tests/test_federation_integration.cpp)
target_link_libraries(test_federation_integration PRIVATE l3kvg_engine gtest_main Threads::Threads $<$<BOOL:${WIN32}>:ws2_32>)
add_test(NAME test_federation_integration COMMAND test_federation_integration)

add_executable(test_federation_complex tests/test_federation_complex.cpp)
target_link_libraries(test_federation_complex PRIVATE l3kvg_engine gtest_main Threads::Threads $<$<BOOL:${WIN32}>:ws2_32>)
add_test(NAME test_federation_complex COMMAND test_federation_complex)

add_executable(test_circuit_breaker tests/test_circuit_breaker.cpp)
target_link_libraries(test_circuit_breaker PRIVATE l3kvg_engine gtest_main Threads::Threads $<$<BOOL:${WIN32}>:ws2_32>)
add_test(NAME test_circuit_breaker COMMAND test_circuit_breaker)

add_executable(test_replication_routing tests/test_replication_routing.cpp)
target_link_libraries(test_replication_routing PRIVATE l3kvg_engine gtest_main Threads::Threads $<$<BOOL:${WIN32}>:ws2_32>)
add_test(NAME test_replication_routing COMMAND test_replication_routing)

add_executable(test_replication_broadcast tests/test_replication_broadcast.cpp)
target_link_libraries(test_replication_broadcast PRIVATE l3kvg_engine gtest_main Threads::Threads $<$<BOOL:${WIN32}>:ws2_32>)
add_test(NAME test_replication_broadcast COMMAND test_replication_broadcast)

add_executable(test_replication_loop tests/test_replication_loop.cpp)
target_link_libraries(test_replication_loop PRIVATE l3kvg_engine gtest_main Threads::Threads $<$<BOOL:${WIN32}>:ws2_32>)
add_test(NAME test_replication_loop COMMAND test_replication_loop)

add_executable(test_replication_conflict tests/test_replication_conflict.cpp)
target_link_libraries(test_replication_conflict PRIVATE l3kvg_engine gtest_main Threads::Threads $<$<BOOL:${WIN32}>:ws2_32>)
add_test(NAME test_replication_conflict COMMAND test_replication_conflict)

add_executable(test_ultimate_mesh tests/test_ultimate_mesh.cpp)
target_link_libraries(test_ultimate_mesh PRIVATE l3kvg_engine gtest_main Threads::Threads $<$<BOOL:${WIN32}>:ws2_32>)
add_test(NAME test_ultimate_mesh COMMAND test_ultimate_mesh)

add_executable(test_secure_mesh tests/test_secure_mesh.cpp)
target_link_libraries(test_secure_mesh PRIVATE l3kvg_engine gtest_main Threads::Threads $<$<BOOL:${WIN32}>:ws2_32>)
add_test(NAME test_secure_mesh COMMAND test_secure_mesh)

add_executable(test_server_concurrency tests/test_server_concurrency.cpp)
target_link_libraries(test_server_concurrency PRIVATE l3kvg_engine gtest_main Threads::Threads $<$<BOOL:${WIN32}>:ws2_32>)
add_test(NAME test_server_concurrency COMMAND test_server_concurrency)

add_executable(l3kvg_server src/server/main.cpp)
target_link_libraries(l3kvg_server PRIVATE l3kvg_engine Threads::Threads $<$<BOOL:${WIN32}>:ws2_32> $<$<BOOL:${WIN32}>:wsock32> $<$<BOOL:${WIN32}>:crypt32>)

add_executable(dbg_server src/server/dbg_server.cpp)
target_link_libraries(dbg_server PRIVATE l3kvg_engine Threads::Threads $<$<BOOL:${WIN32}>:ws2_32> $<$<BOOL:${WIN32}>:wsock32> $<$<BOOL:${WIN32}>:crypt32>)

add_executable(bench_l3kvg tests/bench_l3kvg.cpp)
target_link_libraries(bench_l3kvg PRIVATE l3kvg_engine Threads::Threads)

add_executable(bench_irods_patterns tests/bench_irods_patterns.cpp)
target_link_libraries(bench_irods_patterns PRIVATE l3kvg_engine Threads::Threads)

add_executable(bench_engine_core tests/bench_engine_core.cpp)
target_link_libraries(bench_engine_core PRIVATE l3kvg_engine Threads::Threads)

add_executable(bench_unified tests/bench_unified.cpp)
target_link_libraries(bench_unified PRIVATE l3kvg_engine Threads::Threads $<$<BOOL:${WIN32}>:ws2_32> $<$<BOOL:${WIN32}>:wsock32> $<$<BOOL:${WIN32}>:crypt32>)

add_executable(bench_bifurcated_substrate tests/bench_bifurcated_substrate.cpp)
target_link_libraries(bench_bifurcated_substrate PRIVATE l3kvg_engine Threads::Threads)

