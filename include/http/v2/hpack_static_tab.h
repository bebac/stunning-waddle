#ifndef INCLUDE_HTTP_V2_HPACK_STATIC_TAB_H
#define INCLUDE_HTTP_V2_HPACK_STATIC_TAB_H

#include "http/headers.h"

#include <vector>
#include <exception>

namespace http::v2
{
  static const std::vector<header>& hpack_static_table()
  {
    static const std::vector<header> table = {
      {":authority", ""},
      {":method", "GET"},
      {":method", "POST"},
      {":path", "/"},
      {":path", "/index.html"},
      {":scheme", "http"},
      {":scheme", "https"},
      {":status", "200"},
      {":status", "204"},
      {":status", "206"},
      {":status", "304"},
      {":status", "400"},
      {":status", "404"},
      {":status", "500"},
      {"accept-charset", ""},
      {"accept-encoding", "gzip, deflate"},
      {"accept-language", ""},
      {"accept-ranges", ""},
      {"accept", ""},
      {"access-control-allow-origin", ""},
      {"age", ""},
      {"allow", ""},
      {"authorization", ""},
      {"cache-control", ""},
      {"content-disposition", ""},
      {"content-encoding", ""},
      {"content-language", ""},
      {"content-length", ""},
      {"content-location", ""},
      {"content-range", ""},
      {"content-type", ""},
      {"cookie", ""},
      {"date", ""},
      {"etag", ""},
      {"expect", ""},
      {"expires", ""},
      {"from", ""},
      {"host", ""},
      {"if-match", ""},
      {"if-modified-since", ""},
      {"if-none-match", ""},
      {"if-range", ""},
      {"if-unmodified-since", ""},
      {"last-modified", ""},
      {"link", ""},
      {"location", ""},
      {"max-forwards", ""},
      {"proxy-authenticate", ""},
      {"proxy-authorization", ""},
      {"range", ""},
      {"referer", ""},
      {"refresh", ""},
      {"retry-after", ""},
      {"server", ""},
      {"set-cookie", ""},
      {"strict-transport-security", ""},
      {"transfer-encoding", ""},
      {"user-agent", ""},
      {"vary", ""},
      {"via", ""},
      {"www-authenticate", ""}};
    return table;
  };

  inline constexpr std::size_t hpack_static_table_size() {
    return 61;
  }

  inline bool hpack_is_predefined(size_t index) {
    return index >= 1 && index <= hpack_static_table_size();
  }

  inline const header& hpack_lookup_predefined(size_t index)
  {
    if (index < 1 || index > hpack_static_table_size())
    {
      throw std::out_of_range("HPACK static table index out of range");
    }
    return hpack_static_table()[index - 1];
  }
} // namespace http::v2

#endif // INCLUDE_HTTP_V2_HPACK_STATIC_TAB_H
