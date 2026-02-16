#include "http/client_context.h"

namespace http
{
  client_context::client_context()
  {
    set_protocol_version(protocol_version::v2);
  }

  void client_context::set_protocol_version(protocol_version v)
  {
    switch (v)
    {
    case protocol_version::v1_1:
      throw std::runtime_error("HTTP/1.1 not supported yet");
    case protocol_version::v2:
      engine_ = std::make_unique<v2::engine>();
      break;
    case protocol_version::v3:
      throw std::runtime_error("HTTP/3 not supported yet");
    }
    register_engine_callbacks();
  }

  client_stream client_context::open_stream()
  {
    auto id = engine_->open_stream();

    // Create shared state
    auto state = std::make_shared<stream_state>();

    state->id = id;

    // Bind engine functions - stream doesn't need to know about engine or context
    state->send_headers_fn = [this](uint32_t stream_id, std::string_view method, std::string_view path,
      std::string_view authority, const headers& h, bool end_stream)
    {
      engine_->send_headers(stream_id, method, path, authority, h, end_stream);
    };

    state->send_data_fn = [this](uint32_t stream_id, std::span<const std::byte> data, bool end_stream) {
      engine_->send_data(stream_id, data, end_stream);
    };

    // Auto-register for event dispatching
    streams_[id] = state;

    return client_stream(state);
  }

  void client_context::on_goaway(std::function<void(uint32_t last_stream_id, std::error_code)> cb) {
    on_goaway_ = std::move(cb);
  }

  void client_context::on_connection_error(std::function<void(std::error_code)> cb) {
    on_conn_error_ = std::move(cb);
  }

  void client_context::register_engine_callbacks()
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

  void client_context::dispatch_headers(uint32_t id, const headers& h)
  {
    if (auto it = streams_.find(id); it != streams_.end())
    {
      if (auto& cb = it->second->on_headers) {
        cb(h);
      }
    }
  }

  void client_context::dispatch_data(uint32_t id, std::span<const std::byte> data)
  {
    if (auto it = streams_.find(id); it != streams_.end())
    {
      if (auto& cb = it->second->on_data) {
        cb(data);
      }
    }
  }

  void client_context::dispatch_end(uint32_t id)
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

  void client_context::dispatch_reset(uint32_t id, std::error_code ec)
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
