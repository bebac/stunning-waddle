#ifndef INCLUDE_HTTP_CLIENT_STREAM_H
#define INCLUDE_HTTP_CLIENT_STREAM_H

#include "http/headers.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <system_error>

namespace http
{
  enum class stream_state_enum
  {
    idle,
    open,
    half_closed_local,
    half_closed_remote,
    closed
  };

  struct stream_state
  {
    uint32_t id = 0;
    stream_state_enum state = stream_state_enum::idle;
    bool headers_sent = false;

    // Pre-bound send functions (stream doesn't need to know about engine)
    std::function<void(uint32_t, std::string_view, std::string_view, std::string_view, const headers&, bool)> send_headers_fn;
    std::function<void(uint32_t, std::span<const std::byte>, bool)> send_data_fn;

    // Response callbacks (set by user via stream handle)
    std::function<void(const headers&)> on_headers;
    std::function<void(std::span<const std::byte>)> on_data;
    std::function<void()> on_end;
    std::function<void(std::error_code)> on_reset;
  };

  class client_stream
  {
  public:
    client_stream() = default;

  public:
    explicit operator bool() const { return impl_ != nullptr; }

    // --- Request sending (stream-oriented) ---

    void send_headers(
      std::string_view method,
      std::string_view path,
      std::string_view authority,
      const headers& h = {},
      bool end_stream = false
    );
    void send_data(std::span<const std::byte> data, bool end_stream = false);
    void send_end();

    // --- Response callbacks ---

    void on_headers(std::function<void(const headers&)> cb);
    void on_data(std::function<void(std::span<const std::byte>)> cb);
    void on_end(std::function<void()> cb);
    void on_reset(std::function<void(std::error_code)> cb);

    // --- State accessors ---

    uint32_t id() const;
    stream_state_enum state() const;

  private:
    friend class client_context;

    // Only client_context can create valid streams
    explicit client_stream(std::shared_ptr<stream_state> impl);

  private:
    std::shared_ptr<stream_state> impl_;
  };
}

#endif // INCLUDE_HTTP_CLIENT_STREAM_H
