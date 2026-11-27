#include "doctest/doctest.h"
#include "test_helpers.h"
#include "http/v2/frame.h"
#include "http/error_codes.h"
#include <cstddef>
#include <utility>
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
    http::error_code error_code = http::error_code::no_error;

    http::v2::encode_goaway_frame(dst, last_stream_id, error_code);

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
    http::error_code error_code = http::error_code::protocol_error;

    http::v2::encode_goaway_frame(dst, last_stream_id, error_code);

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
    http::error_code error_code = http::error_code::internal_error;
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
    http::error_code error_code = http::error_code::no_error;
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
    http::error_code error_code = http::error_code::http_1_1_required;

    http::v2::encode_goaway_frame(dst, last_stream_id, error_code);

    // Check payload - Error Code.
    //
    // The unified enum renumbers the HTTP/2-specific codes, so
    // HTTP_1_1_REQUIRED is 0x1006 there. The wire value is the RFC 7540
    // section 7 code, 0x0d: the enum value must never reach the wire.
    // Big-endian encoding: 0x00, 0x00, 0x00, 0x0d
    CHECK(dst[13] == std::byte(0x00));
    CHECK(dst[14] == std::byte(0x00));
    CHECK(dst[15] == std::byte(0x00));
    CHECK(dst[16] == std::byte(0x0d));
  }

  SUBCASE("GOAWAY frame with zero last_stream_id")
  {
    std::vector<std::byte> dst;
    uint32_t last_stream_id = 0; // No streams processed
    http::error_code error_code = http::error_code::no_error;

    http::v2::encode_goaway_frame(dst, last_stream_id, error_code);

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
    http::error_code error_code = http::error_code::protocol_error;

    http::v2::encode_goaway_frame(dst, last_stream_id, error_code);

    CHECK(dst[5] == std::byte(0x00));
    CHECK(dst[6] == std::byte(0x00));
    CHECK(dst[7] == std::byte(0x00));
    CHECK(dst[8] == std::byte(0x00));
  }
}

TEST_CASE("HTTP/2 wire error code mapping")
{
  SUBCASE("HTTP/2 specific enum values never reach the wire")
  {
    // The unified enum keeps the HTTP/2 codes in the 0x1000 range, so a
    // plain cast would put a bogus 32-bit code in the frame.
    CHECK(http::http2_wire_code(http::error_code::settings_timeout) == 0x04);
    CHECK(http::http2_wire_code(http::error_code::frame_size_error) == 0x06);
    CHECK(http::http2_wire_code(http::error_code::compression_error) == 0x09);
    CHECK(http::http2_wire_code(http::error_code::connect_error) == 0x0a);
    CHECK(http::http2_wire_code(http::error_code::enhance_your_calm) == 0x0b);
    CHECK(http::http2_wire_code(http::error_code::inadequate_security) == 0x0c);
    CHECK(http::http2_wire_code(http::error_code::http_1_1_required) == 0x0d);
  }

  SUBCASE("every RFC 7540 wire code round-trips")
  {
    for (uint32_t wire = 0x00; wire <= 0x0d; ++wire)
    {
      CHECK(http::http2_wire_code(http::error_code_from_http2(wire)) == wire);
    }
  }

  SUBCASE("codes without a wire equivalent map to INTERNAL_ERROR")
  {
    // connection_reset (0x04) and timeout (0x05) collide with the wire
    // values of SETTINGS_TIMEOUT and STREAM_CLOSED, so they must not be
    // passed through unchanged.
    CHECK(http::http2_wire_code(http::error_code::connection_reset) == 0x02);
    CHECK(http::http2_wire_code(http::error_code::timeout) == 0x02);

    // HTTP status codes have no HTTP/2 error code either.
    CHECK(http::http2_wire_code(http::error_code::not_found) == 0x02);
    CHECK(http::http2_wire_code(http::error_code::internal_server_error) == 0x02);
  }

  SUBCASE("unknown extension codes survive a re-encode")
  {
    // error_code_from_http2() keeps unrecognised wire codes so they are not
    // lost; encoding them again must not turn them into INTERNAL_ERROR.
    auto extension = http::error_code_from_http2(0x0e);
    CHECK(http::http2_wire_code(extension) == 0x0e);
  }

  SUBCASE("GOAWAY never emits a colliding code")
  {
    std::vector<std::byte> dst;
    http::v2::encode_goaway_frame(dst, 1, http::error_code::timeout);

    REQUIRE(dst.size() == 17);
    // INTERNAL_ERROR, not STREAM_CLOSED (0x05).
    CHECK(dst[13] == std::byte(0x00));
    CHECK(dst[14] == std::byte(0x00));
    CHECK(dst[15] == std::byte(0x00));
    CHECK(dst[16] == std::byte(0x02));
  }

  SUBCASE("GOAWAY encodes the RFC 7540 code for every HTTP/2 error")
  {
    const std::pair<http::error_code, uint32_t> cases[] = {
      {http::error_code::no_error, 0x00},
      {http::error_code::protocol_error, 0x01},
      {http::error_code::internal_error, 0x02},
      {http::error_code::flow_control_error, 0x03},
      {http::error_code::settings_timeout, 0x04},
      {http::error_code::stream_closed, 0x05},
      {http::error_code::frame_size_error, 0x06},
      {http::error_code::refused_stream, 0x07},
      {http::error_code::cancel, 0x08},
      {http::error_code::compression_error, 0x09},
      {http::error_code::connect_error, 0x0a},
      {http::error_code::enhance_your_calm, 0x0b},
      {http::error_code::inadequate_security, 0x0c},
      {http::error_code::http_1_1_required, 0x0d},
    };

    for (const auto& [ec, wire] : cases)
    {
      std::vector<std::byte> dst;
      http::v2::encode_goaway_frame(dst, 1, ec);

      REQUIRE(dst.size() == 17);

      uint32_t encoded =
        (std::to_integer<uint32_t>(dst[13]) << 24) |
        (std::to_integer<uint32_t>(dst[14]) << 16) |
        (std::to_integer<uint32_t>(dst[15]) <<  8) |
         std::to_integer<uint32_t>(dst[16]);

      CHECK(encoded == wire);
    }
  }
}

TEST_CASE("HTTP/2 PING Frame")
{
  SUBCASE("PING ACK echoes the opaque data")
  {
    auto payload = make_bytes(0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08);
    std::vector<std::byte> dst;

    http::v2::encode_ping_frame(dst, payload, true);

    REQUIRE(dst.size() == 17);
    CHECK(dst[0] == std::byte(0x00)); // Length high byte
    CHECK(dst[1] == std::byte(0x00)); // Length mid byte
    CHECK(dst[2] == std::byte(0x08)); // Length low byte (8 bytes payload)
    CHECK(dst[3] == std::byte(http::v2::frame_type::ping));
    CHECK(dst[4] == std::byte(0x01)); // Flags: ACK
    CHECK(dst[5] == std::byte(0x00)); // Stream ID must be 0 for PING
    CHECK(dst[6] == std::byte(0x00));
    CHECK(dst[7] == std::byte(0x00));
    CHECK(dst[8] == std::byte(0x00));

    for (size_t i = 0; i < payload.size(); ++i)
    {
      CHECK(dst[9 + i] == payload[i]);
    }
  }

  SUBCASE("PING without ACK has no ACK flag")
  {
    auto payload = make_bytes(0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11, 0x22);
    std::vector<std::byte> dst;

    http::v2::encode_ping_frame(dst, payload, false);

    REQUIRE(dst.size() == 17);
    CHECK(dst[4] == std::byte(0x00));
  }
}
