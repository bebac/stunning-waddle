#ifndef INCLUDE_HTTP_CLIENT_CONTEXT_H
#define INCLUDE_HTTP_CLIENT_CONTEXT_H

#include "http/protocol_engine.h"
#include "http/client_stream.h"
#include "http/v2/engine.h"

#include <memory>
#include <system_error>
#include <map>

namespace http
{
  class client_context
  {
  public:
    client_context();

  public:
    void set_protocol_version(protocol_version v);

    // // Engine interface passthrough
    auto input_begin() { return engine_->input_begin(); }
    void input_end(size_t n) { engine_->input_end(n); }
    auto output_begin() { return engine_->output_begin(); }
    void output_end(size_t n) { engine_->output_end(n); }

    // Stream management.
    auto open_stream() -> client_stream;
    auto active_streams() -> std::size_t { return streams_.size(); }

    // Connection-level events
    void on_goaway(std::function<void(uint32_t last_stream_id, std::error_code)> cb);
    void on_connection_error(std::function<void(std::error_code)> cb);

    // Settings
    //void set_max_concurrent_streams(uint32_t n);
    //void set_initial_window_size(uint32_t n);

  private:
    void register_engine_callbacks();

    // Event dispatchers called by engine callbacks
    void dispatch_headers(uint32_t id, const headers& h);
    void dispatch_data(uint32_t id, std::span<const std::byte> data);
    void dispatch_end(uint32_t id);
    void dispatch_reset(uint32_t id, std::error_code ec);

  private:
    std::unique_ptr<protocol_engine> engine_;
    std::map<uint32_t, std::shared_ptr<stream_state>> streams_;
    std::function<void(uint32_t, std::error_code)> on_goaway_;
    std::function<void(std::error_code)> on_conn_error_;
  };
}

#endif // INCLUDE_HTTP_CLIENT_CONTEXT_H
