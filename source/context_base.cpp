#include "http/context_base.h"
#include "http/error_codes.h"

namespace http
{
  std::optional<event> context_base::next_event()
  {
    while (true)
    {
      auto ev = engine_->next_event();
      if (!ev) return std::nullopt;

      switch (ev->type)
      {
      case event_type::new_stream:
        // Internal: create stream state. The consumer never sees
        // this event — loop to get the next one.
        on_new_stream_event(ev->stream_id);
        continue;

      case event_type::stream_closed:
        // Internal: update stream state (half-closed remote → closed).
        // Erase from streams_ if fully closed.
        close_stream_internal(ev->stream_id);
        // Consumer still needs this event to close its channels.
        return ev;

      case event_type::stream_reset:
        // Internal: erase from streams_ immediately.
        reset_stream_internal(ev->stream_id);
        // Consumer still needs this event to close its channels.
        return ev;

      default:
        // All other events pass through unchanged.
        return ev;
      }
    }
  }

  stream context_base::get_stream(uint32_t id)
  {
    auto it = streams_.find(id);
    if (it != streams_.end()) {
      return stream(it->second);
    }
    return stream();
  }

  void context_base::close_stream_internal(uint32_t id)
  {
    if (auto it = streams_.find(id); it != streams_.end())
    {
      auto& state = it->second;
      if (state->state == stream_state_enum::half_closed_local)
        state->state = stream_state_enum::closed;
      else if (state->state == stream_state_enum::open)
        state->state = stream_state_enum::half_closed_remote;
      else
        // idle or already half_closed_remote → fully closed
        state->state = stream_state_enum::closed;

      // Clean up closed streams
      if (state->state == stream_state_enum::closed) {
        streams_.erase(it);
      }
    }
  }

  void context_base::reset_stream_internal(uint32_t id)
  {
    streams_.erase(id);
  }

  stream context_base::create_stream_with_state(std::shared_ptr<stream_state> state)
  {
    streams_[state->id] = state;
    return stream(state);
  }
} // namespace http