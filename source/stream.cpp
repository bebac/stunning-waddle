#include "http/stream.h"

namespace http
{
  stream::stream(std::shared_ptr<stream_state> impl)
    : impl_(std::move(impl))
  {
  }

  void stream::send_headers(
    std::string_view method,
    std::string_view path,
    std::string_view authority,
    const headers& h,
    bool end_stream)
  {
    if (!impl_ || !impl_->send_request_headers_fn) {
      return;
    }
    impl_->send_request_headers_fn(impl_->id, method, path, authority, h, end_stream);

    if (end_stream) {
      impl_->state = stream_state_enum::half_closed_local;
    }
    else {
      impl_->state = stream_state_enum::open;
    }

    impl_->headers_sent = true;
  }

  void stream::send_response(
    int status_code,
    const headers& h,
    bool end_stream)
  {
    if (!impl_ || !impl_->send_response_headers_fn) {
      return;
    }
    impl_->send_response_headers_fn(impl_->id, status_code, h, end_stream);

    if (end_stream) {
      impl_->state = stream_state_enum::half_closed_local;
    }
    else {
      impl_->state = stream_state_enum::open;
    }

    impl_->headers_sent = true;
  }

  size_t stream::send_data(std::span<const std::byte> data, bool end_stream)
  {
    if (!impl_) {
      return 0;
    }
    if (!impl_->headers_sent)
    {
      // TODO: Should this set an error state or throw?
      return 0;
    }
    size_t sent = impl_->send_data_fn(impl_->id, data, end_stream);
    // Only transition state if END_STREAM was actually sent (all data accepted).
    if (end_stream && sent == data.size())
    {
      impl_->state = stream_state_enum::half_closed_local;
    }
    return sent;
  }

  void stream::send_end()
  {
    send_data({}, true);
  }

  void stream::on_headers(std::function<void(const headers&)> cb)
  {
    if (impl_) {
      impl_->on_headers = std::move(cb);
    }
  }

  void stream::on_data(std::function<void(std::span<const std::byte>)> cb)
  {
    if (impl_) {
      impl_->on_data = std::move(cb);
    }
  }

  void stream::on_end(std::function<void()> cb)
  {
    if (impl_) {
      impl_->on_end = std::move(cb);
    }
  }

  void stream::on_reset(std::function<void(std::error_code)> cb)
  {
    if (impl_) {
      impl_->on_reset = std::move(cb);
    }
  }

  void stream::on_window_available(std::function<void()> cb)
  {
    if (impl_) {
      impl_->on_window_available = std::move(cb);
      // Invoke immediately if both windows are already available.
      if (stream_send_window() > 0 && connection_send_window() > 0) {
        impl_->on_window_available();
      }
    }
  }

  int64_t stream::stream_send_window() const
  {
    if (impl_ && impl_->stream_send_window_fn) {
      return impl_->stream_send_window_fn(impl_->id);
    }
    return 0;
  }

  int64_t stream::connection_send_window() const
  {
    if (impl_ && impl_->connection_send_window_fn) {
      return impl_->connection_send_window_fn();
    }
    return 0;
  }

  uint32_t stream::id() const
  {
    return impl_ ? impl_->id : 0;
  }

  stream_state_enum stream::state() const
  {
    return impl_ ? impl_->state : stream_state_enum::closed;
  }
}
