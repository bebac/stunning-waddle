#ifndef INCLUDE_HTTP_ADAPTERS_CORO_H
#define INCLUDE_HTTP_ADAPTERS_CORO_H

#include "http/stream.h"
#include "http/headers.h"
#include <charconv>
#include <coroutine>
#include <exception>
#include <memory>
#include <span>
#include <string>
#include <system_error>
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

  class request_handle
  {
  public:
    explicit request_handle(stream s) : stream_(std::move(s))
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

      // Set up callbacks on the stream
      stream_.on_headers([state](const headers& headers) {
        state->res.response_headers = headers;
        auto status = headers.get(":status");
        if (!status.empty())
        {
          std::from_chars(status.data(), status.data() + status.size(), state->res.status_code);
        }
      });

      stream_.on_data([state](std::span<const std::byte> data) {
        state->res.text.append(reinterpret_cast<const char*>(data.data()), data.size());
      });

      stream_.on_end([state]() {
        state->completed = true;
        if (state->continuation)
          state->continuation.resume();
      });

      stream_.on_reset([state](std::error_code error) {
        state->ec = error;
        state->completed = true;
        if (state->continuation)
          state->continuation.resume();
      });

      // Send request immediately (eager start)
      bool has_body = !body.empty();
      stream_.send_headers(method_, path_, host_, h, !has_body);
      if (has_body)
      {
        stream_.send_data({reinterpret_cast<const std::byte*>(body.data()), body.size()}, true);
      }

      // Return thin awaiter with shared state
      return response_awaiter{state};
    }

  private:
    stream stream_;
    std::string method_ = "GET";
    std::string path_ = "/";
    std::string host_;
  };
} // namespace http

#endif // INCLUDE_HTTP_ADAPTERS_CORO_H
