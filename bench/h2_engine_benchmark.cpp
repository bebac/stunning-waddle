#include "timing_harness.h"
#include "http/v2/engine.h"
#include <span>
#include <cstring>

using namespace http::v2;

std::vector<std::byte> prepare_serialized_data(uint32_t stream_id, size_t size, bool end_stream)
{
  std::vector<std::byte> payload(size, std::byte{0x41});
  std::vector<std::byte> serialized;

  const size_t max_frame_size = 16384;
  size_t offset = 0;

  while (offset < size) {
    size_t chunk_size = std::min(size - offset, max_frame_size);
    bool is_last_chunk = (offset + chunk_size == size);

    std::span<const std::byte> chunk(payload.data() + offset, chunk_size);
    encode_data_frame(serialized, stream_id, chunk, is_last_chunk && end_stream);

    offset += chunk_size;
  }

  return serialized;
}

void commit_data(http::protocol_engine& eng, std::span<const std::byte> data)
{
  size_t offset = 0;
  while (offset < data.size()) {
    auto in = eng.input_begin();
    size_t to_copy = std::min(in.size(), data.size() - offset);
    if (to_copy == 0) break;
    std::memcpy(in.data(), data.data() + offset, to_copy);
    eng.input_end(to_copy);
    offset += to_copy;
  }
}

int main()
{
  std::cout << "HTTP/2 Engine Performance Benchmark" << std::endl;
  std::cout << std::string(100, '-') << std::endl;

  std::vector<benchmark_result> results;
  size_t iterations = 100000;
  size_t warm_up = 1000;

  std::vector<http::header> browser_request_headers = {
    {"user-agent", "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) "
                    "Chrome/91.0.4472.114 Safari/537.36"},
    {"accept", "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,image/apng,*/"
                "*;q=0.8,application/signed-exchange;v=b3;q=0.9"},
    {"accept-encoding", "gzip, deflate, br"},
    {"accept-language", "en-US,en;q=0.9"},
    {"cache-control", "max-age=0"},
    {"upgrade-insecure-requests", "1"}};

  std::vector<http::header> full_browser_request = {
    {":method", "GET"},
    {":scheme", "https"},
    {":path", "/index.html"},
    {":authority", "www.example.com"}};
  full_browser_request.insert(full_browser_request.end(), browser_request_headers.begin(), browser_request_headers.end());

  size_t req_headers_size = calculate_headers_size(full_browser_request);

  hpack_context hpack_ctx(4096);
  hpack_buffer literal_block;
  hpack_ctx.encode(literal_block, full_browser_request, true);
  std::vector<std::byte> serialized_literal_headers;
  encode_headers_frame(serialized_literal_headers, 1, literal_block, true, false);

  hpack_buffer indexed_block;
  hpack_ctx.encode(indexed_block, full_browser_request, true);
  std::vector<std::byte> serialized_indexed_headers;
  encode_headers_frame(serialized_indexed_headers, 3, indexed_block, true, false);

  // HEADERS consume (Cold Start)
  results.push_back(timing_harness::run("HEADERS consume (Cold Start)", iterations, warm_up, req_headers_size, [&]() {
      engine eng;
      commit_data(eng, serialized_literal_headers);
    })
  );

  // HEADERS consume (Warm State)
  engine warm_eng_consume;
  commit_data(warm_eng_consume, serialized_literal_headers);

  results.push_back(timing_harness::run("HEADERS consume (Warm State)", iterations, warm_up, req_headers_size, [&]() {
      commit_data(warm_eng_consume, serialized_indexed_headers);
    })
  );

  http::headers request_headers;
  for (const auto& h : browser_request_headers) request_headers.add(h.name, h.value);

  // HEADERS produce (Cold Start)
  results.push_back(timing_harness::run("HEADERS produce (Cold Start)", iterations, warm_up, req_headers_size, [&]() {
      engine eng;
      uint32_t id = eng.open_stream();
      eng.send_request_headers(id, "GET", "/index.html", "www.example.com", request_headers, true);
      auto out = eng.output_begin();
      eng.output_end(out.size());
    })
  );

  // HEADERS produce (Warm State)
  engine warm_eng_produce;
  uint32_t id = warm_eng_produce.open_stream();
  warm_eng_produce.send_request_headers(id, "GET", "/index.html", "www.example.com", request_headers, true);
  warm_eng_produce.output_end(warm_eng_produce.output_begin().size());

  results.push_back(timing_harness::run("HEADERS produce (Warm State)", iterations, warm_up, req_headers_size, [&]() {
      uint32_t sid = warm_eng_produce.open_stream();
      warm_eng_produce.send_request_headers(sid, "GET", "/index.html", "www.example.com", request_headers, true);
      auto out = warm_eng_produce.output_begin();
      warm_eng_produce.output_end(out.size());
    })
  );

  // DATA consume benchmarks
  std::vector<std::byte> data_256 = prepare_serialized_data(1, 256, true);
  std::vector<std::byte> data_8k = prepare_serialized_data(1, 8192, true);
  std::vector<std::byte> data_64k = prepare_serialized_data(1, 65536, true);

  engine data_consume_eng;
  results.push_back(timing_harness::run("DATA consume (256B)", iterations, warm_up, 256, [&]() {
      commit_data(data_consume_eng, data_256);
    })
  );

  results.push_back(timing_harness::run("DATA consume (8KB)", iterations, warm_up, 8192, [&]() {
      commit_data(data_consume_eng, data_8k);
    })
  );

  results.push_back(timing_harness::run("DATA consume (64KB)", iterations / 10, warm_up / 10, 65536, [&]() {
      commit_data(data_consume_eng, data_64k);
    })
  );

  // DATA produce benchmarks
  std::vector<std::byte> payload_256(256, std::byte{0x41});
  std::vector<std::byte> payload_8k(8192, std::byte{0x41});
  std::vector<std::byte> payload_64k(65536, std::byte{0x41});

  engine data_produce_eng;
  results.push_back(timing_harness::run("DATA produce (256B)", iterations, warm_up, 256, [&]() {
      data_produce_eng.send_data(1, payload_256, true);
      auto out = data_produce_eng.output_begin();
      data_produce_eng.output_end(out.size());
    })
  );

  results.push_back(timing_harness::run("DATA produce (8KB)", iterations, warm_up, 8192, [&]() {
      data_produce_eng.send_data(1, payload_8k, true);
      auto out = data_produce_eng.output_begin();
      data_produce_eng.output_end(out.size());
    })
  );

  results.push_back(timing_harness::run("DATA produce (64KB)", iterations / 10, warm_up / 10, 65536, [&]() {
      data_produce_eng.send_data(1, payload_64k, true);
      auto out = data_produce_eng.output_begin();
      data_produce_eng.output_end(out.size());
    })
  );

  // Mixed Traffic Benchmark
  std::vector<std::byte> mixed_data = serialized_indexed_headers;
  std::vector<std::byte> mixed_payload = prepare_serialized_data(3, 8192, true);
  mixed_data.insert(mixed_data.end(), mixed_payload.begin(), mixed_payload.end());
  size_t mixed_total_size = req_headers_size + 8192;

  results.push_back(timing_harness::run("Mixed Traffic (HEADERS+8KB DATA)", iterations, warm_up, mixed_total_size, [&]() {
      engine eng;
      commit_data(eng, serialized_literal_headers);
      commit_data(eng, mixed_data);
    })
  );

  std::cout << std::string(100, '-') << std::endl;
  for (const auto& r : results)
  {
    print_result(r);
  }

  write_json(results, "h2_engine_benchmark.json");
  std::cout << std::string(100, '-') << std::endl;
  std::cout << "Results written to h2_engine_benchmark.json" << std::endl;

  return 0;
}
