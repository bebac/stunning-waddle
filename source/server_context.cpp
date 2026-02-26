#include "http/server_context.h"

namespace http
{
  server_context::server_context()
  {
    set_protocol_version(protocol_version::v2);
  }

  void server_context::set_protocol_version(protocol_version v)
  {
    switch (v)
    {
    case protocol_version::v1_1:
      throw std::runtime_error("HTTP/1.1 not supported yet");
    case protocol_version::v2:
      engine_ = std::make_unique<v2::engine>(connection_role::server);
      break;
    case protocol_version::v3:
      throw std::runtime_error("HTTP/3 not supported yet");
    }
    register_common_callbacks();
    register_server_callbacks();
  }

  void server_context::on_new_stream(std::function<void(stream, const headers&)> cb)
  {
    on_new_stream_ = std::move(cb);
  }

  void server_context::register_server_callbacks()
  {
    // Server-specific: called when engine detects a new stream
    engine_->on_new_stream([this](uint32_t id) {
      dispatch_new_stream(id);
    });
  }

  stream server_context::create_server_stream(uint32_t id)
  {
    auto state = std::make_shared<stream_state>();
    state->id = id;

    // Bind server-side send functions
    state->send_response_headers_fn = [this](uint32_t stream_id, int status_code,
      const headers& h, bool end_stream)
    {
      engine_->send_response_headers(stream_id, status_code, h, end_stream);
    };

    state->send_data_fn = [this](uint32_t stream_id, std::span<const std::byte> data, bool end_stream) {
      engine_->send_data(stream_id, data, end_stream);
    };

    return create_stream_with_state(state);
  }

  void server_context::dispatch_new_stream(uint32_t id)
  {
    // Create the stream but don't notify yet - wait for headers
    create_server_stream(id);
  }

  void server_context::dispatch_headers(uint32_t id, const headers& h)
  {
    if (auto it = streams_.find(id); it != streams_.end())
    {
      auto& state = it->second;

      // First HEADERS on this stream - notify user with stream + request headers
      if (state->state == stream_state_enum::idle)
      {
        state->state = stream_state_enum::open;
        if (on_new_stream_) {
          on_new_stream_(stream(state), h);
        }
      }
      else
      {
        // Subsequent headers (e.g., trailers) - use the on_headers callback
        if (auto& cb = state->on_headers) {
          cb(h);
        }
      }
    }
  }
} // namespace http
