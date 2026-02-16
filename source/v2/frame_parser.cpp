#include "http/v2/frame_parser.h"

namespace http::v2
{
  void frame_parser::on_frame_header(frame_header_callback cb)
  {
    header_cb_ = std::move(cb);
  }

  void frame_parser::on_payload_chunk(payload_chunk_callback cb)
  {
    payload_cb_ = std::move(cb);
  }

  void frame_parser::consume(std::span<const std::byte> buf)
  {
    size_t offset = 0;
    while (offset < buf.size())
    {
      switch (state_)
      {
        case state::reading_header:
        {
          size_t to_copy = std::min(9 - header_pos_, buf.size() - offset);
          std::ranges::copy(buf.subspan(offset, to_copy), header_buffer_.begin() + header_pos_);

          offset += to_copy;
          header_pos_ += to_copy;

          if (header_pos_ == 9)
          {
            process_header(); // sets current_header_ and calls header_cb_
            header_pos_ = 0;
            payload_bytes_read_ = 0;
            state_ = (current_header_.length == 0) ? state::reading_header : state::reading_payload;
          }
          break;
        }
        case state::reading_payload:
        {
          size_t to_consume = std::min(current_header_.length - payload_bytes_read_, buf.size() - offset);

          if (payload_cb_ && to_consume > 0)
          {
            payload_cb_(current_header_, buf.subspan(offset, to_consume));
          }

          offset += to_consume;
          payload_bytes_read_ += to_consume;

          if (payload_bytes_read_ == current_header_.length)
          {
            state_ = state::reading_header;
          }
          break;
        }
      }
    }
  }

  void frame_parser::process_header()
  {
    frame_header h;

    h.length =
      (std::to_integer<uint32_t>(header_buffer_[0]) << 16) |
      (std::to_integer<uint32_t>(header_buffer_[1]) <<  8) |
        std::to_integer<uint32_t>(header_buffer_[2]);
    h.type = static_cast<frame_type>(header_buffer_[3]);
    h.flags = header_buffer_[4];
    h.stream_id =
      (std::to_integer<uint32_t>(header_buffer_[5] & std::byte(0x7F)) << 24) |
      (std::to_integer<uint32_t>(header_buffer_[6]) << 16) |
      (std::to_integer<uint32_t>(header_buffer_[7]) <<  8) |
        std::to_integer<uint32_t>(header_buffer_[8]);
    h.reserved =
      std::to_integer<uint32_t>((header_buffer_[5] >> 7) & std::byte(0x01));

    current_header_ = h;

    if (header_cb_) {
      header_cb_(h);
    }
  }
}
