#include "http/server_context.h"
#include "http/v1/engine.h"

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
      engine_ = std::make_unique<v1::engine>(connection_role::server);
      break;
    case protocol_version::v2:
      engine_ = std::make_unique<v2::engine>(connection_role::server);
      break;
    case protocol_version::v3:
      throw std::runtime_error("HTTP/3 not supported yet");
    }
  }

  void server_context::on_new_stream_event(uint32_t id)
  {
    // Create the stream but don't notify yet - wait for headers
    create_server_stream(id);
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

    state->send_data_fn = [this](uint32_t stream_id, std::span<const std::byte> data, bool end_stream) -> size_t {
      return engine_->send_data(stream_id, data, end_stream);
    };

    state->stream_send_window_fn = [this](uint32_t stream_id) -> int64_t {
      return engine_->stream_send_window(stream_id);
    };

    state->connection_send_window_fn = [this]() -> int64_t {
      return engine_->connection_send_window();
    };

    return create_stream_with_state(state);
  }
} // namespace http