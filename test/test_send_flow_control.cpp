// SPDX-License-Identifier: MIT
// Copyright (c) 2025

#include "doctest/doctest.h"
#include "test_helpers.h"
#include "http/v2/engine.h"
#include "http/v2/frame.h"
#include "http/client_context.h"
#include <vector>
#include <cstring>

namespace
{
  // Build a DATA frame with a payload of `size` bytes (filled with 'x').
  std::vector<std::byte> make_data_frame(uint32_t stream_id, uint32_t size, bool end_stream = false)
  {
    std::vector<std::byte> payload(size, std::byte{'x'});
    std::vector<std::byte> frame;
    http::v2::encode_data_frame(frame, stream_id, payload, end_stream);
    return frame;
  }
}

// =============================================================================
// SECTION 1: send_data() return value and window enforcement
// =============================================================================
//
// These tests verify that send_data() respects flow control windows and returns
// the number of bytes actually accepted (queued for sending).
//

TEST_CASE("Send flow control: send_data enforces window limits")
{
  http::v2::engine engine;
  auto stream_id = engine.open_stream();

  // Drain preface + initial SETTINGS
  mock::skip_preface(engine);
  while (auto f = mock::capture_frame(engine)) {}

  SUBCASE("send_data returns bytes accepted when within window")
  {
    // Default window is 65535
    std::vector<std::byte> data(1000, std::byte{'x'});

    size_t sent = engine.send_data(stream_id, data, false);
    CHECK(sent == 1000);
    CHECK(engine.connection_send_window() == 65535 - 1000);
    CHECK(engine.stream_send_window(stream_id) == 65535 - 1000);
  }

  SUBCASE("send_data returns 0 when window exhausted")
  {
    // Exhaust the window
    std::vector<std::byte> large_data(65535, std::byte{'x'});
    size_t sent = engine.send_data(stream_id, large_data, false);
    CHECK(sent == 65535);

    // Now window should be 0
    CHECK(engine.connection_send_window() == 0);
    CHECK(engine.stream_send_window(stream_id) == 0);

    // Try to send more - should return 0
    std::vector<std::byte> more_data(1000, std::byte{'y'});
    sent = engine.send_data(stream_id, more_data, false);
    CHECK(sent == 0);
  }

  SUBCASE("send_data respects minimum of connection and stream windows")
  {
    // Reduce stream window via SETTINGS
    std::vector<http::v2::setting> settings = {
      {http::v2::settings_id::initial_window_size, 10000}
    };
    std::vector<std::byte> sf;
    http::v2::encode_settings_frame(sf, settings, false);
    mock::recv(engine, sf);
    // Drain SETTINGS ACK
    while (auto f = mock::capture_frame(engine)) {}

    // Now stream window is 10000, connection window is 65535
    // Minimum is 10000

    std::vector<std::byte> data(20000, std::byte{'x'});
    size_t sent = engine.send_data(stream_id, data, false);
    CHECK(sent == 10000);  // Limited by stream window
    CHECK(engine.connection_send_window() == 65535 - 10000);
    CHECK(engine.stream_send_window(stream_id) == 10000 - 10000);
  }

  SUBCASE("send_data with partial send due to window limit")
  {
    // Set a small window
    std::vector<http::v2::setting> settings = {
      {http::v2::settings_id::initial_window_size, 5000}
    };
    std::vector<std::byte> sf;
    http::v2::encode_settings_frame(sf, settings, false);
    mock::recv(engine, sf);
    // Drain SETTINGS ACK
    while (auto f = mock::capture_frame(engine)) {}

    // Try to send 10000 bytes, but only 5000 should be accepted
    std::vector<std::byte> data(10000, std::byte{'x'});
    size_t sent = engine.send_data(stream_id, data, false);
    CHECK(sent == 5000);
    CHECK(engine.stream_send_window(stream_id) == 0);
  }

  SUBCASE("send_data with empty data and END_STREAM works even when window exhausted")
  {
    // Exhaust the window
    std::vector<std::byte> large_data(65535, std::byte{'x'});
    engine.send_data(stream_id, large_data, false);
    // Drain DATA frames
    while (auto f = mock::capture_frame(engine)) {}

    CHECK(engine.connection_send_window() == 0);
    CHECK(engine.stream_send_window(stream_id) == 0);

    // Send empty data with END_STREAM - should still work
    size_t sent = engine.send_data(stream_id, {}, true);
    CHECK(sent == 0);

    // Should have an empty DATA frame with END_STREAM
    auto frame = mock::capture_frame(engine);
    REQUIRE(frame.has_value());
    CHECK(frame->type == 0x00); // DATA
    CHECK(frame->stream_id == stream_id);
    CHECK(frame->length == 0); // Empty payload
    CHECK((frame->flags & 0x01) != 0); // END_STREAM
  }

  SUBCASE("send_data does not set END_STREAM on partial send")
  {
    // Set a small window
    std::vector<http::v2::setting> settings = {
      {http::v2::settings_id::initial_window_size, 5000}
    };
    std::vector<std::byte> sf;
    http::v2::encode_settings_frame(sf, settings, false);
    mock::recv(engine, sf);
    while (auto f = mock::capture_frame(engine)) {}

    // Try to send 10000 bytes with end_stream=true
    // Only 5000 should be sent, and END_STREAM should NOT be set
    std::vector<std::byte> data(10000, std::byte{'x'});
    size_t sent = engine.send_data(stream_id, data, true);
    CHECK(sent == 5000);

    // Check output frames - none should have END_STREAM set
    bool found_end_stream = false;
    while (auto f = mock::capture_frame(engine)) {
      if (f->type == 0x00 && (f->flags & 0x01) != 0) {
        found_end_stream = true;
      }
    }
    CHECK(found_end_stream == false);
  }
}

// =============================================================================
// SECTION 2: Window availability callbacks
// =============================================================================
//
// These tests verify that callbacks are invoked when flow control windows
// transition from exhausted (<=0) to available (>0).
//

TEST_CASE("Send flow control: window availability callbacks")
{
  SUBCASE("stream on_window_available callback invoked immediately if window > 0")
  {
    http::client_context ctx;
    auto stream = ctx.open_stream();
    stream.send_headers("POST", "/upload", "example.com", {}, false);

    // Drain all output (preface + SETTINGS + HEADERS)
    mock::skip_preface(ctx);
    while (auto f = mock::capture_frame(ctx)) {}

    bool callback_invoked = false;
    stream.on_window_available([&]() {
      callback_invoked = true;
    });

    // Window should be available initially (65535 > 0)
    CHECK(callback_invoked == true);
  }

  SUBCASE("stream callback invoked after stream WINDOW_UPDATE when connection window > 0")
  {
    http::client_context ctx;
    auto stream = ctx.open_stream();
    stream.send_headers("POST", "/upload", "example.com", {}, false);

    // Drain all output
    mock::skip_preface(ctx);
    while (auto f = mock::capture_frame(ctx)) {}

    // Exhaust both windows
    std::vector<std::byte> data(65535, std::byte{'x'});
    stream.send_data(data, false);
    // Drain DATA frames
    while (auto f = mock::capture_frame(ctx)) {}

    // Replenish connection window only
    std::vector<std::byte> conn_wu;
    http::v2::encode_window_update_frame(conn_wu, 0, 65535);
    mock::recv(ctx, conn_wu);

    // Register callback - should NOT fire (stream window is 0)
    bool callback_invoked = false;
    stream.on_window_available([&]() {
      callback_invoked = true;
    });
    CHECK(callback_invoked == false);

    // Send stream-level WINDOW_UPDATE
    std::vector<std::byte> wu;
    http::v2::encode_window_update_frame(wu, stream.id(), 1000);
    mock::recv(ctx, wu);

    // Now both windows are > 0, callback should fire
    CHECK(callback_invoked == true);
  }

  SUBCASE("stream callback invoked after connection WINDOW_UPDATE when stream window > 0")
  {
    http::client_context ctx;
    auto stream = ctx.open_stream();
    stream.send_headers("POST", "/upload", "example.com", {}, false);

    // Drain all output
    mock::skip_preface(ctx);
    while (auto f = mock::capture_frame(ctx)) {}

    // Exhaust both windows
    std::vector<std::byte> data(65535, std::byte{'x'});
    stream.send_data(data, false);
    while (auto f = mock::capture_frame(ctx)) {}

    // Replenish stream window only
    std::vector<std::byte> stream_wu;
    http::v2::encode_window_update_frame(stream_wu, stream.id(), 65535);
    mock::recv(ctx, stream_wu);

    // Register callback - should NOT fire (connection window is 0)
    bool callback_invoked = false;
    stream.on_window_available([&]() {
      callback_invoked = true;
    });
    CHECK(callback_invoked == false);

    // Send connection-level WINDOW_UPDATE
    std::vector<std::byte> conn_wu;
    http::v2::encode_window_update_frame(conn_wu, 0, 1000);
    mock::recv(ctx, conn_wu);

    // Now both windows are > 0, callback should fire
    CHECK(callback_invoked == true);
  }

  SUBCASE("connection callback invoked after connection WINDOW_UPDATE")
  {
    http::client_context ctx;
    auto stream = ctx.open_stream();
    stream.send_headers("POST", "/upload", "example.com", {}, false);

    // Drain all output
    mock::skip_preface(ctx);
    while (auto f = mock::capture_frame(ctx)) {}

    // Exhaust connection window
    std::vector<std::byte> data(65535, std::byte{'x'});
    stream.send_data(data, false);
    while (auto f = mock::capture_frame(ctx)) {}

    CHECK(stream.connection_send_window() == 0);

    bool callback_invoked = false;
    ctx.on_connection_window_available([&]() {
      callback_invoked = true;
    });

    // Send connection-level WINDOW_UPDATE
    std::vector<std::byte> wu;
    http::v2::encode_window_update_frame(wu, 0, 1000);
    mock::recv(ctx, wu);

    CHECK(callback_invoked == true);
  }

  SUBCASE("callback not invoked when window remains exhausted")
  {
    http::client_context ctx;
    auto stream = ctx.open_stream();
    stream.send_headers("POST", "/upload", "example.com", {}, false);

    // Drain all output
    mock::skip_preface(ctx);
    while (auto f = mock::capture_frame(ctx)) {}

    // Exhaust both windows
    std::vector<std::byte> data(65535, std::byte{'x'});
    stream.send_data(data, false);
    while (auto f = mock::capture_frame(ctx)) {}

    // Register callback - should NOT fire (window is exhausted)
    int callback_count = 0;
    stream.on_window_available([&]() {
      callback_count++;
    });
    CHECK(callback_count == 0);

    // No WINDOW_UPDATE sent - callback should not fire
    CHECK(callback_count == 0);
  }

  SUBCASE("stream callback invoked after SETTINGS_INITIAL_WINDOW_SIZE increases window")
  {
    http::client_context ctx;
    auto stream = ctx.open_stream();
    stream.send_headers("POST", "/upload", "example.com", {}, false);

    // Drain all output
    mock::skip_preface(ctx);
    while (auto f = mock::capture_frame(ctx)) {}

    // Exhaust both windows
    std::vector<std::byte> data(65535, std::byte{'x'});
    stream.send_data(data, false);
    while (auto f = mock::capture_frame(ctx)) {}

    // Replenish connection window only
    std::vector<std::byte> conn_wu;
    http::v2::encode_window_update_frame(conn_wu, 0, 65535);
    mock::recv(ctx, conn_wu);

    // Register callback - should NOT fire (stream window is 0)
    bool callback_invoked = false;
    stream.on_window_available([&]() {
      callback_invoked = true;
    });
    CHECK(callback_invoked == false);

    // Send SETTINGS with larger initial_window_size
    // Delta = 100000 - 65535 = 34465, stream window goes from 0 to 34465
    std::vector<http::v2::setting> settings = {
      {http::v2::settings_id::initial_window_size, 100000}
    };
    std::vector<std::byte> sf;
    http::v2::encode_settings_frame(sf, settings, false);
    mock::recv(ctx, sf);

    // Callback should fire
    CHECK(callback_invoked == true);
  }
}

// =============================================================================
// SECTION 3: Integration scenarios
// =============================================================================
//
// These tests verify realistic usage patterns with flow control.
//

TEST_CASE("Send flow control: realistic send pattern with backpressure")
{
  http::client_context ctx;
  auto stream = ctx.open_stream();
  stream.send_headers("POST", "/upload", "example.com", {}, false);

  // Drain all output
  mock::skip_preface(ctx);
  while (auto f = mock::capture_frame(ctx)) {}

  // Large body to send
  std::vector<std::byte> body(200000, std::byte{'x'});

  // Initial send - limited by window (65535)
  size_t total_sent = stream.send_data(body, false);
  CHECK(total_sent == 65535);

  // Drain DATA frames
  while (auto f = mock::capture_frame(ctx)) {}

  // Set up callback to send remaining data
  stream.on_window_available([&]() {
    size_t remaining = body.size() - total_sent;
    if (remaining == 0) return;

    size_t sent = stream.send_data(
      std::span<const std::byte>(body.data() + total_sent, remaining), true);
    total_sent += sent;
  });

  // Callback should NOT fire immediately (window is exhausted)
  CHECK(total_sent == 65535);

  // Simulate receiving WINDOW_UPDATE frames to replenish windows
  while (total_sent < body.size())
  {
    size_t before = total_sent;

    // Replenish both windows
    std::vector<std::byte> conn_wu;
    http::v2::encode_window_update_frame(conn_wu, 0, 65535);
    mock::recv(ctx, conn_wu);

    std::vector<std::byte> stream_wu;
    http::v2::encode_window_update_frame(stream_wu, stream.id(), 65535);
    mock::recv(ctx, stream_wu);

    // Drain any output generated by callback
    while (auto f = mock::capture_frame(ctx)) {}

    if (total_sent == before) break; // No progress, avoid infinite loop
  }

  CHECK(total_sent == body.size());
}