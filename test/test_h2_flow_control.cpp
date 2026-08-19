#include "doctest/doctest.h"
#include "test_helpers.h"
#include "http/v2/engine.h"
#include "http/v2/frame.h"
#include <vector>
#include <cstring>
#include <numeric>
#include <set>

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

  // Build a raw frame with arbitrary type, flags, and stream_id, with zero
  // payload. Useful for constructing frames like PING ACK that have no
  // dedicated encoder in the test helpers.
  std::vector<std::byte> make_empty_frame(uint8_t type, uint8_t flags, uint32_t stream_id)
  {
    std::vector<std::byte> frame;
    // Length: 0
    frame.push_back(std::byte{0x00});
    frame.push_back(std::byte{0x00});
    frame.push_back(std::byte{0x00});
    // Type
    frame.push_back(static_cast<std::byte>(type));
    // Flags
    frame.push_back(static_cast<std::byte>(flags));
    // Stream ID (31-bit, MSB reserved)
    frame.push_back(static_cast<std::byte>((stream_id >> 24) & 0x7F));
    frame.push_back(static_cast<std::byte>((stream_id >> 16) & 0xFF));
    frame.push_back(static_cast<std::byte>((stream_id >> 8) & 0xFF));
    frame.push_back(static_cast<std::byte>(stream_id & 0xFF));
    return frame;
  }
}

TEST_CASE("WINDOW_UPDATE frame encoding")
{
  SUBCASE("Connection-level WINDOW_UPDATE (stream 0)")
  {
    std::vector<std::byte> dst;
    http::v2::encode_window_update_frame(dst, 0, 32768);

    // 9-byte header + 4-byte payload = 13 bytes
    REQUIRE(dst.size() == 13);

    // Length: 4
    CHECK(dst[0] == std::byte(0x00));
    CHECK(dst[1] == std::byte(0x00));
    CHECK(dst[2] == std::byte(0x04));

    // Type: WINDOW_UPDATE (0x08)
    CHECK(dst[3] == std::byte(0x08));

    // Flags: 0
    CHECK(dst[4] == std::byte(0x00));

    // Stream ID: 0
    CHECK(dst[5] == std::byte(0x00));
    CHECK(dst[6] == std::byte(0x00));
    CHECK(dst[7] == std::byte(0x00));
    CHECK(dst[8] == std::byte(0x00));

    // Increment: 32768 = 0x00008000
    CHECK(dst[9]  == std::byte(0x00));
    CHECK(dst[10] == std::byte(0x00));
    CHECK(dst[11] == std::byte(0x80));
    CHECK(dst[12] == std::byte(0x00));
  }

  SUBCASE("Stream-level WINDOW_UPDATE")
  {
    std::vector<std::byte> dst;
    http::v2::encode_window_update_frame(dst, 1, 65535);

    REQUIRE(dst.size() == 13);

    // Stream ID: 1
    CHECK(dst[8] == std::byte(0x01));

    // Increment: 65535 = 0x0000FFFF
    CHECK(dst[9]  == std::byte(0x00));
    CHECK(dst[10] == std::byte(0x00));
    CHECK(dst[11] == std::byte(0xFF));
    CHECK(dst[12] == std::byte(0xFF));
  }

  SUBCASE("Reserved bit is always zero")
  {
    std::vector<std::byte> dst;
    // Even if we pass a value with MSB set, the reserved bit should be 0.
    http::v2::encode_window_update_frame(dst, 0, 0xFFFFFFFF);

    // The MSB of the 4-byte payload must be 0.
    CHECK((dst[9] & std::byte(0x80)) == std::byte(0x00));
  }
}

TEST_CASE("Engine flow control: WINDOW_UPDATE generation")
{
  http::v2::engine engine;
  auto stream_id = engine.open_stream();

  // Drain preface + initial SETTINGS from output.
  mock::skip_preface(engine);
  while (auto f = mock::capture_frame(engine)) {}

  SUBCASE("No WINDOW_UPDATE for small DATA frames")
  {
    // Send 100 bytes — well below the threshold (65535/2 = 32767).
    auto df = make_data_frame(stream_id, 100);
    mock::recv(engine, df);

    // Check output: should have no WINDOW_UPDATE.
    auto frame = mock::capture_frame(engine);
    CHECK_FALSE(frame.has_value());
  }

  SUBCASE("WINDOW_UPDATE after exceeding half the initial window")
  {
    // The default initial window is 65535. Threshold = 65535/2 = 32767.
    // Send a DATA frame of 33000 bytes (exceeds threshold).
    auto df = make_data_frame(stream_id, 33000);
    mock::recv(engine, df);

    // Should produce two WINDOW_UPDATE frames:
    //   1. Connection-level (stream 0)
    //   2. Stream-level (stream 1)
    auto f1 = mock::capture_frame(engine);
    REQUIRE(f1.has_value());
    CHECK(f1->type == 0x08);  // WINDOW_UPDATE

    auto f2 = mock::capture_frame(engine);
    REQUIRE(f2.has_value());
    CHECK(f2->type == 0x08);  // WINDOW_UPDATE

    // One should be for stream 0, the other for the stream.
    std::set<uint32_t> stream_ids = {f1->stream_id, f2->stream_id};
    CHECK(stream_ids.count(0) == 1);
    CHECK(stream_ids.count(stream_id) == 1);

    // Both should have increment == 33000.
    auto parse_increment = [](const mock::frame_info& f) -> uint32_t {
      REQUIRE(f.payload.size() == 4);
      return (static_cast<uint32_t>(f.payload[0] & std::byte(0x7F)) << 24) |
             (static_cast<uint32_t>(f.payload[1]) << 16) |
             (static_cast<uint32_t>(f.payload[2]) << 8) |
              static_cast<uint32_t>(f.payload[3]);
    };

    CHECK(parse_increment(*f1) == 33000);
    CHECK(parse_increment(*f2) == 33000);

    // No more frames.
    CHECK_FALSE(mock::capture_frame(engine).has_value());
  }

  SUBCASE("Accumulation: multiple small frames trigger WINDOW_UPDATE")
  {
    // Send many small DATA frames that together exceed the threshold.
    uint32_t total = 0;
    while (total < 33000) {
      uint32_t chunk = 1000;
      auto df = make_data_frame(stream_id, chunk);
      mock::recv(engine, df);
      total += chunk;
    }

    // Collect all output frames.
    std::vector<mock::frame_info> frames;
    while (auto f = mock::capture_frame(engine)) {
      frames.push_back(*f);
    }

    // Should have at least one connection-level and one stream-level WINDOW_UPDATE.
    int conn_updates = 0, stream_updates = 0;
    for (const auto& f : frames) {
      if (f.type == 0x08 && f.stream_id == 0) conn_updates++;
      if (f.type == 0x08 && f.stream_id == stream_id) stream_updates++;
    }

    CHECK(conn_updates >= 1);
    CHECK(stream_updates >= 1);
  }

  SUBCASE("Stream close cleans up per-stream tracking")
  {
    // Send a DATA frame with END_STREAM.
    auto df = make_data_frame(stream_id, 100, /*end_stream=*/true);

    bool stream_closed = false;
    engine.on_stream_closed([&](uint32_t) { stream_closed = true; });

    mock::recv(engine, df);

    CHECK(stream_closed == true);

    // After stream close, a new stream should start with fresh tracking.
    auto stream_id_2 = engine.open_stream();

    // Send enough data on the new stream to trigger WINDOW_UPDATE.
    auto df2 = make_data_frame(stream_id_2, 33000);
    mock::recv(engine, df2);

    bool found_stream_update = false;
    while (auto f = mock::capture_frame(engine)) {
      if (f->type == 0x08 && f->stream_id == stream_id_2) {
        found_stream_update = true;
      }
    }
    CHECK(found_stream_update == true);
  }
}

TEST_CASE("Engine flow control: large streaming response")
{
  http::v2::engine engine;
  auto stream_id = engine.open_stream();

  // Drain startup output.
  mock::skip_preface(engine);
  while (auto f = mock::capture_frame(engine)) {}

  // Simulate a 200KB streaming response — well beyond the 65535 initial window.
  // This verifies that WINDOW_UPDATEs allow the full transfer.
  uint32_t total_received = 0;
  std::vector<uint8_t> all_data;
  int window_updates_sent = 0;

  engine.on_data([&](uint32_t sid, const uint8_t* data, size_t len) {
    REQUIRE(sid == stream_id);
    all_data.insert(all_data.end(), data, data + len);
  });

  const uint32_t total_to_send = 200000;
  const uint32_t chunk_size = 8192;  // Typical HTTP/2 DATA frame size.

  uint32_t sent = 0;
  while (sent < total_to_send)
  {
    uint32_t this_chunk = std::min(chunk_size, total_to_send - sent);
    bool last = (sent + this_chunk >= total_to_send);

    auto df = make_data_frame(stream_id, this_chunk, last);
    mock::recv(engine, df);
    sent += this_chunk;

    // Drain WINDOW_UPDATE frames (simulating the I/O pump flushing output).
    while (auto f = mock::capture_frame(engine)) {
      if (f->type == 0x08) window_updates_sent++;
    }
  }

  // All data should have been received.
  CHECK(all_data.size() == total_to_send);

  // Multiple WINDOW_UPDATEs should have been generated.
  CHECK(window_updates_sent >= 4);  // 200KB / ~32KB threshold ≈ 6 updates per level.
}

TEST_CASE("Engine flow control: outgoing (send-side) window tracking")
{
  using http::v2::engine;

  SUBCASE("Windows initialize to the default 65535")
  {
    engine eng;
    auto sid = eng.open_stream();
    CHECK(eng.connection_send_window() == 65535);
    CHECK(eng.stream_send_window(sid) == 65535);
  }

  SUBCASE("Connection-level WINDOW_UPDATE credits the connection window")
  {
    engine eng;
    eng.open_stream();

    std::vector<std::byte> wu;
    http::v2::encode_window_update_frame(wu, 0, 10000);
    mock::recv(eng, wu);

    CHECK(eng.connection_send_window() == 65535 + 10000);
  }

  SUBCASE("Stream-level WINDOW_UPDATE credits only that stream")
  {
    engine eng;
    auto sid = eng.open_stream();

    std::vector<std::byte> wu;
    http::v2::encode_window_update_frame(wu, sid, 5000);
    mock::recv(eng, wu);

    CHECK(eng.stream_send_window(sid) == 65535 + 5000);
    CHECK(eng.connection_send_window() == 65535);  // unchanged
  }

  SUBCASE("send_data consumes both connection and stream windows")
  {
    engine eng;
    auto sid = eng.open_stream();

    std::vector<std::byte> body(1000, std::byte{'x'});
    eng.send_data(sid, body, /*end_stream=*/false);

    CHECK(eng.connection_send_window() == 65535 - 1000);
    CHECK(eng.stream_send_window(sid) == 65535 - 1000);
  }

  SUBCASE("WINDOW_UPDATE replenishes what send_data consumed")
  {
    engine eng;
    auto sid = eng.open_stream();

    std::vector<std::byte> body(40000, std::byte{'x'});
    eng.send_data(sid, body, false);
    CHECK(eng.connection_send_window() == 65535 - 40000);

    std::vector<std::byte> wu;
    http::v2::encode_window_update_frame(wu, 0, 40000);
    mock::recv(eng, wu);
    CHECK(eng.connection_send_window() == 65535);
  }

  SUBCASE("SETTINGS_INITIAL_WINDOW_SIZE adjusts existing and future streams")
  {
    engine eng;
    auto sid = eng.open_stream();
    CHECK(eng.stream_send_window(sid) == 65535);

    std::vector<http::v2::setting> settings = {
      {http::v2::settings_id::initial_window_size, 100000}
    };
    std::vector<std::byte> sf;
    http::v2::encode_settings_frame(sf, settings, /*ack=*/false);
    mock::recv(eng, sf);

    // Existing stream is adjusted by the delta (+34465).
    CHECK(eng.stream_send_window(sid) == 100000);
    // The connection-level window is NOT affected by SETTINGS.
    CHECK(eng.connection_send_window() == 65535);
    // A stream opened afterwards starts at the updated initial size.
    auto sid2 = eng.open_stream();
    CHECK(eng.stream_send_window(sid2) == 100000);
  }

  SUBCASE("WINDOW_UPDATE split across reads is reassembled")
  {
    engine eng;
    eng.open_stream();

    std::vector<std::byte> wu;
    http::v2::encode_window_update_frame(wu, 0, 12345);
    REQUIRE(wu.size() == 13);

    // Deliver the frame as two separate reads: 11 bytes then 2 bytes, splitting
    // the 4-byte payload across the boundary to exercise reassembly.
    std::vector<std::byte> part1(wu.begin(), wu.begin() + 11);
    std::vector<std::byte> part2(wu.begin() + 11, wu.end());
    mock::recv(eng, part1);
    mock::recv(eng, part2);

    CHECK(eng.connection_send_window() == 65535 + 12345);
  }
}

TEST_CASE("END_STREAM flag only fires closed_cb_ for DATA and HEADERS")
{
  http::v2::engine engine;
  auto stream_id = engine.open_stream();

  // Drain startup output.
  mock::skip_preface(engine);
  while (auto f = mock::capture_frame(engine)) {}

  SUBCASE("Zero-length PING ACK does not fire closed_cb_")
  {
    bool stream_closed = false;
    engine.on_stream_closed([&](uint32_t) { stream_closed = true; });

    // PING ACK: type=0x06, flags=0x01 (ACK), length=0, stream_id=0.
    // Flag 0x01 is ACK for PING, not END_STREAM. Before the fix this would
    // spuriously fire closed_cb_ because any zero-length frame with flag
    // 0x01 was treated as END_STREAM.
    auto ping_ack = make_empty_frame(0x06, 0x01, 0);
    mock::recv(engine, ping_ack);

    CHECK_FALSE(stream_closed);
  }

  SUBCASE("Zero-length DATA with END_STREAM still fires closed_cb_")
  {
    bool stream_closed = false;
    engine.on_stream_closed([&](uint32_t id) {
      CHECK(id == stream_id);
      stream_closed = true;
    });

    // DATA with END_STREAM and zero payload — should still fire closed_cb_.
    auto df = make_data_frame(stream_id, 0, /*end_stream=*/true);
    mock::recv(engine, df);

    CHECK(stream_closed);
  }
}

TEST_CASE("set_initial_window_size produces SETTINGS and WINDOW_UPDATE")
{
  http::v2::engine engine;

  // Must be called before open_stream() so SETTINGS includes the value.
  engine.set_initial_window_size(1048576);

  auto stream_id = engine.open_stream();

  // Drain preface.
  mock::skip_preface(engine);

  // First frame should be SETTINGS with initial_window_size = 1048576.
  auto settings_frame = mock::capture_frame(engine);
  REQUIRE(settings_frame.has_value());
  CHECK(settings_frame->type == 0x04);  // SETTINGS
  CHECK(settings_frame->length == 6);   // One setting entry (6 bytes)

  // Parse the setting: u16 id, u32 value (big-endian).
  REQUIRE(settings_frame->payload.size() == 6);
  uint16_t setting_id =
    (static_cast<uint16_t>(settings_frame->payload[0]) << 8) |
    static_cast<uint16_t>(settings_frame->payload[1]);
  uint32_t setting_value =
    (static_cast<uint32_t>(settings_frame->payload[2]) << 24) |
    (static_cast<uint32_t>(settings_frame->payload[3]) << 16) |
    (static_cast<uint32_t>(settings_frame->payload[4]) << 8) |
    static_cast<uint32_t>(settings_frame->payload[5]);

  CHECK(setting_id == static_cast<uint16_t>(http::v2::settings_id::initial_window_size));
  CHECK(setting_value == 1048576);

  // Second frame should be WINDOW_UPDATE on stream 0 for the delta.
  // Delta = 1048576 - 65535 = 983041.
  auto wu_frame = mock::capture_frame(engine);
  REQUIRE(wu_frame.has_value());
  CHECK(wu_frame->type == 0x08);  // WINDOW_UPDATE
  CHECK(wu_frame->stream_id == 0);
  REQUIRE(wu_frame->payload.size() == 4);
  uint32_t increment =
    (static_cast<uint32_t>(wu_frame->payload[0] & std::byte(0x7F)) << 24) |
    (static_cast<uint32_t>(wu_frame->payload[1]) << 16) |
    (static_cast<uint32_t>(wu_frame->payload[2]) << 8) |
    static_cast<uint32_t>(wu_frame->payload[3]);
  CHECK(increment == 1048576 - 65535);

  // No more frames.
  CHECK_FALSE(mock::capture_frame(engine).has_value());
}

TEST_CASE("Receive-side flow control enforcement")
{
  http::v2::engine engine;
  auto stream_id = engine.open_stream();

  // Drain startup output.
  mock::skip_preface(engine);
  while (auto f = mock::capture_frame(engine)) {}

  SUBCASE("Data within window does not trigger error")
  {
    bool error_fired = false;
    engine.on_connection_error([&](std::error_code) {
      error_fired = true;
    });

    // Send 65535 bytes — exactly the default window.
    auto df = make_data_frame(stream_id, 65535);
    mock::recv(engine, df);

    CHECK_FALSE(error_fired);

    // Drain any WINDOW_UPDATE frames.
    while (auto f = mock::capture_frame(engine)) {}
  }

  SUBCASE("Large transfer does not exhaust connection window")
  {
    // The connection recv window starts at 65535. As data arrives in chunks
    // (limited by the 16384-byte input buffer), update_flow_control()
    // replenishes the window via WINDOW_UPDATE whenever consumed bytes
    // exceed half the initial window size. This verifies the window never
    // goes negative during a sustained transfer larger than the window.
    bool error_fired = false;
    engine.on_connection_error([&](std::error_code) {
      error_fired = true;
    });

    // Simulate a 200KB transfer — well beyond the 65535 window.
    uint32_t total_received = 0;
    const uint32_t chunk_size = 8192;
    const uint32_t total_to_send = 200000;

    while (total_received < total_to_send)
    {
      uint32_t this_chunk = std::min(chunk_size, total_to_send - total_received);
      bool last = (total_received + this_chunk >= total_to_send);
      auto df = make_data_frame(stream_id, this_chunk, last);
      mock::recv(engine, df);
      total_received += this_chunk;

      // Drain WINDOW_UPDATE frames.
      while (auto f = mock::capture_frame(engine)) {}
    }

    CHECK_FALSE(error_fired);
    CHECK(total_received == total_to_send);
  }

  SUBCASE("Stream-level window violation triggers FLOW_CONTROL_ERROR")
  {
    http::v2::engine small_engine;
    small_engine.set_initial_window_size(1000);
    auto sid = small_engine.open_stream();

    // Drain startup output.
    mock::skip_preface(small_engine);
    while (auto f = mock::capture_frame(small_engine)) {}

    int error_count = 0;
    std::error_code received_error;
    small_engine.on_connection_error([&](std::error_code ec) {
      error_count++;
      received_error = ec;
    });

    // Send 1001 bytes — exceeds the stream's 1000-byte window but not the
    // connection's 65535-byte window. The stream-level check should still
    // fire FLOW_CONTROL_ERROR.
    auto df = make_data_frame(sid, 1001);
    mock::recv(small_engine, df);

    CHECK(error_count >= 1);
    CHECK(received_error == http::make_error_code(http::error_code::flow_control_error));
  }
}
