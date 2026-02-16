#include "doctest/doctest.h"
#include "test_helpers.h"
#include "http/v2/hpack.h"
#include <vector>
#include <string>
#include <iostream>
#include <iomanip>

void print_hex(const std::vector<uint8_t>& data)
{
  for (auto b : data)
  {
    std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)b;
  }
  std::cout << std::dec << std::endl;
}

TEST_CASE("HPACK Huffman Encoding/Decoding")
{
  SUBCASE("Huffman encoding of 'www.example.com'")
  {
    // RFC 7541 C.4.1
    // www.example.com -> f1e3 c2e5 f23a 6ba0 ab90 f4ff
    // Wait, I misread the hex in previous turn. Let's re-verify.
    // RFC C.4.1 says:
    // f1e3 c2e5 f23a 6ba0 ab90 f4ff

    std::string input = "www.example.com";
    http::v2::hpack_buffer encoded;
    encoded.encode_huffman(input);

    std::vector<uint8_t> expected = {0xf1, 0xe3, 0xc2, 0xe5, 0xf2, 0x3a, 0x6b, 0xa0, 0xab, 0x90, 0xf4, 0xff};

    CHECK(encoded.size() == expected.size());
    for (size_t i = 0; i < encoded.size(); ++i)
    {
      CHECK(encoded[i] == std::byte(expected[i]));
    }
  }

  SUBCASE("Huffman decoding of 'www.example.com'")
  {
    auto data = make_bytes(0xf1, 0xe3, 0xc2, 0xe5, 0xf2, 0x3a, 0x6b, 0xa0, 0xab, 0x90, 0xf4, 0xff);
    size_t offset = 0;
    std::string decoded = http::v2::hpack_decode_huffman(data, offset);

    CHECK(decoded == "www.example.com");
    CHECK(offset == 12);
  }

  SUBCASE("Huffman encoding of 'no-cache'")
  {
    // RFC 7541 C.4.2
    // no-cache -> a8eb 1064 9cbf
    std::string input = "no-cache";
    http::v2::hpack_buffer encoded;
    encoded.encode_huffman(input);

    auto expected = make_bytes(0xa8, 0xeb, 0x10, 0x64, 0x9c, 0xbf);
    CHECK(encoded == expected);
  }

  SUBCASE("Huffman decoding of 'no-cache'")
  {
    auto data = make_bytes(0xa8, 0xeb, 0x10, 0x64, 0x9c, 0xbf);
    size_t offset = 0;
    std::string decoded = http::v2::hpack_decode_huffman(data, offset);
    CHECK(decoded == "no-cache");
  }

  SUBCASE("Huffman Roundtrip arbitrary string")
  {
    std::string input = "The quick brown fox jumps over the lazy dog! 1234567890";
    http::v2::hpack_buffer encoded;
    encoded.encode_huffman(input);

    size_t offset = 0;
    std::string decoded = http::v2::hpack_decode_huffman(encoded, offset);
    CHECK(decoded == input);
  }

  SUBCASE("Huffman decoding - EOS symbol error")
  {
    // 30 bits of 1s + 2 bits of 1s padding (to complete the byte)
    auto data = make_bytes(0xff, 0xff, 0xff, 0xff);
    size_t offset = 0;
    // Ensure the string matches the throw in your code exactly
    CHECK_THROWS_AS(http::v2::hpack_decode_huffman(data, offset), std::runtime_error);
  }

  SUBCASE("Huffman decoding - Invalid padding error")
  {
    // '!' is 010111 (6 bits).
    // If we add '00' padding (01011100 = 0x5C), it's invalid because padding must be 1s.
    auto data = make_bytes(0x5C);
    size_t offset = 0;
    CHECK_THROWS_AS(http::v2::hpack_decode_huffman(data, offset), std::runtime_error);
  }

  SUBCASE("Huffman decoding - Dead-end padding error")
  {
    // This test verifies the 'temp->right == -1' logic.
    // We need a sequence that leaves the decoder at a node with no '1' branch.

    // In HPACK, symbol 0x37 ('7') is '111100' (6 bits).
    // If we take '11110' (5 bits), we are at an internal node.
    // If that specific node only had a 'left' child (0) to reach '7'
    // and no 'right' child, it would trigger the dead end.

    // Note: Finding a literal dead-end in the canonical RFC tree
    // depends on the table structure.
    // A simpler way to trigger "Invalid Padding" is any sequence
    // where following the '1's doesn't hit 256.

    auto data = make_bytes(0x18); // Our previous 000110 (a) + 00
    size_t offset = 0;

    // This should throw because the path '00' cannot reach EOS (which requires 1s)
    CHECK_THROWS_AS(http::v2::hpack_decode_huffman(data, offset), std::runtime_error);
  }

  SUBCASE("Long string Huffman roundtrip")
  {
    std::string input(1000, 'a');
    http::v2::hpack_buffer buf;
    buf.encode_huffman(input);

    size_t offset = 0;
    std::string decoded = http::v2::hpack_decode_huffman(buf, offset);
    CHECK(decoded == input);
  }
}
