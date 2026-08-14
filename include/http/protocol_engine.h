#ifndef INCLUDE_HTTP_PROTOCOL_ENGINE_H
#define INCLUDE_HTTP_PROTOCOL_ENGINE_H

#include "http/headers.h"
#include "http/error_codes.h"

#include <cstddef>
#include <cstdint>
#include <functional>
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
    using data_callback = std::function<void(uint32_t, const uint8_t*, size_t)>;
    using stream_closed_callback = std::function<void(uint32_t)>;
    using stream_reset_callback = std::function<void(uint32_t, std::error_code)>;
    using headers_callback = std::function<void(uint32_t, const headers&)>;
    using new_stream_callback = std::function<void(uint32_t)>;
    using goaway_callback = std::function<void(uint32_t last_stream_id, http::error_code error_code)>;
    using connection_error_callback = std::function<void(std::error_code)>;
    using connection_window_available_callback = std::function<void()>;
    using stream_window_available_callback = std::function<void(uint32_t)>;

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
    // --- Output: engine -> network ---
    //
    // Interface to extract data from the protocol engine
    // call output_begin to get data ready to be sent, when sent has
    // completed, call ouput_end with actual number of bytes sent.
    //
    virtual auto output_begin() -> std::span<const std::byte> = 0;
    virtual void output_end(size_t n) = 0;

    // Output readiness notification
    // 
    // has_output() returns true if there is pending data to send.
    // Use this for level-triggered driving: check before each I/O operation
    // or poll() call to determine if POLLOUT should be requested.
    //
    // on_output_ready() registers a callback that is invoked when the engine
    // transitions from having no output to having output (edge-triggered).
    // Use this for edge-triggered driving (e.g., with epoll, io_uring, or
    // to wake a sleeping event loop). The callback is invoked synchronously
    // from the point where output is added, so it should not perform I/O
    // directly but rather signal the event loop (e.g., via eventfd, self-pipe).
    //
    // Level-triggered example:
    //   while (engine->has_output()) { poll with POLLOUT; write; }
    //
    // Edge-triggered example:
    //   engine->on_output_ready([&] { event_loop.wake(); });
    //   // In event loop: when POLLOUT fires, write until WANT_WRITE
    //
    virtual bool has_output() const = 0;

    using output_ready_callback = std::function<void()>;
    virtual void on_output_ready(output_ready_callback cb) = 0;

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

    virtual size_t send_data(uint32_t stream_id, std::span<const std::byte> data, bool end_stream) = 0;
    virtual void send_reset(uint32_t stream_id, std::error_code ec) = 0;

    // --- Flow control window inspection ---
    virtual int64_t connection_send_window() const = 0;
    virtual int64_t stream_send_window(uint32_t stream_id) const = 0;

  public:
    void on_headers(headers_callback cb)
    {
      headers_cb_ = std::move(cb);
    }

    void on_data(data_callback cb)
    {
      data_cb_ = std::move(cb);
    }

    void on_stream_closed(stream_closed_callback cb)
    {
      closed_cb_ = std::move(cb);
    }

    void on_stream_reset(stream_reset_callback cb)
    {
      reset_cb_ = std::move(cb);
    }

    // Server mode: called when client opens a new stream
    void on_new_stream(new_stream_callback cb)
    {
      new_stream_cb_ = std::move(cb);
    }

    void on_goaway(goaway_callback cb)
    {
      goaway_cb_ = std::move(cb);
    }

    void on_connection_error(connection_error_callback cb)
    {
      conn_error_cb_ = std::move(cb);
    }

    void on_connection_window_available(connection_window_available_callback cb)
    {
      conn_window_available_cb_ = std::move(cb);
    }

    void on_stream_window_available(stream_window_available_callback cb)
    {
      stream_window_available_cb_ = std::move(cb);
    }

  protected:
    headers_callback headers_cb_;
    data_callback data_cb_;
    stream_closed_callback closed_cb_;
    stream_reset_callback reset_cb_;
    new_stream_callback new_stream_cb_;
    goaway_callback goaway_cb_;
    connection_error_callback conn_error_cb_;
    output_ready_callback output_ready_cb_;
    connection_window_available_callback conn_window_available_cb_;
    stream_window_available_callback stream_window_available_cb_;
  };
} // namespace http

#endif // INCLUDE_HTTP_PROTOCOL_ENGINE_H
