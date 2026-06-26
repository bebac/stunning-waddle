#include "doctest/doctest.h"
#include "test_helpers.h"
#include "http/v2/frame.h"
#include <vector>

TEST_CASE("HTTP/2 Frame Header Definitions")
{
  http::v2::frame_header header;
  header.length = 0x123456;
  header.type = http::v2::frame_type::headers;
  header.flags = std::byte(0x01);
  header.stream_id = 0x7FFFFFFF;

  CHECK(static_cast<uint32_t>(header.length) == 0x123456);
  CHECK(header.type == http::v2::frame_type::headers);
  CHECK(header.flags == std::byte(0x01));
  CHECK(static_cast<uint32_t>(header.stream_id) == 0x7FFFFFFF);
}

TEST_CASE("HTTP/2 DATA Frame")
{
  auto payload = make_bytes('h', 'e', 'l', 'l', 'o');
  std::vector<std::byte> dst;
  http::v2::encode_data_frame(dst, 1, payload, true);

  CHECK(dst[0] == std::byte(0));
  CHECK(dst[1] == std::byte(0));
  CHECK(dst[2] == std::byte(5));
  CHECK(dst[3] == std::byte(http::v2::frame_type::data));
  CHECK(dst[4] == std::byte(0x01));
  CHECK(dst[5] == std::byte(0));
  CHECK(dst[6] == std::byte(0));
  CHECK(dst[7] == std::byte(0));
  CHECK(dst[8] == std::byte(1));
  CHECK(std::ranges::equal(std::span(dst).subspan(9, 5), payload));
}

TEST_CASE("HTTP/2 HEADERS Frame")
{
  auto header_block = make_bytes(0x82, 0x86, 0x84); // GET, http, /
  std::vector<std::byte> dst;
  http::v2::encode_headers_frame(dst, 1, header_block, true, true); // stream_id, block, end_headers, end_stream

  CHECK(dst[0] == std::byte(0));
  CHECK(dst[1] == std::byte(0));
  CHECK(dst[2] == std::byte(3)); // Length
  CHECK(dst[3] == std::byte(http::v2::frame_type::headers)); // Type
  // Flags: END_STREAM (0x01) | END_HEADERS (0x04) = 0x05
  CHECK(dst[4] == std::byte(0x05));
  CHECK(dst[5] == std::byte(0));
  CHECK(dst[6] == std::byte(0));
  CHECK(dst[7] == std::byte(0));
  CHECK(dst[8] == std::byte(1)); // Stream ID
  CHECK(std::ranges::equal(std::span(dst).subspan(9, 3), header_block));
}

TEST_CASE("HTTP/2 SETTINGS Frame")
{
  std::vector<http::v2::setting> settings;
  settings.push_back({http::v2::settings_id::max_concurrent_streams, 100});

  std::vector<std::byte> dst;
  http::v2::encode_settings_frame(dst, settings, false); // ack = false

  // Header
  CHECK(dst[0] == std::byte(0));
  CHECK(dst[1] == std::byte(0));
  CHECK(dst[2] == std::byte(6)); // Length: 2 bytes ID + 4 bytes value
  CHECK(dst[3] == std::byte(http::v2::frame_type::settings)); // Type
  CHECK(dst[4] == std::byte(0)); // Flags: 0 (no ACK)
  CHECK(dst[5] == std::byte(0));
  CHECK(dst[6] == std::byte(0));
  CHECK(dst[7] == std::byte(0));
  CHECK(dst[8] == std::byte(0)); // Stream ID: 0

  // Payload
  // Identifier (16 bits)
  uint16_t id = static_cast<uint16_t>(http::v2::settings_id::max_concurrent_streams);
  CHECK(dst[9] == std::byte((id >> 8) & 0xFF));
  CHECK(dst[10] == std::byte(id & 0xFF));
  // Value (32 bits)
  uint32_t value = 100;
  CHECK(dst[11] == std::byte((value >> 24) & 0xFF));
  CHECK(dst[12] == std::byte((value >> 16) & 0xFF));
  CHECK(dst[13] == std::byte((value >> 8) & 0xFF));
  CHECK(dst[14] == std::byte(value & 0xFF));
}

TEST_CASE("Frame Serialization")
{
  auto payload = make_bytes('h', 'i');
  std::vector<std::byte> dst;
  http::v2::encode_data_frame(dst, 1, payload, true);

  // Header (9 bytes) + Payload (2 bytes) = 11 bytes
  CHECK(dst.size() == 11);

  // Check Header
  CHECK(dst[0] == std::byte(0x00));
  CHECK(dst[1] == std::byte(0x00));
  CHECK(dst[2] == std::byte(0x02));                                    // Length: 2
  CHECK(dst[3] == static_cast<std::byte>(http::v2::frame_type::data)); // Type: DATA
  CHECK(dst[4] == std::byte(0x01));                                    // Flags: END_STREAM
  CHECK(dst[5] == std::byte(0x00));
  CHECK(dst[6] == std::byte(0x00));
  CHECK(dst[7] == std::byte(0x00));
  CHECK(dst[8] == std::byte(0x01)); // Stream ID: 1

  // Check Payload
  CHECK(dst[9] == std::byte('h'));
  CHECK(dst[10] == std::byte('i'));
}

TEST_CASE("HTTP/2 GOAWAY Frame")
{
  SUBCASE("Basic GOAWAY frame without debug data")
  {
    std::vector<std::byte> dst;
    uint32_t last_stream_id = 0x7FFFFFFF; // Maximum valid 31-bit stream ID
    uint32_t error_code = 0x00; // NO_ERROR

    http::v2::encode_goaway_frame(dst, last_stream_id, error_code);

    // Frame header (9 bytes) + payload (8 bytes) = 17 bytes
    CHECK(dst.size() == 17);

    // Check frame header
    CHECK(dst[0] == std::byte(0x00)); // Length high byte
    CHECK(dst[1] == std::byte(0x00)); // Length mid byte
    CHECK(dst[2] == std::byte(0x08)); // Length low byte (8 bytes payload)
    CHECK(dst[3] == std::byte(http::v2::frame_type::goaway)); // Type: GOAWAY (0x07)
    CHECK(dst[4] == std::byte(0x00)); // Flags: none
    CHECK(dst[5] == std::byte(0x00)); // Reserved/Stream ID high byte
    CHECK(dst[6] == std::byte(0x00)); // Stream ID mid-high byte
    CHECK(dst[7] == std::byte(0x00)); // Stream ID mid-low byte
    CHECK(dst[8] == std::byte(0x00)); // Stream ID low byte (must be 0 for GOAWAY)

    // Check payload - Last Stream ID (4 bytes, big-endian)
    CHECK(dst[9] == std::byte(0x7F));  // 0x7FFFFFFF >> 24
    CHECK(dst[10] == std::byte(0xFF)); // 0x7FFFFFFF >> 16
    CHECK(dst[11] == std::byte(0xFF)); // 0x7FFFFFFF >> 8
    CHECK(dst[12] == std::byte(0xFF)); // 0x7FFFFFFF

    // Check payload - Error Code (4 bytes, big-endian)
    CHECK(dst[13] == std::byte(0x00)); // 0x00 >> 24
    CHECK(dst[14] == std::byte(0x00)); // 0x00 >> 16
    CHECK(dst[15] == std::byte(0x00)); // 0x00 >> 8
    CHECK(dst[16] == std::byte(0x00)); // 0x00
  }

  SUBCASE("GOAWAY frame with PROTOCOL_ERROR")
  {
    std::vector<std::byte> dst;
    uint32_t last_stream_id = 1;
    uint32_t error_code = 0x01; // PROTOCOL_ERROR

    http::v2::encode_goaway_frame(dst, last_stream_id, error_code);

    CHECK(dst.size() == 17);

    // Check payload - Last Stream ID
    CHECK(dst[9] == std::byte(0x00));
    CHECK(dst[10] == std::byte(0x00));
    CHECK(dst[11] == std::byte(0x00));
    CHECK(dst[12] == std::byte(0x01));

    // Check payload - Error Code (PROTOCOL_ERROR = 0x01)
    CHECK(dst[13] == std::byte(0x00));
    CHECK(dst[14] == std::byte(0x00));
    CHECK(dst[15] == std::byte(0x00));
    CHECK(dst[16] == std::byte(0x01));
  }

  SUBCASE("GOAWAY frame with debug data")
  {
    std::vector<std::byte> dst;
    uint32_t last_stream_id = 42;
    uint32_t error_code = 0x02; // INTERNAL_ERROR
    auto debug_data = make_bytes('D', 'E', 'B', 'U', 'G');

    http::v2::encode_goaway_frame(dst, last_stream_id, error_code, debug_data);

    // Frame header (9 bytes) + payload (8 + 5 bytes) = 22 bytes
    CHECK(dst.size() == 22);

    // Check frame header - length should be 13 (8 + 5)
    CHECK(dst[0] == std::byte(0x00));
    CHECK(dst[1] == std::byte(0x00));
    CHECK(dst[2] == std::byte(13));

    // Check payload - Last Stream ID (42)
    CHECK(dst[9] == std::byte(0x00));
    CHECK(dst[10] == std::byte(0x00));
    CHECK(dst[11] == std::byte(0x00));
    CHECK(dst[12] == std::byte(42));

    // Check payload - Error Code (INTERNAL_ERROR = 0x02)
    CHECK(dst[13] == std::byte(0x00));
    CHECK(dst[14] == std::byte(0x00));
    CHECK(dst[15] == std::byte(0x00));
    CHECK(dst[16] == std::byte(0x02));

    // Check debug data
    CHECK(dst[17] == std::byte('D'));
    CHECK(dst[18] == std::byte('E'));
    CHECK(dst[19] == std::byte('B'));
    CHECK(dst[20] == std::byte('U'));
    CHECK(dst[21] == std::byte('G'));
  }

  SUBCASE("GOAWAY frame with empty debug data")
  {
    std::vector<std::byte> dst;
    uint32_t last_stream_id = 100;
    uint32_t error_code = 0x00; // NO_ERROR
    std::vector<std::byte> empty_debug_data;

    http::v2::encode_goaway_frame(dst, last_stream_id, error_code, empty_debug_data);

    // Should be same as basic case (no debug data)
    CHECK(dst.size() == 17);

    // Check payload - Last Stream ID (100)
    CHECK(dst[9] == std::byte(0x00));
    CHECK(dst[10] == std::byte(0x00));
    CHECK(dst[11] == std::byte(0x00));
    CHECK(dst[12] == std::byte(100));
  }

  SUBCASE("GOAWAY frame with maximum error code")
  {
    std::vector<std::byte> dst;
    uint32_t last_stream_id = 1;
    uint32_t error_code = 0x0D; // HTTP_1_1_REQUIRED (highest defined error code)

    http::v2::encode_goaway_frame(dst, last_stream_id, error_code);

    CHECK(dst.size() == 17);

    // Check payload - Error Code (HTTP_1_1_REQUIRED = 0x0D)
    CHECK(dst[13] == std::byte(0x00));
    CHECK(dst[14] == std::byte(0x00));
    CHECK(dst[15] == std::byte(0x00));
    CHECK(dst[16] == std::byte(0x0D));
  }

  SUBCASE("GOAWAY frame with zero last_stream_id")
  {
    std::vector<std::byte> dst;
    uint32_t last_stream_id = 0; // No streams processed
    uint32_t error_code = 0x00; // NO_ERROR

    http::v2::encode_goaway_frame(dst, last_stream_id, error_code);

    CHECK(dst.size() == 17);

    // Check payload - Last Stream ID (0)
    CHECK(dst[9] == std::byte(0x00));
    CHECK(dst[10] == std::byte(0x00));
    CHECK(dst[11] == std::byte(0x00));
    CHECK(dst[12] == std::byte(0x00));
  }

  SUBCASE("GOAWAY frame preserves stream_id = 0 requirement")
  {
    std::vector<std::byte> dst;
    uint32_t last_stream_id = 123;
    uint32_t error_code = 0x01;

    http::v2::encode_goaway_frame(dst, last_stream_id, error_code);

    // Stream ID in header must be 0 for GOAWAY frames
    CHECK(dst[5] == std::byte(0x00));
    CHECK(dst[6] == std::byte(0x00));
    CHECK(dst[7] == std::byte(0x00));
    CHECK(dst[8] == std::byte(0x00));
  }
}
