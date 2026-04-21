cmake_minimum_required(VERSION 3.20)
project(L3KVG VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Include L3KV, which transitively includes lite3-cpp and now ZeroMQ via FetchContent
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../L3KV ${CMAKE_CURRENT_BINARY_DIR}/L3KV)



add_library(l3kvg_engine STATIC
    src/Engine.cpp
    src/Node.cpp
    src/Query.cpp
    src/Cypher.cpp
    src/ClusterResolver.cpp
    src/RemoteL3KVClient.cpp
    src/EdgeCoordinator.cpp
)

target_include_directories(l3kvg_engine PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    "${CMAKE_CURRENT_SOURCE_DIR}/../lib/boost_1_89_0"
    "${CMAKE_CURRENT_SOURCE_DIR}/../L3KV/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../lib/liblite3client/external"
)

if(MSVC)
    target_compile_options(l3kvg_engine PRIVATE /bigobj)
endif()

target_link_libraries(l3kvg_engine PUBLIC
    l3kv_engine
    lite3-cpp
    libzmq
    cppzmq
    ws2_32
    iphlpapi
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

add_executable(test_cluster_resolver tests/test_cluster_resolver.cpp)
target_link_libraries(test_cluster_resolver PRIVATE l3kvg_engine gtest_main Threads::Threads)

add_executable(test_remote_client tests/test_remote_client.cpp)
target_link_libraries(test_remote_client PRIVATE l3kvg_engine gtest_main Threads::Threads ws2_32)

add_executable(test_edge_coordinator tests/test_edge_coordinator.cpp)
target_link_libraries(test_edge_coordinator PRIVATE l3kvg_engine gtest_main Threads::Threads ws2_32)

add_executable(test_async_traversal tests/test_async_traversal.cpp)
target_link_libraries(test_async_traversal PRIVATE l3kvg_engine gtest_main Threads::Threads ws2_32)

add_executable(test_edge_properties tests/test_edge_properties.cpp)
target_link_libraries(test_edge_properties PRIVATE l3kvg_engine gtest_main Threads::Threads ws2_32)

add_test(NAME test_engine COMMAND test_l3kvg)
add_test(NAME test_fluent COMMAND test_fluent_api)
add_test(NAME test_cypher_ast COMMAND test_cypher)
add_test(NAME test_cluster COMMAND test_cluster_resolver)
add_test(NAME test_remote COMMAND test_remote_client)
add_test(NAME test_coordinator COMMAND test_edge_coordinator)
add_test(NAME test_async COMMAND test_async_traversal)
add_test(NAME test_edge_props COMMAND test_edge_properties)

add_executable(l3kvg_server src/server/main.cpp)
target_link_libraries(l3kvg_server PRIVATE l3kvg_engine Threads::Threads ws2_32 wsock32 crypt32)

add_executable(dbg_server src/server/dbg_server.cpp)
target_link_libraries(dbg_server PRIVATE l3kvg_engine Threads::Threads ws2_32 wsock32 crypt32)

add_executable(bench_l3kvg tests/bench_l3kvg.cpp)
target_link_libraries(bench_l3kvg PRIVATE l3kvg_engine Threads::Threads)

add_executable(bench_distributed tests/bench_distributed.cpp)
target_link_libraries(bench_distributed PRIVATE l3kvg_engine Threads::Threads ws2_32 wsock32 crypt32)

add_executable(bench_unified tests/bench_unified.cpp)
target_link_libraries(bench_unified PRIVATE l3kvg_engine Threads::Threads ws2_32 wsock32 crypt32)
