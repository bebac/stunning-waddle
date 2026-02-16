#include "http/v2/hpack_context.h"
#include "http/v2/hpack_static_tab.h"

namespace http::v2
{
  hpack_context::hpack_context(uint32_t max_table_size)
    : max_table_size_(max_table_size), current_table_size_(0)
  {
  }

  void hpack_context::set_max_table_size(uint32_t new_max)
  {
    max_table_size_ = new_max;
    evict_until_size(max_table_size_);
  }

  void hpack_context::add_to_dynamic_table(const header& h)
  {
    uint32_t entry_size = static_cast<uint32_t>(h.name.size() + h.value.size() + 32);
    if (entry_size > max_table_size_)
    {
      dynamic_table_.clear();
      current_table_size_ = 0;
      return;
    }

    evict_until_size(max_table_size_ - entry_size);
    dynamic_table_.push_front(h);
    current_table_size_ += entry_size;
  }

  std::vector<header> hpack_context::decode(const uint8_t* data, size_t len)
  {
    std::vector<header> headers;
    size_t offset = 0;
    auto span = std::span<const std::byte>(reinterpret_cast<const std::byte*>(data), len);

    while (offset < len)
    {
      uint8_t b = data[offset];
      if (b & 0x80)
      {
        // Indexed Header Field
        uint32_t index = hpack_decode_int(span, offset, 7);
        headers.push_back(get_header(index));
      }
      else if (b & 0x40)
      {
        // Literal Header Field with Incremental Indexing
        uint32_t index = hpack_decode_int(span, offset, 6);
        header h;
        if (index == 0)
        {
          h.name = hpack_decode_string(span, offset);
        }
        else
        {
          h.name = get_header(index).name;
        }
        h.value = hpack_decode_string(span, offset);
        add_to_dynamic_table(h);
        headers.push_back(h);
      }
      else if (b & 0x20)
      {
        // Dynamic Table Size Update
        uint32_t new_size = hpack_decode_int(span, offset, 5);
        set_max_table_size(new_size);
      }
      else
      {
        // Literal Header Field without Indexing or Never Indexed
        uint32_t index = hpack_decode_int(span, offset, 4);
        header h;
        if (index == 0)
        {
          h.name = hpack_decode_string(span, offset);
        }
        else
        {
          h.name = get_header(index).name;
        }
        h.value = hpack_decode_string(span, offset);
        headers.push_back(h);
      }
    }

    return headers;
  }

  void hpack_context::encode(hpack_buffer& buf, const std::vector<header>& headers, bool use_huffman)
  {
    for (const auto& h : headers)
    {
      // 1. Search for exact match
      int exact_match_idx = find_exact_match(h);
      if (exact_match_idx != -1)
      {
        buf.encode_header_indexed(static_cast<uint32_t>(exact_match_idx));
        continue;
      }

      // 2. Search for name-only match
      int name_match_idx = find_name_match(h.name);
      if (name_match_idx != -1)
      {
        buf.encode_header_literal_inc_index_indexed_name(static_cast<uint32_t>(name_match_idx), h.value, use_huffman);
        add_to_dynamic_table(h);
      }
      else
      {
        buf.encode_header_literal_inc_index_new_name(h.name, h.value, use_huffman, use_huffman);
        add_to_dynamic_table(h);
      }
    }
  }

  const header& hpack_context::get_header(size_t index) const
  {
    if (hpack_is_predefined(index))
    {
      return hpack_lookup_predefined(index);
    }
    else
    {
      size_t dynamic_index = index - 62;
      if (dynamic_index >= dynamic_table_.size())
      {
        throw std::out_of_range("HPACK dynamic table index out of range");
      }
      return dynamic_table_[dynamic_index];
    }
  }

  std::size_t hpack_context::dynamic_table_size() const
  {
    return dynamic_table_.size();
  }

  std::size_t hpack_context::current_size() const
  {
    return current_table_size_;
  }

  int hpack_context::find_exact_match(const header& h) const
  {
    // Static table
    const auto& tab = hpack_static_table();
    for (unsigned i = 0; i < hpack_static_table_size(); ++i)
    {
      const auto& sh = tab[i];
      if (sh.name == h.name && sh.value == h.value)
        return i+1;
    }
    // Dynamic table
    for (size_t i = 0; i < dynamic_table_.size(); ++i)
    {
      if (dynamic_table_[i].name == h.name && dynamic_table_[i].value == h.value)
        return static_cast<int>(i + 62);
    }
    return -1;
  }

  int hpack_context::find_name_match(std::string_view name) const
  {
    // Static table
    const auto& tab = hpack_static_table();
    for (unsigned i = 0; i < hpack_static_table_size(); ++i)
    {
      const auto& sh = tab[i];
      if (sh.name == name)
        return i+1;
    }
    // Dynamic table
    for (size_t i = 0; i < dynamic_table_.size(); ++i)
    {
      if (dynamic_table_[i].name == name)
        return static_cast<int>(i + 62);
    }
    return -1;
  }

  void hpack_context::evict_until_size(uint32_t limit)
  {
    while (current_table_size_ > limit && !dynamic_table_.empty())
    {
      const auto& oldest = dynamic_table_.back();
      current_table_size_ -= static_cast<uint32_t>(oldest.name.size() + oldest.value.size() + 32);
      dynamic_table_.pop_back();
    }
  }
}
