#ifndef INCLUDE_HTTP_V2_HPACK_HUFFMAN_H
#define INCLUDE_HTTP_V2_HPACK_HUFFMAN_H

#include <cstdint>
#include <vector>

namespace http::v2
{
  struct huffman_flag_tag_t {
    static constexpr unsigned value = 0x80;
  };
  inline constexpr huffman_flag_tag_t huffman_flag_tag{};

  struct alignas(8) huffman_node
  {
    int16_t left = -1;  // -1 if no child, otherwise index in node pool
    int16_t right = -1; // -1 if no child, otherwise index in node pool
    int16_t sym = -1; // -1 if internal node, 0-256 if leaf

    bool is_leaf() const { return sym != -1; }
  };

  const std::vector<huffman_node>& hpack_huffman_tree();
}

#endif // INCLUDE_HTTP_V2_HPACK_HUFFMAN_H