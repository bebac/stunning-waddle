include(FetchContent)

FetchContent_Declare(
  doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG        v2.4.12
)
FetchContent_MakeAvailable(doctest)

# Create the test executable
add_executable(unit_tests
    test_h2_engine_usability.cpp
    test_h2_engine.cpp
    test_h2_frame_parser.cpp
    test_h2_frame.cpp
    test_h2_hpack_compliance.cpp
    test_h2_hpack.cpp
    test_h2_huffman.cpp
    test_h2_request_payload.cpp
    test_headers.cpp
    test_http_client_context.cpp
    test_server_context.cpp
    test_server_engine.cpp
    test_main.cpp
)

target_compile_features(unit_tests PRIVATE cxx_std_20)

# Link against your library AND doctest
target_link_libraries(unit_tests
    PRIVATE
        stunning-waddle
        doctest::doctest
)

# Register the executable with CTest
add_test(NAME AllTests COMMAND unit_tests)
