#include "doctest/doctest.h"
#include "test_helpers.h"
#include "http/client_context.h"
#include "http/server_context.h"
#include "http/event.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{
  void feed(http::context_base& ctx, std::string_view s)
  {
    mock::recv(ctx, std::as_bytes(std::span(s.data(), s.size())));
  }

  std::string take_output(http::context_base& ctx)
  {
    auto out = ctx.output_begin();
    std::string s(reinterpret_cast<const char*>(out.data()), out.size());
    ctx.output_end(out.size());
    return s;
  }

  std::vector<http::event> drain(http::context_base& ctx)
  {
    std::vector<http::event> events;
    while (ctx.has_events()) {
      auto ev = ctx.next_event();
      if (ev) {
        events.push_back(std::move(*ev));
      }
    }
    return events;
  }

  std::span<const std::byte> bytes(std::string_view s)
  {
    return std::as_bytes(std::span(s.data(), s.size()));
  }
} // namespace

TEST_CASE("v1 client_context: request/response through the context")
{
  http::client_context ctx;
  ctx.set_protocol_version(http::protocol_version::v1_1);

  auto s = ctx.open_stream();
  REQUIRE(s.id() == 1);

  http::headers h;
  h.add("content-type", "application/json");
  s.send_headers("POST", "/mcp", "localhost:6006", h, false);
  s.send_data(bytes("{}"), true);

  REQUIRE(take_output(ctx) ==
    "POST /mcp HTTP/1.1\r\n"
    "Host: localhost:6006\r\n"
    "content-type: application/json\r\n"
    "Transfer-Encoding: chunked\r\n"
    "\r\n"
    "2\r\n{}\r\n0\r\n\r\n");

  feed(ctx, "HTTP/1.1 200 OK\r\ncontent-type: text/event-stream\r\n"
            "content-length: 2\r\n\r\n{}");

  auto events = drain(ctx);
  REQUIRE(events.size() == 3);
  REQUIRE(events[0].is_headers());
  REQUIRE(events[0].stream_id == 1);
  {
    const auto& rh = std::get<http::headers>(events[0].payload);
    REQUIRE(rh.get(":status") == "200");
    REQUIRE(rh.get("content-type") == "text/event-stream");
  }
  REQUIRE(events[1].is_data());
  REQUIRE(events[2].is_stream_closed());
  REQUIRE(events[2].stream_id == 1);
}

TEST_CASE("v1 server_context: request events and response via stream handle")
{
  http::server_context ctx;
  ctx.set_protocol_version(http::protocol_version::v1_1);

  feed(ctx, "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");

  // Drain one event at a time: capture the stream handle when the
  // headers event arrives. (The context erases the stream when the
  // request's stream_closed is drained, before the response is sent —
  // the same pattern the mini-server uses.)
  std::vector<http::event> events;
  http::stream s;
  while (ctx.has_events()) {
    auto ev = ctx.next_event();
    if (!ev) {
      break;
    }
    if (ev->is_headers()) {
      s = ctx.get_stream(ev->stream_id);
      const auto& h = std::get<http::headers>(ev->payload);
      REQUIRE(h.get(":method") == "GET");
      REQUIRE(h.get(":path") == "/");
      REQUIRE(h.get(":authority") == "localhost");
    }
    events.push_back(std::move(*ev));
  }

  REQUIRE(events.size() == 2);
  REQUIRE(events[0].is_headers());
  REQUIRE(events[1].is_stream_closed());

  REQUIRE(s);
  s.send_response(200, {}, true);
  REQUIRE(take_output(ctx) == "HTTP/1.1 200 OK\r\n\r\n");
}
