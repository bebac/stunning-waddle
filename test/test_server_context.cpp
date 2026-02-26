#include "doctest/doctest.h"
#include "test_helpers.h"
#include "http/server_context.h"
#include "http/v2/hpack_context.h"
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
}

TEST_CASE("server_context: accepts incoming streams")
{
  http::server_context ctx;

  establish_connection(ctx);

  SUBCASE("on_new_stream fires when client sends HEADERS")
  {
    bool stream_received = false;
    http::headers received_headers;
    uint32_t received_stream_id = 0;

    ctx.on_new_stream([&](http::stream s, const http::headers& h) {
      stream_received = true;
      received_stream_id = s.id();
      received_headers = h;
    });

    auto headers_frame = make_headers_frame(1, {
      {":method", "GET"},
      {":scheme", "https"},
      {":authority", "localhost"},
      {":path", "/test"}
    }, true);

    mock::recv(ctx, headers_frame);

    CHECK(stream_received == true);
    CHECK(received_stream_id == 1);
    CHECK(received_headers.get(":method") == "GET");
    CHECK(received_headers.get(":path") == "/test");
  }

  SUBCASE("active_streams returns correct count")
  {
    ctx.on_new_stream([](http::stream, const http::headers&) {});

    CHECK(ctx.active_streams() == 0);

    // Send request on stream 1 (not end_stream)
    auto req1 = make_headers_frame(1, {
      {":method", "GET"}, {":scheme", "https"}, {":authority", "localhost"}, {":path", "/1"}
    }, false);
    mock::recv(ctx, req1);
    CHECK(ctx.active_streams() == 1);

    // Send request on stream 3
    auto req2 = make_headers_frame(3, {
      {":method", "GET"}, {":scheme", "https"}, {":authority", "localhost"}, {":path", "/2"}
    }, false);
    mock::recv(ctx, req2);
    CHECK(ctx.active_streams() == 2);
  }
}

TEST_CASE("server_context: sends responses")
{
  http::server_context ctx;

  establish_connection(ctx);

  http::stream captured_stream;
  ctx.on_new_stream([&](http::stream s, const http::headers&) {
    captured_stream = std::move(s);
  });

  // Client sends request
  auto req = make_headers_frame(1, {
    {":method", "GET"}, {":scheme", "https"}, {":authority", "localhost"}, {":path", "/"}
  }, true);
  mock::recv(ctx, req);

  REQUIRE(captured_stream);

  SUBCASE("send_response sends HEADERS with :status")
  {
    http::headers response_headers;
    response_headers.add("content-type", "text/plain");

    captured_stream.send_response(200, response_headers, true);

    auto frame = mock::capture_frame(ctx);
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

    auto headers_frame = mock::capture_frame(ctx);
    REQUIRE(headers_frame.has_value());
    CHECK(headers_frame->type == 0x01);
    CHECK((headers_frame->flags & 0x01) == 0); // NOT END_STREAM

    auto data_frame = mock::capture_frame(ctx);
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

  std::string received_body;
  bool request_ended = false;

  ctx.on_new_stream([&](http::stream s, const http::headers&) {
    s.on_data([&](std::span<const std::byte> data) {
      received_body.append(reinterpret_cast<const char*>(data.data()), data.size());
    });
    s.on_end([&]() {
      request_ended = true;
    });
  });

  // Client sends POST with body
  auto req = make_headers_frame(1, {
    {":method", "POST"},
    {":scheme", "https"},
    {":authority", "localhost"},
    {":path", "/upload"},
    {"content-length", "11"}
  }, false);
  mock::recv(ctx, req);

  // Client sends DATA frame
  auto data_frame = make_bytes(
    0x00, 0x00, 0x0b,       // Length 11
    0x00,                   // Type: DATA
    0x01,                   // Flags: END_STREAM
    0x00, 0x00, 0x00, 0x01, // Stream ID 1
    'H', 'e', 'l', 'l', 'o', ' ', 'W', 'o', 'r', 'l', 'd'
  );
  mock::recv(ctx, data_frame);

  CHECK(received_body == "Hello World");
  CHECK(request_ended == true);
}

TEST_CASE("server_context: multiplexing isolation")
{
  http::server_context ctx;

  establish_connection(ctx);

  std::map<uint32_t, std::string> stream_paths;

  ctx.on_new_stream([&](http::stream s, const http::headers& h) {
    stream_paths[s.id()] = h.get(":path");
  });

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

  mock::recv(ctx, req1);
  mock::recv(ctx, req2);
  mock::recv(ctx, req3);

  CHECK(stream_paths.size() == 3);
  CHECK(stream_paths[1] == "/first");
  CHECK(stream_paths[3] == "/second");
  CHECK(stream_paths[5] == "/third");
}
