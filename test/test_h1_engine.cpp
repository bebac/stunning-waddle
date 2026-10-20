#include "doctest/doctest.h"
#include "test_helpers.h"
#include "http/v1/engine.h"
#include "http/event.h"

#include <cstddef>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{
  void feed(http::protocol_engine& e, std::string_view s)
  {
    mock::recv(e, std::as_bytes(std::span(s.data(), s.size())));
  }

  std::string take_output(http::protocol_engine& e)
  {
    auto out = e.output_begin();
    std::string s(reinterpret_cast<const char*>(out.data()), out.size());
    e.output_end(out.size());
    return s;
  }

  std::vector<http::event> drain(http::protocol_engine& e)
  {
    std::vector<http::event> events;
    while (e.has_events()) {
      auto ev = e.next_event();
      if (ev) {
        events.push_back(std::move(*ev));
      }
    }
    return events;
  }

  std::string data_text(const http::event& ev)
  {
    const auto& v = std::get<std::vector<std::byte>>(ev.payload);
    return std::string(reinterpret_cast<const char*>(v.data()), v.size());
  }

  std::span<const std::byte> bytes(std::string_view s)
  {
    return std::as_bytes(std::span(s.data(), s.size()));
  }

  // Open a stream and send a bodyless GET, consuming the output.
  void send_get(http::protocol_engine& e, uint32_t id, std::string_view path)
  {
    e.send_request_headers(id, "GET", path, "example.com", {}, true);
    take_output(e);
  }
} // namespace

TEST_CASE("v1 client: GET request serialization")
{
  http::v1::engine e(http::connection_role::client);
  auto id = e.open_stream();
  REQUIRE(id == 1);
  e.send_request_headers(id, "GET", "/index.html", "example.com", {}, true);
  REQUIRE(take_output(e) ==
    "GET /index.html HTTP/1.1\r\nHost: example.com\r\n\r\n");
}

TEST_CASE("v1 client: empty authority produces no Host header")
{
  http::v1::engine e(http::connection_role::client);
  auto id = e.open_stream();
  e.send_request_headers(id, "GET", "/", "", {}, true);
  REQUIRE(take_output(e) == "GET / HTTP/1.1\r\n\r\n");
}

TEST_CASE("v1 client: POST with content-length passes body through")
{
  http::v1::engine e(http::connection_role::client);
  auto id = e.open_stream();
  http::headers h;
  h.add("content-length", "5");
  e.send_request_headers(id, "POST", "/", "example.com", h, false);
  REQUIRE(take_output(e) ==
    "POST / HTTP/1.1\r\nHost: example.com\r\ncontent-length: 5\r\n\r\n");
  auto sent = e.send_data(id, bytes("hello"), true);
  REQUIRE(sent == 5);
  REQUIRE(take_output(e) == "hello");
}

TEST_CASE("v1 client: POST without content-length uses chunked framing")
{
  http::v1::engine e(http::connection_role::client);
  auto id = e.open_stream();
  e.send_request_headers(id, "POST", "/", "example.com", {}, false);
  REQUIRE(take_output(e) ==
    "POST / HTTP/1.1\r\nHost: example.com\r\nTransfer-Encoding: chunked\r\n\r\n");
  e.send_data(id, bytes("hello"), false);
  REQUIRE(take_output(e) == "5\r\nhello\r\n");
  e.send_data(id, bytes("world"), false);
  REQUIRE(take_output(e) == "5\r\nworld\r\n");
  e.send_data(id, {}, true);
  REQUIRE(take_output(e) == "0\r\n\r\n");
}

TEST_CASE("v1 client: response with content-length produces headers/data/closed")
{
  http::v1::engine e(http::connection_role::client);
  auto id = e.open_stream();
  send_get(e, id, "/");

  feed(e, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\ncontent-length: 5\r\n\r\nhello");
  auto events = drain(e);
  REQUIRE(events.size() == 3);
  REQUIRE(events[0].is_headers());
  REQUIRE(events[0].stream_id == 1);
  {
    const auto& h = std::get<http::headers>(events[0].payload);
    REQUIRE(h.get(":status") == "200");
    // Header names are lowercased on receive.
    REQUIRE(h.get("content-type") == "text/plain");
  }
  REQUIRE(events[1].is_data());
  REQUIRE(data_text(events[1]) == "hello");
  REQUIRE(events[2].is_stream_closed());
  REQUIRE(events[2].stream_id == 1);
}

TEST_CASE("v1 client: chunked response is dechunked")
{
  http::v1::engine e(http::connection_role::client);
  auto id = e.open_stream();
  send_get(e, id, "/");

  feed(e, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
          "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n");
  auto events = drain(e);
  REQUIRE(events.size() == 4);
  REQUIRE(events[0].is_headers());
  REQUIRE(events[1].is_data());
  REQUIRE(data_text(events[1]) == "hello");
  REQUIRE(events[2].is_data());
  REQUIRE(data_text(events[2]) == " world");
  REQUIRE(events[3].is_stream_closed());
}

TEST_CASE("v1 client: close-delimited response completes at EOF")
{
  http::v1::engine e(http::connection_role::client);
  auto id = e.open_stream();
  send_get(e, id, "/");

  feed(e, "HTTP/1.1 200 OK\r\n\r\nabc");
  auto events = drain(e);
  REQUIRE(events.size() == 2);
  REQUIRE(events[0].is_headers());
  REQUIRE(events[1].is_data());
  REQUIRE(data_text(events[1]) == "abc");

  e.eof();
  auto tail = drain(e);
  REQUIRE(tail.size() == 2);
  REQUIRE(tail[0].is_stream_closed());
  REQUIRE(tail[0].stream_id == 1);
  REQUIRE(tail[1].is_goaway());
  const auto& info = std::get<http::goaway_info>(tail[1].payload);
  REQUIRE(info.last_stream_id == 1);
  REQUIRE(info.ec == http::error_code::no_error);
}

TEST_CASE("v1 client: EOF mid-message is a connection error")
{
  http::v1::engine e(http::connection_role::client);
  auto id = e.open_stream();
  send_get(e, id, "/");

  feed(e, "HTTP/1.1 200 OK\r\ncontent-length: 10\r\n\r\nabc");
  auto events = drain(e);
  REQUIRE(events.size() == 2);
  REQUIRE(events[1].is_data());

  e.eof();
  auto tail = drain(e);
  REQUIRE(tail.size() == 1);
  REQUIRE(tail[0].is_connection_error());
  REQUIRE(std::get<std::error_code>(tail[0].payload) ==
    http::make_error_code(http::error_code::connection_reset));
}

TEST_CASE("v1 client: interim 1xx responses are skipped")
{
  http::v1::engine e(http::connection_role::client);
  auto id = e.open_stream();
  send_get(e, id, "/");

  feed(e, "HTTP/1.1 100 Continue\r\n\r\n"
          "HTTP/1.1 200 OK\r\ncontent-length: 2\r\n\r\nhi");
  auto events = drain(e);
  REQUIRE(events.size() == 3);
  REQUIRE(events[0].is_headers());
  REQUIRE(std::get<http::headers>(events[0].payload).get(":status") == "200");
  REQUIRE(events[1].is_data());
  REQUIRE(events[2].is_stream_closed());
}

TEST_CASE("v1 client: HEAD response has no body")
{
  http::v1::engine e(http::connection_role::client);
  auto id = e.open_stream();
  e.send_request_headers(id, "HEAD", "/", "example.com", {}, true);
  take_output(e);

  // Content-Length describes the would-be body; no body follows.
  feed(e, "HTTP/1.1 200 OK\r\ncontent-length: 100\r\n\r\n");
  auto events = drain(e);
  REQUIRE(events.size() == 2);
  REQUIRE(events[0].is_headers());
  REQUIRE(events[1].is_stream_closed());
}

TEST_CASE("v1 client: 204 response has no body")
{
  http::v1::engine e(http::connection_role::client);
  auto id = e.open_stream();
  send_get(e, id, "/");

  feed(e, "HTTP/1.1 204 No Content\r\n\r\n");
  auto events = drain(e);
  REQUIRE(events.size() == 2);
  REQUIRE(events[0].is_headers());
  REQUIRE(std::get<http::headers>(events[0].payload).get(":status") == "204");
  REQUIRE(events[1].is_stream_closed());
}

TEST_CASE("v1 client: pipelined requests are serialized and demuxed in order")
{
  http::v1::engine e(http::connection_role::client);
  auto id1 = e.open_stream();
  auto id3 = e.open_stream();
  REQUIRE(id1 == 1);
  REQUIRE(id3 == 3);

  e.send_request_headers(id1, "GET", "/a", "example.com", {}, true);
  e.send_request_headers(id3, "GET", "/b", "example.com", {}, true);

  // Only the first request is on the wire; the second is staged.
  REQUIRE(take_output(e) == "GET /a HTTP/1.1\r\nHost: example.com\r\n\r\n");

  // Response 1 arrives: events on stream 1, then request 2 is flushed.
  feed(e, "HTTP/1.1 200 OK\r\ncontent-length: 1\r\n\r\nA");
  auto ev1 = drain(e);
  REQUIRE(ev1.size() == 3);
  REQUIRE(ev1[0].stream_id == 1);
  REQUIRE(ev1[0].is_headers());
  REQUIRE(ev1[1].is_data());
  REQUIRE(data_text(ev1[1]) == "A");
  REQUIRE(ev1[2].is_stream_closed());

  REQUIRE(take_output(e) == "GET /b HTTP/1.1\r\nHost: example.com\r\n\r\n");

  feed(e, "HTTP/1.1 200 OK\r\ncontent-length: 1\r\n\r\nB");
  auto ev2 = drain(e);
  REQUIRE(ev2.size() == 3);
  REQUIRE(ev2[0].stream_id == 3);
  REQUIRE(ev2[0].is_headers());
  REQUIRE(ev2[2].is_stream_closed());
}

TEST_CASE("v1 client: Connection: close emits goaway after the response")
{
  http::v1::engine e(http::connection_role::client);
  auto id = e.open_stream();
  http::headers h;
  h.add("connection", "close");
  e.send_request_headers(id, "GET", "/", "example.com", h, true);
  take_output(e);

  feed(e, "HTTP/1.1 200 OK\r\ncontent-length: 2\r\nconnection: close\r\n\r\nhi");
  auto events = drain(e);
  REQUIRE(events.size() == 4);
  REQUIRE(events[0].is_headers());
  REQUIRE(events[1].is_data());
  REQUIRE(events[2].is_stream_closed());
  REQUIRE(events[3].is_goaway());

  // Further input is ignored once the connection is done.
  feed(e, "garbage");
  REQUIRE(!e.has_events());
}

TEST_CASE("v1 client: malformed status line is a connection error")
{
  http::v1::engine e(http::connection_role::client);
  auto id = e.open_stream();
  send_get(e, id, "/");

  feed(e, "GARBAGE\r\n\r\n");
  auto events = drain(e);
  REQUIRE(events.size() == 1);
  REQUIRE(events[0].is_connection_error());
  REQUIRE(std::get<std::error_code>(events[0].payload) ==
    http::make_error_code(http::error_code::protocol_error));
}

TEST_CASE("v1 client: split reads produce the same message")
{
  const std::string response =
    "HTTP/1.1 200 OK\r\ncontent-type: text/plain\r\ncontent-length: 11\r\n\r\nhello world";

  http::v1::engine split(http::connection_role::client);
  auto id = split.open_stream();
  send_get(split, id, "/");
  for (char c : response) {
    feed(split, std::string_view(&c, 1));
  }
  auto actual = drain(split);

  // Data events arrive per read; the reassembled message is the same.
  REQUIRE(actual.front().is_headers());
  REQUIRE(std::get<http::headers>(actual.front().payload).get(":status") == "200");
  REQUIRE(actual.back().is_stream_closed());
  std::string body;
  for (const auto& ev : actual) {
    if (ev.is_data()) {
      const auto& v = std::get<std::vector<std::byte>>(ev.payload);
      body.append(reinterpret_cast<const char*>(v.data()), v.size());
    }
  }
  REQUIRE(body == "hello world");
}

TEST_CASE("v1 server: request parsing produces pseudo-headers")
{
  http::v1::engine e(http::connection_role::server);
  feed(e, "POST /mcp HTTP/1.1\r\nHost: localhost:6006\r\nContent-Length: 4\r\n\r\nbody");
  auto events = drain(e);
  REQUIRE(events.size() == 4);
  REQUIRE(events[0].is_new_stream());
  REQUIRE(events[0].stream_id == 1);
  REQUIRE(events[1].is_headers());
  {
    const auto& h = std::get<http::headers>(events[1].payload);
    REQUIRE(h.get(":method") == "POST");
    REQUIRE(h.get(":path") == "/mcp");
    REQUIRE(h.get(":authority") == "localhost:6006");
    REQUIRE(h.get("content-length") == "4");
  }
  REQUIRE(events[2].is_data());
  REQUIRE(data_text(events[2]) == "body");
  REQUIRE(events[3].is_stream_closed());
}

TEST_CASE("v1 server: response without content-length uses chunked framing")
{
  http::v1::engine e(http::connection_role::server);
  feed(e, "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n");
  drain(e);

  e.send_response_headers(1, 200, {}, false);
  REQUIRE(take_output(e) ==
    "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
  e.send_data(1, bytes("hi"), true);
  REQUIRE(take_output(e) == "2\r\nhi\r\n0\r\n\r\n");
}

TEST_CASE("v1 server: keep-alive parses a second request after the response")
{
  http::v1::engine e(http::connection_role::server);
  feed(e, "GET /a HTTP/1.1\r\nHost: example.com\r\n\r\n");
  drain(e);
  e.send_response_headers(1, 200, {}, true);
  REQUIRE(take_output(e) == "HTTP/1.1 200 OK\r\n\r\n");

  feed(e, "GET /b HTTP/1.1\r\nHost: example.com\r\n\r\n");
  auto events = drain(e);
  REQUIRE(events.size() == 3);
  REQUIRE(events[0].is_new_stream());
  REQUIRE(events[0].stream_id == 3);
  REQUIRE(events[1].is_headers());
  REQUIRE(std::get<http::headers>(events[1].payload).get(":path") == "/b");
  REQUIRE(events[2].is_stream_closed());
}

TEST_CASE("v1 server: Connection: close request → response says close and goaway fires")
{
  http::v1::engine e(http::connection_role::server);
  feed(e, "GET / HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n");
  drain(e);

  e.send_response_headers(1, 200, {}, true);
  REQUIRE(take_output(e) == "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n");
  auto events = drain(e);
  REQUIRE(events.size() == 1);
  REQUIRE(events[0].is_goaway());
  const auto& info = std::get<http::goaway_info>(events[0].payload);
  REQUIRE(info.last_stream_id == 1);
  REQUIRE(info.ec == http::error_code::no_error);
}

TEST_CASE("v1 server: response to HEAD has no body")
{
  http::v1::engine e(http::connection_role::server);
  feed(e, "HEAD / HTTP/1.1\r\nHost: example.com\r\n\r\n");
  drain(e);

  http::headers h;
  h.add("content-length", "5");
  e.send_response_headers(1, 200, h, false);
  REQUIRE(take_output(e) == "HTTP/1.1 200 OK\r\ncontent-length: 5\r\n\r\n");
  // No body is sent even if the caller tries.
  REQUIRE(e.send_data(1, bytes("hello"), true) == 0);
  REQUIRE(take_output(e).empty());
}

TEST_CASE("v1 server: Expect: 100-continue is answered before the body")
{
  http::v1::engine e(http::connection_role::server);
  feed(e, "POST / HTTP/1.1\r\nHost: example.com\r\ncontent-length: 4\r\n"
          "Expect: 100-continue\r\n\r\n");
  auto events = drain(e);
  REQUIRE(events.size() == 2);
  REQUIRE(events[0].is_new_stream());
  REQUIRE(events[1].is_headers());
  REQUIRE(take_output(e) == "HTTP/1.1 100 Continue\r\n\r\n");

  feed(e, "body");
  auto bev = drain(e);
  REQUIRE(bev.size() == 2);
  REQUIRE(bev[0].is_data());
  REQUIRE(data_text(bev[0]) == "body");
  REQUIRE(bev[1].is_stream_closed());
}

TEST_CASE("v1 round-trip: client and server engines exchange a message")
{
  http::v1::engine client(http::connection_role::client);
  http::v1::engine server(http::connection_role::server);

  auto id = client.open_stream();
  http::headers h;
  h.add("content-type", "application/json");
  client.send_request_headers(id, "POST", "/mcp", "localhost:6006", h, false);
  client.send_data(id, bytes("{}"), true);

  feed(server, take_output(client));
  auto sevents = drain(server);
  REQUIRE(sevents.size() == 4);
  REQUIRE(sevents[0].is_new_stream());
  REQUIRE(sevents[1].is_headers());
  REQUIRE(sevents[2].is_data());
  REQUIRE(data_text(sevents[2]) == "{}");
  REQUIRE(sevents[3].is_stream_closed());

  http::headers rh;
  rh.add("content-length", "0");
  server.send_response_headers(1, 202, rh, true);
  feed(client, take_output(server));
  auto cevents = drain(client);
  REQUIRE(cevents.size() == 2);
  REQUIRE(cevents[0].is_headers());
  REQUIRE(std::get<http::headers>(cevents[0].payload).get(":status") == "202");
  REQUIRE(cevents[1].is_stream_closed());
}

TEST_CASE("v1: send windows are unlimited")
{
  http::v1::engine e(http::connection_role::client);
  REQUIRE(e.connection_send_window() == std::numeric_limits<int64_t>::max());
  REQUIRE(e.stream_send_window(1) == std::numeric_limits<int64_t>::max());
  REQUIRE(e.stream_send_window(999) == std::numeric_limits<int64_t>::max());
}
