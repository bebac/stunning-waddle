add_executable(hpack_benchmark
    hpack_benchmark.cpp
)

target_compile_features(hpack_benchmark PRIVATE cxx_std_20)

target_include_directories(hpack_benchmark
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(hpack_benchmark
    PRIVATE
        stunning-waddle
)

add_executable(h2_engine_benchmark
    h2_engine_benchmark.cpp
)

target_compile_features(h2_engine_benchmark PRIVATE cxx_std_20)

target_include_directories(h2_engine_benchmark
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(h2_engine_benchmark
    PRIVATE
        stunning-waddle
)
