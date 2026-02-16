#ifndef INCLUDE_HTTP_V2_HPACK_H
#define INCLUDE_HTTP_V2_HPACK_H

#include "http/headers.h"
#include "http/v2/hpack_huffman.h"
#include <vector>
#include <stdexcept>
#include <string>
#include <cstdint>
#include <span>

namespace http::v2
{
  struct hpack_limits
  {
    uint32_t max_int = 0xFFFFFFFF;
    uint32_t max_string_len = 65536; // 64KB default
  };

  class hpack_buffer : public std::vector<std::byte>
  {
  public:
    using std::vector<std::byte>::vector;
  public:
    void encode(huffman_flag_tag_t tag);
    void encode(uint8_t value);
    void encode(uint32_t val, uint8_t prefix_bits);
    void encode(std::string_view str, bool use_huffman = false);
  public:
    size_t encode_huffman_len(std::string_view str);
    void encode_huffman(std::string_view str);
    void encode_header_indexed(uint32_t index);
    void encode_header_literal_inc_index_indexed_name(uint32_t index, std::string_view value, bool huffman = false);
    void encode_header_literal_inc_index_new_name(std::string_view name, std::string_view value, bool name_huffman = false, bool value_huffman = false);
    void encode_header_literal_no_index_indexed_name(uint32_t index, std::string_view value, bool huffman = false);
#if 0 // unused?
    void encode_header_literal_no_index_new_name(std::string_view name, std::string_view value, bool name_huffman = false, bool value_huffman = false)
    {
      encode(static_cast<uint8_t>(0x00));
      encode(0, 4);
      encode(name, name_huffman);
      encode(value, value_huffman);
    }
#endif
    void encode_header_literal_never_index_indexed_name(uint32_t index, std::string_view value, bool huffman = false);
#if 0 // unused?
    void encode_header_literal_never_index_new_name(std::string_view name, std::string_view value, bool name_huffman = false, bool value_huffman = false)
    {
      encode(static_cast<uint8_t>(0x10));
      encode(0, 4);
      encode(name, name_huffman);
      encode(value, value_huffman);
    }
#endif
    void encode_table_size_update(uint32_t size);
  };

  uint32_t hpack_decode_int(std::span<const std::byte> span, size_t& offset, uint8_t prefix_bits, uint32_t max_val = 0xFFFFFFFF);

  std::string hpack_decode_huffman(std::span<const std::byte> span, size_t& offset);
  std::string hpack_decode_string(std::span<const std::byte> span, size_t& offset, uint32_t max_len = 65536);
} // namespace http::v2

#endif // INCLUDE_HTTP_V2_HPACK_H
