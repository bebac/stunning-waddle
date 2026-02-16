#include "doctest/doctest.h"
#include "test_helpers.h"
#include "http/client_context.h"

TEST_CASE("client_context: dispatcher routes frames to correct stream")
{
  http::client_context ctx;

  ctx.set_protocol_version(http::protocol_version::v2);

  // 1. Open a stream and capture its state
  auto stream = ctx.open_stream();
  uint32_t stream_id = stream.id();

  bool headers_received = false;
  std::string body_received;
  bool stream_ended = false;

  // 2. Attach manual callbacks
  stream.on_headers([&](const http::headers& h) {
    headers_received = true;
    // Check for specific header if needed
  });

  stream.on_data([&](std::span<const std::byte> data) {
    for (auto b : data)
      body_received += static_cast<char>(b);
  });

  stream.on_end([&]() { stream_ended = true; });

  // 3. Mock Server: Send HEADERS (Stream ID matches)
  // Using Stream 1 (usually the first client stream)
  auto header_frame = make_bytes(
      0x00, 0x00, 0x01,       // Length
      0x01,                   // Type: HEADERS
      0x05,                   // Flags: END_STREAM | END_HEADERS (Let's end it here for simplicity)
      0x00, 0x00, 0x00, 0x01, // Stream 1
      0x88                    // :status 200
  );

  mock::recv(ctx, header_frame);

  // 4. Verify Dispatcher worked
  CHECK(headers_received == true);
  CHECK(stream_ended == true);
}

TEST_CASE("client_context: multiplexing isolation")
{
  http::client_context ctx;

  ctx.set_protocol_version(http::protocol_version::v2);

  auto s1 = ctx.open_stream(); // Should be ID 1
  auto s2 = ctx.open_stream(); // Should be ID 3

  int s1_count = 0;
  int s2_count = 0;

  s1.on_data([&](auto) { s1_count++; });
  s2.on_data([&](auto) { s2_count++; });

  // Mock Data frame for Stream 3 (s2)
  auto data_s3 = make_bytes(
      0x00, 0x00, 0x01,       // Length 1
      0x00,                   // Type: DATA
      0x00,                   // Flags
      0x00, 0x00, 0x00, 0x03, // Stream 3
      0x41                    // 'A'
  );

  mock::recv(ctx, data_s3);

  CHECK(s1_count == 0); // Stream 1 should be untouched
  CHECK(s2_count == 1); // Stream 3 should have received data
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
