#include "http/v2/hpack_huffman.h"
#include "http/v2/hpack_huffman_tab.h"

namespace http::v2
{
  static std::vector<huffman_node> build_hpack_huffman_tree()
  {
    std::vector<huffman_node> tree;

    tree.reserve(513);   // 2 × 257 − 1 = 513 nodes
    tree.emplace_back(); // Root at index 0

    for (int i = 0; i < 257; ++i)
    {
      const auto& entry = hpack_huffman_table[i];
      size_t current_idx = 0;

      for (int bit = entry.len - 1; bit >= 0; --bit)
      {
        bool bit_val = (entry.code >> bit) & 1;

        // Get a pointer/ref, but be careful of vector reallocations
        int next_idx = bit_val ? tree[current_idx].right : tree[current_idx].left;

        if (next_idx == -1)
        {
          next_idx = static_cast<int16_t>(tree.size());
          tree.emplace_back();
          bit_val ? tree[current_idx].right = next_idx : tree[current_idx].left = next_idx;
        }
        current_idx = next_idx;
      }
      tree[current_idx].sym = static_cast<int16_t>(i);
    }
    return tree;
  }

  const std::vector<huffman_node>& hpack_huffman_tree()
  {
    static const std::vector<huffman_node> tree = build_hpack_huffman_tree();
    return tree;
  }
}
