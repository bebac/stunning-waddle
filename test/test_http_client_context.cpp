#include "doctest/doctest.h"
#include "test_helpers.h"
#include "http/client_context.h"
#include "http/event.h"
#include <algorithm>
#include <ranges>

namespace
{
  // Feed data to a context. input_end() does not auto-drain events.
  void feed_data(http::client_context& ctx, std::span<const std::byte> data)
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

  // Drain all queued events and return them for inspection.
  std::vector<http::event> drain_all_events(http::client_context& ctx)
  {
    std::vector<http::event> events;
    while (ctx.has_events()) {
      auto ev = ctx.next_event();
      if (ev) events.push_back(std::move(*ev));
    }
    return events;
  }
}

TEST_CASE("client_context: dispatcher routes frames to correct stream")
{
  http::client_context ctx;

  ctx.set_protocol_version(http::protocol_version::v2);

  // 1. Open a stream
  auto stream = ctx.open_stream();
  uint32_t stream_id = stream.id();

  // 2. Mock Server: Send HEADERS (Stream ID matches)
  // Using Stream 1 (usually the first client stream)
  auto header_frame = make_bytes(
      0x00, 0x00, 0x01,       // Length
      0x01,                   // Type: HEADERS
      0x05,                   // Flags: END_STREAM | END_HEADERS
      0x00, 0x00, 0x00, 0x01, // Stream 1
      0x88                    // :status 200
  );

  feed_data(ctx, header_frame);
  auto events = drain_all_events(ctx);

  // 3. Verify events: headers + stream_closed
  REQUIRE(events.size() == 2);
  CHECK(events[0].is_headers());
  CHECK(events[0].stream_id == stream_id);

  CHECK(events[1].is_stream_closed());
  CHECK(events[1].stream_id == stream_id);
}

TEST_CASE("client_context: multiplexing isolation")
{
  http::client_context ctx;

  ctx.set_protocol_version(http::protocol_version::v2);

  auto s1 = ctx.open_stream(); // Should be ID 1
  auto s2 = ctx.open_stream(); // Should be ID 3

  // Mock Data frame for Stream 3 (s2)
  auto data_s3 = make_bytes(
      0x00, 0x00, 0x01,       // Length 1
      0x00,                   // Type: DATA
      0x00,                   // Flags
      0x00, 0x00, 0x00, 0x03, // Stream 3
      0x41                    // 'A'
  );

  feed_data(ctx, data_s3);
  auto events = drain_all_events(ctx);

  // Should have exactly one data event for stream 3
  REQUIRE(events.size() == 1);
  CHECK(events[0].is_data());
  CHECK(events[0].stream_id == 3);
}

TEST_CASE("client_context: stream transmission produces valid H2 frames")
{
    http::client_context ctx;

    ctx.set_protocol_version(http::protocol_version::v2);

    auto stream = ctx.open_stream();
    stream.send_headers("GET", "/index.html", "localhost", {}, false);

    // 1. Clear the H2 connection "Magic" string
    mock::skip_preface(ctx);

    // 2. Clear the initial SETTINGS frame (this is likely the rest of your 53 bytes)
    auto settings = mock::capture_frame(ctx);
    REQUIRE(settings.has_value());
    CHECK(settings->type == 0x04); // Type 4 is SETTINGS

    // 3. NOW your HEADERS frame should be next in line
    auto frame = mock::capture_frame(ctx);

    REQUIRE(frame.has_value());
    CHECK(frame->type == 0x01); // HEADERS
}

TEST_CASE("client_context: headers are correctly HPACK encoded")
{
  http::client_context ctx;
  ctx.set_protocol_version(http::protocol_version::v2);

  auto stream = ctx.open_stream();

  stream.send_headers("GET", "/test", "localhost");

  mock::skip_preface(ctx);
  mock::capture_frame(ctx); // Skip SETTINGS

  auto frame = mock::capture_frame(ctx);
  REQUIRE(frame->type == 0x01);

  // Use your internal HPACK decoder to verify the payload
  http::v2::hpack_context decoder;
  auto headers = decoder.decode(reinterpret_cast<uint8_t*>(frame->payload.data()), frame->payload.size());

  CHECK(headers[0].name == ":method");
  CHECK(headers[0].value == "GET");
  CHECK(headers[1].name == ":scheme");
  CHECK(headers[1].value == "https");
  CHECK(headers[2].name == ":authority");
  CHECK(headers[2].value == "localhost");
  CHECK(headers[3].name == ":path");
  CHECK(headers[3].value == "/test");
}