#ifndef INCLUDE_HTTP_HEADERS_H
#define INCLUDE_HTTP_HEADERS_H

#include <string>
#include <vector>

namespace http
{
  struct header {
    std::string name;
    std::string value;
  };

  class headers
  {
  public:
    headers() = default;
    headers(std::vector<header> data);

    headers(std::initializer_list<header> init) : data_(init) {}

    void add(std::string name, std::string value);
    size_t size() const;

    const header& operator[](size_t index) const;

    const std::vector<header>& get() const;
    std::string get(std::string_view name) const;

  private:
    std::vector<header> data_;
  };
} // namespace http

#endif // INCLUDE_HTTP_HEADERS_H
