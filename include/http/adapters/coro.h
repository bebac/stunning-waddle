#ifndef INCLUDE_HTTP_ADAPTERS_CORO_H
#define INCLUDE_HTTP_ADAPTERS_CORO_H

#include "http/stream.h"
#include "http/headers.h"
#include "http/context_base.h"
#include "http/event.h"
#include <charconv>
#include <coroutine>
#include <exception>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace http
{

  struct response {
    int status_code = 0;
    headers response_headers;
    std::string text;
  };

  // --- Coroutine Machinery ---

  struct task_base {
    std::coroutine_handle<> handle;
    explicit task_base(std::coroutine_handle<> h) : handle(h)
    {
    }
    ~task_base()
    {
      if (handle)
        handle.destroy();
    }

    task_base(const task_base&) = delete;
    task_base& operator=(const task_base&) = delete;
    task_base(task_base&& other) noexcept : handle(std::exchange(other.handle, nullptr))
    {
    }
    task_base& operator=(task_base&& other) noexcept
    {
      if (this != &other)
      {
        if (handle)
          handle.destroy();
        handle = std::exchange(other.handle, nullptr);
      }
      return *this;
    }

    bool done() const
    {
      return handle && handle.done();
    }
    void resume()
    {
      if (handle && !handle.done())
        handle.resume();
    }
  };

  template <typename T = void> struct task : task_base {
    struct promise_type {
      std::exception_ptr exception;
      T result;

      task get_return_object() { return task(std::coroutine_handle<promise_type>::from_promise(*this)); }
      std::suspend_always initial_suspend() { return {}; }
      std::suspend_always final_suspend() noexcept { return {}; }
      void unhandled_exception() { exception = std::current_exception(); }
      void return_value(T value) { result = std::move(value); }
    };

    using task_base::task_base;

    T get()
    {
      auto& p = std::coroutine_handle<promise_type>::from_address(handle.address()).promise();
      if (p.exception)
        std::rethrow_exception(p.exception);
      return std::move(p.result);
    }
  };

  template <> struct task<void> : task_base {
    struct promise_type {
      std::exception_ptr exception;

      task get_return_object() { return task(std::coroutine_handle<promise_type>::from_promise(*this)); }
      std::suspend_always initial_suspend() { return {}; }
      std::suspend_always final_suspend() noexcept { return {}; }
      void unhandled_exception() { exception = std::current_exception(); }
      void return_void() {}
    };

    using task_base::task_base;

    void get()
    {
      auto& p = std::coroutine_handle<promise_type>::from_address(handle.address()).promise();
      if (p.exception)
        std::rethrow_exception(p.exception);
    }
  };

  // --- HTTP Awaiter & Handle ---

  struct response_state
  {
    response res{};
    std::error_code ec{};
    std::coroutine_handle<> continuation{};
    bool completed = false;

    // Pending request body data that hasn't been fully sent yet due to
    // flow control window exhaustion. `send_offset` tracks how many bytes
    // have been accepted by the engine so far.
    std::vector<std::byte> pending_body;
    size_t send_offset = 0;

    // Called when a stream_window_available event is received for this
    // stream, to attempt sending remaining body data.
    std::function<void()> try_send_remaining;
  };

  struct response_awaiter {
    std::shared_ptr<response_state> state;

    bool await_ready() const noexcept
    {
      return state->completed;
    }

    void await_suspend(std::coroutine_handle<> handle)
    {
      state->continuation = handle;
    }

    response await_resume()
    {
      if (state->ec)
        throw std::system_error(state->ec);
      return std::move(state->res);
    }
  };

  // Registry that maps stream IDs to response_state objects.
  // The event loop calls pump() to drain events from the context
  // and dispatch them to the appropriate response_state.
  class request_registry
  {
  public:
    void register_stream(uint32_t id, std::shared_ptr<response_state> state)
    {
      entries_[id] = std::move(state);
    }

    void pump(context_base& ctx)
    {
      while (ctx.has_events())
      {
        auto ev = ctx.next_event();
        if (!ev) break;
        dispatch(*ev);
      }
    }

    bool empty() const { return entries_.empty(); }

  private:
    void dispatch(const event& ev)
    {
      auto it = entries_.find(ev.stream_id);
      if (it == entries_.end()) return;
      auto& state = it->second;

      switch (ev.type)
      {
      case event_type::headers:
      {
        state->res.response_headers = std::get<headers>(ev.payload);
        auto status = state->res.response_headers.get(":status");
        if (!status.empty())
        {
          std::from_chars(status.data(),
            status.data() + status.size(),
            state->res.status_code);
        }
        break;
      }

      case event_type::data:
      {
        const auto& data_vec = std::get<std::vector<std::byte>>(ev.payload);
        state->res.text.append(
          reinterpret_cast<const char*>(data_vec.data()),
          data_vec.size());
        break;
      }

      case event_type::stream_closed:
        state->completed = true;
        if (state->continuation)
          state->continuation.resume();
        entries_.erase(it);
        break;

      case event_type::stream_reset:
        state->ec = std::get<std::error_code>(ev.payload);
        state->completed = true;
        if (state->continuation)
          state->continuation.resume();
        entries_.erase(it);
        break;

      case event_type::stream_window_available:
        if (state->try_send_remaining)
          state->try_send_remaining();
        break;

      default:
        break;
      }
    }

  private:
    std::unordered_map<uint32_t, std::shared_ptr<response_state>> entries_;
  };

  class request_handle
  {
  public:
    explicit request_handle(context_base& ctx, stream s, request_registry& registry)
      : ctx_(ctx), stream_(std::move(s)), registry_(registry)
    {
    }

    request_handle& method(std::string_view m)
    {
      method_ = m;
      return *this;
    }
    request_handle& path(std::string_view p)
    {
      path_ = p;
      return *this;
    }
    request_handle& host(std::string_view h)
    {
      host_ = h;
      return *this;
    }

    auto execute(headers h = {}, std::string_view body = "")
    {
      if (!body.empty() && h.get("content-length").empty())
      {
        h.add("content-length", std::to_string(body.size()));
      }

      // Create shared state for this request
      auto state = std::make_shared<response_state>();

      // Register with the registry so events are dispatched to this state
      registry_.register_stream(stream_.id(), state);

      // Send request immediately (eager start)
      bool has_body = !body.empty();
      stream_.send_headers(method_, path_, host_, h, !has_body);
      if (has_body)
      {
        state->pending_body.assign(
          reinterpret_cast<const std::byte*>(body.data()),
          reinterpret_cast<const std::byte*>(body.data()) + body.size());

        // Attempt initial send. If the window is exhausted before all data
        // is sent, the registry will call try_send_remaining when a
        // stream_window_available event arrives.
        auto try_send_remaining = [this, state]() {
          if (state->send_offset >= state->pending_body.size()) return;

          size_t remaining = state->pending_body.size() - state->send_offset;
          size_t sent = stream_.send_data(
            std::span<const std::byte>(state->pending_body.data() + state->send_offset, remaining),
            true);
          state->send_offset += sent;
        };

        try_send_remaining();

        if (state->send_offset < state->pending_body.size())
        {
          // Window exhausted — registry will call try_send_remaining when
          // a stream_window_available event arrives for this stream.
          state->try_send_remaining = std::move(try_send_remaining);
        }
      }

      // Return thin awaiter with shared state
      return response_awaiter{state};
    }

  private:
    context_base& ctx_;
    stream stream_;
    request_registry& registry_;
    std::string method_ = "GET";
    std::string path_ = "/";
    std::string host_;
  };
} // namespace http

#endif // INCLUDE_HTTP_ADAPTERS_CORO_H
