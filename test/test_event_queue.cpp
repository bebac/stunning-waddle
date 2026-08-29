#include "doctest/doctest.h"
#include "test_helpers.h"
#include "http/v2/engine.h"
#include "http/v2/hpack_context.h"
#include "http/event.h"
#include <vector>
#include <cstring>

namespace
{
  constexpr std::string_view client_preface{"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"};

  auto make_preface_bytes()
  {
    std::vector<std::byte> bytes;
    bytes.reserve(client_preface.size());
    for (char c : client_preface) {
      bytes.push_back(static_cast<std::byte>(c));
    }
    return bytes;
  }

  // Build a HEADERS frame with END_HEADERS flag for the given stream_id.
  auto make_headers_frame(uint32_t stream_id, const std::vector<http::header>& hdrs, bool end_stream = false)
  {
    http::v2::hpack_context encoder;
    http::v2::hpack_buffer header_block;
    encoder.encode(header_block, hdrs);

    std::vector<std::byte> frame;
    uint32_t len = static_cast<uint32_t>(header_block.size());
    frame.push_back(static_cast<std::byte>((len >> 16) & 0xff));
    frame.push_back(static_cast<std::byte>((len >> 8) & 0xff));
    frame.push_back(static_cast<std::byte>(len & 0xff));
    frame.push_back(std::byte(0x01)); // Type: HEADERS
    uint8_t flags = 0x04; // END_HEADERS
    if (end_stream) flags |= 0x01;
    frame.push_back(static_cast<std::byte>(flags));
    frame.push_back(static_cast<std::byte>((stream_id >> 24) & 0xff));
    frame.push_back(static_cast<std::byte>((stream_id >> 16) & 0xff));
    frame.push_back(static_cast<std::byte>((stream_id >> 8) & 0xff));
    frame.push_back(static_cast<std::byte>(stream_id & 0xff));
    frame.insert(frame.end(), header_block.begin(), header_block.end());
    return frame;
  }

  // Build a DATA frame for the given stream_id.
  auto make_data_frame(uint32_t stream_id, std::string_view payload, bool end_stream = false)
  {
    std::vector<std::byte> frame;
    uint32_t len = static_cast<uint32_t>(payload.size());
    frame.push_back(static_cast<std::byte>((len >> 16) & 0xff));
    frame.push_back(static_cast<std::byte>((len >> 8) & 0xff));
    frame.push_back(static_cast<std::byte>(len & 0xff));
    frame.push_back(std::byte(0x00)); // Type: DATA
    uint8_t flags = end_stream ? 0x01 : 0x00;
    frame.push_back(static_cast<std::byte>(flags));
    frame.push_back(static_cast<std::byte>((stream_id >> 24) & 0xff));
    frame.push_back(static_cast<std::byte>((stream_id >> 16) & 0xff));
    frame.push_back(static_cast<std::byte>((stream_id >> 8) & 0xff));
    frame.push_back(static_cast<std::byte>(stream_id & 0xff));
    for (char c : payload) {
      frame.push_back(static_cast<std::byte>(c));
    }
    return frame;
  }

  // Build a RST_STREAM frame for the given stream_id.
  auto make_rst_stream_frame(uint32_t stream_id, uint32_t error_code = 1)
  {
    std::vector<std::byte> frame;
    frame.push_back(std::byte(0x00)); // Length: 4
    frame.push_back(std::byte(0x00));
    frame.push_back(std::byte(0x04));
    frame.push_back(std::byte(0x03)); // Type: RST_STREAM
    frame.push_back(std::byte(0x00)); // Flags: none
    frame.push_back(static_cast<std::byte>((stream_id >> 24) & 0xff));
    frame.push_back(static_cast<std::byte>((stream_id >> 16) & 0xff));
    frame.push_back(static_cast<std::byte>((stream_id >> 8) & 0xff));
    frame.push_back(static_cast<std::byte>(stream_id & 0xff));
    // Error code (4 bytes, big-endian)
    frame.push_back(static_cast<std::byte>((error_code >> 24) & 0xff));
    frame.push_back(static_cast<std::byte>((error_code >> 16) & 0xff));
    frame.push_back(static_cast<std::byte>((error_code >> 8) & 0xff));
    frame.push_back(static_cast<std::byte>(error_code & 0xff));
    return frame;
  }

  // Drain all events from the engine into a vector.
  auto drain_events(http::v2::engine& engine)
  {
    std::vector<http::event> events;
    while (auto ev = engine.next_event()) {
      events.push_back(std::move(*ev));
    }
    return events;
  }

  // Convert a byte vector to a string for comparison.
  auto to_string(const std::vector<std::byte>& bytes)
  {
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }
}

TEST_CASE("Event Queue: has_events() / next_event() on idle engine")
{
  http::v2::engine engine(http::connection_role::server);
  CHECK_FALSE(engine.has_events());
  CHECK_FALSE(engine.next_event().has_value());
}

TEST_CASE("Event Queue: events fire for HEADERS + DATA(END_STREAM)")
{
  http::v2::engine engine(http::connection_role::server);

  // Establish connection
  auto preface = make_preface_bytes();
  mock::recv(engine, preface);
  engine.output_end(engine.output_begin().size()); // Consume server SETTINGS

  // Send HEADERS (new stream) then DATA with END_STREAM
  auto headers_frame = make_headers_frame(1, {
    {":method", "POST"},
    {":scheme", "https"},
    {":authority", "localhost"},
    {":path", "/api"},
  });
  auto data_frame = make_data_frame(1, "hello", true);

  mock::recv(engine, headers_frame);
  mock::recv(engine, data_frame);

  // Verify events were queued
  CHECK(engine.has_events());
  auto events = drain_events(engine);

  // Expected event order: new_stream, headers, data, stream_closed
  REQUIRE(events.size() == 4);
  CHECK(events[0].is_new_stream());
  CHECK(events[0].stream_id == 1);
  CHECK(events[1].is_headers());
  CHECK(events[1].stream_id == 1);
  CHECK(events[2].is_data());
  CHECK(events[2].stream_id == 1);
  CHECK(events[3].is_stream_closed());
  CHECK(events[3].stream_id == 1);

  // Verify data event payload
  auto& data_payload = std::get<std::vector<std::byte>>(events[2].payload);
  std::string data_str(reinterpret_cast<const char*>(data_payload.data()), data_payload.size());
  CHECK(data_str == "hello");

  // Verify headers event payload
  auto& hdrs = std::get<http::headers>(events[1].payload);
  CHECK(hdrs.get(":method") == "POST");
  CHECK(hdrs.get(":path") == "/api");

  // Queue should now be empty
  CHECK_FALSE(engine.has_events());
  CHECK_FALSE(engine.next_event().has_value());
}

TEST_CASE("Event Queue: events are FIFO within a single input_end call")
{
  http::v2::engine engine(http::connection_role::server);

  // Establish connection
  auto preface = make_preface_bytes();
  mock::recv(engine, preface);
  engine.output_end(engine.output_begin().size());

  // Send HEADERS + DATA + DATA(END_STREAM) in a single recv
  auto headers_frame = make_headers_frame(1, {
    {":method", "GET"},
    {":scheme", "https"},
    {":authority", "localhost"},
    {":path", "/"},
  });
  auto data1 = make_data_frame(1, "abc", false);
  auto data2 = make_data_frame(1, "def", true);

  // Combine all frames into one buffer
  std::vector<std::byte> combined;
  combined.insert(combined.end(), headers_frame.begin(), headers_frame.end());
  combined.insert(combined.end(), data1.begin(), data1.end());
  combined.insert(combined.end(), data2.begin(), data2.end());

  mock::recv(engine, combined);

  auto events = drain_events(engine);

  // Expected: new_stream, headers, data, data, stream_closed
  REQUIRE(events.size() == 5);
  CHECK(events[0].is_new_stream());
  CHECK(events[1].is_headers());
  CHECK(events[2].is_data());
  CHECK(events[3].is_data());
  CHECK(events[4].is_stream_closed());

  // Verify data payloads in order
  auto& d0 = std::get<std::vector<std::byte>>(events[2].payload);
  CHECK(std::string(reinterpret_cast<const char*>(d0.data()), d0.size()) == "abc");
  auto& d1 = std::get<std::vector<std::byte>>(events[3].payload);
  CHECK(std::string(reinterpret_cast<const char*>(d1.data()), d1.size()) == "def");
}

TEST_CASE("Event Queue: stream_reset event fires")
{
  http::v2::engine engine(http::connection_role::server);

  // Establish connection
  auto preface = make_preface_bytes();
  mock::recv(engine, preface);
  engine.output_end(engine.output_begin().size());

  // First open a stream via HEADERS
  auto headers_frame = make_headers_frame(1, {
    {":method", "GET"},
    {":scheme", "https"},
    {":authority", "localhost"},
    {":path", "/"},
  });
  mock::recv(engine, headers_frame);
  drain_events(engine); // Clear new_stream + headers events

  // Now send RST_STREAM
  auto rst_frame = make_rst_stream_frame(1);
  mock::recv(engine, rst_frame);

  auto events = drain_events(engine);
  REQUIRE(events.size() == 1);
  CHECK(events[0].is_stream_reset());
  CHECK(events[0].stream_id == 1);
  // Payload should be an error_code
  CHECK(std::holds_alternative<std::error_code>(events[0].payload));
}

TEST_CASE("Event Queue: HEADERS with END_STREAM fires stream_closed event")
{
  http::v2::engine engine(http::connection_role::server);

  // Establish connection
  auto preface = make_preface_bytes();
  mock::recv(engine, preface);
  engine.output_end(engine.output_begin().size());

  // Send HEADERS with END_HEADERS | END_STREAM
  auto headers_frame = make_headers_frame(1, {
    {":method", "GET"},
    {":scheme", "https"},
    {":authority", "localhost"},
    {":path", "/"},
  }, true);

  mock::recv(engine, headers_frame);

  auto events = drain_events(engine);

  // Expected: new_stream, headers, stream_closed
  REQUIRE(events.size() == 3);
  CHECK(events[0].is_new_stream());
  CHECK(events[1].is_headers());
  CHECK(events[2].is_stream_closed());
  CHECK(events[2].stream_id == 1);
}