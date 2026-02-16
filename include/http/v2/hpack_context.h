#ifndef INCLUDE_HTTP_V2_HPACK_CONTEXT_H
#define INCLUDE_HTTP_V2_HPACK_CONTEXT_H

#include "http/v2/hpack.h"
#include "http/headers.h"

#include <cstdint>
#include <deque>

namespace http::v2
{
  class hpack_context
  {
  public:
    hpack_context(uint32_t max_table_size = 4096);

  public:
    void set_max_table_size(uint32_t new_max);
    void add_to_dynamic_table(const header& h);

    std::vector<header> decode(const uint8_t* data, size_t len);
    void encode(hpack_buffer& buf, const std::vector<header>& headers, bool use_huffman = true);

    const header& get_header(size_t index) const;

    std::size_t dynamic_table_size() const;
    std::size_t current_size() const;

  private:
    int find_exact_match(const header& h) const;
    int find_name_match(std::string_view name) const;

    void evict_until_size(uint32_t limit);

  private:
    std::deque<header> dynamic_table_;
    uint32_t max_table_size_;
    uint32_t current_table_size_;
  };
}

#endif // INCLUDE_HTTP_V2_HPACK_CONTEXT_H
