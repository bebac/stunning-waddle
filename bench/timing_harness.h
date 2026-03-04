#pragma once

#include "http/headers.h"
#include <chrono>
#include <iostream>
#include <vector>
#include <string>
#include <numeric>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <fstream>

struct benchmark_result {
  std::string name;
  size_t iterations;
  double avg_ns;
  double min_ns;
  double max_ns;
  double std_dev;
  double throughput_ops_sec;
  double throughput_mb_sec;
};

class timing_harness
{
public:
  template <typename Func>
  static benchmark_result run(const std::string& name, size_t iterations, size_t warm_up, size_t data_size, Func&& func)
  {
    for (size_t i = 0; i < warm_up; ++i)
    {
      func();
    }

    std::vector<double> samples;
    samples.reserve(iterations);

    for (size_t i = 0; i < iterations; ++i)
    {
      auto start = std::chrono::high_resolution_clock::now();
      func();
      auto end = std::chrono::high_resolution_clock::now();
      samples.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
    }

    double sum = std::accumulate(samples.begin(), samples.end(), 0.0);
    double avg = sum / iterations;
    double min = *std::min_element(samples.begin(), samples.end());
    double max = *std::max_element(samples.begin(), samples.end());

    double sq_sum = std::inner_product(samples.begin(), samples.end(), samples.begin(), 0.0);
    double std_dev = std::sqrt(std::abs(sq_sum / iterations - avg * avg));

    double ops_sec = 1e9 / avg;
    double mb_sec = (ops_sec * data_size) / (1024.0 * 1024.0);

    return {name, iterations, avg, min, max, std_dev, ops_sec, mb_sec};
  }
};

inline void print_result(const benchmark_result& res)
{
  std::cout << std::left << std::setw(35) << res.name << " | Avg: " << std::fixed << std::setprecision(2)
            << std::setw(10) << res.avg_ns << " ns"
            << " | Ops/s: " << std::setw(12) << (size_t)res.throughput_ops_sec << " | " << std::setw(8)
            << res.throughput_mb_sec << " MB/s" << std::endl;
}

inline void write_json(const std::vector<benchmark_result>& results, const std::string& filename)
{
  std::ofstream f(filename);
  f << "{\n  \"results\": [\n";
  for (size_t i = 0; i < results.size(); ++i)
  {
    const auto& r = results[i];
    f << "    {\n"
      << "      \"name\": \"" << r.name << "\",\n"
      << "      \"iterations\": " << r.iterations << ",\n"
      << "      \"avg_ns\": " << r.avg_ns << ",\n"
      << "      \"min_ns\": " << r.min_ns << ",\n"
      << "      \"max_ns\": " << r.max_ns << ",\n"
      << "      \"std_dev\": " << r.std_dev << ",\n"
      << "      \"ops_sec\": " << r.throughput_ops_sec << ",\n"
      << "      \"mb_sec\": " << r.throughput_mb_sec << "\n"
      << "    }" << (i == results.size() - 1 ? "" : ",") << "\n";
  }
  f << "  ]\n}\n";
}

inline size_t calculate_headers_size(const std::vector<http::header>& headers)
{
  size_t size = 0;
  for (const auto& h : headers)
  {
    size += h.name.size() + h.value.size();
  }
  return size;
}
