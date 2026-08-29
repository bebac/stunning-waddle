#ifndef INCLUDE_HTTP_EVENT_H
#define INCLUDE_HTTP_EVENT_H

#include "http/headers.h"
#include "http/error_codes.h"

#include <cstddef>
#include <cstdint>
#include <system_error>
#include <variant>
#include <vector>

namespace http
{
  // Event types produced by the protocol engine during input_end().
  // Events are queued internally and consumed via next_event().
  enum class event_type
  {
    new_stream,
    headers,
    data,
    stream_closed,
    stream_reset,
    goaway,
    connection_error,
    stream_window_available,
    connection_window_available
  };

  // GOAWAY payload: last stream ID + error code.
  struct goaway_info
  {
    uint32_t last_stream_id = 0;
    error_code ec = error_code::no_error;
  };

  // A protocol event produced by the engine.
  //
  // Events own their data. The consumer can process them synchronously
  // or co_await between events — the data remains valid until the
  // event is destroyed or moved.
  //
  // Payload access by event type:
  //   headers                     -> std::get<http::headers>(payload)
  //   data                        -> std::get<std::vector<std::byte>>(payload)
  //   stream_reset                -> std::get<std::error_code>(payload)
  //   connection_error            -> std::get<std::error_code>(payload)
  //   goaway                      -> std::get<goaway_info>(payload)
  //   new_stream                  -> payload is std::monostate
  //   stream_closed               -> payload is std::monostate
  //   stream_window_available     -> payload is std::monostate
  //   connection_window_available -> payload is std::monostate
  struct event
  {
    uint32_t stream_id = 0;
    event_type type;

    std::variant<
      std::monostate,
      http::headers,
      std::vector<std::byte>,
      std::error_code,
      goaway_info
    > payload;

    // --- Constructors ---

    event() = default;

    event(uint32_t sid, event_type t)
      : stream_id(sid), type(t) {}

    event(uint32_t sid, event_type t, http::headers h)
      : stream_id(sid), type(t), payload(std::move(h)) {}

    event(uint32_t sid, event_type t, std::vector<std::byte> data)
      : stream_id(sid), type(t), payload(std::move(data)) {}

    event(uint32_t sid, event_type t, std::error_code ec)
      : stream_id(sid), type(t), payload(ec) {}

    event(uint32_t sid, event_type t, goaway_info info)
      : stream_id(sid), type(t), payload(std::move(info)) {}

    // --- Convenience type checks ---

    bool is_new_stream() const { return type == event_type::new_stream; }
    bool is_headers() const { return type == event_type::headers; }
    bool is_data() const { return type == event_type::data; }
    bool is_stream_closed() const { return type == event_type::stream_closed; }
    bool is_stream_reset() const { return type == event_type::stream_reset; }
    bool is_goaway() const { return type == event_type::goaway; }
    bool is_connection_error() const { return type == event_type::connection_error; }
    bool is_stream_window_available() const { return type == event_type::stream_window_available; }
    bool is_connection_window_available() const { return type == event_type::connection_window_available; }
  };

} // namespace http

#endif // INCLUDE_HTTP_EVENT_H