#include "http/v2/hpack.h"
#include "http/v2/hpack_static_tab.h"
#include "http/v2/hpack_huffman_tab.h"


namespace http::v2
{
  void hpack_buffer::encode(huffman_flag_tag_t tag)
  {
    push_back(std::byte(tag.value));
  }

  void hpack_buffer::encode(uint8_t value)
  {
    push_back(std::byte(value));
  }

  void hpack_buffer::encode(uint32_t val, uint8_t prefix_bits)
  {
    uint8_t mask = (1 << prefix_bits) - 1;
    if (val < mask)
    {
      back() |= std::byte(val);
    }
    else
    {
      back() |= std::byte(mask);
      val -= mask;
      while (val >= 128)
      {
        push_back(std::byte((val & 127) | 128));
        val >>= 7;
      }
      push_back(std::byte(val));
    }
  }

  void hpack_buffer::encode(std::string_view str, bool use_huffman)
  {
    if (use_huffman)
    {
      auto hlen = encode_huffman_len(str);
      encode(huffman_flag_tag);
      encode(static_cast<uint32_t>(hlen), 7);
      encode_huffman(str);
    }
    else
    {
      push_back(std::byte(0)); // Huffman flag (0)
      encode(static_cast<uint32_t>(str.size()), 7);
      insert(end(), reinterpret_cast<const std::byte*>(str.begin()), reinterpret_cast<const std::byte*>(str.end()));
    }
  }

  size_t hpack_buffer::encode_huffman_len(std::string_view str)
  {
    size_t bits = 0;
    for (uint8_t c : str)
    {
      bits += hpack_huffman_table[c].len;
    }
    // Round up to the nearest byte
    return (bits + 7) / 8;
  }

  void hpack_buffer::encode_huffman(std::string_view str)
  {
    uint64_t bit_buf = 0;
    int bit_count = 0;

    for (uint8_t c : str)
    {
      const auto& entry = hpack_huffman_table[c];
      bit_buf = (bit_buf << entry.len) | entry.code;
      bit_count += entry.len;

      while (bit_count >= 8)
      {
        push_back(static_cast<std::byte>(bit_buf >> (bit_count - 8)));
        bit_count -= 8;
      }
    }

    if (bit_count > 0)
    {
      // Padding with EOS (all ones)
      const auto& eos = hpack_huffman_table[256];
      bit_buf = (bit_buf << (8 - bit_count)) | (eos.code >> (eos.len - (8 - bit_count)));
      push_back(static_cast<std::byte>(bit_buf));
    }
  }

  void hpack_buffer::encode_header_indexed(uint32_t index)
  {
    encode(static_cast<uint8_t>(0x80));
    encode(index, 7);
  }

  void hpack_buffer::encode_header_literal_inc_index_indexed_name(uint32_t index, std::string_view value, bool huffman)
  {
    encode(static_cast<uint8_t>(0x40));
    encode(index, 6);
    encode(value, huffman);
  }

  void hpack_buffer::encode_header_literal_inc_index_new_name(std::string_view name, std::string_view value, bool name_huffman, bool value_huffman)
  {
    encode(static_cast<uint8_t>(0x40));
    encode(0, 6);
    encode(name, name_huffman);
    encode(value, value_huffman);
  }

  void hpack_buffer::encode_header_literal_no_index_indexed_name(uint32_t index, std::string_view value, bool huffman)
  {
    encode(static_cast<uint8_t>(0x00));
    encode(index, 4);
    encode(value, huffman);
  }

  void hpack_buffer::encode_header_literal_never_index_indexed_name(uint32_t index, std::string_view value, bool huffman)
  {
    encode(static_cast<uint8_t>(0x10));
    encode(index, 4);
    encode(value, huffman);
  }

  void hpack_buffer::encode_table_size_update(uint32_t size)
  {
    encode(static_cast<uint8_t>(0x20));
    encode(size, 5);
  }

  uint32_t hpack_decode_int(std::span<const std::byte> span, size_t& offset, uint8_t prefix_bits, uint32_t max_val)
  {
    if (offset >= span.size()) {
      throw std::out_of_range("HPACK decode_int: EOF");
    }

    unsigned mask = (1 << prefix_bits) - 1;
    unsigned val = static_cast<unsigned>(span[offset++] & std::byte(mask));

    if (val < mask) {
      return val;
    }

    unsigned m = 0;

    while (offset < span.size())
    {
      unsigned b = static_cast<unsigned>(span[offset++]);
      unsigned chunk = (b & 127);

      // Check for potential bit-shift overflow (32-bit limit)
      if (m >= 31 || (m == 28 && chunk > 7))
      {
        throw std::runtime_error("HPACK decode_int: Integer overflow");
      }

      unsigned add = chunk << m;

      // Check if val + add would exceed max_val
      if (add > max_val || val > max_val - add)
      {
        throw std::runtime_error("HPACK decode_int: Integer overflow/limit exceeded");
      }

      val += add;
      m += 7;

      if ((b & 128) == 0)
        return val;
    }

    throw std::runtime_error("HPACK decode_int: Incomplete integer");
  }

  std::string hpack_decode_huffman(std::span<const std::byte> span, size_t& offset)
  {
    std::string result;
    result.reserve(span.size() + (span.size() >> 1));

    const auto& tree = hpack_huffman_tree();
    const huffman_node* const base_ptr = tree.data();
    const huffman_node* node = base_ptr;

    for (const auto byte : span)
    {
      for (int bit = 7; bit >= 0; --bit)
      {
        auto bval = ((byte >> bit) & std::byte(1)) == std::byte(1);

        int16_t next_idx = bval ? node->right : node->left;

        if (next_idx == -1) [[unlikely]]
        {
          throw std::runtime_error("HPACK decode_huffman: Invalid code");
        }

        node = base_ptr + next_idx;

        if (node->is_leaf())
        {
          if (node->sym == 256) [[unlikely]]
          {
            throw std::runtime_error("HPACK decode_huffman: EOS symbol in string");
          }
          result.push_back(static_cast<char>(node->sym));
          node = base_ptr;
        }
      }
    }

    // RFC 7541: Check if the trailing bits are a valid prefix of EOS
    if (node != base_ptr)
    {
      const huffman_node* temp = node;

      while (!temp->is_leaf())
      {
        // If there's no path for a '1' bit, it can't be EOS padding
        if (temp->right == -1)
        {
          throw std::runtime_error("HPACK decode_huffman: Invalid padding");
        }
        temp = base_ptr + temp->right;
      }

      if (temp->sym != 256)
      {
        throw std::runtime_error("HPACK decode_huffman: Invalid padding (must be all 1s)");
      }
    }

    offset += span.size();
    return result;
  }

  std::string hpack_decode_string(std::span<const std::byte> span, size_t& offset, uint32_t max_len)
  {
    if (offset >= span.size())
      throw std::out_of_range("HPACK decode_string: EOF");

    bool huffman = (span[offset] & std::byte(huffman_flag_tag.value)) != std::byte(0);

    size_t str_len = hpack_decode_int(span, offset, 7, max_len);

    if (str_len > max_len) {
      throw std::runtime_error("HPACK decode_string: String length limit exceeded");
    }

    if (offset + str_len > span.size()) {
      throw std::out_of_range("HPACK decode_string: Incomplete string payload");
    }

    if (huffman) {
      //return hpack_decode_huffman(std::span<const uint8_t>(span.data() + offset, str_len), offset);
      return hpack_decode_huffman(span.subspan(offset, str_len), offset);
    }

    std::string s(reinterpret_cast<const char*>(span.data() + offset), str_len);
    offset += str_len;
    return s;
  }
}
