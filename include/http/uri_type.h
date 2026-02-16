#ifndef INCLUDE_HTTP_URI_TYPE_H
#define INCLUDE_HTTP_URI_TYPE_H

#include <concepts>
#include <string>
#include <string_view>
#include <format>

namespace http
{
  template <typename T>
  concept uri_type = requires(const T& u) {
    { u.scheme() }   -> std::convertible_to<std::string_view>;
    { u.userinfo() } -> std::convertible_to<std::string_view>;
    { u.host() }     -> std::convertible_to<std::string_view>;
    { u.port() }     -> std::convertible_to<std::string_view>;
    { u.path() }     -> std::convertible_to<std::string_view>;
    { u.query() }    -> std::convertible_to<std::string_view>;
    { u.fragment() } -> std::convertible_to<std::string_view>;
  };

  template <uri_type U> std::string get_full_path(const U& uri)
  {
    std::string_view p = uri.path();
    std::string_view q = uri.query();

    if (p.empty())
      p = "/";
    if (q.empty())
      return std::string(p);

    return std::format("{}?{}", p, q);
  }
} // namespace http

#endif // INCLUDE_HTTP_URI_TYPE_H
