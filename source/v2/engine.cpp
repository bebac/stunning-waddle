#include "http/v2/engine.h"
#include "http/error_codes.h"
#include <iostream>

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
    if ( pending_out_.size() > connection_send_window())
    {
      // TODO - What should we do if we have more outgoing data than connection send window allows?
    }
    return std::span(reinterpret_cast<const std::byte*>(pending_out_.data()), pending_out_.size());
  }

  void engine::output_end(size_t n)
  {
    size_t to_erase = std::min(n, pending_out_.size());
    pending_out_.erase(pending_out_.begin(), pending_out_.begin() + to_erase);
  }

  uint32_t engine::open_stream()
  {
    if (goaway_received_)
    {
      // TODO - Throw, return "invalid" stream id (maybe max uint32_t?) or
      // perhpas change to return an std::optional.
    }

    auto id = next_stream_id_;

    if (role_ == connection_role::client && state_ == state::idle)
    {
      write_preface();
      write_settings();
      state_ = state::preface_sent;
    }

    known_streams_.insert(id);
    stream_send_window_[id] = static_cast<int64_t>(peer_initial_window_size_);
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

    // Track outgoing flow-control consumption. NOTE: we intentionally do not
    // gate/queue here yet — send_data still emits the whole body. Honoring the
    // window before sending needs async coordination with the I/O pump and is a
    // follow-up. Keeping the counters accurate is the prerequisite for that.
    if (!data.empty())
    {
      auto consumed = static_cast<int64_t>(data.size());
      connection_send_window_ -= consumed;
      get_or_init_stream_send_window(stream_id) -= consumed;
    }
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
        update_flow_control(h.stream_id, static_cast<uint32_t>(len));
        if ((h.flags & std::byte{0x01}) != std::byte{0})
        {
          stream_consumed_.erase(h.stream_id);
          stream_send_window_.erase(h.stream_id);
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
        //std::cerr << "[http] frame_type::priority unhandled" << std::endl;
        break;
      case frame_type::rst_stream:
        if (reset_cb_) {
          reset_cb_(h.stream_id, std::make_error_code(std::errc::protocol_error));
        }
        break;
      case frame_type::settings:
        // Reassemble the payload (it may arrive in chunks), then apply the
        // settings we track, e.g. SETTINGS_INITIAL_WINDOW_SIZE for send windows.
        control_payload_.insert(
          control_payload_.end(),
          reinterpret_cast<const std::byte*>(data),
          reinterpret_cast<const std::byte*>(data) + len);
        if (control_payload_.size() >= h.length) {
          handle_settings_payload(control_payload_);
          control_payload_.clear();
        }
        break;
      case frame_type::push_promise:
        //std::cerr << "[http] frame_type::push_promise unhandled" << std::endl;
        break;
      case frame_type::ping:
        //std::cerr << "[http] frame_type::ping unhandled" << std::endl;
        break;
      case frame_type::goaway:
        // Reassemble payload (minimum 8 bytes: last_stream_id + error_code)
        control_payload_.insert(
          control_payload_.end(),
          reinterpret_cast<const std::byte*>(data),
          reinterpret_cast<const std::byte*>(data) + len);

        if (control_payload_.size() >= h.length) {
          handle_goaway_payload(control_payload_, h.length);
          control_payload_.clear();
        }
        break;
      case frame_type::window_update:
        // Reassemble the 4-byte payload (it may be split across reads) and add
        // the increment to the matching outgoing window (stream 0 = connection).
        control_payload_.insert(
          control_payload_.end(),
          reinterpret_cast<const std::byte*>(data),
          reinterpret_cast<const std::byte*>(data) + len);
        if (control_payload_.size() >= h.length) {
          handle_window_update(h.stream_id, control_payload_);
          control_payload_.clear();
        }
        break;
      case frame_type::continuation:
        //std::cerr << "[http] fframe_type::continuation unhandled" << std::endl;
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

  void engine::update_flow_control(uint32_t stream_id, uint32_t bytes_received)
  {
    // Accumulate consumed bytes for both connection and stream.
    connection_consumed_ += bytes_received;
    stream_consumed_[stream_id] += bytes_received;

    // Send connection-level WINDOW_UPDATE when half the window is consumed.
    uint32_t threshold = initial_window_size_ / 2;

    if (connection_consumed_ >= threshold) {
      encode_window_update_frame(pending_out_, 0, connection_consumed_);
      connection_consumed_ = 0;
    }

    // Send stream-level WINDOW_UPDATE.
    if (stream_consumed_[stream_id] >= threshold) {
      encode_window_update_frame(pending_out_, stream_id, stream_consumed_[stream_id]);
      stream_consumed_[stream_id] = 0;
    }
  }

  int64_t& engine::get_or_init_stream_send_window(uint32_t stream_id)
  {
    auto it = stream_send_window_.find(stream_id);
    if (it == stream_send_window_.end()) {
      it = stream_send_window_.emplace(
        stream_id, static_cast<int64_t>(peer_initial_window_size_)).first;
    }
    return it->second;
  }

  int64_t engine::stream_send_window(uint32_t stream_id) const
  {
    auto it = stream_send_window_.find(stream_id);
    if (it == stream_send_window_.end()) {
      return static_cast<int64_t>(peer_initial_window_size_);
    }
    return it->second;
  }

  void engine::handle_window_update(uint32_t stream_id, std::span<const std::byte> payload)
  {
    if (payload.size() < 4) {
      // Malformed WINDOW_UPDATE (should be FRAME_SIZE_ERROR); ignore for now.
      return;
    }

    // 31-bit increment; the most-significant bit is reserved.
    uint32_t increment =
      (std::to_integer<uint32_t>(payload[0] & std::byte(0x7F)) << 24) |
      (std::to_integer<uint32_t>(payload[1]) << 16) |
      (std::to_integer<uint32_t>(payload[2]) <<  8) |
       std::to_integer<uint32_t>(payload[3]);

    if (increment == 0) {
      // A zero increment is a PROTOCOL_ERROR per RFC 7540 6.9.
      // TODO: surface as a connection/stream error once GOAWAY/RST exists.
      return;
    }

    if (stream_id == 0) {
      connection_send_window_ += static_cast<int64_t>(increment);
    }
    else {
      get_or_init_stream_send_window(stream_id) += static_cast<int64_t>(increment);
    }
  }

  void engine::handle_settings_payload(std::span<const std::byte> payload)
  {
    // SETTINGS payload is a sequence of 6-byte entries: u16 identifier, u32 value.
    for (size_t i = 0; i + 6 <= payload.size(); i += 6) {
      uint16_t id =
        static_cast<uint16_t>(std::to_integer<uint16_t>(payload[i]) << 8) |
         std::to_integer<uint16_t>(payload[i + 1]);
      uint32_t value =
        (std::to_integer<uint32_t>(payload[i + 2]) << 24) |
        (std::to_integer<uint32_t>(payload[i + 3]) << 16) |
        (std::to_integer<uint32_t>(payload[i + 4]) <<  8) |
         std::to_integer<uint32_t>(payload[i + 5]);

      if (static_cast<settings_id>(id) == settings_id::initial_window_size) {
        apply_peer_initial_window_size(value);
      }
    }
  }

  void engine::handle_goaway_payload(std::span<const std::byte> payload, uint32_t length)
  {
    // RFC 7540 6.8: GOAWAY payload is last_stream_id (4 bytes) + error_code (4 bytes) + debug_data
    if (length < 8) {
      // FRAME_SIZE_ERROR - but we can't send GOAWAY for this since we're processing GOAWAY
      return;
    }

    // Extract last_stream_id (31-bit, MSB must be 0)
    uint32_t last_stream_id =
      (std::to_integer<uint32_t>(payload[0]) << 24) |
      (std::to_integer<uint32_t>(payload[1]) << 16) |
      (std::to_integer<uint32_t>(payload[2]) << 8) |
      std::to_integer<uint32_t>(payload[3]);

    // Ensure MSB is 0 (31-bit value)
    if ((last_stream_id & 0x80000000) != 0) {
      // Invalid - treat as connection error
      return;
    }

    // Extract error_code (32-bit) from wire format
    uint32_t wire_error_code =
      (std::to_integer<uint32_t>(payload[4]) << 24) |
      (std::to_integer<uint32_t>(payload[5]) << 16) |
      (std::to_integer<uint32_t>(payload[6]) << 8) |
      std::to_integer<uint32_t>(payload[7]);

    // Convert to unified error_code
    http::error_code error_code = static_cast<http::error_code>(wire_error_code);

    // Extract debug data (if any)
    std::vector<std::byte> debug_data;
    if (length > 8) {
      debug_data.assign(payload.begin() + 8, payload.begin() + length);
    }

    // Update state
    goaway_received_ = true;
    last_goaway_stream_id_ = last_stream_id;
    last_goaway_error_code_ = error_code;

    // Invoke callback
    if (goaway_cb_) {
      goaway_cb_(last_stream_id, error_code);
    }
  }

  void engine::send_goaway(uint32_t last_stream_id, http::error_code error_code, std::span<const std::byte> debug_data)
  {
    encode_goaway_frame(pending_out_, last_stream_id, error_code, debug_data);
  }

  void engine::apply_peer_initial_window_size(uint32_t new_size)
  {
    // RFC 7540 6.9.2: a change to SETTINGS_INITIAL_WINDOW_SIZE retroactively
    // adjusts the send window of every existing stream by the delta. The
    // connection-level window is unaffected.
    int64_t delta = static_cast<int64_t>(new_size) -
                    static_cast<int64_t>(peer_initial_window_size_);

    for (auto& entry : stream_send_window_) {
      entry.second += delta;
    }

    peer_initial_window_size_ = new_size;
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
