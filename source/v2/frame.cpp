#include "http/v2/frame.h"

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
}
