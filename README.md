# stunning-waddle

**A Modern, Sans-I/O HTTP/1.1, HTTP/2, and HTTP/3 Library for C++20.**

`stunning-waddle` is designed with a "Protocol-First" philosophy. By decoupling the HTTP state machine from the network layer, it can be integrated into any event loop, custom transport, or embedded environment without architectural friction.

## 🎯 Vision

* **Absolute Decoupling:** Not tied to any specific socket, TLS, or compression library. You provide the bytes; the library provides the logic.
* **Uniform API:** Switch between HTTP/1.1, HTTP/2, and HTTP/3 (QUIC) seamlessly using a consistent, high-level interface.
* **Modern by Default:** Target c++20 and newer.

---

## Async/Coroutines Example

Multiplexing multiple requests over a single connection, clean and simple. See https://github.com/bebac/stunning-waddle-examples

```cpp
#include <http/client_context.h>
#include <http/adapters/coro.h>

http::task<void> fect_example(http::client &client)
{
  try
  {
    // POST request with a JSON body
    auto res = co_await client.request("POST", "/anything").execute(
      {
        {"content-type", "application/json"}
      },
      R"({"message":"json post test"})"
    );

    std::cout << "[coro] Status: " << res.status_code << std::endl;
    std::cout << "[coro] Body: " << res.text << std::endl;
  }
  catch (const std::exception &e)
  {
    std::cerr << "[coro] Error: " << e.what() << std::endl;
  }
}

```

---

## Project Structure

The library is split into the core protocol engine and optional adapters.

| Component | Responsibility |
| --- | --- |
| **`v1::engine`** | HTTP/1.1 text protocol: request/response parsing, chunked framing, serial (pipelined) stream multiplexing. |
| **`v2::engine`** | Binary framing, HPACK, stream state management, and flow control. |
| **`client_context` and `server_context`** | The "Switchboard" for multiplexing and stream routing. |
| **`hpack`** | Header compression/decompression. |
| **`Adapters`** | Glue code for Coroutines. |

---

## Building & Testing

`stunning-waddle` uses CMake and automatically manages its own test dependencies via `FetchContent`.

```bash
mkdir build && cd build
cmake .. -DBUILD_TESTING=ON
cmake --build .

# Run the comprehensive test suite (doctest)
ctest -V

```
---

## Design Overview

`stunning-waddle` follows the **Sans-I/O** philosophy. The library implements the state machines for HTTP/1.1, H2, and HPACK as pure data-processing engines. It does not perform any system calls, manage sockets, or handle encryption.
### Driving the Engine

You "drive" the protocol by acting as the bridge between the network and the engine. This makes integration with any event loop (like `epoll`, `io_uring`, or `asio`) trivial.

1. **Input:** Pass raw bytes from your transport (TCP/TLS) into the engine.
2. **Process:** The engine updates state, manages flow control, and resumes coroutines.
3. **Output:** The engine prepares outgoing frames; you simply read them and write them to the wire.

```cpp
// Conceptual loop: Move bytes from Engine -> Socket and Socket -> Engine
void sync_io(http::client_context& ctx, auto& socket) {
    // 1. Flush Engine -> Network
    auto out = ctx.output_begin();
    if (!out.empty()) {
        ctx.output_end(socket.send(out.data(), out.size()));
    }

    // 2. Fill Network -> Engine
    auto in = ctx.input_begin();
    if (!in.empty()) {
        ctx.input_end(socket.recv(in.data(), in.size()));
    }
}
```

#### Output Readiness: Level-Triggered vs Edge-Triggered Driving

The library provides two mechanisms for determining when there is output to send:

**Level-Triggered (Polling):** Use `has_output()` to check if there is pending data before each I/O operation. This is ideal for simple loops or level-triggered event systems like `poll()`:

```cpp
// Level-triggered example with poll()
pollfd pfd{.fd = socket_fd, .events = POLLIN};
if (ctx.has_output()) {
    pfd.events |= POLLOUT;  // Only request writability when we have data
}
poll(&pfd, 1, timeout_ms);

if (pfd.revents & POLLOUT) {
    auto out = ctx.output_begin();
    if (!out.empty()) {
        ssize_t n = send(pfd.fd, out.data(), out.size(), 0);
        ctx.output_end(n);
    }
}
```

**Edge-Triggered (Event-Driven):** Use `on_output_ready()` to register a callback that fires when the engine transitions from having no output to having output. This is ideal for edge-triggered systems like `epoll` or `io_uring`, or to wake a sleeping event loop:

```cpp
// Edge-triggered example with epoll
ctx.on_output_ready([&]() {
    // Signal the event loop (e.g., via eventfd, self-pipe, or uv_async)
    event_loop.wake();
});

// In event loop: when EPOLLOUT fires
while (ctx.has_output()) {
    auto out = ctx.output_begin();
    ssize_t n = send(socket_fd, out.data(), out.size(), MSG_DONTWAIT);
    ctx.output_end(n);
    if (n < 0 && errno == EAGAIN) break;
}
```

**Key Differences:**
- **Level-triggered:** Simple, works well with polling. Must re-check `has_output()` before each write attempt.
- **Edge-triggered:** More efficient for high-scale I/O multiplexing. The callback fires once when output becomes available; you must drain all output in your event handler.

Both approaches can be used together: use `on_output_ready()` to wake your event loop, then use `has_output()` in a loop to drain all pending output.

### Consuming Protocol Events

After calling `input_end(n)`, the engine may have produced protocol events (headers received, data received, stream closed, etc.). Events are queued internally and own their data. Drain them via `has_events()` / `next_event()`:

```cpp
ctx.input_end(n);

while (ctx.has_events()) {
  auto ev = ctx.next_event();
  if (!ev) break;

  switch (ev->type) {
  case http::event_type::headers:
    // ev->stream_id, std::get<http::headers>(ev->payload)
    break;
  case http::event_type::data:
    // std::get<std::vector<std::byte>>(ev->payload)
    break;
  case http::event_type::stream_closed:
    // stream is done — use ctx.get_stream(ev->stream_id) if needed
    break;
  case http::event_type::stream_reset:
    // std::get<std::error_code>(ev->payload)
    break;
  case http::event_type::goaway:
    // std::get<http::goaway_info>(ev->payload)
    break;
  case http::event_type::connection_error:
    // std::get<std::error_code>(ev->payload)
    break;
  default:
    break;
  }
}
```

The context handles stream lifecycle internally — `new_stream` events are consumed by the context and never returned to the caller. Use `get_stream(id)` to retrieve a stream handle when processing a `headers` event.

---

## Roadmap

The project is currently **Client-focused**, providing a high-level fluent API for making requests. However, the underlying engines are being built for protocol symmetry.

* [x] **HTTP/2 Core:** Binary framing and HPACK compliance.
* [x] **Multiplexing:** Concurrent stream support.
* [x] **C++20 Adapters:** Coroutine-based `request_handle` for Clients.
* [x] **Server Support:** Engine-level support for accepting streams and serving responses.
* [x] **Receive Flow Control:** Automatic `WINDOW_UPDATE` generation based on consumed bytes (both connection and per-stream).
* [x] **Send Flow Control:** Enforcement of peer's advertised send windows with partial sends, window availability callbacks (stream and connection level), and immediate invocation when windows are already open.
* [ ] **HTTP/2: CONTINUATION Frames:** Split large header blocks into CONTINUATION frames when sending and reassemble them when receiving (currently a header block must fit in a single HEADERS frame).
* [ ] **HTTP/2: Stream Cancellation:** Implement `send_reset()` (RST_STREAM) — it is currently a no-op in `v2::engine`.
* [ ] **HTTP/2: PING/PONG:** Answer incoming PING frames with PONG (currently ignored) for keep-alive and connection health checks.
* [ ] **HTTP/2: Peer SETTINGS:** Honor peer-advertised `SETTINGS_MAX_FRAME_SIZE` when sending DATA and enforce `SETTINGS_MAX_CONCURRENT_STREAMS`; only `SETTINGS_INITIAL_WINDOW_SIZE` is applied today.
* [ ] **HTTP/2: Server Push:** (Optional) Server-initiated streams via PUSH_PROMISE, as outlined under Protocol Symmetry below.
* [ ] **Streaming Request Bodies:** Replace the eager-copy body model in `request_handle::execute()` with a producer or sink-style API so large request bodies can be streamed without buffering the entire payload in memory.
* [ ] **Streaming Response Bodies:** Avoid buffering the full response body in `response_state::res.text`; provide an async iterator or callback-based interface so consumers can process response data incrementally as it arrives.
* [ ] **SSE Event Streaming:** Coroutine adapter wrapping `sse_parser` so consumers can `co_await` individual `sse_event` values from a long-lived stream, with proper integration into the event / flow-control lifecycle.
* [ ] **Content-Encoding:** Support `gzip`/`deflate` decoding (and encoding), pluggable so the library stays compression-library-agnostic.
* [ ] **Server-Side Coroutine Adapter:** Coroutine adapter for servers — the current `adapters/coro.h` is client-only — so servers can `co_await` request bodies and stream responses.
* [x] **HTTP/1.1:** Text-based engine (`http::v1::engine`) behind the uniform `protocol_engine` interface, client and server roles — see "HTTP/1.1 Notes" below.
* [ ] **HTTP/1.1: Over-long header lines:** A start/header/chunk-size line that exceeds the 16 KB input buffer currently fails the connection; grow the buffer (or make the limit configurable) for line-oriented parsing.
* [ ] **HTTP/1.1: Request-Smuggling Hardening:** Reject requests that carry both `Transfer-Encoding` and `Content-Length` (RFC 7230 §3.3.3) instead of silently preferring chunked.
* [ ] **HTTP/3:** QUIC integration and UDP transport logic.
* [ ] **WebSockets:** RFC 6455 support — the `Upgrade` handshake on top of the HTTP/1.1 engine (and RFC 8441 extended CONNECT for HTTP/2), plus a Sans-I/O frame codec (text/binary/ping/pong/close, masking, fragmentation) exposed through the same stream/event model.

### HTTP/1.1 Notes

Enable HTTP/1.1 with `ctx.set_protocol_version(http::protocol_version::v1_1)` on a `client_context` or `server_context`. Design details: the header comment in `include/http/v1/engine.h`.

* **Serialization:** HTTP/1.1 has no multiplexing. The engine keeps the stream-oriented API by serializing — pipelined requests go on the wire in open order and responses are demuxed strictly in request order. The server parses one request at a time and waits for the response before parsing the next.
* **Pseudo-headers:** responses synthesize `:status`; requests synthesize `:method`, `:path`, `:authority`; received header names are lowercased — the same vocabulary as HTTP/2.
* **Flow control:** none. Send windows report "unlimited" and `send_data` always accepts the full buffer.
* **Framing:** a caller-supplied `Content-Length` passes through; otherwise bodies use `Transfer-Encoding: chunked`. The parser handles Content-Length, chunked, and close-delimited bodies, skips interim 1xx responses, treats HEAD/204/304 as bodyless, and answers `Expect: 100-continue`.
* **Connection lifecycle:** keep-alive by default, `Connection: close` handling in both directions, and `protocol_engine::eof()` (with a `context_base::eof()` passthrough) so close-delimited bodies complete at EOF and an idle EOF surfaces as `goaway`.
* **Limitations:** `send_reset` is a no-op (HTTP/1.1 has no stream reset; close the connection instead); received trailers are parsed but ignored; the engine does not auto-add `Host`, `Content-Length`, or `Connection` — the caller sets them explicitly; transport (TLS, sockets) remains the consumer's job, as with HTTP/2.
* **Line length:** a single start/header/chunk-size line is limited to the 16 KB input buffer; a longer line fails the connection (see the roadmap item above).

---

## Protocol Symmetry (Client & Server)

The "Sans-I/O" design means the core state machines do not know if they are a client or a server; they only know how to transition between HTTP states based on byte input.

While the current Adapters are built for Client use-cases, the `v2::engine` is being architected to allow:
1. **Server-side Stream Handling:** Responding to incoming `HEADERS` frames.
2. **Server Push:** (Optional) initiating streams from the server side.
3. **Common Logic:** Sharing the same HPACK and framing logic regardless of the connection role.
