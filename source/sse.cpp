# include "http/sse.h"
# include <cctype>
# include <charconv>

namespace http
{
  std::span<const std::byte> strip_utf8_bom(std::span<const std::byte> chunk)
  {
    // A UTF-8 BOM is exactly 3 bytes long
    if (
      chunk.size() >= 3 &&
      chunk[0] == std::byte{0xEF} &&
      chunk[1] == std::byte{0xBB} &&
      chunk[2] == std::byte{0xBF}
    )
    {
      // Return a new span starting after the 3 BOM bytes
      return chunk.subspan(3);
    }

    // Return the original span unchanged if no BOM is present
    return chunk;
  }

  void sse_parser::parse(std::span<const std::byte> chunk,  std::function<void(sse_event)> on_event)
  {
    auto bytes = chunk;

    if ( state_ == state::init )
    {
      bytes = strip_utf8_bom(chunk);
      state_ = state::start_line;
    }

    for (const auto& b : bytes)
    {
      char c = std::to_integer<char>(b);

      if (last_was_cr_ && c == '\n')
      {
        last_was_cr_ = false;
        continue;
      }
      last_was_cr_ = (c == '\r');

      if (c == '\n' || c == '\r')
      {
        switch (state_)
        {
        case state::init:
          [[fallthrough]];
        case state::start_line:
          // Empty line triggers a dispatch
          if (!current_event_.data.empty() || !current_event_.type.empty() || !current_event_.id.empty())
          {
            // Strip the trailing LF appended by the last "data:" field
            if (current_event_.data.back() == '\n') {
              current_event_.data.pop_back();
            }
            // Dispatch a copy or reference of the current event state
            on_event(current_event_);
          }
          // Clear only type and data; id and retry persist across events
          current_event_.type.clear();
          current_event_.data.clear();
          break;
        case state::in_field_name:
          current_field_value_.clear();
          process_field();
          break;
        case state::after_colon:
        case state::in_field_value:
          process_field();
          break;
        case state::in_comment:
          break;
        }
        current_field_name_.clear();
        current_field_value_.clear();
        state_ = state::start_line;
        continue;
      }
      else
      {
        switch (state_)
        {
        case state::init:
          [[fallthrough]];
        case state::start_line:
          if (c == ':') {
            state_ = state::in_comment;
          }
          else {
            current_field_name_ += c;
            state_ = state::in_field_name;
          }
          break;
        case state::in_comment:
          break;
        case state::in_field_name:
          if (c == ':') {
            state_ = state::after_colon;
          }
          else {
            current_field_name_ += c;
          }
          break;
        case state::after_colon:
          if (c == ' ') {
            state_ = state::in_field_value;
            break;
          }
          else {
            state_ = state::in_field_value;
            [[fallthrough]];
          }
        case state::in_field_value:
          current_field_value_ += c;
          break;
        }
      }
    }
  }

  void sse_parser::process_field()
  {
    if (current_field_name_ == "data")
    {
      current_event_.data.append(current_field_value_);
      current_event_.data.push_back('\n');
    }
    else if (current_field_name_ == "event")
    {
      current_event_.type = current_field_value_;
    }
    else if (current_field_name_ == "id")
    {
      if (current_field_value_.find('\0') == std::string::npos)
      {
        current_event_.id = current_field_value_;
      }
    }
    else if (current_field_name_ == "retry")
    {
      bool valid_digits = !current_field_value_.empty();
      for (char c : current_field_value_)
      {
        if (c < '0' || c > '9')
        {
          valid_digits = false;
          break;
        }
      }
      if (valid_digits)
      {
        try
        {
          uint64_t ms = std::stoull(current_field_value_);
          current_event_.retry = std::chrono::milliseconds(ms);
        }
        catch (...)
        {
          // Ignore on parsing overflow
        }
      }
    }
  }

  void sse_parser::reset()
  {
    state_ = state::init;
    current_field_name_.clear();
    current_field_value_.clear();
    current_event_.clear();
  }
} // namespace http
