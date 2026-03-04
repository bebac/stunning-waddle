cmake_minimum_required(VERSION 3.15)
project(stunning-waddle VERSION 0.1.0 LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Define the Library
add_library(stunning-waddle STATIC
    source/context_base.cpp
    source/client_context.cpp
    source/server_context.cpp
    source/stream.cpp
    source/headers.cpp
    source/v2/engine.cpp
    source/v2/frame_parser.cpp
    source/v2/frame.cpp
    source/v2/hpack_context.cpp
    source/v2/hpack_huffman.cpp
    source/v2/hpack.cpp
    # Add other .cpp files here
)

# Set up Include Paths
target_include_directories(stunning-waddle
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        src
)

target_compile_features(stunning-waddle PRIVATE cxx_std_20)

option(BUILD_TESTING "Build the testing suite" OFF)
option(BUILD_BENCHMARKS "Build the benchmark suite" OFF)

if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(test)
endif()

if(BUILD_BENCHMARKS)
    add_subdirectory(bench)
endif()

message(STATUS "BUILD_TESTING value is: ${BUILD_TESTING}")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test/CMakeLists.txt")
    message(STATUS "test/CMakeLists.txt FOUND")
else()
    message(STATUS "test/CMakeLists.txt NOT FOUND")
endif()
