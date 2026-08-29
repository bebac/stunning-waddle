#include "doctest/doctest.h"
#include "test_helpers.h"
#include "http/server_context.h"
#include "http/event.h"
#include "http/v2/hpack_context.h"
#include <vector>
#include <cstring>
#include <algorithm>
#include <ranges>

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
    frame.push_back(std::byte(flags));
    frame.push_back(static_cast<std::byte>((stream_id >> 24) & 0x7f));
    frame.push_back(static_cast<std::byte>((stream_id >> 16) & 0xff));
    frame.push_back(static_cast<std::byte>((stream_id >> 8) & 0xff));
    frame.push_back(static_cast<std::byte>(stream_id & 0xff));
    frame.insert(frame.end(), header_block.begin(), header_block.end());
    return frame;
  }

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
    mock::recv(ctx, preface);

    // Consume server's SETTINGS and SETTINGS ACK
    mock::capture_frame(ctx);
    mock::capture_frame(ctx);
  }

  // Feed data to a context. input_end() does not auto-drain events,
  // so events remain in the queue for next_event().
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

  // Drain all queued events and return them for inspection.
  std::vector<http::event> drain_all_events(http::server_context& ctx)
  {
    std::vector<http::event> events;
    while (ctx.has_events()) {
      auto ev = ctx.next_event();
      if (ev) events.push_back(std::move(*ev));
    }
    return events;
  }
}

TEST_CASE("server_context: accepts incoming streams")
{
  http::server_context ctx;

  establish_connection(ctx);

  SUBCASE("headers event fires when client sends HEADERS")
  {
    auto headers_frame = make_headers_frame(1, {
      {":method", "GET"},
      {":scheme", "https"},
      {":authority", "localhost"},
      {":path", "/test"}
    }, true);

    feed_data(ctx, headers_frame);
    auto events = drain_all_events(ctx);

    // Should have headers + stream_closed events
    REQUIRE(events.size() >= 1);
    CHECK(events[0].is_headers());
    CHECK(events[0].stream_id == 1);

    auto& h = std::get<http::headers>(events[0].payload);
    CHECK(h.get(":method") == "GET");
    CHECK(h.get(":path") == "/test");

    // Stream should have been created internally
    CHECK(ctx.active_streams() == 0); // closed because END_STREAM

    // get_stream should return empty (stream was closed)
    auto s = ctx.get_stream(1);
    CHECK_FALSE(s);
  }

  SUBCASE("active_streams returns correct count")
  {
    CHECK(ctx.active_streams() == 0);

    // Send request on stream 1 (not end_stream)
    auto req1 = make_headers_frame(1, {
      {":method", "GET"}, {":scheme", "https"}, {":authority", "localhost"}, {":path", "/1"}
    }, false);
    feed_data(ctx, req1);
    drain_all_events(ctx);
    CHECK(ctx.active_streams() == 1);

    // Send request on stream 3
    auto req2 = make_headers_frame(3, {
      {":method", "GET"}, {":scheme", "https"}, {":authority", "localhost"}, {":path", "/2"}
    }, false);
    feed_data(ctx, req2);
    drain_all_events(ctx);
    CHECK(ctx.active_streams() == 2);
  }
}

TEST_CASE("server_context: sends responses")
{
  http::server_context ctx;

  establish_connection(ctx);

  // Client sends request
  auto req = make_headers_frame(1, {
    {":method", "GET"}, {":scheme", "https"}, {":authority", "localhost"}, {":path", "/"}
  }, true);
  feed_data(ctx, req);
  auto events = drain_all_events(ctx);

  // Should have headers + stream_closed
  REQUIRE(events.size() >= 1);
  CHECK(events[0].is_headers());

  // Stream was closed (END_STREAM), so get_stream won't work.
  // For response tests, we need a non-end_stream request.
  // Redo with a non-end_stream request.
  http::server_context ctx2;
  establish_connection(ctx2);

  auto req2 = make_headers_frame(1, {
    {":method", "GET"}, {":scheme", "https"}, {":authority", "localhost"}, {":path", "/"}
  }, false);
  feed_data(ctx2, req2);
  drain_all_events(ctx2);

  auto captured_stream = ctx2.get_stream(1);
  REQUIRE(captured_stream);

  SUBCASE("send_response sends HEADERS with :status")
  {
    http::headers response_headers;
    response_headers.add("content-type", "text/plain");

    captured_stream.send_response(200, response_headers, true);

    auto frame = mock::capture_frame(ctx2);
    REQUIRE(frame.has_value());
    CHECK(frame->type == 0x01); // HEADERS
    CHECK(frame->stream_id == 1);
    CHECK((frame->flags & 0x01) != 0); // END_STREAM
    CHECK((frame->flags & 0x04) != 0); // END_HEADERS

    // Decode and verify :status
    http::v2::hpack_context decoder;
    auto decoded = decoder.decode(
      reinterpret_cast<uint8_t*>(frame->payload.data()),
      frame->payload.size()
    );

    bool found_status = false;
    for (const auto& h : decoded) {
      if (h.name == ":status") {
        CHECK(h.value == "200");
        found_status = true;
      }
    }
    CHECK(found_status);
  }

  SUBCASE("send_response followed by send_data")
  {
    captured_stream.send_response(200, {}, false);

    std::string body = "Hello from server!";
    captured_stream.send_data(
      std::span<const std::byte>(reinterpret_cast<const std::byte*>(body.data()), body.size()),
      true);

    auto headers_frame = mock::capture_frame(ctx2);
    REQUIRE(headers_frame.has_value());
    CHECK(headers_frame->type == 0x01);
    CHECK((headers_frame->flags & 0x01) == 0); // NOT END_STREAM

    auto data_frame = mock::capture_frame(ctx2);
    REQUIRE(data_frame.has_value());
    CHECK(data_frame->type == 0x00); // DATA
    CHECK(data_frame->stream_id == 1);
    CHECK((data_frame->flags & 0x01) != 0); // END_STREAM

    std::string received_body(
      reinterpret_cast<const char*>(data_frame->payload.data()),
      data_frame->payload.size()
    );
    CHECK(received_body == "Hello from server!");
  }
}

TEST_CASE("server_context: handles request body")
{
  http::server_context ctx;

  establish_connection(ctx);

  // Client sends POST with body (not end_stream)
  auto req = make_headers_frame(1, {
    {":method", "POST"},
    {":scheme", "https"},
    {":authority", "localhost"},
    {":path", "/upload"},
    {"content-length", "11"}
  }, false);
  feed_data(ctx, req);

  // Client sends DATA frame
  auto data_frame = make_bytes(
    0x00, 0x00, 0x0b,       // Length 11
    0x00,                   // Type: DATA
    0x01,                   // Flags: END_STREAM
    0x00, 0x00, 0x00, 0x01, // Stream ID 1
    'H', 'e', 'l', 'l', 'o', ' ', 'W', 'o', 'r', 'l', 'd'
  );
  feed_data(ctx, data_frame);

  auto events = drain_all_events(ctx);

  // Expect: headers, data, stream_closed
  REQUIRE(events.size() == 3);
  CHECK(events[0].is_headers());
  CHECK(events[0].stream_id == 1);

  CHECK(events[1].is_data());
  CHECK(events[1].stream_id == 1);
  auto& data = std::get<std::vector<std::byte>>(events[1].payload);
  std::string received_body(reinterpret_cast<const char*>(data.data()), data.size());
  CHECK(received_body == "Hello World");

  CHECK(events[2].is_stream_closed());
  CHECK(events[2].stream_id == 1);
}

TEST_CASE("server_context: multiplexing isolation")
{
  http::server_context ctx;

  establish_connection(ctx);

  // Client opens multiple streams
  auto req1 = make_headers_frame(1, {
    {":method", "GET"}, {":scheme", "https"}, {":authority", "localhost"}, {":path", "/first"}
  }, true);
  auto req2 = make_headers_frame(3, {
    {":method", "GET"}, {":scheme", "https"}, {":authority", "localhost"}, {":path", "/second"}
  }, true);
  auto req3 = make_headers_frame(5, {
    {":method", "GET"}, {":scheme", "https"}, {":authority", "localhost"}, {":path", "/third"}
  }, true);

  // Feed all at once
  std::vector<std::byte> combined;
  combined.insert(combined.end(), req1.begin(), req1.end());
  combined.insert(combined.end(), req2.begin(), req2.end());
  combined.insert(combined.end(), req3.begin(), req3.end());
  feed_data(ctx, combined);

  auto events = drain_all_events(ctx);

  // Expect: headers(1), stream_closed(1), headers(3), stream_closed(3),
  //         headers(5), stream_closed(5)
  REQUIRE(events.size() == 6);

  std::map<uint32_t, std::string> stream_paths;

  for (auto& ev : events) {
    if (ev.is_headers()) {
      auto& h = std::get<http::headers>(ev.payload);
      stream_paths[ev.stream_id] = h.get(":path");
    }
  }

  CHECK(stream_paths.size() == 3);
  CHECK(stream_paths[1] == "/first");
  CHECK(stream_paths[3] == "/second");
  CHECK(stream_paths[5] == "/third");
}