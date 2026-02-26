#include "http/v2/engine.h"

namespace http::v2
{
  namespace
  {
    constexpr std::string_view preface{"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"};
  }

  engine::engine(connection_role role)
    : role_(role)
  {
    // Server uses even stream IDs (for push), client uses odd
    next_stream_id_ = (role == connection_role::server) ? 2 : 1;

    // Server starts waiting for client preface
    if (role == connection_role::server) {
      state_ = state::awaiting_preface;
    }

    parser_.on_frame_header(
      [this](frame_header h) {
        this->handle_frame_header(h);
      }
    );

    parser_.on_payload_chunk(
      [this](frame_header h, std::span<const std::byte> buf) {
        this->handle_payload_chunk(h, reinterpret_cast<const uint8_t*>(buf.data()), buf.size());
      }
    );
  }

  std::span<std::byte> engine::input_begin()
  {
    if (state_ == state::awaiting_preface) {
      // Server mode: buffer for preface validation
      return std::span(input_buffer_.data() + input_buffered_,
                       input_buffer_.size() - input_buffered_);
    }
    return std::span(input_buffer_.data(), input_buffer_.size());
  }

  void engine::input_end(size_t n)
  {
    if (n == 0) return;

    if (state_ == state::awaiting_preface) {
      input_buffered_ += n;
      if (input_buffered_ >= preface.size() && try_consume_client_preface()) {
        // Preface validated, send our SETTINGS
        write_settings();
        state_ = state::ready;
        // Process any remaining data after preface (offset past preface)
        size_t remaining = input_buffered_ - preface.size();
        if (remaining > 0) {
          parser_.consume(std::span<const std::byte>(
            input_buffer_.data() + preface.size(), remaining));
        }
        input_buffered_ = 0;
      }
      return;
    }

    parser_.consume(std::span<const std::byte>(input_buffer_.data(), n));
  }

  std::span<const std::byte> engine::output_begin()
  {
    return std::span(reinterpret_cast<const std::byte*>(pending_out_.data()), pending_out_.size());
  }

  void engine::output_end(size_t n)
  {
    size_t to_erase = std::min(n, pending_out_.size());
    pending_out_.erase(pending_out_.begin(), pending_out_.begin() + to_erase);
  }

  uint32_t engine::open_stream()
  {
    auto id = next_stream_id_;

    if (role_ == connection_role::client && state_ == state::idle)
    {
      write_preface();
      write_settings();
      state_ = state::preface_sent;
    }

    known_streams_.insert(id);
    next_stream_id_ += 2;
    return id;
  }

  void engine::send_request_headers(
    uint32_t stream_id,
    std::string_view method,
    std::string_view path,
    std::string_view authority,
    const headers& headers,
    bool end_stream
  )
  {
    std::vector<http::header> header_list = {
      {":method", std::string(method)},
      {":scheme", "https"},
      {":authority", std::string(authority)},
      {":path", std::string(path)}};

    for (const auto& h : headers.get())
    {
      header_list.push_back(h);
    }

    send_headers_frame(stream_id, header_list, end_stream);
  }

  void engine::send_response_headers(
    uint32_t stream_id,
    int status_code,
    const headers& headers,
    bool end_stream
  )
  {
    std::vector<http::header> header_list = {
      {":status", std::to_string(status_code)}};

    for (const auto& h : headers.get())
    {
      header_list.push_back(h);
    }

    send_headers_frame(stream_id, header_list, end_stream);
  }

  void engine::send_headers_frame(
    uint32_t stream_id,
    const std::vector<http::header>& header_list,
    bool end_stream
  )
  {
    hpack_buffer header_block;
    encode_ctx_.encode(header_block, header_list);

    // TODO: Handle splitting header block into CONTINUATION frames if needed
    encode_headers_frame(pending_out_, stream_id, header_block, true, end_stream);
  }

  void engine::send_data(uint32_t stream_id, std::span<const std::byte> data, bool end_stream)
  {
    const size_t max_size = 16384;

    size_t offset = 0;
    do
    {
      size_t chunk_size = std::min(data.size() - offset, max_size);

      offset += chunk_size;

      bool is_last_chunk = (offset == data.size());
      bool final_flag = is_last_chunk && end_stream;

      encode_data_frame(pending_out_, stream_id, data.subspan(offset - chunk_size, chunk_size), final_flag);
    }
    while (offset < data.size());
  }

  void engine::handle_frame_header(frame_header h)
  {
    if (h.type == frame_type::settings)
    {
      bool is_ack = (h.flags & std::byte{0x01}) != std::byte{0};
      if (!is_ack)
      {
        // Automatically send SETTINGS ACK
        encode_settings_frame(pending_out_, {}, true);
      }
    }
    else
    {
      if ((h.flags & std::byte(0x01)) != std::byte{0} && h.length == 0)
      {
        if (closed_cb_) {
          closed_cb_(h.stream_id);
        }
      }
    }
  }

  void engine::handle_payload_chunk(frame_header h, const uint8_t* data, size_t len)
  {
    switch (h.type)
    {
      case frame_type::data:
        if (data_cb_) {
          data_cb_(h.stream_id, data, len);
        }
        if ((h.flags & std::byte{0x01}) != std::byte{0})
        {
          if (closed_cb_) {
            closed_cb_(h.stream_id);
          }
        }
        break;
      case frame_type::headers:
        // Server mode: check if this is a new stream from client
        if (role_ == connection_role::server && known_streams_.find(h.stream_id) == known_streams_.end())
        {
          known_streams_.insert(h.stream_id);
          if (new_stream_cb_) {
            new_stream_cb_(h.stream_id);
          }
        }
        if ((h.flags & std::byte(0x04)) != std::byte{0}) // END_HEADERS
        {
          auto decoded = decode_ctx_.decode(data, len);
          if (headers_cb_) {
            headers_cb_(h.stream_id, http::headers(decoded));
          }
        }
        if ((h.flags & std::byte(0x01)) != std::byte{0})
        {
          if (closed_cb_) {
            closed_cb_(h.stream_id);
          }
        }
        break;
      case frame_type::priority:
        break;
      case frame_type::rst_stream:
        if (reset_cb_) {
          reset_cb_(h.stream_id, std::make_error_code(std::errc::protocol_error));
        }
        break;
      case frame_type::settings:
        break;
      case frame_type::push_promise:
        break;
      case frame_type::ping:
        break;
      case frame_type::goaway:
        break;
      case frame_type::window_update:
        break;
      case frame_type::continuation:
        break;
    }
  }

  void engine::write_preface()
  {
    // 1. Queue the connection preface
    pending_out_.insert(
      pending_out_.end(),
      reinterpret_cast<const std::byte*>(preface.data()),
      reinterpret_cast<const std::byte*>(preface.data()) + preface.size()
    );
  }

  void engine::write_settings()
  {
    // Queue initial empty SETTINGS frame
    encode_settings_frame(pending_out_, {}, false);
  }

  bool engine::try_consume_client_preface()
  {
    // Validate preface (caller already checked we have enough bytes)
    if (std::memcmp(input_buffer_.data(), preface.data(), preface.size()) != 0) {
      // Invalid preface - this is a connection error
      // TODO: Send GOAWAY and close connection
      return false;
    }
    return true;
  }
}
