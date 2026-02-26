#include "http/context_base.h"

namespace http
{
  void context_base::on_goaway(std::function<void(uint32_t last_stream_id, std::error_code)> cb)
  {
    on_goaway_ = std::move(cb);
  }

  void context_base::on_connection_error(std::function<void(std::error_code)> cb)
  {
    on_conn_error_ = std::move(cb);
  }

  void context_base::register_common_callbacks()
  {
    engine_->on_headers([this](uint32_t id, const headers& h) {
      dispatch_headers(id, h);
    });

    engine_->on_data([this](uint32_t id, const uint8_t* d, size_t l) {
      dispatch_data(id, std::span<const std::byte>(reinterpret_cast<const std::byte*>(d), l));
    });

    engine_->on_stream_closed([this](uint32_t id) {
      dispatch_end(id);
    });

    engine_->on_stream_reset([this](uint32_t id, std::error_code ec) {
      dispatch_reset(id, ec);
    });
  }

  stream context_base::create_stream_with_state(std::shared_ptr<stream_state> state)
  {
    streams_[state->id] = state;
    return stream(state);
  }

  void context_base::dispatch_data(uint32_t id, std::span<const std::byte> data)
  {
    if (auto it = streams_.find(id); it != streams_.end())
    {
      if (auto& cb = it->second->on_data) {
        cb(data);
      }
    }
  }

  void context_base::dispatch_end(uint32_t id)
  {
    if (auto it = streams_.find(id); it != streams_.end())
    {
      auto& state = it->second;
      if (state->state == stream_state_enum::half_closed_local)
        state->state = stream_state_enum::closed;
      else
        state->state = stream_state_enum::half_closed_remote;

      if (auto& cb = state->on_end) {
        cb();
      }

      // Clean up closed streams
      if (state->state == stream_state_enum::closed) {
        streams_.erase(it);
      }
    }
  }

  void context_base::dispatch_reset(uint32_t id, std::error_code ec)
  {
    if (auto it = streams_.find(id); it != streams_.end())
    {
      if (auto& cb = it->second->on_reset) {
        cb(ec);
      }
      streams_.erase(it);
    }
  }
} // namespace http
