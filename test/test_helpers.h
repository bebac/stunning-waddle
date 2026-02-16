#ifndef TEST_HELPERS_H
#define TEST_HELPERS_H

#include "http/protocol_engine.h"

#include <vector>
#include <span>
#include <ranges>
#include <algorithm>
#include <iostream>
#include <cstring>

template<typename... Ts>
auto make_bytes(Ts... args) {
    return std::vector<std::byte>{std::byte(static_cast<unsigned char>(args))...};
}

namespace mock
{
  template <typename T>
  concept SansIOEngine = requires(T t, size_t n) {
    { t.input_begin() } -> std::same_as<std::span<std::byte>>;
    { t.input_end(n) } -> std::same_as<void>;
    { t.output_begin() } -> std::same_as<std::span<const std::byte>>;
    { t.output_end(n) } -> std::same_as<void>;
  };

  template<SansIOEngine T>
  void recv(T& target, std::span<const std::byte> data)
  {
    while (!data.empty())
    {
      auto in_buffer = target.input_begin();

      if (in_buffer.empty()) {
        break;
      }

      const size_t bytes_to_copy = std::min(in_buffer.size(), data.size());
      std::ranges::copy(data.first(bytes_to_copy), in_buffer.begin());

      target.input_end(bytes_to_copy);
      data = data.subspan(bytes_to_copy);
    }
  }

  struct frame_info {
    uint32_t length;
    uint8_t type;
    uint8_t flags;
    uint32_t stream_id;
    std::vector<std::byte> payload;
  };

  template<SansIOEngine T>
  std::optional<frame_info> capture_frame(T& target)
  {
    auto out = target.output_begin();

    //std::cout << "capture frame len=" << out.size() << std::endl;

    // HTTP/2 frame header is exactly 9 bytes
    if (out.size() < 9)
      return std::nullopt;

    frame_info frame;

    // 1. Parse Length (24-bit integer)
    frame.length =
      (static_cast<uint32_t>(out[0]) << 16) |
      (static_cast<uint32_t>(out[1]) << 8) |
      static_cast<uint32_t>(out[2]);

    // 2. Parse Type and Flags
    frame.type = static_cast<uint8_t>(out[3]);
    frame.flags = static_cast<uint8_t>(out[4]);

    // 3. Parse Stream Identifier (31-bit, mask out the reserved bit)
    frame.stream_id =
      (static_cast<uint32_t>(out[5] & std::byte(0x7f)) << 24) |
      (static_cast<uint32_t>(out[6]) << 16) |
      (static_cast<uint32_t>(out[7]) << 8) |
      static_cast<uint32_t>(out[8]);

    // Ensure the full payload is available in the current span
    if (out.size() < 9 + frame.length)
      return std::nullopt;

    // 4. Extract Payload
    frame.payload.assign(out.begin() + 9, out.begin() + 9 + frame.length);

    // 5. Consume the data so the next call gets the next frame
    target.output_end(9 + frame.length);

    return frame;
  }

  template<SansIOEngine T>
  inline void skip_preface(T& target)
  {
    auto out = target.output_begin();
    std::string_view preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

    if (out.size() >= preface.size())
    {
      // If the buffer starts with the preface, consume it
      if (std::memcmp(out.data(), preface.data(), preface.size()) == 0)
      {
        target.output_end(preface.size());
      }
    }
  }
} // namespace mock

#endif // TEST_HELPERS_H
