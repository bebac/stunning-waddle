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
      engine_ = std::make_unique<v2::engine>(connection_role::client);
      break;
    case protocol_version::v3:
      throw std::runtime_error("HTTP/3 not supported yet");
    }
    register_common_callbacks();
  }

  stream client_context::open_stream()
  {
    auto id = engine_->open_stream();

    auto state = std::make_shared<stream_state>();
    state->id = id;

    // Bind client-side send functions
    state->send_request_headers_fn = [this](uint32_t stream_id, std::string_view method, std::string_view path,
      std::string_view authority, const headers& h, bool end_stream)
    {
      engine_->send_request_headers(stream_id, method, path, authority, h, end_stream);
    };

    state->send_data_fn = [this](uint32_t stream_id, std::span<const std::byte> data, bool end_stream) {
      engine_->send_data(stream_id, data, end_stream);
    };

    return create_stream_with_state(state);
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
} // namespace http
