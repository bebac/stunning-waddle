#ifndef INCLUDE_HTTP_V2_FRAME_H
#define INCLUDE_HTTP_V2_FRAME_H

#include <cstdint>
#include <vector>
#include <map>
#include <span>

namespace http::v2
{
  enum class frame_type : uint8_t {
    data = 0x00,
    headers = 0x01,
    priority = 0x02,
    rst_stream = 0x03,
    settings = 0x04,
    push_promise = 0x05,
    ping = 0x06,
    goaway = 0x07,
    window_update = 0x08,
    continuation = 0x09
  };

  enum class settings_id : uint16_t {
    header_table_size = 0x01,
    enable_push = 0x02,
    max_concurrent_streams = 0x03,
    initial_window_size = 0x04,
    max_frame_size = 0x05,
    max_header_list_size = 0x06
  };

  struct frame_header {
    uint32_t length : 24;
    frame_type type;
    std::byte flags;
    uint32_t stream_id : 31;
    uint32_t reserved : 1;
  };

  struct setting {
    settings_id id;
    uint32_t value;
  };

  void encode_data_frame(
    std::vector<std::byte>& dst,
    uint32_t stream_id,
    std::span<const std::byte> payload,
    bool end_stream
  );

  void encode_headers_frame(
    std::vector<std::byte>& dst,
    uint32_t stream_id,
    std::span<const std::byte> header_block,
    bool end_headers,
    bool end_stream);

  void encode_settings_frame(
    std::vector<std::byte>& dst,
    const std::vector<setting>& settings,
    bool ack
  );
} // namespace http::v2

#endif // INCLUDE_HTTP_V2_FRAME_H
