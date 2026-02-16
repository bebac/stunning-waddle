#include "doctest/doctest.h"
#include "test_helpers.h"
#include "http/v2/frame_parser.h"
#include <vector>
#include <iostream>

TEST_CASE("HTTP/2 Frame Parser")
{
  http::v2::frame_parser parser;
  bool frame_received = false;
  uint32_t received_length = 0;
  http::v2::frame_type received_type = http::v2::frame_type::data;
  std::byte received_flags = std::byte(0);
  uint32_t received_stream_id = 0;

  parser.on_frame_header([&](http::v2::frame_header h) {
    frame_received = true;
    received_length = h.length;
    received_type = h.type;
    received_flags = h.flags;
    received_stream_id = h.stream_id;
  });

  SUBCASE("Parse a complete DATA frame")
  {
    // Length: 5, Type: 0 (DATA), Flags: 1 (END_STREAM), Stream ID: 1
    auto raw_frame = make_bytes(
      0x00, 0x00, 0x05,           // Length
      0x00,                       // Type
      0x01,                       // Flags
      0x00, 0x00, 0x00, 0x01,     // Stream ID
      'h',  'e',  'l',  'l',  'o' // Payload
    );

    parser.consume(raw_frame);

    CHECK(frame_received == true);
    CHECK(received_length == 5);
    CHECK(received_type == http::v2::frame_type::data);
    CHECK(received_flags == std::byte(0x01));
    CHECK(received_stream_id == 1);
  }

  SUBCASE("Parse a frame split across two buffers")
  {
   auto part1 = make_bytes(
      0x00, 0x00, 0x05, // Length
      0x00,             // Type
      0x01,             // Flags
      0x00, 0x00        // Partial Stream ID
   );

    auto part2 = make_bytes(
      0x00, 0x01,               // Remaining Stream ID
      'h',  'e',  'l', 'l', 'o' // Payload
    );

    parser.consume(part1);
    CHECK(frame_received == false); // Not enough for header yet (needs 9 bytes)

    parser.consume(part2);
    CHECK(frame_received == true);
    CHECK(received_length == 5);
    CHECK(received_type == http::v2::frame_type::data);
  }
}
