#include "doctest/doctest.h"
#include "test_helpers.h"
#include "http/server_context.h"
#include "http/v2/hpack_context.h"
#include "http/event.h"
#include "http/error_codes.h"
#include <vector>
#include <cstring>
#include <ranges>
#include <algorithm>

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
    frame.push_back(static_cast<std::byte>((stream_id >> 24) & 0x7f));
    frame.push_back(static_cast<std::byte>((stream_id >> 16) & 0xff));
    frame.push_back(static_cast<std::byte>((stream_id >> 8) & 0xff));
    frame.push_back(static_cast<std::byte>(stream_id & 0xff));
    frame.insert(frame.end(), header_block.begin(), header_block.end());
    return frame;
  }

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
    frame.push_back(static_cast<std::byte>((stream_id >> 24) & 0x7f));
    frame.push_back(static_cast<std::byte>((stream_id >> 16) & 0xff));
    frame.push_back(static_cast<std::byte>((stream_id >> 8) & 0xff));
    frame.push_back(static_cast<std::byte>(stream_id & 0xff));
    for (char c : payload) {
      frame.push_back(static_cast<std::byte>(c));
    }
    return frame;
  }

  auto make_rst_stream_frame(uint32_t stream_id, uint32_t error_code = 1)
  {
    std::vector<std::byte> frame;
    frame.push_back(std::byte(0x00)); // Length: 4
    frame.push_back(std::byte(0x00));
    frame.push_back(std::byte(0x04));
    frame.push_back(std::byte(0x03)); // Type: RST_STREAM
    frame.push_back(std::byte(0x00)); // Flags: none
    frame.push_back(static_cast<std::byte>((stream_id >> 24) & 0x7f));
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

  // Feed data to a server_context. input_end() does not auto-drain events.
  void feed_data(http::server_context& ctx, std::span<const std::byte> data)
  {
    while (!data.empty())
    {
      auto in_buffer = ctx.input_begin();
      if (in_buffer.empty()) break;
      size_t n = std::min(in_buffer.size(), data.size());
      std::ranges::copy(data.first(n), in_buffer.begin());
      ctx.input_end(n);
      data = data.subspan(n);
    }
  }

  // Establish connection using feed_data.
  void establish_connection(http::server_context& ctx)
  {
    auto preface = make_preface_bytes();
    // Add client SETTINGS
    auto settings = make_bytes(
      0x00, 0x00, 0x00,       // Length 0
      0x04,                   // Type SETTINGS
      0x00,                   // Flags
      0x00, 0x00, 0x00, 0x00  // Stream ID 0
    );
    preface.insert(preface.end(), settings.begin(), settings.end());
    feed_data(ctx, preface);

    // Consume server's SETTINGS and SETTINGS ACK
    mock::capture_frame(ctx);
    mock::capture_frame(ctx);
  }
}

TEST_CASE("Direct Event Draining: has_events / next_event on idle context")
{
  http::server_context ctx;
  establish_connection(ctx);

  CHECK_FALSE(ctx.has_events());
  CHECK_FALSE(ctx.next_event().has_value());
}

TEST_CASE("Direct Event Draining: next_event handles new_stream internally")
{
  http::server_context ctx;
  establish_connection(ctx);

  CHECK(ctx.active_streams() == 0);

  // Send HEADERS frame (opens a new stream)
  auto headers_frame = make_headers_frame(1, {
    {":method", "GET"},
    {":scheme", "https"},
    {":authority", "localhost"},
    {":path", "/test"},
  });
  feed_data(ctx, headers_frame);

  CHECK(ctx.has_events());

  // The first event should be headers, NOT new_stream.
  // The context should have created the stream internally.
  auto ev = ctx.next_event();
  REQUIRE(ev.has_value());
  CHECK(ev->is_headers());
  CHECK(ev->stream_id == 1);

  // Stream should have been created internally
  CHECK(ctx.active_streams() == 1);

  // get_stream should return a valid handle
  auto s = ctx.get_stream(1);
  CHECK(s);
  CHECK(s.id() == 1);

  // No more events
  CHECK_FALSE(ctx.has_events());
}

TEST_CASE("Direct Event Draining: next_event returns data events")
{
  http::server_context ctx;
  establish_connection(ctx);

  // Send HEADERS + DATA
  auto headers_frame = make_headers_frame(1, {
    {":method", "POST"},
    {":scheme", "https"},
    {":authority", "localhost"},
    {":path", "/api"},
  });
  auto data_frame = make_data_frame(1, "hello", false);

  // Combine into one feed
  std::vector<std::byte> combined;
  combined.insert(combined.end(), headers_frame.begin(), headers_frame.end());
  combined.insert(combined.end(), data_frame.begin(), data_frame.end());
  feed_data(ctx, combined);

  // First event: headers (new_stream consumed internally)
  auto ev1 = ctx.next_event();
  REQUIRE(ev1.has_value());
  CHECK(ev1->is_headers());
  CHECK(ev1->stream_id == 1);

  // Verify headers payload
  auto& hdrs = std::get<http::headers>(ev1->payload);
  CHECK(hdrs.get(":method") == "POST");
  CHECK(hdrs.get(":path") == "/api");

  // Second event: data
  auto ev2 = ctx.next_event();
  REQUIRE(ev2.has_value());
  CHECK(ev2->is_data());
  CHECK(ev2->stream_id == 1);

  // Verify data payload
  auto& data = std::get<std::vector<std::byte>>(ev2->payload);
  std::string data_str(reinterpret_cast<const char*>(data.data()), data.size());
  CHECK(data_str == "hello");

  // No more events
  CHECK_FALSE(ctx.has_events());
}

TEST_CASE("Direct Event Draining: next_event handles stream_closed internally")
{
  http::server_context ctx;
  establish_connection(ctx);

  // Send HEADERS with END_STREAM (opens stream and closes it)
  auto headers_frame = make_headers_frame(1, {
    {":method", "GET"},
    {":scheme", "https"},
    {":authority", "localhost"},
    {":path", "/"},
  }, true);
  feed_data(ctx, headers_frame);

  // First event: headers
  auto ev1 = ctx.next_event();
  REQUIRE(ev1.has_value());
  CHECK(ev1->is_headers());

  // Stream should exist
  CHECK(ctx.active_streams() == 1);
  CHECK(ctx.get_stream(1));

  // Second event: stream_closed (returned to consumer)
  auto ev2 = ctx.next_event();
  REQUIRE(ev2.has_value());
  CHECK(ev2->is_stream_closed());
  CHECK(ev2->stream_id == 1);

  // Stream should have been cleaned up internally
  CHECK(ctx.active_streams() == 0);
  CHECK_FALSE(ctx.get_stream(1));

  // No more events
  CHECK_FALSE(ctx.has_events());
}

TEST_CASE("Direct Event Draining: next_event handles stream_reset internally")
{
  http::server_context ctx;
  establish_connection(ctx);

  // Open a stream via HEADERS
  auto headers_frame = make_headers_frame(1, {
    {":method", "GET"},
    {":scheme", "https"},
    {":authority", "localhost"},
    {":path", "/"},
  });
  feed_data(ctx, headers_frame);

  // Consume headers event
  auto ev1 = ctx.next_event();
  REQUIRE(ev1.has_value());
  CHECK(ev1->is_headers());

  // Stream should exist
  CHECK(ctx.active_streams() == 1);
  CHECK(ctx.get_stream(1));

  // Send RST_STREAM
  auto rst_frame = make_rst_stream_frame(1);
  feed_data(ctx, rst_frame);

  // Event: stream_reset (returned to consumer)
  auto ev2 = ctx.next_event();
  REQUIRE(ev2.has_value());
  CHECK(ev2->is_stream_reset());
  CHECK(ev2->stream_id == 1);

  // Verify error code payload
  CHECK(std::holds_alternative<std::error_code>(ev2->payload));

  // Stream should have been cleaned up internally
  CHECK(ctx.active_streams() == 0);
  CHECK_FALSE(ctx.get_stream(1));
}

TEST_CASE("Direct Event Draining: get_stream returns empty for unknown stream")
{
  http::server_context ctx;
  establish_connection(ctx);

  auto s = ctx.get_stream(999);
  CHECK_FALSE(s);
}

TEST_CASE("Direct Event Draining: on_output_ready passthrough fires")
{
  http::server_context ctx;
  establish_connection(ctx);

  // Consume any remaining output
  while (ctx.has_output()) {
    auto out = ctx.output_begin();
    ctx.output_end(out.size());
  }

  bool callback_fired = false;
  ctx.on_output_ready([&]() {
    callback_fired = true;
  });

  CHECK_FALSE(callback_fired);

  // Send SETTINGS frame to trigger auto-ACK (produces output)
  auto sf = make_bytes(
    0x00, 0x00, 0x00,      // Length 0
    0x04,                  // Type SETTINGS
    0x00,                  // Flags (None)
    0x00, 0x00, 0x00, 0x00 // Stream ID 0
  );
  feed_data(ctx, sf);

  CHECK(callback_fired);
}

TEST_CASE("Direct Event Draining: goaway and connection_error pass through")
{
  http::server_context ctx;
  establish_connection(ctx);

  // Send GOAWAY frame
  auto goaway_frame = make_bytes(
    0x00, 0x00, 0x08,      // Length 8
    0x07,                  // Type GOAWAY
    0x00,                  // Flags
    0x00, 0x00, 0x00, 0x00, // Stream ID 0
    0x00, 0x00, 0x00, 0x05, // Last stream ID: 5
    0x00, 0x00, 0x00, 0x00  // Error code: NO_ERROR
  );
  feed_data(ctx, goaway_frame);

  auto ev = ctx.next_event();
  REQUIRE(ev.has_value());
  CHECK(ev->is_goaway());

  auto& info = std::get<http::goaway_info>(ev->payload);
  CHECK(info.last_stream_id == 5);
  CHECK(info.ec == http::error_code::no_error);
}