#ifndef INCLUDE_HTTP_PROTOCOL_ENGINE_H
#define INCLUDE_HTTP_PROTOCOL_ENGINE_H

#include "http/headers.h"

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

  class protocol_engine
  {
  public:
    using data_callback = std::function<void(uint32_t, const uint8_t*, size_t)>;
    using stream_closed_callback = std::function<void(uint32_t)>;
    using stream_reset_callback = std::function<void(uint32_t, std::error_code)>;
    using headers_callback = std::function<void(uint32_t, const headers&)>;

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

    //
    // --- Stream interface ---
    //
    virtual uint32_t open_stream() = 0;

    virtual void send_headers(
      uint32_t stream_id,
      std::string_view method,
      std::string_view path,
      std::string_view authority,
      const headers& headers,
      bool end_stream
    ) = 0;

    virtual void send_data(uint32_t stream_id, std::span<const std::byte> data, bool end_stream) = 0;
    virtual void send_reset(uint32_t stream_id, std::error_code ec) = 0;

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

  protected:
    headers_callback headers_cb_;
    data_callback data_cb_;
    stream_closed_callback closed_cb_;
    stream_reset_callback reset_cb_;
  };
} // namespace http

#endif // INCLUDE_HTTP_PROTOCOL_ENGINE_H
