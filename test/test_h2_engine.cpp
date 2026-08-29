#include "doctest/doctest.h"
#include "test_helpers.h"
#include "http/v2/engine.h"
#include "http/client_context.h"
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
    // Feed a DATA frame: Length 5, Type 0, Flags 0, Stream ID 1, Payload "hello"
    auto df = make_bytes(0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 'h', 'e', 'l', 'l', 'o');

    mock::recv(engine, df);

    // Drain events and verify data event
    std::vector<http::event> events;
    while (auto ev = engine.next_event()) {
      events.push_back(std::move(*ev));
    }

    REQUIRE(events.size() == 1);
    CHECK(events[0].is_data());
    CHECK(events[0].stream_id == 1);

    auto& data_payload = std::get<std::vector<std::byte>>(events[0].payload);
    std::string received(reinterpret_cast<const char*>(data_payload.data()), data_payload.size());
    CHECK(received == "hello");
  }
}

TEST_CASE("HTTP/2 Engine Output Readiness")
{
  SUBCASE("has_output() returns true when engine has pending data")
  {
    http::v2::engine engine;

    // Engine starts empty - needs open_stream() to generate preface
    CHECK(engine.has_output() == false);

    // Open a stream - this generates preface and SETTINGS
    auto stream_id = engine.open_stream();

    // Now should have output
    CHECK(engine.has_output() == true);

    // Consume all output
    auto out = engine.output_begin();
    engine.output_end(out.size());

    // Now should have no output
    CHECK(engine.has_output() == false);
  }

  SUBCASE("has_output() returns true after sending data on a stream")
  {
    http::v2::engine engine;

    // Consume initial preface and SETTINGS
    auto stream_id = engine.open_stream();
    mock::skip_preface(engine);
    auto out = engine.output_begin();
    engine.output_end(out.size());

    CHECK(engine.has_output() == false);

    // Send data on the stream - this should produce DATA frame output
    auto data = make_bytes('t', 'e', 's', 't');
    engine.send_data(stream_id, std::span(data), false);

    // Should now have output (DATA frame)
    CHECK(engine.has_output() == true);
  }

  SUBCASE("has_output() returns true after sending request headers")
  {
    http::v2::engine engine;

    // Consume initial output
    mock::skip_preface(engine);
    auto out = engine.output_begin();
    engine.output_end(out.size());

    CHECK(engine.has_output() == false);

    // Open a stream and send headers
    auto stream_id = engine.open_stream();
    engine.send_request_headers(stream_id, "GET", "/", "example.com", {}, false);

    // Should now have output (HEADERS frame)
    CHECK(engine.has_output() == true);
  }

  SUBCASE("has_output() returns true after sending data")
  {
    http::v2::engine engine;

    // Consume initial output
    mock::skip_preface(engine);
    auto out = engine.output_begin();
    engine.output_end(out.size());

    auto stream_id = engine.open_stream();

    // Send data
    auto data = make_bytes('t', 'e', 's', 't');
    engine.send_data(stream_id, std::span(data), false);

    // Should now have output (DATA frame)
    CHECK(engine.has_output() == true);
  }

  SUBCASE("has_output() returns true after receiving SETTINGS (auto-ACK)")
  {
    http::v2::engine engine;

    // Consume initial output
    mock::skip_preface(engine);
    auto out = engine.output_begin();
    engine.output_end(out.size());

    CHECK(engine.has_output() == false);

    // Feed an empty SETTINGS frame - should trigger auto-ACK
    auto sf = make_bytes(
        0x00, 0x00, 0x00,      // Length 0
        0x04,                  // Type SETTINGS
        0x00,                  // Flags (None)
        0x00, 0x00, 0x00, 0x00 // Stream ID 0
    );

    mock::recv(engine, sf);

    // Should now have output (SETTINGS ACK)
    CHECK(engine.has_output() == true);
  }

  SUBCASE("has_output() works with context_base")
  {
    // Test that the passthrough methods in context_base work correctly
    http::client_context ctx;

    // Engine starts empty
    CHECK(ctx.has_output() == false);

    // Open a stream through the context - should generate output
    auto stream_id = ctx.open_stream();

    // Should have output
    CHECK(ctx.has_output() == true);
  }

  SUBCASE("on_output_ready() fires when output transitions from empty to non-empty")
  {
    http::v2::engine engine;

    int callback_count = 0;
    engine.on_output_ready([&]() {
      callback_count++;
    });

    // Engine starts empty - no callback yet
    CHECK(callback_count == 0);

    // Open a stream - this generates preface and SETTINGS.
    // write_preface() fires the callback (empty -> non-empty).
    // write_settings() does NOT fire again (already non-empty).
    auto stream_id = engine.open_stream();

    // Callback should have fired exactly once
    CHECK(callback_count == 1);

    // Consume all output
    auto out = engine.output_begin();
    engine.output_end(out.size());

    // Reset counter
    callback_count = 0;

    // Send request headers - should generate HEADERS frame and fire callback
    engine.send_request_headers(stream_id, "GET", "/", "example.com", {}, false);

    // Callback should have fired again (empty -> non-empty transition)
    CHECK(callback_count == 1);
  }

  SUBCASE("on_output_ready() does not fire when output is already non-empty")
  {
    http::v2::engine engine;

    int callback_count = 0;
    engine.on_output_ready([&]() {
      callback_count++;
    });

    // Open a stream - fires callback once
    auto stream_id = engine.open_stream();
    CHECK(callback_count == 1);

    // Send request headers - should NOT fire again (output already non-empty)
    engine.send_request_headers(stream_id, "GET", "/", "example.com", {}, false);
    CHECK(callback_count == 1);
  }

  SUBCASE("on_output_ready() fires when SETTINGS ACK is generated")
  {
    http::v2::engine engine;

    // Consume all initial output
    mock::skip_preface(engine);
    auto out = engine.output_begin();
    engine.output_end(out.size());

    bool callback_fired = false;
    engine.on_output_ready([&]() {
      callback_fired = true;
    });

    CHECK(callback_fired == false);

    // Feed SETTINGS frame - should trigger auto-ACK
    auto sf = make_bytes(
        0x00, 0x00, 0x00,      // Length 0
        0x04,                  // Type SETTINGS
        0x00,                  // Flags (None)
        0x00, 0x00, 0x00, 0x00 // Stream ID 0
    );

    mock::recv(engine, sf);

    // Callback should have fired
    CHECK(callback_fired == true);
  }

  SUBCASE("on_output_ready() is a no-op when no callback is registered")
  {
    http::v2::engine engine;

    // No callback registered - should not crash
    auto stream_id = engine.open_stream();
    CHECK(engine.has_output() == true);
  }
}