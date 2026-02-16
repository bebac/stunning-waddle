#include "http/client_stream.h"

namespace http
{
  client_stream::client_stream(std::shared_ptr<stream_state> impl)
    : impl_(std::move(impl))
  {
  }

  void client_stream::send_headers(
    std::string_view method,
    std::string_view path,
    std::string_view authority,
    const headers& h,
    bool end_stream)
  {
    if (!impl_) {
      return;
    }
    impl_->send_headers_fn(impl_->id, method, path, authority, h, end_stream);

    if (end_stream) {
      impl_->state = stream_state_enum::half_closed_local;
    }
    else {
      impl_->state = stream_state_enum::open;
    }

    impl_->headers_sent = true;
  }

  void client_stream::send_data(std::span<const std::byte> data, bool end_stream)
  {
    if (!impl_) {
      return;
    }
    if (!impl_->headers_sent)
    {
      // TODO: Should this set an error state or throw?
      return;
    }
    impl_->send_data_fn(impl_->id, data, end_stream);
    if (end_stream)
    {
      impl_->state = stream_state_enum::half_closed_local;
    }
  }

  void client_stream::send_end()
  {
    send_data({}, true);
  }

  void client_stream::on_headers(std::function<void(const headers&)> cb)
  {
    if (impl_) {
      impl_->on_headers = std::move(cb);
    }
  }

  void client_stream::on_data(std::function<void(std::span<const std::byte>)> cb)
  {
    if (impl_) {
      impl_->on_data = std::move(cb);
    }
  }

  void client_stream::on_end(std::function<void()> cb)
  {
    if (impl_) {
      impl_->on_end = std::move(cb);
    }
  }

  void client_stream::on_reset(std::function<void(std::error_code)> cb)
  {
    if (impl_) {
      impl_->on_reset = std::move(cb);
    }
  }

  uint32_t client_stream::id() const
  {
    return impl_ ? impl_->id : 0;
  }
  stream_state_enum client_stream::state() const
  {
    return impl_ ? impl_->state : stream_state_enum::closed;
  }
}
