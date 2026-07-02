#ifndef INCLUDE_HTTP_SSE_H
#define INCLUDE_HTTP_SSE_H

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>

namespace http
{
  // Represents a single Server-Sent Event.
  struct sse_event
  {
    std::string id;       // Last event ID
    std::string type;     // Event type (from "event:" field)
    std::string data;     // Data buffer (from "data:" fields)
    std::optional<std::chrono::milliseconds> retry;

  public:
    sse_event() = default;

    void clear()
    {
      id.clear();
      type.clear();
      data.clear();
      retry.reset();
    }
  };

  // Parser for Server-Sent Events (SSE) format.
  class sse_parser
  {
  public:
    void parse(std::span<const std::byte> chunk, std::function<void(sse_event)>);
    void reset();

  private:
    void process_field();

  private:
    enum class state
    {
      init,
      start_line,
      in_field_name,
      after_colon,
      in_field_value,
      in_comment
    };

  private:
    state state_ = state::init;
    bool last_was_cr_ = false;

    sse_event current_event_;

    std::string current_field_name_;
    std::string current_field_value_;
  };
} // namespace http

#endif // INCLUDE_HTTP_SSE_H
