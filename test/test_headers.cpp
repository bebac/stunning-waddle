#include "doctest/doctest.h"
#include "http/headers.h"

TEST_CASE("HTTP Header Structure")
{
  http::header h{"content-type", "application/json"};
  CHECK(h.name == "content-type");
  CHECK(h.value == "application/json");
}

TEST_CASE("Header Block Representation")
{
  http::headers headers;
  headers.add("server", "conductor");
  headers.add("x-version", "1.0");

  CHECK(headers.size() == 2);
  CHECK(headers[0].name == "server");
  CHECK(headers[1].value == "1.0");
}
