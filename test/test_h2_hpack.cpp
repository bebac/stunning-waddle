#include "doctest/doctest.h"
#include "test_helpers.h"
#include "http/v2/hpack_context.h"
#include "http/v2/hpack_static_tab.h"

TEST_CASE("HPACK Static Table Lookup")
{
  SUBCASE("Lookup by index")
  {
    auto h2 = http::v2::hpack_lookup_predefined(2);
    CHECK(h2.name == ":method");
    CHECK(h2.value == "GET");

    auto h4 = http::v2::hpack_lookup_predefined(4);
    CHECK(h4.name == ":path");
    CHECK(h4.value == "/");
  }

  SUBCASE("Invalid index")
  {
    CHECK_THROWS_AS(http::v2::hpack_lookup_predefined(0), std::out_of_range);
    CHECK_THROWS_AS(http::v2::hpack_lookup_predefined(62), std::out_of_range);
  }
}

TEST_CASE("HPACK Integer Decoding")
{
  SUBCASE("Small integer (fits in prefix)")
  {
    auto data = make_bytes(10);
    size_t offset = 0;
    uint32_t val = http::v2::hpack_decode_int(data, offset, 5);
    CHECK(val == 10);
    CHECK(offset == 1);
  }

  SUBCASE("Large integer (needs extra bytes)")
  {
    // Value 1337, prefix 5 bits (max 31)
    // 1337 - 31 = 1306
    // 1306 = 128 * 10 + 26
    // Encoding: [prefix: 31], [154 (128+26)], [10]
    auto data = make_bytes(0xFF, 0x9A, 0x0A);
    size_t offset = 0;
    uint32_t val = http::v2::hpack_decode_int(data, offset, 5);
    CHECK(val == 1337);
    CHECK(offset == 3);
  }

  SUBCASE("Integer limit exceeded")
  {
    auto data = make_bytes(0xFF, 0x9A, 0x0A);
    size_t offset = 0;
    CHECK_THROWS_WITH_AS(http::v2::hpack_decode_int(data, offset, 5, 1000), "HPACK decode_int: Integer overflow/limit exceeded", std::runtime_error);
  }
}

TEST_CASE("HPACK String Decoding")
{

  SUBCASE("Raw string")
  {
    // Length 5, "hello"
    auto data = make_bytes(0x05, 'h', 'e', 'l', 'l', 'o');
    size_t offset = 0;
    std::string s = http::v2::hpack_decode_string(data, offset);
    CHECK(s == "hello");
    CHECK(offset == 6);
  }

  SUBCASE("String length limit exceeded")
  {
    auto data = make_bytes(0x05, 'h', 'e', 'l', 'l', 'o');
    size_t offset = 0;
    CHECK_THROWS_WITH_AS(http::v2::hpack_decode_string(data, offset, 3), "HPACK decode_string: String length limit exceeded", std::runtime_error);
  }
}

TEST_CASE("HPACK Dynamic Table Management")
{
  http::v2::hpack_context ctx(256); // 256 octets max

  SUBCASE("Initial state")
  {
    CHECK(ctx.dynamic_table_size() == 0);
    CHECK(ctx.current_size() == 0);
  }

  SUBCASE("Add entry and lookup")
  {
    ctx.add_to_dynamic_table({":authority", "www.example.com"});
    // Size = 10 (name) + 15 (value) + 32 = 57
    CHECK(ctx.current_size() == 57);
    CHECK(ctx.dynamic_table_size() == 1);

    auto& h = ctx.get_header(62);
    CHECK(h.name == ":authority");
    CHECK(h.value == "www.example.com");
  }

  SUBCASE("Eviction on overflow")
  {
    http::v2::hpack_context ctx2(100);
    ctx2.add_to_dynamic_table({"custom-key", "custom-value"}); // 10 + 12 + 32 = 54
    CHECK(ctx2.current_size() == 54);

    ctx2.add_to_dynamic_table({"other-key", "other-value"}); // 9 + 11 + 32 = 52
    // 54 + 52 = 106 > 100. First entry must be evicted.
    CHECK(ctx2.current_size() == 52);
    CHECK(ctx2.dynamic_table_size() == 1);

    auto& h = ctx2.get_header(62);
    CHECK(h.name == "other-key");
    CHECK_THROWS_AS(ctx2.get_header(63), std::out_of_range);
  }

  SUBCASE("Table size update")
  {
    ctx.add_to_dynamic_table({"a", "b"}); // 1 + 1 + 32 = 34
    ctx.add_to_dynamic_table({"c", "d"}); // 1 + 1 + 32 = 34
    CHECK(ctx.current_size() == 68);

    ctx.set_max_table_size(40);
    // One entry must be evicted.
    CHECK(ctx.current_size() == 34);
    CHECK(ctx.dynamic_table_size() == 1);
    CHECK(ctx.get_header(62).name == "c");
  }
}

TEST_CASE("HPACK Engine - Encoder/Decoder Roundtrip")
{
  http::v2::hpack_context encode_ctx(4096);
  http::v2::hpack_context decode_ctx(4096);

  SUBCASE("Simple Request Roundtrip")
  {
    std::vector<http::header> headers = {
        {":method", "GET"},
        {":scheme", "https"},
        {":path", "/index.html"},
        {":authority", "www.example.com"},
        {"custom-key", "custom-value"}};

    http::v2::hpack_buffer buf;
    encode_ctx.encode(buf, headers, true); // Use Huffman

    auto decoded_headers = decode_ctx.decode(reinterpret_cast<uint8_t*>(buf.data()), buf.size());

    REQUIRE(decoded_headers.size() == headers.size());
    for (size_t i = 0; i < headers.size(); ++i)
    {
      CHECK(decoded_headers[i].name == headers[i].name);
      CHECK(decoded_headers[i].value == headers[i].value);
    }

    // Verify dynamic table state
    CHECK(encode_ctx.dynamic_table_size() == 2); // authority and custom-key
    CHECK(decode_ctx.dynamic_table_size() == 2);
  }

  SUBCASE("Consecutive Requests with Indexing")
  {
    std::vector<http::header> req1 = {
        {":method", "GET"},
        {":authority", "www.example.com"}};

    http::v2::hpack_buffer buf1;
    encode_ctx.encode(buf1, req1, false);
    decode_ctx.decode(reinterpret_cast<uint8_t*>(buf1.data()), buf1.size());

    std::vector<http::header> req2 = {
        {":method", "GET"},
        {":authority", "www.example.com"},
        {"cache-control", "no-cache"}};

    http::v2::hpack_buffer buf2;
    encode_ctx.encode(buf2, req2, false);

    // In req2, :method: GET should be indexed (static),
    // :authority: www.example.com should be indexed (dynamic)
    // cache-control: no-cache should be literal with indexing

    auto decoded_req2 = decode_ctx.decode(reinterpret_cast<uint8_t*>(buf2.data()), buf2.size());
    REQUIRE(decoded_req2.size() == 3);
    CHECK(decoded_req2[1].name == ":authority");
    CHECK(decoded_req2[1].value == "www.example.com");
  }
}

TEST_CASE("HPACK Engine - Literal Representations")
{
  http::v2::hpack_context ctx(4096);

  SUBCASE("Literal Header Field without Indexing")
  {
    http::v2::hpack_buffer buf;
    // :path: /test (indexed name 4, literal value)
    buf.encode_header_literal_no_index_indexed_name(4, "/test", false);
    auto decoded = ctx.decode(reinterpret_cast<uint8_t*>(buf.data()), buf.size());
    REQUIRE(decoded.size() == 1);
    CHECK(decoded[0].name == ":path");
    CHECK(decoded[0].value == "/test");
    CHECK(ctx.dynamic_table_size() == 0); // Should not be indexed
  }

  SUBCASE("Literal Header Field Never Indexed")
  {
    http::v2::hpack_buffer buf;
    buf.encode_header_literal_never_index_indexed_name(32, "secret", false); // cookie
    auto decoded = ctx.decode(reinterpret_cast<uint8_t*>(buf.data()), buf.size());
    REQUIRE(decoded.size() == 1);
    CHECK(decoded[0].name == "cookie");
    CHECK(decoded[0].value == "secret");
    CHECK(ctx.dynamic_table_size() == 0);
  }
}

TEST_CASE("HPACK Integer Encoding")
{
  SUBCASE("Small integer")
  {
    http::v2::hpack_buffer buf;
    buf.push_back(std::byte(0)); // Prefix byte
    buf.encode(10, 5);
    CHECK(buf.size() == 1);
    CHECK(buf[0] == std::byte(10));
  }

  SUBCASE("Large integer")
  {
    http::v2::hpack_buffer buf;
    buf.push_back(std::byte(0x1F)); // 5 bits set (max)
    buf.encode(1337, 5);
    CHECK(buf.size() == 3);
    CHECK(buf[0] == std::byte(0x1F));
    CHECK(buf[1] == std::byte(0x9A));
    CHECK(buf[2] == std::byte(0x0A));
  }
}

TEST_CASE("HPACK String Encoding")
{
  SUBCASE("Raw string")
  {
    http::v2::hpack_buffer buf;
    buf.encode("hello");
    CHECK(buf.size() == 6);
    CHECK(buf[0] == std::byte(0x05));
    CHECK(std::string(reinterpret_cast<char*>(buf.data() + 1), 5) == "hello");
  }
}

TEST_CASE("HPACK Header Encoding")
{
  SUBCASE("Indexed Header Field")
  {
    http::v2::hpack_buffer buf;
    buf.encode_header_indexed(2);
    CHECK(buf.size() == 1);
    CHECK(buf[0] == std::byte(0x82));
  }

  SUBCASE("Literal Header Field without Indexing — Indexed Name")
  {
    http::v2::hpack_buffer buf;
    buf.encode_header_literal_no_index_indexed_name(4, "/test");
    CHECK(buf.size() == 1 + 6); // 1 byte prefix/index + 1 byte len + 5 bytes value
    CHECK((buf[0] & std::byte(0xF0)) == std::byte(0x00));
    CHECK((buf[0] & std::byte(0x0F)) == std::byte(0x04));
    CHECK(buf[1] == std::byte(0x05));
    CHECK(std::string(reinterpret_cast<char*>(buf.data() + 2), 5) == "/test");
  }
}
