#include "doctest/doctest.h"
#include "test_helpers.h"
#include "http/context_base.h"
#include "http/client_context.h"
#include "http/server_context.h"
#include "http/v2/engine.h"
#include "http/error_codes.h"
#include <vector>
#include <cstring>

// Ensure we're using the vendored stunning-waddle headers
static_assert(requires { typename http::context_base; }, "context_base should be available");
static_assert(requires { typename http::client_context; }, "client_context should be available");
static_assert(requires { typename http::server_context; }, "server_context should be available");

TEST_CASE("Context Callbacks - on_goaway")
{
  GIVEN("A client context with on_goaway callback registered")
  {
    http::client_context client_ctx;
    bool goaway_called = false;
    uint32_t last_stream_id = 0;
    std::error_code error_code;

    client_ctx.on_goaway([&](uint32_t stream_id, std::error_code ec) {
      goaway_called = true;
      last_stream_id = stream_id;
      error_code = ec;
    });

    // Clear initial preface and settings from output
    auto out = client_ctx.output_begin();
    client_ctx.output_end(out.size());

    WHEN("A GOAWAY frame is received")
    {
      // Create a GOAWAY frame: length 8, type 7, flags 0, stream 0
      // Payload: last_stream_id (4 bytes) + error_code (4 bytes)
      auto goaway_frame = make_bytes(
        0x00, 0x00, 0x08,      // Length 8
        0x07,                  // Type GOAWAY
        0x00,                  // Flags
        0x00, 0x00, 0x00, 0x00, // Stream ID 0
        0x00, 0x00, 0x00, 0x01, // Last stream ID: 1
        0x00, 0x00, 0x00, 0x01  // Error code: PROTOCOL_ERROR (1)
      );

      mock::recv(client_ctx, goaway_frame);

      THEN("The on_goaway callback should be triggered")
      {
        CHECK(goaway_called == true);
        CHECK(last_stream_id == 1);
        CHECK(error_code == http::make_error_code(http::error_code::protocol_error));
      }
    }
  }

  GIVEN("A server context with on_goaway callback registered")
  {
    http::server_context server_ctx;
    bool goaway_called = false;
    uint32_t last_stream_id = 0;
    std::error_code error_code;

    server_ctx.on_goaway([&](uint32_t stream_id, std::error_code ec) {
      goaway_called = true;
      last_stream_id = stream_id;
      error_code = ec;
    });

    // Server needs to receive valid preface first to get past preface validation
    auto preface = make_bytes(
      'P', 'R', 'I', ' ', '*', ' ', 'H', 'T', 'T', 'P', '/', '2', '.', '0', '\r', '\n', '\r', '\n', 'S', 'M', '\r', '\n', '\r', '\n'
    );
    mock::recv(server_ctx, preface);
    
    // Clear any output (SETTINGS ACK, etc.)
    auto out = server_ctx.output_begin();
    if (out.size() > 0) {
      server_ctx.output_end(out.size());
    }

    WHEN("A GOAWAY frame is received")
    {
      // Create a GOAWAY frame
      auto goaway_frame = make_bytes(
        0x00, 0x00, 0x08,      // Length 8
        0x07,                  // Type GOAWAY
        0x00,                  // Flags
        0x00, 0x00, 0x00, 0x00, // Stream ID 0
        0x00, 0x00, 0x00, 0x03, // Last stream ID: 3
        0x00, 0x00, 0x00, 0x00  // Error code: NO_ERROR (0)
      );

      mock::recv(server_ctx, goaway_frame);

      THEN("The on_goaway callback should be triggered")
      {
        CHECK(goaway_called == true);
        CHECK(last_stream_id == 3);
        CHECK(error_code == http::make_error_code(http::error_code::no_error));
      }
    }
  }
}

TEST_CASE("Context Callbacks - on_connection_error")
{
  GIVEN("A server context with on_connection_error callback registered")
  {
    http::server_context server_ctx;
    bool error_called = false;
    std::error_code error_code;

    server_ctx.on_connection_error([&](std::error_code ec) {
      error_called = true;
      error_code = ec;
    });

    WHEN("An invalid client preface is received")
    {
      // Send invalid preface (wrong bytes) - need 24 bytes to trigger validation
      auto invalid_preface = make_bytes(
        'I', 'N', 'V', 'A', 'L', 'I', 'D', ' ', 'P', 'R', 'E', 'F', 'A', 'C', 'E',
        ' ', 'T', 'E', 'S', 'T', ' ', 'D', 'A', 'T', 'A'
      );

      mock::recv(server_ctx, invalid_preface);

      THEN("The on_connection_error callback should be triggered with protocol_error")
      {
        CHECK(error_called == true);
        CHECK(error_code == http::make_error_code(http::error_code::protocol_error));
      }
    }
  }

  GIVEN("A server context with on_connection_error callback for WINDOW_UPDATE error")
  {
    http::server_context server_ctx;
    bool error_called = false;
    std::error_code error_code;

    server_ctx.on_connection_error([&](std::error_code ec) {
      error_called = true;
      error_code = ec;
    });

    // First, send valid preface to get past preface validation
    auto preface_bytes = make_bytes(
      'P', 'R', 'I', ' ', '*', ' ', 'H', 'T', 'T', 'P', '/', '2', '.', '0', '\r', '\n', '\r', '\n', 'S', 'M', '\r', '\n', '\r', '\n'
    );
    mock::recv(server_ctx, preface_bytes);

    // Clear any output (SETTINGS ACK, etc.)
    auto out = server_ctx.output_begin();
    if (out.size() > 0) {
      server_ctx.output_end(out.size());
    }

    WHEN("A WINDOW_UPDATE frame with zero increment is received")
    {
      // Create a WINDOW_UPDATE frame with zero increment (protocol error)
      auto window_update_frame = make_bytes(
        0x00, 0x00, 0x04,      // Length 4
        0x08,                  // Type WINDOW_UPDATE
        0x00,                  // Flags
        0x00, 0x00, 0x00, 0x00, // Stream ID 0
        0x00, 0x00, 0x00, 0x00  // Zero increment - PROTOCOL_ERROR
      );

      mock::recv(server_ctx, window_update_frame);

      THEN("The on_connection_error callback should be triggered with protocol_error")
      {
        CHECK(error_called == true);
        CHECK(error_code == http::make_error_code(http::error_code::protocol_error));
      }
    }
  }

  GIVEN("A server context with on_connection_error callback for invalid GOAWAY payload")
  {
    http::server_context server_ctx;
    bool error_called = false;
    std::error_code error_code;

    server_ctx.on_connection_error([&](std::error_code ec) {
      error_called = true;
      error_code = ec;
    });

    // First, send valid preface to get past preface validation
    auto preface_bytes = make_bytes(
      'P', 'R', 'I', ' ', '*', ' ', 'H', 'T', 'T', 'P', '/', '2', '.', '0', '\r', '\n', '\r', '\n', 'S', 'M', '\r', '\n', '\r', '\n'
    );
    mock::recv(server_ctx, preface_bytes);

    // Clear any output
    auto out = server_ctx.output_begin();
    if (out.size() > 0) {
      server_ctx.output_end(out.size());
    }

    WHEN("A GOAWAY frame with invalid last_stream_id (MSB set) is received")
    {
      // Create a GOAWAY frame with invalid last_stream_id (MSB set to 1)
      auto goaway_frame = make_bytes(
        0x00, 0x00, 0x08,      // Length 8
        0x07,                  // Type GOAWAY
        0x00,                  // Flags
        0x00, 0x00, 0x00, 0x00, // Stream ID 0
        0x80, 0x00, 0x00, 0x00, // Last stream ID: 0x80000000 (invalid - MSB set)
        0x00, 0x00, 0x00, 0x00  // Error code: NO_ERROR
      );

      mock::recv(server_ctx, goaway_frame);

      THEN("The on_connection_error callback should be triggered with frame_size_error")
      {
        CHECK(error_called == true);
        CHECK(error_code == http::make_error_code(http::error_code::frame_size_error));
      }
    }
  }
}

TEST_CASE("Context Callbacks - Multiple callbacks")
{
  GIVEN("A server context with both on_goaway and on_connection_error callbacks")
  {
    http::server_context server_ctx;
    bool goaway_called = false;
    bool error_called = false;

    server_ctx.on_goaway([&](uint32_t, std::error_code) {
      goaway_called = true;
    });

    server_ctx.on_connection_error([&](std::error_code) {
      error_called = true;
    });

    WHEN("An invalid preface is received")
    {
      // Need 24 bytes to trigger preface validation
      auto invalid_preface = make_bytes(
        'I', 'N', 'V', 'A', 'L', 'I', 'D', ' ', 'P', 'R', 'E', 'F', 'A', 'C', 'E',
        ' ', 'T', 'E', 'S', 'T', ' ', 'D', 'A', 'T', 'A'
      );
      mock::recv(server_ctx, invalid_preface);

      THEN("Only the connection error callback should be triggered")
      {
        CHECK(error_called == true);
        CHECK(goaway_called == false);
      }
    }

    // Reset for next test
    error_called = false;
    goaway_called = false;

    // Send valid preface
    auto preface_bytes = make_bytes(
      'P', 'R', 'I', ' ', '*', ' ', 'H', 'T', 'T', 'P', '/', '2', '.', '0', '\r', '\n', '\r', '\n', 'S', 'M', '\r', '\n', '\r', '\n'
    );
    mock::recv(server_ctx, preface_bytes);

    // Clear output
    auto out = server_ctx.output_begin();
    if (out.size() > 0) {
      server_ctx.output_end(out.size());
    }

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

      mock::recv(server_ctx, goaway_frame);

      THEN("Only the goaway callback should be triggered")
      {
        CHECK(goaway_called == true);
        CHECK(error_called == false);
      }
    }
  }
}

TEST_CASE("Context Callbacks - Callback replacement")
{
  GIVEN("A server context where callbacks are replaced")
  {
    http::server_context server_ctx;
    int goaway_call_count = 0;
    int error_call_count = 0;

    // First callback
    server_ctx.on_goaway([&](uint32_t, std::error_code) {
      goaway_call_count++;
    });

    server_ctx.on_connection_error([&](std::error_code) {
      error_call_count++;
    });

    // Replace callbacks
    server_ctx.on_goaway([&](uint32_t, std::error_code) {
      goaway_call_count += 10; // Different behavior
    });

    server_ctx.on_connection_error([&](std::error_code) {
      error_call_count += 10; // Different behavior
    });

    WHEN("Events are triggered")
    {
      // Trigger connection error - need 24 bytes to trigger preface validation
      auto invalid_preface = make_bytes(
        'I', 'N', 'V', 'A', 'L', 'I', 'D', ' ', 'P', 'R', 'E', 'F', 'A', 'C', 'E',
        ' ', 'T', 'E', 'S', 'T', ' ', 'D', 'A', 'T', 'A'
      );
      mock::recv(server_ctx, invalid_preface);

      THEN("The new callbacks should be called")
      {
        CHECK(error_call_count == 10);
        CHECK(goaway_call_count == 0);
      }
    }
  }
}