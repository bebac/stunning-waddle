#include "doctest/doctest.h"
#include "test_helpers.h"
#include "http/v2/engine.h"
#include <vector>

TEST_CASE("HTTP/2 Engine Request Payload")
{
  http::v2::engine engine;

  // Consume preface and initial settings
  engine.output_end(24 + 9);

  SUBCASE("send_request with payload generates HEADERS and DATA frames")
  {
    std::string payload_str = "{\"key\": \"value\"}";
    auto payload = std::as_bytes(std::span(payload_str));

    http::headers h;
    engine.send_headers(1, "POST", "/test", "localhost", h, false);
    engine.send_data(1, payload, true);

    auto out = engine.output_begin();
    CHECK(out.size() > 0);

    // We expect:
    // 1. HEADERS frame (Stream ID 1, Flags: END_HEADERS=0x04)
    // 2. DATA frame (Stream ID 1, Flags: END_STREAM=0x01)

    // Check HEADERS frame header
    // Type is 0x01 (HEADERS)
    CHECK(out[3] == std::byte(0x01));
    // Flags: END_HEADERS (0x04) is set, but END_STREAM (0x01) should NOT be set.
    CHECK((out[4] & std::byte(0x01)) == std::byte(0x00));
    CHECK((out[4] & std::byte(0x04)) == std::byte(0x04));

    uint32_t headers_len =
        (static_cast<uint32_t>(out[0]) << 16) | (static_cast<uint32_t>(out[1]) << 8) | static_cast<uint32_t>(out[2]);

    // Check DATA frame header
    size_t data_frame_offset = 9 + headers_len;
    CHECK(out.size() > data_frame_offset);

    // Type is 0x00 (DATA)
    CHECK(out[data_frame_offset + 3] == std::byte(0x00));
    // Flags: END_STREAM (0x01) should be set
    CHECK((out[data_frame_offset + 4] & std::byte(0x01)) == std::byte(0x01));

    uint32_t data_len = (
      static_cast<uint32_t>(out[data_frame_offset + 0]) << 16) |
      (static_cast<uint32_t>(out[data_frame_offset + 1]) << 8) |
      static_cast<uint32_t>(out[data_frame_offset + 2]
    );

    CHECK(data_len == payload.size());

    // Check payload content
    std::string received_payload(reinterpret_cast<const char*>(&out[data_frame_offset + 9]), data_len);
    CHECK(received_payload == payload_str);

    engine.output_end(out.size());
  }

  SUBCASE("Large payload generates multiple DATA frames")
  {
    // Create a payload larger than 16384 bytes
    std::vector<std::byte> large_payload(20000, std::byte('A'));

    http::headers h;
    engine.send_headers(1, "POST", "/large", "localhost", h, false);
    engine.send_data(1, large_payload, true);

    auto out = engine.output_begin();
    CHECK(out.size() > 0);

    // HEADERS frame
    uint32_t headers_len =
        (static_cast<uint32_t>(out[0]) << 16) | (static_cast<uint32_t>(out[1]) << 8) | static_cast<uint32_t>(out[2]);

    // First DATA frame (should be 16384 bytes)
    size_t data1_offset = 9 + headers_len;
    uint32_t data1_len = (
      static_cast<uint32_t>(out[data1_offset + 0]) << 16) |
      (static_cast<uint32_t>(out[data1_offset + 1]) << 8) |
      static_cast<uint32_t>(out[data1_offset + 2]
    );
    CHECK(data1_len == 16384);
    CHECK((out[data1_offset + 4] & std::byte(0x01)) == std::byte(0x00)); // END_STREAM NOT set

    // Second DATA frame (should be remaining bytes)
    size_t data2_offset = data1_offset + 9 + 16384;
    uint32_t data2_len = (
      static_cast<uint32_t>(out[data2_offset + 0]) << 16) |
      (static_cast<uint32_t>(out[data2_offset + 1]) << 8) |
      static_cast<uint32_t>(out[data2_offset + 2]
    );
    CHECK(data2_len == 20000 - 16384);
    CHECK((out[data2_offset + 4] & std::byte(0x01)) == std::byte(0x01)); // END_STREAM set

    engine.output_end(out.size());
  }
}
