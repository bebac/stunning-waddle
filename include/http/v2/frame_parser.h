#ifndef INCLUDE_HTTP_V2_FRAME_PARSER_H
#define INCLUDE_HTTP_V2_FRAME_PARSER_H

#include "http/v2/frame.h"
#include <functional>
#include <vector>
#include <cstring>
#include <ranges>
#include <algorithm>

namespace http::v2
{
  class frame_parser
  {
  private:
    enum class state
    {
      reading_header,
      reading_payload
    };

  public:
    using frame_header_callback = std::function<void(frame_header)>;
    using payload_chunk_callback = std::function<void(frame_header, std::span<const std::byte>)>;

    void on_frame_header(frame_header_callback cb);
    void on_payload_chunk(payload_chunk_callback cb);
    void consume(std::span<const std::byte> buf);

  private:
    void process_header();

  private:
    state state_ = state::reading_header;
    std::array<std::byte, 9> header_buffer_;
    std::size_t header_pos_ = 0;
    frame_header current_header_;
    std::size_t payload_bytes_read_ = 0;
    frame_header_callback header_cb_;
    payload_chunk_callback payload_cb_;
  };
} // namespace http::v2

#endif // INCLUDE_HTTP_V2_FRAME_PARSER_H
