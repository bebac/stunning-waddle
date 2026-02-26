#include "doctest/doctest.h"
#include "test_helpers.h"
#include "http/v2/engine.h"
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
}

TEST_CASE("HTTP/2 Server Engine: connection establishment")
{
  http::v2::engine engine(http::connection_role::server);

  SUBCASE("Server does not produce preface on construction")
  {
    auto out = engine.output_begin();
    CHECK(out.empty());
  }

  SUBCASE("Server validates client preface before processing frames")
  {
    // Send client preface
    auto preface = make_preface_bytes();
    mock::recv(engine, preface);

    // Server should now produce SETTINGS frame
    auto out = engine.output_begin();
    REQUIRE(out.size() >= 9);
    CHECK(out[3] == std::byte(0x04)); // Type: SETTINGS
    CHECK(out[4] == std::byte(0x00)); // Not ACK
  }

  SUBCASE("Server handles client preface + SETTINGS in single recv")
  {
    // Client preface followed by empty SETTINGS
    auto preface = make_preface_bytes();
    auto settings = make_bytes(
        0x00, 0x00, 0x00,       // Length 0
        0x04,                   // Type SETTINGS
        0x00,                   // Flags (None)
        0x00, 0x00, 0x00, 0x00  // Stream ID 0
    );
    preface.insert(preface.end(), settings.begin(), settings.end());

    mock::recv(engine, preface);

    // Server should produce: SETTINGS + SETTINGS ACK
    auto frame1 = mock::capture_frame(engine);
    REQUIRE(frame1.has_value());
    CHECK(frame1->type == 0x04);  // SETTINGS
    CHECK(frame1->flags == 0x00); // Not ACK (server's initial settings)

    auto frame2 = mock::capture_frame(engine);
    REQUIRE(frame2.has_value());
    CHECK(frame2->type == 0x04);  // SETTINGS
    CHECK(frame2->flags == 0x01); // ACK
  }
}

TEST_CASE("HTTP/2 Server Engine: receiving requests")
{
  http::v2::engine engine(http::connection_role::server);

  // Establish connection
  auto preface = make_preface_bytes();
  mock::recv(engine, preface);
  engine.output_end(9); // Consume server's SETTINGS

  SUBCASE("Server fires on_new_stream when receiving HEADERS on new stream")
  {
    uint32_t received_stream_id = 0;
    engine.on_new_stream([&](uint32_t id) {
      received_stream_id = id;
    });

    // Encode a minimal request HEADERS
    http::v2::hpack_context encoder;
    http::v2::hpack_buffer header_block;
    encoder.encode(header_block, {
      {":method", "GET"},
      {":scheme", "https"},
      {":authority", "localhost"},
      {":path", "/"}
    });

    // Build HEADERS frame: stream 1, END_HEADERS flag
    std::vector<std::byte> frame;
    uint32_t len = static_cast<uint32_t>(header_block.size());
    frame.push_back(static_cast<std::byte>((len >> 16) & 0xff));
    frame.push_back(static_cast<std::byte>((len >> 8) & 0xff));
    frame.push_back(static_cast<std::byte>(len & 0xff));
    frame.push_back(std::byte(0x01)); // Type: HEADERS
    frame.push_back(std::byte(0x04)); // Flags: END_HEADERS
    frame.push_back(std::byte(0x00));
    frame.push_back(std::byte(0x00));
    frame.push_back(std::byte(0x00));
    frame.push_back(std::byte(0x01)); // Stream ID 1
    frame.insert(frame.end(), header_block.begin(), header_block.end());

    mock::recv(engine, frame);

    CHECK(received_stream_id == 1);
  }

  SUBCASE("Server receives and decodes request headers")
  {
    http::headers received_headers;
    engine.on_headers([&](uint32_t id, const http::headers& h) {
      (void)id;
      received_headers = h;
    });

    // Encode request headers
    http::v2::hpack_context encoder;
    http::v2::hpack_buffer header_block;
    encoder.encode(header_block, {
      {":method", "POST"},
      {":scheme", "https"},
      {":authority", "example.com"},
      {":path", "/api/data"},
      {"content-type", "application/json"}
    });

    // Build HEADERS frame
    std::vector<std::byte> frame;
    uint32_t len = static_cast<uint32_t>(header_block.size());
    frame.push_back(static_cast<std::byte>((len >> 16) & 0xff));
    frame.push_back(static_cast<std::byte>((len >> 8) & 0xff));
    frame.push_back(static_cast<std::byte>(len & 0xff));
    frame.push_back(std::byte(0x01)); // Type: HEADERS
    frame.push_back(std::byte(0x04)); // Flags: END_HEADERS
    frame.push_back(std::byte(0x00));
    frame.push_back(std::byte(0x00));
    frame.push_back(std::byte(0x00));
    frame.push_back(std::byte(0x01)); // Stream ID 1
    frame.insert(frame.end(), header_block.begin(), header_block.end());

    mock::recv(engine, frame);

    CHECK(received_headers.get(":method") == "POST");
    CHECK(received_headers.get(":path") == "/api/data");
    CHECK(received_headers.get("content-type") == "application/json");
  }
}

TEST_CASE("HTTP/2 Server Engine: sending responses")
{
  http::v2::engine engine(http::connection_role::server);

  // Establish connection
  auto preface = make_preface_bytes();
  mock::recv(engine, preface);
  mock::capture_frame(engine); // Consume server's SETTINGS

  SUBCASE("Server sends response headers with :status")
  {
    // Simulate receiving a request first (server needs to know about stream 1)
    http::v2::hpack_context encoder;
    http::v2::hpack_buffer req_block;
    encoder.encode(req_block, {{":method", "GET"}, {":scheme", "https"}, {":authority", "localhost"}, {":path", "/"}});

    std::vector<std::byte> req_frame;
    uint32_t len = static_cast<uint32_t>(req_block.size());
    req_frame.push_back(static_cast<std::byte>((len >> 16) & 0xff));
    req_frame.push_back(static_cast<std::byte>((len >> 8) & 0xff));
    req_frame.push_back(static_cast<std::byte>(len & 0xff));
    req_frame.push_back(std::byte(0x01)); // HEADERS
    req_frame.push_back(std::byte(0x05)); // END_HEADERS | END_STREAM
    req_frame.push_back(std::byte(0x00));
    req_frame.push_back(std::byte(0x00));
    req_frame.push_back(std::byte(0x00));
    req_frame.push_back(std::byte(0x01)); // Stream 1
    req_frame.insert(req_frame.end(), req_block.begin(), req_block.end());
    mock::recv(engine, req_frame);

    // Now send response
    http::headers response_headers;
    response_headers.add("content-type", "text/plain");

    engine.send_response_headers(1, 200, response_headers, true);

    // Capture and decode the response HEADERS frame
    auto frame = mock::capture_frame(engine);
    REQUIRE(frame.has_value());
    CHECK(frame->type == 0x01); // HEADERS
    CHECK(frame->stream_id == 1);
    CHECK((frame->flags & 0x01) != 0); // END_STREAM
    CHECK((frame->flags & 0x04) != 0); // END_HEADERS

    // Decode the headers
    http::v2::hpack_context decoder;
    auto decoded = decoder.decode(
      reinterpret_cast<uint8_t*>(frame->payload.data()),
      frame->payload.size()
    );

    // Find :status
    bool found_status = false;
    for (const auto& h : decoded) {
      if (h.name == ":status") {
        CHECK(h.value == "200");
        found_status = true;
      }
    }
    CHECK(found_status);
  }

  SUBCASE("Server sends response with body")
  {
    // First receive a request
    http::v2::hpack_context encoder;
    http::v2::hpack_buffer req_block;
    encoder.encode(req_block, {{":method", "GET"}, {":scheme", "https"}, {":authority", "localhost"}, {":path", "/"}});

    std::vector<std::byte> req_frame;
    uint32_t len = static_cast<uint32_t>(req_block.size());
    req_frame.push_back(static_cast<std::byte>((len >> 16) & 0xff));
    req_frame.push_back(static_cast<std::byte>((len >> 8) & 0xff));
    req_frame.push_back(static_cast<std::byte>(len & 0xff));
    req_frame.push_back(std::byte(0x01));
    req_frame.push_back(std::byte(0x05));
    req_frame.push_back(std::byte(0x00));
    req_frame.push_back(std::byte(0x00));
    req_frame.push_back(std::byte(0x00));
    req_frame.push_back(std::byte(0x01));
    req_frame.insert(req_frame.end(), req_block.begin(), req_block.end());
    mock::recv(engine, req_frame);

    // Send response headers (not end stream)
    engine.send_response_headers(1, 200, {}, false);

    // Send response body
    std::string body = "Hello, World!";
    engine.send_data(1,
      std::span<const std::byte>(reinterpret_cast<const std::byte*>(body.data()), body.size()),
      true);

    // Verify HEADERS frame
    auto headers_frame = mock::capture_frame(engine);
    REQUIRE(headers_frame.has_value());
    CHECK(headers_frame->type == 0x01);
    CHECK((headers_frame->flags & 0x01) == 0); // NOT END_STREAM

    // Verify DATA frame
    auto data_frame = mock::capture_frame(engine);
    REQUIRE(data_frame.has_value());
    CHECK(data_frame->type == 0x00); // DATA
    CHECK(data_frame->stream_id == 1);
    CHECK((data_frame->flags & 0x01) != 0); // END_STREAM

    std::string received_body(
      reinterpret_cast<const char*>(data_frame->payload.data()),
      data_frame->payload.size()
    );
    CHECK(received_body == "Hello, World!");
  }
}
