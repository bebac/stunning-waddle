#ifndef INCLUDE_HTTP_CONTEXT_BASE_H
#define INCLUDE_HTTP_CONTEXT_BASE_H

#include "http/protocol_engine.h"
#include "http/stream.h"
#include "http/v2/engine.h"

#include <memory>
#include <system_error>
#include <map>

namespace http
{
  class context_base
  {
  public:
    virtual ~context_base() = default;

    // Engine interface passthrough (Sans-I/O)
    auto input_begin() { return engine_->input_begin(); }
    void input_end(size_t n) { engine_->input_end(n); }
    auto output_begin() { return engine_->output_begin(); }
    void output_end(size_t n) { engine_->output_end(n); }

    // Stream management
    auto active_streams() const -> std::size_t { return streams_.size(); }

    // Connection-level events
    void on_goaway(std::function<void(uint32_t last_stream_id, std::error_code)> cb);
    void on_connection_error(std::function<void(std::error_code)> cb);

  protected:
    context_base() = default;

    // Called by subclasses after setting up engine
    void register_common_callbacks();

    // Common stream creation - subclass provides the bound functions
    stream create_stream_with_state(std::shared_ptr<stream_state> state);

    // Common event dispatchers
    void dispatch_data(uint32_t id, std::span<const std::byte> data);
    void dispatch_end(uint32_t id);
    void dispatch_reset(uint32_t id, std::error_code ec);

    // Subclass-specific header dispatch (client vs server behavior differs)
    virtual void dispatch_headers(uint32_t id, const headers& h) = 0;

  protected:
    std::unique_ptr<protocol_engine> engine_;
    std::map<uint32_t, std::shared_ptr<stream_state>> streams_;
    std::function<void(uint32_t, std::error_code)> on_goaway_;
    std::function<void(std::error_code)> on_conn_error_;
  };
}

#endif // INCLUDE_HTTP_CONTEXT_BASE_H
