#include "doctest/doctest.h"
#include "test_helpers.h"
#include "http/v2/engine.h"
#include "http/headers.h"
#include <vector>
#include <span>
#include <cstring>

TEST_CASE("HTTP/2 Engine Usability")
{
  http::v2::engine engine;

  SUBCASE("Engine can send a request using common headers")
  {
    http::headers headers;
    headers.add("user-agent", "test-agent");

    uint32_t stream_id = engine.open_stream();
    CHECK(stream_id == 1);

    engine.send_headers(stream_id, "GET", "/", "localhost", headers, true);

    // Clear preface and settings from buffer
    engine.output_end(24 + 9);

    // Now check for HEADERS frame
    auto out = engine.output_begin();
    CHECK(out.size() >= 9);
    CHECK(out[3] == std::byte(0x01)); // Type: HEADERS
    CHECK(out[8] == std::byte(0x01)); // Stream ID: 1

    engine.output_end(out.size());
  }

  SUBCASE("Engine supports granular callbacks")
  {
    bool headers_received = false;
    bool data_received = false;
    bool stream_closed = false;

    engine.on_headers([&](uint32_t stream_id, const http::headers& headers) {
      (void)stream_id;
      (void)headers;
      headers_received = true;
    });

    engine.on_data([&](uint32_t stream_id, const uint8_t* data, size_t len) {
      (void)stream_id;
      (void)data;
      (void)len;
      data_received = true;
    });

    engine.on_stream_closed([&](uint32_t stream_id) {
      (void)stream_id;
      stream_closed = true;
    });

    // Feed a HEADERS frame (Stream 1, END_HEADERS)
    auto hf = make_bytes(
        0x00, 0x00, 0x01,       // Length 1
        0x01,                   // Type HEADERS
        0x04,                   // Flags END_HEADERS
        0x00, 0x00, 0x00, 0x01, // Stream ID 1
        0x88                    // Payload (Indexed :status 200)
    );

    mock::recv(engine, hf);

    CHECK(headers_received == true);

    // Feed a DATA frame (Stream 1, END_STREAM)
    auto df = make_bytes(
        0x00, 0x00, 0x05,      // Length 5
        0x00,                  // Type DATA
        0x01,                  // Flags END_STREAM
        0x00, 0x00, 0x00, 0x01, // Stream ID 1
        'h', 'e', 'l', 'l', 'o'
    );

    mock::recv(engine, df);

    CHECK(data_received == true);
    CHECK(stream_closed == true);
  }
}
