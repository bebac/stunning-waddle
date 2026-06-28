#ifndef INCLUDE_HTTP_V2_ENGINE_H
#define INCLUDE_HTTP_V2_ENGINE_H

#include "http/protocol_engine.h"
#include "http/headers.h"
#include "http/error_codes.h"
#include "http/v2/frame.h"
#include "http/v2/frame_parser.h"
#include "http/v2/hpack_context.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace http::v2
{
  // Unified stream flow control state
  struct stream_flow_state
  {
    uint32_t consumed_bytes = 0; // Bytes consumed (incoming flow control)
    int64_t send_window = 0;     // Send window (outgoing flow control)
    bool known = false;          // Whether this stream is known/acknowledged
  };

  class engine : public protocol_engine
  {
  private:
    enum class state
    {
      idle,
      awaiting_preface,    // Server: waiting for client preface
      preface_sent,        // Client: sent preface
      ready                // Connection established
    };
  public:
    explicit engine(connection_role role = connection_role::client);

    auto input_begin() -> std::span<std::byte> override;
    void input_end(size_t n) override;
    auto output_begin() -> std::span<const std::byte> override;
    void output_end(size_t n) override;

    uint32_t open_stream() override;

    void send_request_headers(
      uint32_t stream_id,
      std::string_view method,
      std::string_view path,
      std::string_view authority,
      const headers& headers,
      bool end_stream
    ) override;

    void send_response_headers(
      uint32_t stream_id,
      int status_code,
      const headers& headers,
      bool end_stream
    ) override;

    void send_data(uint32_t stream_id, std::span<const std::byte> data, bool end_stream) override;

    void send_reset([[maybe_unused ]] uint32_t stream_id, [[maybe_unused ]] std::error_code ec) override
    {
    }

    void send_goaway(uint32_t last_stream_id, http::error_code error_code, std::span<const std::byte> debug_data = {});

  public:
    // --- Outgoing (send-side) flow-control inspection ---
    //
    // How much DATA the peer currently allows us to send. The connection-level
    // window (stream 0) is shared by all streams; each stream also has its own
    // window. These are tracked from received WINDOW_UPDATE/SETTINGS frames but
    // are not yet enforced by send_data.
    int64_t connection_send_window() const { return connection_send_window_; }
    int64_t stream_send_window(uint32_t stream_id) const;

  private:
    void handle_frame_header(frame_header h);
    void handle_payload_chunk(frame_header h, const uint8_t* data, size_t len);
    bool try_consume_client_preface();
    void handle_window_update(uint32_t stream_id, std::span<const std::byte> payload);
    void handle_settings_payload(std::span<const std::byte> payload);
    void handle_goaway_payload(std::span<const std::byte> payload, uint32_t length);
    void apply_peer_initial_window_size(uint32_t new_size);
    stream_flow_state& get_or_init_stream_flow_state(uint32_t stream_id);

  private:
    void write_preface();
    void write_settings();
    void send_headers_frame(uint32_t stream_id, const std::vector<http::header>& header_list, bool end_stream);

  private:
    void update_flow_control(uint32_t stream_id, uint32_t bytes_received);

  private:
    connection_role role_;
    state state_ = state::idle;
    std::array<std::byte, 16384> input_buffer_;
    size_t input_buffered_ = 0;  // For preface validation
    std::vector<std::byte> pending_out_;
    frame_parser parser_;
    hpack_context encode_ctx_;
    hpack_context decode_ctx_;
    uint32_t next_stream_id_ = 1;

    // Flow control: track consumed bytes and emit WINDOW_UPDATEs.
    static constexpr uint32_t default_initial_window_size_ = 65535;
    uint32_t initial_window_size_ = default_initial_window_size_;
    uint32_t connection_consumed_ = 0;

    // Send-side flow control: the peer's advertised windows that bound how much
    // DATA we may send. The connection-level window is shared across all
    // streams. Signed so a SETTINGS-driven reduction of the initial window can
    // make a stream window legitimately negative (RFC 7540 6.9.2).
    int64_t connection_send_window_ = default_initial_window_size_;
    uint32_t peer_initial_window_size_ = default_initial_window_size_;

    // Unified stream tracking: combines known_streams_, stream_consumed_, and stream_send_window_
    std::map<uint32_t, stream_flow_state> stream_flow_states_;

    // Reassembly buffer for control-frame payloads (WINDOW_UPDATE / SETTINGS),
    // which the parser may deliver across multiple chunks.
    std::vector<std::byte> control_payload_;

    bool goaway_received_ = false;
    uint32_t last_goaway_stream_id_ = 0;
    http::error_code last_goaway_error_code_ = http::error_code::no_error;
  };
} // namespace http::v2

#endif // INCLUDE_HTTP_V2_ENGINE_H
