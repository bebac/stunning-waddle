#ifndef INCLUDE_HTTP_CONTEXT_BASE_H
#define INCLUDE_HTTP_CONTEXT_BASE_H

#include "http/protocol_engine.h"
#include "http/stream.h"
#include "http/v2/engine.h"

#include <functional>
#include <memory>
#include <system_error>
#include <unordered_map>

namespace http
{
  class context_base
  {
  public:
    virtual ~context_base() = default;

    // Engine interface passthrough (Sans-I/O)
    auto input_begin() { return engine_->input_begin(); }
    void input_end(size_t n) { engine_->input_end(n); }
    void eof() { engine_->eof(); }
    auto output_begin() { return engine_->output_begin(); }
    void output_end(size_t n) { engine_->output_end(n); }

    // Output readiness interface
    bool has_output() const { return engine_->has_output(); }
    void on_output_ready(std::function<void()> cb) { engine_->on_output_ready(std::move(cb)); }

    // Direct event access — consumers drain events directly via
    // has_events() / next_event(). The context handles stream
    // lifecycle events (new_stream, stream_closed, stream_reset) internally:
    //   - new_stream: creates stream state, consumer never sees this event
    //   - stream_closed: updates stream state, returns event to consumer
    //   - stream_reset: cleans up stream, returns event to consumer
    // Use get_stream(id) to retrieve the stream handle when processing
    // a headers event.
    bool has_events() const { return engine_->has_events(); }
    auto next_event() -> std::optional<event>;

    // Stream management
    auto get_stream(uint32_t id) -> stream;
    auto active_streams() const -> std::size_t { return streams_.size(); }

  protected:
    context_base() = default;

    // Called when a new_stream event is received. Default is no-op;
    // server_context overrides to create the stream.
    virtual void on_new_stream_event(uint32_t /*id*/) {}

    // Common stream creation - subclass provides the bound functions
    auto create_stream_with_state(std::shared_ptr<stream_state> state) -> stream;

    // Internal stream lifecycle helpers (no callback firing).
    // Used by next_event() to manage stream state transparently.
    void close_stream_internal(uint32_t id);
    void reset_stream_internal(uint32_t id);

  protected:
    std::unique_ptr<protocol_engine> engine_;
    std::unordered_map<uint32_t, std::shared_ptr<stream_state>> streams_;
  };
}

#endif // INCLUDE_HTTP_CONTEXT_BASE_H