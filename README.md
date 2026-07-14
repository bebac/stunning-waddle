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

---

## Roadmap

The project is currently **Client-focused**, providing a high-level fluent API for making requests. However, the underlying engines are being built for protocol symmetry.

* [x] **HTTP/2 Core:** Binary framing and HPACK compliance.
* [x] **Multiplexing:** Concurrent stream support.
* [x] **C++20 Adapters:** Coroutine-based `request_handle` for Clients.
* [x] **Server Support:** Engine-level support for accepting streams and serving responses.
* [x] **Receive Flow Control:** Automatic `WINDOW_UPDATE` generation based on consumed bytes (both connection and per-stream).
* [ ] **Send Flow Control:** Enforcement of peer's advertised send windows at the API level (tracking implemented, enforcement pending).
* [ ] **HTTP/1.1:** Implementation of the text-based engine.
* [ ] **HTTP/3:** QUIC integration and UDP transport logic.

---

## Protocol Symmetry (Client & Server)

The "Sans-I/O" design means the core state machines do not know if they are a client or a server; they only know how to transition between HTTP states based on byte input.

While the current Adapters are built for Client use-cases, the `v2::engine` is being architected to allow:
1. **Server-side Stream Handling:** Responding to incoming `HEADERS` frames.
2. **Server Push:** (Optional) initiating streams from the server side.
3. **Common Logic:** Sharing the same HPACK and framing logic regardless of the connection role.
