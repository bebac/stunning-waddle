#include "timing_harness.h"
#include "http/v2/hpack_context.h"
#include <span>

using namespace http::v2;

int main()
{
  std::cout << "HPACK Performance Benchmark" << std::endl;
  std::cout << std::string(80, '-') << std::endl;

  std::vector<benchmark_result> results;
  size_t iterations_primitives = 1000000;
  size_t iterations_hpack = 500000;
  size_t warm_up = 10000;

  // Huffman Primitives
  std::string short_str = "www.example.com";
  std::string long_str = "The quick brown fox jumps over the lazy dog! 1234567890. This is a longer string to test "
                         "Huffman performance more thoroughly.";

  results.push_back(
    timing_harness::run("Huffman Encode (Short)", iterations_primitives, warm_up, short_str.size(), [&]() {
      hpack_buffer buf;
      buf.encode_huffman(short_str);
    })
  );

  results.push_back(
    timing_harness::run("Huffman Encode (Long)", iterations_primitives, warm_up, long_str.size(), [&]() {
      hpack_buffer buf;
      buf.encode_huffman(long_str);
    })
  );

  hpack_buffer short_huff;
  short_huff.encode_huffman(short_str);
  results.push_back(
    timing_harness::run("Huffman Decode (Short)", iterations_primitives, warm_up, short_str.size(), [&]() {
      size_t offset = 0;
      hpack_decode_huffman(std::span(short_huff), offset);
    })
  );

  hpack_buffer long_huff;
  long_huff.encode_huffman(long_str);
  results.push_back(
    timing_harness::run("Huffman Decode (Long)", iterations_primitives, warm_up, long_str.size(), [&]() {
      size_t offset = 0;
      hpack_decode_huffman(std::span(long_huff), offset);
    })
  );

  // Full HPACK Benchmarks
  std::vector<http::header> browser_request = {
    {":method", "GET"},
    {":scheme", "https"},
    {":path", "/index.html"},
    {":authority", "www.example.com"},
    {"user-agent", "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) "
                    "Chrome/91.0.4472.114 Safari/537.36"},
    {"accept", "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,image/apng,*/"
                "*;q=0.8,application/signed-exchange;v=b3;q=0.9"},
    {"accept-encoding", "gzip, deflate, br"},
    {"accept-language", "en-US,en;q=0.9"},
    {"cache-control", "max-age=0"},
    {"upgrade-insecure-requests", "1"}};

  size_t req_size = calculate_headers_size(browser_request);

  // Stateless Encode
  results.push_back(timing_harness::run("HPACK Encode (Stateless)", iterations_hpack, warm_up, req_size, [&]() {
      hpack_context ctx(4096);
      hpack_buffer buf;
      ctx.encode(buf, browser_request, true);
    })
  );

  // Stateless Decode
  hpack_context static_ctx(4096);
  hpack_buffer stateless_buffer;
  static_ctx.encode(stateless_buffer, browser_request, true);

  results.push_back(timing_harness::run("HPACK Decode (Stateless)", iterations_hpack, warm_up, req_size, [&]() {
      hpack_context ctx(4096);
      ctx.decode(reinterpret_cast<const uint8_t*>(stateless_buffer.data()), stateless_buffer.size());
    })
  );

  // Stateful Encode
  hpack_context stateful_encode_ctx(4096);
  hpack_buffer dummy;
  stateful_encode_ctx.encode(dummy, browser_request, true);

  results.push_back(timing_harness::run("HPACK Encode (Stateful)", iterations_hpack, warm_up, req_size, [&]() {
      hpack_buffer buf;
      stateful_encode_ctx.encode(buf, browser_request, true);
    })
  );

  // Stateful Decode
  hpack_context stateful_decode_ctx(4096);
  hpack_buffer indexed_buffer;
  stateful_decode_ctx.encode(indexed_buffer, browser_request, true);

  hpack_context decoder_state(4096);
  decoder_state.decode(reinterpret_cast<const uint8_t*>(stateless_buffer.data()), stateless_buffer.size());

  results.push_back(timing_harness::run("HPACK Decode (Stateful)", iterations_hpack, warm_up, req_size, [&]() {
      decoder_state.decode(reinterpret_cast<const uint8_t*>(indexed_buffer.data()), indexed_buffer.size());
    })
  );

  for (const auto& r : results)
  {
    print_result(r);
  }

  write_json(results, "hpack_benchmark.json");
  std::cout << std::string(80, '-') << std::endl;
  std::cout << "Results written to hpack_benchmark.json" << std::endl;

  return 0;
}
