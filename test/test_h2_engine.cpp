#include "doctest/doctest.h"
#include "test_helpers.h"
#include "http/v2/engine.h"
#include <vector>
#include <cstring>

TEST_CASE("HTTP/2 Engine Sans-I/O")
{
  http::v2::engine engine;

  auto id = engine.open_stream();

  CHECK(id == 1);

  SUBCASE("Engine starts by producing the connection preface")
  {
    auto out = engine.output_begin();

    // Preface is 24 bytes
    CHECK(out.size() >= 24);
    std::string preface(reinterpret_cast<const char*>(out.data()), 24);
    CHECK(preface == "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n");

    engine.output_end(24);
  }

  SUBCASE("Engine produces an initial SETTINGS frame")
  {
    // Consume preface first
    engine.output_end(24);

    // Next should be SETTINGS frame (9 bytes header + 0 bytes payload for empty settings)
    auto out = engine.output_begin();
    CHECK(out.size() >= 9);
    CHECK(out[3] == std::byte(0x04)); // Type: SETTINGS
    CHECK(out[8] == std::byte(0x00)); // Stream ID: 0

    engine.output_end(9);
  }

  SUBCASE("Engine handles incoming SETTINGS frame and sends ACK")
  {
    // Feed an empty SETTINGS frame to the engine (not an ACK)
    auto sf = make_bytes(
        0x00, 0x00, 0x00,      // Length 0
        0x04,                  // Type SETTINGS
        0x00,                  // Flags (None)
        0x00, 0x00, 0x00, 0x00 // Stream ID 0
    );

    mock::recv(engine, sf);

    // Clear preface and initial settings first from output
    engine.output_end(24 + 9);

    // Now check for ACK
    auto out = engine.output_begin();
    CHECK(out.size() == 9);
    CHECK(out[3] == std::byte(0x04)); // Type SETTINGS
    CHECK(out[4] == std::byte(0x01)); // Flags ACK

    engine.output_end(9);
  }

  SUBCASE("Engine dispatches incoming DATA frames")
  {
    bool data_received = false;
    std::vector<uint8_t> received_payload;

    engine.on_data([&](uint32_t stream_id, const uint8_t* data, size_t len) {
      (void)stream_id;
      data_received = true;
      received_payload.assign(data, data + len);
    });

    // Feed a DATA frame: Length 5, Type 0, Flags 0, Stream ID 1, Payload "hello"
    auto df = make_bytes(0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 'h', 'e', 'l', 'l', 'o');

    mock::recv(engine, df);

    CHECK(data_received == true);
    CHECK(received_payload == std::vector<uint8_t>{'h', 'e', 'l', 'l', 'o'});
  }
}
