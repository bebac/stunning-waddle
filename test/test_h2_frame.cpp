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
