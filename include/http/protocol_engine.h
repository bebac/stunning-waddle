#ifndef INCLUDE_HTTP_PROTOCOL_ENGINE_H
#define INCLUDE_HTTP_PROTOCOL_ENGINE_H

#include "http/headers.h"
#include "http/error_codes.h"
#include "http/event.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>

namespace http
{
  enum class protocol_version
  {
    v1_1,
    v2,
    v3
  };

  enum class connection_role
  {
    client,
    server
  };

  class protocol_engine
  {
  public:
    virtual ~protocol_engine() = default;

    //
    // --- Input: network -> engine ---
    //
    // Interface to feed data into the protocol engine. Call input_begin
    // to obtain the buffer to receive into. When receive is complete, call
    // input_end with to actual number of bytes received.
    //
    virtual auto input_begin() -> std::span<std::byte> = 0;
    virtual void input_end(size_t n) = 0;

    //
    // --- EOF ---
    //
    // Signals that the peer closed the connection (a read returned 0).
    // The default implementation is a no-op; engines that distinguish
    // EOF from a zero-length read (e.g. HTTP/1.1 close-delimited
    // bodies) override it.
    //
    virtual void eof() {}

    //
    // --- Output: engine -> network ---
    //
    // Interface to extract data from the protocol engine
    // call output_begin to get data ready to be sent, when sent has
    // completed, call ouput_end with actual number of bytes sent.
    //
    virtual auto output_begin() -> std::span<const std::byte> = 0;
    virtual void output_end(size_t n) = 0;

    // Output readiness: level-triggered polling.
    //
    // has_output() returns true if there is pending data to send.
    // Use this for level-triggered driving: check before each I/O operation
    // or poll() call to determine if POLLOUT should be requested.
    //
    // Level-triggered example:
    //   while (engine->has_output()) { poll with POLLOUT; write; }
    //
    virtual bool has_output() const = 0;

    // Output readiness: edge-triggered notification.
    //
    // on_output_ready() registers a callback that is invoked when the
    // engine transitions from having no output to having output. The
    // callback fires only on the empty→non-empty transition, not on
    // every write. It is reset when output_end() drains the buffer
    // to empty.
    //
    // Use this for edge-triggered driving (e.g., with epoll, io_uring,
    // or to wake a sleeping event loop). The callback is invoked
    // synchronously from the point where output is added, so it should
    // not perform I/O directly but rather signal the event loop.
    //
    // If no callback is registered, output readiness is polled via
    // has_output().
    using output_ready_callback = std::function<void()>;
    virtual void on_output_ready(output_ready_callback cb) = 0;

    //
    // --- Events ---
    //
    // After calling input_end(n), the engine may have produced events
    // (headers received, data received, stream closed, etc.). Events are
    // queued internally and own their data. The consumer drains them via
    // has_events() / next_event().
    //
    // Events are FIFO within a single input_end() call.
    //
    virtual bool has_events() const = 0;
    virtual auto next_event() -> std::optional<event> = 0;

    //
    // --- Stream interface ---
    //
    virtual uint32_t open_stream() = 0;

    // Client: send request headers
    virtual void send_request_headers(
      uint32_t stream_id,
      std::string_view method,
      std::string_view path,
      std::string_view authority,
      const headers& headers,
      bool end_stream
    ) = 0;

    // Server: send response headers
    virtual void send_response_headers(
      uint32_t stream_id,
      int status_code,
      const headers& headers,
      bool end_stream
    ) = 0;

    virtual auto send_data(uint32_t stream_id, std::span<const std::byte> data, bool end_stream) -> size_t = 0;
    virtual void send_reset(uint32_t stream_id, std::error_code ec) = 0;

    // --- Flow control window inspection ---
    virtual auto connection_send_window() const -> int64_t = 0;
    virtual auto stream_send_window(uint32_t stream_id) const -> int64_t = 0;

    // --- Flow control configuration ---
    virtual void set_initial_window_size(uint32_t size) {}
  };
} // namespace http

#endif // INCLUDE_HTTP_PROTOCOL_ENGINE_H