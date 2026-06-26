#include "http/v2/frame.h"
#include "http/error_codes.h"

namespace http::v2
{
  void write_frame_header(std::vector<std::byte>& dst, const frame_header& hdr)
  {
    // Length (24 bits)
    dst.push_back(static_cast<std::byte>((hdr.length >> 16) & 0xFF));
    dst.push_back(static_cast<std::byte>((hdr.length >> 8) & 0xFF));
    dst.push_back(static_cast<std::byte>(hdr.length & 0xFF));

    // Type & Flags
    dst.push_back(static_cast<std::byte>(hdr.type));
    dst.push_back(hdr.flags);

    // Stream ID (31 bits)
    uint32_t id = hdr.stream_id & 0x7FFFFFFF;
    dst.push_back(static_cast<std::byte>((id >> 24) & 0xFF));
    dst.push_back(static_cast<std::byte>((id >> 16) & 0xFF));
    dst.push_back(static_cast<std::byte>((id >> 8) & 0xFF));
    dst.push_back(static_cast<std::byte>(id & 0xFF));
  }

  void encode_data_frame(
    std::vector<std::byte>& dst,
    uint32_t stream_id,
    std::span<const std::byte> payload,
    bool end_stream
  )
  {
    frame_header hdr{
      .length = static_cast<uint32_t>(payload.size()),
      .type = frame_type::data,
      .flags = static_cast<std::byte>(end_stream ? 0x01 : 0x00),
      .stream_id = stream_id,
      .reserved = 0
    };

    write_frame_header(dst, hdr);

    if (!payload.empty())
    {
      dst.insert(dst.end(), payload.begin(), payload.end());
    }
  }

  void encode_headers_frame(
    std::vector<std::byte>& dst,
    uint32_t stream_id,
    std::span<const std::byte> header_block,
    bool end_headers,
    bool end_stream
  )
  {
    std::byte flags = std::byte{0x00};
    if (end_stream)
      flags |= std::byte{0x01};
    if (end_headers)
      flags |= std::byte{0x04};

    frame_header hdr{
      .length = static_cast<uint32_t>(header_block.size()),
      .type = frame_type::headers,
      .flags = flags,
      .stream_id = stream_id,
      .reserved = 0
    };

    write_frame_header(dst, hdr);

    if (!header_block.empty())
    {
      dst.insert(dst.end(), header_block.begin(), header_block.end());
    }
  }

  void encode_settings_frame(
    std::vector<std::byte>& dst,
    const std::vector<setting>& settings,
    bool ack
  )
  {
    frame_header hdr{
      .length = static_cast<uint32_t>(settings.size() * 6),
      .type = frame_type::settings,
      .flags = ack ? std::byte{0x01} : std::byte{0x00},
      .stream_id = 0,
      .reserved = 0
    };

    write_frame_header(dst, hdr);

    for (const auto& s : settings)
    {
      uint16_t id = static_cast<uint16_t>(s.id);
      dst.push_back(std::byte((id >> 8) & 0xFF));
      dst.push_back(std::byte(id & 0xFF));
      dst.push_back(std::byte((s.value >> 24) & 0xFF));
      dst.push_back(std::byte((s.value >> 16) & 0xFF));
      dst.push_back(std::byte((s.value >> 8) & 0xFF));
      dst.push_back(std::byte(s.value & 0xFF));
    }
  }

  void encode_window_update_frame(
    std::vector<std::byte>& dst,
    uint32_t stream_id,
    uint32_t increment
  )
  {
    frame_header hdr{
      .length = 4,
      .type = frame_type::window_update,
      .flags = std::byte{0x00},
      .stream_id = stream_id,
      .reserved = 0
    };

    write_frame_header(dst, hdr);

    // Payload: 31-bit window size increment (MSB reserved, must be 0).
    uint32_t val = increment & 0x7FFFFFFF;
    dst.push_back(static_cast<std::byte>((val >> 24) & 0xFF));
    dst.push_back(static_cast<std::byte>((val >> 16) & 0xFF));
    dst.push_back(static_cast<std::byte>((val >> 8) & 0xFF));
    dst.push_back(static_cast<std::byte>(val & 0xFF));
  }

  void encode_goaway_frame(
    std::vector<std::byte>& dst,
    uint32_t last_stream_id,
    http::error_code error_code,
    std::span<const std::byte> debug_data
  )
  {
    // Minimum payload: 8 bytes (last_stream_id + error_code)
    size_t payload_size = 8 + debug_data.size();

    frame_header hdr{
      .length = static_cast<uint32_t>(payload_size),
      .type = frame_type::goaway,
      .flags = std::byte{0x00},
      .stream_id = 0,  // MUST be 0 for GOAWAY
      .reserved = 0
    };

    write_frame_header(dst, hdr);

    // Last Stream ID (31-bit, MSB must be 0)
    uint32_t stream_id_val = last_stream_id & 0x7FFFFFFF;
    dst.push_back(static_cast<std::byte>((stream_id_val >> 24) & 0xFF));
    dst.push_back(static_cast<std::byte>((stream_id_val >> 16) & 0xFF));
    dst.push_back(static_cast<std::byte>((stream_id_val >> 8) & 0xFF));
    dst.push_back(static_cast<std::byte>(stream_id_val & 0xFF));

    // Error Code (32-bit) - convert unified error_code to wire format
    uint32_t wire_error_code = static_cast<uint32_t>(error_code);
    dst.push_back(static_cast<std::byte>((wire_error_code >> 24) & 0xFF));
    dst.push_back(static_cast<std::byte>((wire_error_code >> 16) & 0xFF));
    dst.push_back(static_cast<std::byte>((wire_error_code >> 8) & 0xFF));
    dst.push_back(static_cast<std::byte>(wire_error_code & 0xFF));

    // Debug data (optional)
    if (!debug_data.empty()) {
      dst.insert(dst.end(), debug_data.begin(), debug_data.end());
    }
  }
}
