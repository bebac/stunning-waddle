#ifndef INCLUDE_HTTP_V2_ENGINE_H
#define INCLUDE_HTTP_V2_ENGINE_H

#include "http/protocol_engine.h"
#include "http/headers.h"
#include "http/v2/frame.h"
#include "http/v2/frame_parser.h"
#include "http/v2/hpack_context.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace http::v2
{
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

  private:
    void handle_frame_header(frame_header h);
    void handle_payload_chunk(frame_header h, const uint8_t* data, size_t len);
    bool try_consume_client_preface();

  private:
    void write_preface();
    void write_settings();
    void send_headers_frame(uint32_t stream_id, const std::vector<http::header>& header_list, bool end_stream);

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
    std::set<uint32_t> known_streams_;  // Track streams we know about
  };
} // namespace http::v2

#endif // INCLUDE_HTTP_V2_ENGINE_H
