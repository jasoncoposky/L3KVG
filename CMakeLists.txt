cmake_minimum_required(VERSION 3.20)
project(L3KVG VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Include L3KV, which transitively includes lite3-cpp
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../L3KV ${CMAKE_CURRENT_BINARY_DIR}/L3KV)

add_library(l3kvg_engine STATIC
    src/Engine.cpp
    src/Node.cpp
    src/Query.cpp
)

target_include_directories(l3kvg_engine PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    "${CMAKE_CURRENT_SOURCE_DIR}/../lib/boost_1_89_0"
)

target_link_libraries(l3kvg_engine PUBLIC
    l3kv_engine
    lite3-cpp
)

if(MSVC)
    target_compile_definitions(l3kvg_engine PRIVATE -D_WIN32_WINNT=0x0A00 -DNOMINMAX)
endif()

# Tests
enable_testing()
find_package(Threads REQUIRED)
add_executable(test_l3kvg tests/test_engine.cpp)
target_link_libraries(test_l3kvg PRIVATE l3kvg_engine gtest_main Threads::Threads)

add_executable(test_fluent_api tests/test_fluent_api.cpp)
target_link_libraries(test_fluent_api PRIVATE l3kvg_engine gtest_main Threads::Threads)

add_executable(bench_l3kvg tests/bench_l3kvg.cpp)
target_link_libraries(bench_l3kvg PRIVATE l3kvg_engine Threads::Threads)
