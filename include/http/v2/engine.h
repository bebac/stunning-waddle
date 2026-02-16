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
      preface_sent,
      preface_recevied
    };
  public:
    engine();

    auto input_begin() -> std::span<std::byte> override;
    void input_end(size_t n) override;
    auto output_begin() -> std::span<const std::byte> override;
    void output_end(size_t n) override;

    uint32_t open_stream() override;

    void send_headers(
      uint32_t stream_id,
      std::string_view method,
      std::string_view path,
      std::string_view authority,
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

  private:
    void write_preface();
    void write_settings();

  private:
    state state_ = state::idle;
    std::array<std::byte, 16384> input_buffer_;
    std::vector<std::byte> pending_out_;
    frame_parser parser_;
    hpack_context encode_ctx_;
    hpack_context decode_ctx_;
    uint32_t next_stream_id_ = 1;
  };
} // namespace http::v2

#endif // INCLUDE_HTTP_V2_ENGINE_H
