#include "doctest/doctest.h"
#include "test_helpers.h"
#include "http/context_base.h"
#include "http/client_context.h"
#include "http/server_context.h"
#include "http/v2/engine.h"
#include "http/event.h"
#include "http/error_codes.h"
#include <vector>
#include <cstring>
#include <algorithm>
#include <ranges>

// Ensure we're using the vendored stunning-waddle headers
static_assert(requires { typename http::context_base; }, "context_base should be available");
static_assert(requires { typename http::client_context; }, "client_context should be available");
static_assert(requires { typename http::server_context; }, "server_context should be available");

namespace mock
{
  // Feed data to a context without auto-draining events.
  // Feed data to a context. input_end() does not auto-drain events,
  // so events remain in the queue for next_event().
  void feed_data(http::context_base& ctx, std::span<const std::byte> data)
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
  std::vector<http::event> drain_all_events(http::context_base& ctx)
  {
    std::vector<http::event> events;
    while (ctx.has_events()) {
      auto ev = ctx.next_event();
      if (ev) events.push_back(std::move(*ev));
    }
    return events;
  }
}

TEST_CASE("Context Events - goaway")
{
  GIVEN("A client context receiving a GOAWAY frame")
  {
    http::client_context client_ctx;

    // Clear initial preface and settings from output
    auto out = client_ctx.output_begin();
    client_ctx.output_end(out.size());

    WHEN("A GOAWAY frame is received")
    {
      auto goaway_frame = make_bytes(
        0x00, 0x00, 0x08,      // Length 8
        0x07,                  // Type GOAWAY
        0x00,                  // Flags
        0x00, 0x00, 0x00, 0x00, // Stream ID 0
        0x00, 0x00, 0x00, 0x01, // Last stream ID: 1
        0x00, 0x00, 0x00, 0x01  // Error code: PROTOCOL_ERROR (1)
      );

      mock::feed_data(client_ctx, goaway_frame);
      auto events = mock::drain_all_events(client_ctx);

      THEN("A goaway event should be in the queue")
      {
        REQUIRE(events.size() == 1);
        CHECK(events[0].is_goaway());

        auto& info = std::get<http::goaway_info>(events[0].payload);
        CHECK(info.last_stream_id == 1);
        CHECK(info.ec == http::error_code::protocol_error);
      }
    }
  }

  GIVEN("A server context receiving a GOAWAY frame")
  {
    http::server_context server_ctx;

    // Server needs to receive valid preface first to get past preface validation
    auto preface = make_bytes(
      'P', 'R', 'I', ' ', '*', ' ', 'H', 'T', 'T', 'P', '/', '2', '.', '0', '\r', '\n', '\r', '\n', 'S', 'M', '\r', '\n', '\r', '\n'
    );
    mock::feed_data(server_ctx, preface);

    // Clear any output (SETTINGS ACK, etc.)
    auto out = server_ctx.output_begin();
    if (out.size() > 0) {
      server_ctx.output_end(out.size());
    }

    // Drain any events from preface processing
    mock::drain_all_events(server_ctx);

    WHEN("A GOAWAY frame is received")
    {
      auto goaway_frame = make_bytes(
        0x00, 0x00, 0x08,      // Length 8
        0x07,                  // Type GOAWAY
        0x00,                  // Flags
        0x00, 0x00, 0x00, 0x00, // Stream ID 0
        0x00, 0x00, 0x00, 0x03, // Last stream ID: 3
        0x00, 0x00, 0x00, 0x00  // Error code: NO_ERROR (0)
      );

      mock::feed_data(server_ctx, goaway_frame);
      auto events = mock::drain_all_events(server_ctx);

      THEN("A goaway event should be in the queue")
      {
        REQUIRE(events.size() == 1);
        CHECK(events[0].is_goaway());

        auto& info = std::get<http::goaway_info>(events[0].payload);
        CHECK(info.last_stream_id == 3);
        CHECK(info.ec == http::error_code::no_error);
      }
    }
  }
}

TEST_CASE("Context Events - connection_error")
{
  GIVEN("A server context receiving an invalid client preface")
  {
    http::server_context server_ctx;

    WHEN("An invalid preface is received")
    {
      auto invalid_preface = make_bytes(
        'I', 'N', 'V', 'A', 'L', 'I', 'D', ' ', 'P', 'R', 'E', 'F', 'A', 'C', 'E',
        ' ', 'T', 'E', 'S', 'T', ' ', 'D', 'A', 'T', 'A'
      );

      mock::feed_data(server_ctx, invalid_preface);
      auto events = mock::drain_all_events(server_ctx);

      THEN("A connection_error event should be in the queue")
      {
        REQUIRE(events.size() == 1);
        CHECK(events[0].is_connection_error());

        auto& ec = std::get<std::error_code>(events[0].payload);
        CHECK(ec == http::make_error_code(http::error_code::protocol_error));
      }
    }
  }

  GIVEN("A server context receiving a WINDOW_UPDATE with zero increment")
  {
    http::server_context server_ctx;

    // First, send valid preface to get past preface validation
    auto preface_bytes = make_bytes(
      'P', 'R', 'I', ' ', '*', ' ', 'H', 'T', 'T', 'P', '/', '2', '.', '0', '\r', '\n', '\r', '\n', 'S', 'M', '\r', '\n', '\r', '\n'
    );
    mock::feed_data(server_ctx, preface_bytes);

    // Clear any output (SETTINGS ACK, etc.)
    auto out = server_ctx.output_begin();
    if (out.size() > 0) {
      server_ctx.output_end(out.size());
    }

    // Drain any events from preface processing
    mock::drain_all_events(server_ctx);

    WHEN("A WINDOW_UPDATE frame with zero increment is received")
    {
      auto window_update_frame = make_bytes(
        0x00, 0x00, 0x04,      // Length 4
        0x08,                  // Type WINDOW_UPDATE
        0x00,                  // Flags
        0x00, 0x00, 0x00, 0x00, // Stream ID 0
        0x00, 0x00, 0x00, 0x00  // Zero increment - PROTOCOL_ERROR
      );

      mock::feed_data(server_ctx, window_update_frame);
      auto events = mock::drain_all_events(server_ctx);

      THEN("A connection_error event should be in the queue")
      {
        REQUIRE(events.size() == 1);
        CHECK(events[0].is_connection_error());

        auto& ec = std::get<std::error_code>(events[0].payload);
        CHECK(ec == http::make_error_code(http::error_code::protocol_error));
      }
    }
  }

  GIVEN("A server context receiving a GOAWAY with invalid last_stream_id")
  {
    http::server_context server_ctx;

    // First, send valid preface to get past preface validation
    auto preface_bytes = make_bytes(
      'P', 'R', 'I', ' ', '*', ' ', 'H', 'T', 'T', 'P', '/', '2', '.', '0', '\r', '\n', '\r', '\n', 'S', 'M', '\r', '\n', '\r', '\n'
    );
    mock::feed_data(server_ctx, preface_bytes);

    // Clear any output
    auto out = server_ctx.output_begin();
    if (out.size() > 0) {
      server_ctx.output_end(out.size());
    }

    // Drain any events from preface processing
    mock::drain_all_events(server_ctx);

    WHEN("A GOAWAY frame with invalid last_stream_id (MSB set) is received")
    {
      auto goaway_frame = make_bytes(
        0x00, 0x00, 0x08,      // Length 8
        0x07,                  // Type GOAWAY
        0x00,                  // Flags
        0x00, 0x00, 0x00, 0x00, // Stream ID 0
        0x80, 0x00, 0x00, 0x00, // Last stream ID: 0x80000000 (invalid - MSB set)
        0x00, 0x00, 0x00, 0x00  // Error code: NO_ERROR
      );

      mock::feed_data(server_ctx, goaway_frame);
      auto events = mock::drain_all_events(server_ctx);

      THEN("A connection_error event should be in the queue")
      {
        REQUIRE(events.size() == 1);
        CHECK(events[0].is_connection_error());

        auto& ec = std::get<std::error_code>(events[0].payload);
        CHECK(ec == http::make_error_code(http::error_code::frame_size_error));
      }
    }
  }
}

TEST_CASE("Context Events - multiple events")
{
  GIVEN("A server context")
  {
    http::server_context server_ctx;

    WHEN("An invalid preface is received")
    {
      auto invalid_preface = make_bytes(
        'I', 'N', 'V', 'A', 'L', 'I', 'D', ' ', 'P', 'R', 'E', 'F', 'A', 'C', 'E',
        ' ', 'T', 'E', 'S', 'T', ' ', 'D', 'A', 'T', 'A'
      );

      mock::feed_data(server_ctx, invalid_preface);
      auto events = mock::drain_all_events(server_ctx);

      THEN("Only a connection_error event should be present")
      {
        REQUIRE(events.size() == 1);
        CHECK(events[0].is_connection_error());
      }
    }
  }

  GIVEN("A server context with an established connection")
  {
    http::server_context server_ctx;

    // Send valid preface
    auto preface_bytes = make_bytes(
      'P', 'R', 'I', ' ', '*', ' ', 'H', 'T', 'T', 'P', '/', '2', '.', '0', '\r', '\n', '\r', '\n', 'S', 'M', '\r', '\n', '\r', '\n'
    );
    mock::feed_data(server_ctx, preface_bytes);

    // Clear output
    auto out = server_ctx.output_begin();
    if (out.size() > 0) {
      server_ctx.output_end(out.size());
    }

    // Drain any events from preface processing
    mock::drain_all_events(server_ctx);

    WHEN("A valid GOAWAY frame is received")
    {
      auto goaway_frame = make_bytes(
        0x00, 0x00, 0x08,      // Length 8
        0x07,                  // Type GOAWAY
        0x00,                  // Flags
        0x00, 0x00, 0x00, 0x00, // Stream ID 0
        0x00, 0x00, 0x00, 0x05, // Last stream ID: 5
        0x00, 0x00, 0x00, 0x00  // Error code: NO_ERROR
      );

      mock::feed_data(server_ctx, goaway_frame);
      auto events = mock::drain_all_events(server_ctx);

      THEN("Only a goaway event should be present")
      {
        REQUIRE(events.size() == 1);
        CHECK(events[0].is_goaway());

        auto& info = std::get<http::goaway_info>(events[0].payload);
        CHECK(info.last_stream_id == 5);
      }
    }
  }
}