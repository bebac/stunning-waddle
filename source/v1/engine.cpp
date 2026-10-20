#include "http/v1/engine.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>
#include <limits>

namespace http::v1
{
  namespace
  {
    constexpr int64_t unlimited_window = std::numeric_limits<int64_t>::max();

    std::string to_lower(std::string_view s)
    {
      std::string out;
      out.reserve(s.size());
      for (char c : s) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
      }
      return out;
    }
  } // namespace

  engine::engine(connection_role role)
    : role_(role)
  {
  }

  // --- Sans-I/O buffers ---

  std::span<std::byte> engine::input_begin()
  {
    return std::span(
      input_buffer_.data() + input_buffered_,
      input_buffer_.size() - input_buffered_
    );
  }

  void engine::input_end(size_t n)
  {
    if (n == 0) {
      return;
    }
    input_buffered_ += n;
    parse_available();
  }

  std::span<const std::byte> engine::output_begin()
  {
    return std::span(pending_out_.data(), pending_out_.size());
  }

  void engine::output_end(size_t n)
  {
    size_t to_erase = std::min(n, pending_out_.size());
    pending_out_.erase(pending_out_.begin(), pending_out_.begin() + to_erase);
    if (pending_out_.empty()) {
      had_output_ = false;
    }
  }

  // --- Events ---

  std::optional<event> engine::next_event()
  {
    if (event_queue_.empty()) {
      return std::nullopt;
    }
    auto ev = std::move(event_queue_.front());
    event_queue_.pop_front();
    return ev;
  }

  // --- EOF ---

  void engine::eof()
  {
    if (eof_seen_) {
      return;
    }
    eof_seen_ = true;

    switch (state_)
    {
      case parse_state::body_until_close:
        // A close-delimited body ends at EOF: the message is complete.
        // Mark the connection done first so a staged pipelined request
        // head is not flushed onto a closing connection.
        connection_done_ = true;
        if (active_stream_) {
          complete_exchange(*active_stream_);
        }
        else {
          mark_connection_done();
        }
        break;

      case parse_state::start_line:
        if (role_ == connection_role::client && active_stream_) {
          // The response never arrived.
          connection_error(error_code::connection_reset);
        }
        else {
          // No exchange in flight: the peer closed an idle connection.
          mark_connection_done();
        }
        break;

      case parse_state::done:
        break;

      default:
        // headers / body_length / chunk_* / awaiting_response: the peer
        // went away in the middle of an exchange.
        connection_error(error_code::connection_reset);
        break;
    }
  }

  // --- Streams ---

  uint32_t engine::open_stream()
  {
    auto id = next_stream_id_;
    next_stream_id_ += 2;

    auto& info = streams_[id];
    info.id = id;
    stream_order_.push_back(id);
    return id;
  }

  void engine::send_request_headers(
    uint32_t stream_id,
    std::string_view method,
    std::string_view path,
    std::string_view authority,
    const headers& headers,
    bool end_stream)
  {
    auto it = streams_.find(stream_id);
    if (it == streams_.end() || it->second.head_sent) {
      return;
    }
    auto& info = it->second;
    info.method = std::string(method);
    info.head = serialize_request_head(info, method, path, authority, headers, end_stream);
    info.close_after = iequals(find_header_ci(headers, "connection"), "close");
    if (end_stream) {
      info.request_complete = true;
    }
    flush_pending();
  }

  void engine::send_response_headers(
    uint32_t stream_id,
    int status_code,
    const headers& headers,
    bool end_stream)
  {
    auto it = streams_.find(stream_id);
    if (it == streams_.end() || it->second.head_sent) {
      return;
    }
    auto& info = it->second;

    // A "Connection: close" response also closes the connection.
    if (iequals(find_header_ci(headers, "connection"), "close")) {
      info.close_after = true;
    }

    append_pending(serialize_response_head(info, status_code, headers, end_stream));
    info.head_sent = true;

    if (end_stream) {
      complete_response(stream_id);
    }
  }

  size_t engine::send_data(uint32_t stream_id, std::span<const std::byte> data, bool end_stream)
  {
    auto it = streams_.find(stream_id);
    if (
      it == streams_.end() || it->second.finished ||
      it->second.request_complete || !it->second.head_sent
    )
    {
      return 0;
    }
    auto& info = it->second;

    if (info.no_body) {
      // Responses to HEAD and 1xx/204/304 carry no body.
      if (end_stream && role_ == connection_role::server) {
        complete_response(stream_id);
      }
      return 0;
    }

    // Frame the bytes.
    std::string framed;
    if (info.chunked) {
      if (!data.empty()) {
        char size_line[32];
        auto res = std::to_chars(size_line, size_line + sizeof(size_line), data.size(), 16);
        framed.append(size_line, res.ptr);
        framed.append("\r\n");
        framed.append(reinterpret_cast<const char*>(data.data()), data.size());
        framed.append("\r\n");
      }
      if (end_stream) {
        framed.append("0\r\n\r\n");
      }
    } else {
      framed.append(reinterpret_cast<const char*>(data.data()), data.size());
    }

    if (active_stream_ == stream_id) {
      append_pending(framed);
    }
    else {
      // Pipelined: the request head has not been flushed yet, so the
      // body is staged behind it.
      info.body_staged.append(framed);
    }

    if (end_stream) {
      if (role_ == connection_role::server) {
        complete_response(stream_id);
      }
      else {
        info.request_complete = true;
      }
    }
    return data.size();
  }

  void engine::send_reset(uint32_t /*stream_id*/, std::error_code /*ec*/)
  {
    // HTTP/1.1 has no stream reset. To abort an exchange, close the
    // connection; the peer will see EOF (a connection error).
  }

  // --- Flow control: HTTP/1.1 has none ---

  int64_t engine::connection_send_window() const
  {
    return unlimited_window;
  }

  int64_t engine::stream_send_window(uint32_t /*stream_id*/) const
  {
    return unlimited_window;
  }

  // --- Send-side serialization ---

  std::string engine::serialize_request_head(
    stream_info& info,
    std::string_view method,
    std::string_view path,
    std::string_view authority,
    const headers& h,
    bool end_stream)
  {
    std::string head;
    head.reserve(256);
    head.append(method).append(" ").append(path).append(" HTTP/1.1\r\n");

    // The authority argument becomes the Host header unless the caller
    // already set one.
    if (!authority.empty() && find_header_ci(h, "host").empty()) {
      head.append("Host: ").append(authority).append("\r\n");
    }

    for (const auto& hdr : h.get()) {
      head.append(hdr.name).append(": ").append(hdr.value).append("\r\n");
    }

    if (!end_stream && find_header_ci(h, "content-length").empty()) {
      // A body follows without a known length: use chunked framing.
      info.chunked = true;
      head.append("Transfer-Encoding: chunked\r\n");
    }

    head.append("\r\n");
    return head;
  }

  std::string engine::serialize_response_head(
    stream_info& info,
    int status_code,
    const headers& h,
    bool end_stream)
  {
    std::string head;
    head.reserve(256);
    head.append("HTTP/1.1 ").append(std::to_string(status_code))
      .append(" ").append(reason_phrase(status_code)).append("\r\n");

    // Responses to HEAD and 1xx/204/304 carry no body.
    info.no_body = iequals(info.method, "HEAD") ||
      (status_code >= 100 && status_code < 200) ||
      status_code == 204 || status_code == 304;

    // If the request said "Connection: close", say so in the response.
    if (info.close_after && find_header_ci(h, "connection").empty()) {
      head.append("Connection: close\r\n");
    }

    for (const auto& hdr : h.get()) {
      head.append(hdr.name).append(": ").append(hdr.value).append("\r\n");
    }

    if (!info.no_body && !end_stream && find_header_ci(h, "content-length").empty()) {
      info.chunked = true;
      head.append("Transfer-Encoding: chunked\r\n");
    }

    head.append("\r\n");
    return head;
  }

  void engine::flush_pending()
  {
    if (active_stream_ || connection_done_) {
      return;
    }
    for (auto id : stream_order_) {
      auto it = streams_.find(id);
      if (it == streams_.end() || it->second.finished) {
        continue;
      }
      auto& info = it->second;
      if (info.head.empty()) {
        // The oldest unfinished stream has no request head yet: later
        // streams must wait (requests go on the wire in open order).
        break;
      }
      append_pending(info.head);
      append_pending(info.body_staged);
      info.head.clear();
      info.body_staged.clear();
      info.head_sent = true;
      active_stream_ = id;
      state_ = parse_state::start_line;
      return;
    }
  }

  void engine::append_pending(std::string_view bytes)
  {
    pending_out_.insert(
      pending_out_.end(),
      reinterpret_cast<const std::byte*>(bytes.data()),
      reinterpret_cast<const std::byte*>(bytes.data()) + bytes.size()
    );
    maybe_invoke_output_ready();
  }

  void engine::maybe_invoke_output_ready()
  {
    if (output_ready_cb_ && !had_output_) {
      had_output_ = true;
      output_ready_cb_();
    }
  }

  // --- Receive-side parser ---

  void engine::parse_available()
  {
    size_t offset = 0;
    while (true)
    {
      if (offset >= input_buffered_) {
        // No more data buffered.
        compact(offset);
        return;
      }
      switch (state_)
      {
        case parse_state::start_line:
        {
          if (role_ == connection_role::client && !active_stream_) {
            // Data with no exchange in flight.
            connection_error(error_code::protocol_error);
            return;
          }

          std::string_view line;
          if (!take_line(offset, line)) {
            return;
          }

          if (role_ == connection_role::client) {
            int status = 0;
            if (!parse_status_line(line, status)) {
              connection_error(error_code::protocol_error);
              return;
            }
            // Skip interim 1xx responses (except 101); the final
            // response follows on the same connection.
            if (status >= 100 && status < 200 && status != 101) {
              state_ = parse_state::skip_interim;
              break;
            }
            status_code_ = status;
            message_headers_ = headers{};
            state_ = parse_state::headers;
          }
          else
          {
            std::string_view method, target;
            if (!parse_request_line(line, method, target)) {
              connection_error(error_code::protocol_error);
              return;
            }
            auto id = next_stream_id_;
            next_stream_id_ += 2;
            auto& info = streams_[id];
            info.id = id;
            info.method = std::string(method);
            stream_order_.push_back(id);
            active_stream_ = id;
            pending_target_ = std::string(target);
            message_headers_ = headers{};
            event_queue_.emplace_back(event{id, event_type::new_stream});
            state_ = parse_state::headers;
          }
          break;
        }

        case parse_state::skip_interim:
        {
          std::string_view line;
          if (!take_line(offset, line)) {
            return;
          }
          if (line.empty()) {
            // End of the interim response's header block.
            state_ = parse_state::start_line;
          }
          break;
        }

        case parse_state::headers:
        {
          std::string_view line;
          if (!take_line(offset, line)) {
            return;
          }
          if (line.empty()) {
            // End of the header block.
            handle_headers_complete();
            break;
          }
          auto colon = line.find(':');
          if (colon != std::string_view::npos && colon > 0) {
            message_headers_.add(
              to_lower(line.substr(0, colon)),
              std::string(trim_ows(line.substr(colon + 1)))
            );
          }
          // Lines without a colon are ignored.
          break;
        }

        case parse_state::body_length:
        {
          size_t take = std::min(input_buffered_ - offset, body_remaining_);
          emit_data(offset, take);
          offset += take;
          body_remaining_ -= take;
          if (body_remaining_ == 0) {
            finish_message();
          }
          break;
        }

        case parse_state::chunk_size:
        {
          std::string_view line;
          if (!take_line(offset, line)) {
            return;
          }
          // Hex size, possibly with chunk extensions after ';'.
          auto semi = line.find(';');
          auto hexpart = trim_ows(line.substr(0, semi));
          uint64_t size = 0;
          auto res = std::from_chars(hexpart.data(), hexpart.data() + hexpart.size(), size, 16);
          if (res.ec != std::errc{} || res.ptr != hexpart.data() + hexpart.size()) {
            connection_error(error_code::protocol_error);
            return;
          }
          body_remaining_ = static_cast<size_t>(size);
          state_ = (size == 0) ? parse_state::chunk_trailer : parse_state::chunk_data;
          break;
        }

        case parse_state::chunk_data:
        {
          size_t take = std::min(input_buffered_ - offset, body_remaining_);
          emit_data(offset, take);
          offset += take;
          body_remaining_ -= take;
          if (body_remaining_ == 0) {
            state_ = parse_state::chunk_crlf;
          }
          break;
        }

        case parse_state::chunk_crlf:
        {
          std::string_view line;
          if (!take_line(offset, line)) {
            return;
          }
          // The line after chunk data must be empty (the CRLF); be
          // lenient and accept anything.
          state_ = parse_state::chunk_size;
          break;
        }

        case parse_state::chunk_trailer:
        {
          std::string_view line;
          if (!take_line(offset, line)) {
            return;
          }
          if (line.empty()) {
            // End of trailers: the message is complete.
            finish_message();
          }
          // Trailer lines are ignored.
          break;
        }

        case parse_state::body_until_close:
        {
          size_t avail = input_buffered_ - offset;
          emit_data(offset, avail);
          offset += avail;
          // The message completes at EOF (see eof()).
          break;
        }

        case parse_state::awaiting_response:
        case parse_state::done:
          compact(offset);
          return;
      }
    }
  }

  void engine::handle_headers_complete()
  {
    if (!active_stream_) {
      connection_error(error_code::protocol_error);
      return;
    }
    auto id = *active_stream_;
    auto it = streams_.find(id);
    if (it == streams_.end()) {
      connection_error(error_code::protocol_error);
      return;
    }
    auto& info = it->second;

    // Connection close semantics (shared by both roles).
    auto conn = find_header_ci(message_headers_, "connection");
    if (iequals(conn, "close") || (http_minor_ == 0 && !iequals(conn, "keep-alive"))) {
      info.close_after = true;
    }

    if (role_ == connection_role::client) {
      // Synthesize the :status pseudo-header from the status line.
      headers hdrs;
      hdrs.add(":status", std::to_string(status_code_));
      for (const auto& h : message_headers_.get()) {
        hdrs.add(h.name, h.value);
      }
      event_queue_.emplace_back(event{id, event_type::headers, std::move(hdrs)});

      // Responses to HEAD and 204/304 carry no body.
      if (iequals(info.method, "HEAD") || status_code_ == 204 || status_code_ == 304) {
        complete_exchange(id);
        return;
      }

      auto te = find_header_ci(message_headers_, "transfer-encoding");
      auto cl = find_header_ci(message_headers_, "content-length");
      if (te.find("chunked") != std::string::npos) {
        state_ = parse_state::chunk_size;
      }
      else if (!cl.empty()) {
        if (!parse_content_length(cl, body_remaining_)) {
          connection_error(error_code::protocol_error);
          return;
        }
        if (body_remaining_ == 0) {
          complete_exchange(id);
          return;
        }
        state_ = parse_state::body_length;
      }
      else {
        // No length information: the body ends at connection close.
        state_ = parse_state::body_until_close;
      }
    }
    else {
      // Synthesize the request pseudo-headers.
      headers hdrs;
      hdrs.add(":method", info.method);
      hdrs.add(":path", pending_target_);
      hdrs.add(":authority", find_header_ci(message_headers_, "host"));
      for (const auto& h : message_headers_.get()) {
        hdrs.add(h.name, h.value);
      }
      event_queue_.emplace_back(event{id, event_type::headers, std::move(hdrs)});

      // Request body framing: a request without Content-Length or
      // chunked has no body.
      auto te = find_header_ci(message_headers_, "transfer-encoding");
      auto cl = find_header_ci(message_headers_, "content-length");
      if (te.find("chunked") != std::string::npos) {
        state_ = parse_state::chunk_size;
        send_100_continue();
      }
      else if (!cl.empty()) {
        if (!parse_content_length(cl, body_remaining_)) {
          connection_error(error_code::protocol_error);
          return;
        }
        if (body_remaining_ == 0) {
          complete_request(id);
          return;
        }
        state_ = parse_state::body_length;
        send_100_continue();
      }
      else {
        complete_request(id);
        return;
      }
    }
  }

  void engine::finish_message()
  {
    if (!active_stream_) {
      connection_error(error_code::protocol_error);
      return;
    }
    if (role_ == connection_role::client) {
      complete_exchange(*active_stream_);
    }
    else {
      complete_request(*active_stream_);
    }
  }

  void engine::complete_request(uint32_t id)
  {
    event_queue_.emplace_back(event{id, event_type::stream_closed});
    last_stream_id_ = id;
    auto it = streams_.find(id);
    if (it != streams_.end()) {
      it->second.request_body_done = true;
    }
    // Do not parse the next (pipelined) request until the consumer has
    // sent the response — responses must go out in request order.
    state_ = parse_state::awaiting_response;
  }

  void engine::complete_response(uint32_t id)
  {
    last_stream_id_ = id;
    bool close = false;
    auto it = streams_.find(id);
    if (it != streams_.end()) {
      auto& info = it->second;
      info.finished = true;
      // If the request body was not fully read, the connection cannot
      // be reused (the remaining bytes cannot be resynchronized).
      close = info.close_after || !info.request_body_done;
      streams_.erase(it);
    }
    erase_stream_order(id);
    active_stream_ = std::nullopt;

    if (close || connection_done_)
    {
      mark_connection_done();
    }
    else
    {
      // Keep-alive: parse the next (possibly pipelined) request now —
      // its bytes may already be buffered, and the parser otherwise
      // only runs on input_end.
      state_ = parse_state::start_line;
      parse_available();
    }
  }

  void engine::complete_exchange(uint32_t id)
  {
    event_queue_.emplace_back(event{id, event_type::stream_closed});
    last_stream_id_ = id;
    bool close = false;
    auto it = streams_.find(id);
    if (it != streams_.end())
    {
      close = it->second.close_after;
      streams_.erase(it);
    }
    erase_stream_order(id);
    if (active_stream_ == id)
    {
      active_stream_ = std::nullopt;
    }
    if (close || connection_done_)
    {
      mark_connection_done();
    }
    else
    {
      state_ = parse_state::start_line;
      // Promote the next pipelined request, if one is staged.
      flush_pending();
    }
  }

  void engine::mark_connection_done()
  {
    connection_done_ = true;
    state_ = parse_state::done;
    active_stream_ = std::nullopt;
    if (!goaway_sent_)
    {
      goaway_sent_ = true;
      event_queue_.emplace_back(event{0, event_type::goaway, goaway_info{last_stream_id_, error_code::no_error}});
    }
  }

  void engine::connection_error(error_code ec)
  {
    if (connection_done_) {
      return;
    }
    event_queue_.emplace_back(event{0, event_type::connection_error, make_error_code(ec)});
    connection_done_ = true;
    state_ = parse_state::done;
    active_stream_ = std::nullopt;
  }

  void engine::erase_stream_order(uint32_t id)
  {
    for (auto it = stream_order_.begin(); it != stream_order_.end(); ++it)
    {
      if (*it == id)
      {
        stream_order_.erase(it);
        return;
      }
    }
  }

  void engine::compact(size_t offset)
  {
    if (offset == 0)
    {
      return;
    }
    if (offset < input_buffered_)
    {
      std::memmove(input_buffer_.data(), input_buffer_.data() + offset, input_buffered_ - offset);
    }
    input_buffered_ -= offset;
  }

  auto engine::find_line(size_t offset) const
    -> std::optional<std::pair<std::string_view, size_t>>
  {
    for (size_t i = offset; i < input_buffered_; ++i) {
      if (input_buffer_[i] == std::byte{'\n'}) {
        size_t len = i - offset;
        if (len > 0 && input_buffer_[i - 1] == std::byte{'\r'}) {
          --len;
        }
        return std::pair{
          std::string_view(reinterpret_cast<const char*>(input_buffer_.data() + offset), len),
          i + 1
        };
      }
    }
    return std::nullopt;
  }

  bool engine::take_line(size_t& offset, std::string_view& out)
  {
    auto line = find_line(offset);
    if (!line)
    {
      // Incomplete line: compact and wait for more data. A line that
      // fills the whole buffer can never complete.
      compact(offset);
      if (input_buffered_ == input_buffer_.size())
      {
        connection_error(error_code::protocol_error);
      }
      return false;
    }
    offset = line->second;
    out = line->first;
    return true;
  }

  void engine::emit_data(size_t offset, size_t n)
  {
    if (!active_stream_ || n == 0) {
      return;
    }
    event_queue_.emplace_back(
      event{
        *active_stream_,
        event_type::data,
        std::vector<std::byte>(
          input_buffer_.data() + offset,
          input_buffer_.data() + offset + n
        )
      }
    );
  }

  bool engine::parse_status_line(std::string_view line, int& status)
  {
    // "HTTP/1.x SP 3DIGIT [SP reason-phrase]"
    if (line.size() < 12) {
      return false;
    }
    if (line.substr(0, 7) != "HTTP/1.") {
      return false;
    }
    if (line[8] != ' ') {
      return false;
    }
    char minor = line[7];
    if (minor < '0' || minor > '9') {
      return false;
    }
    http_minor_ = minor - '0';
    auto code = line.substr(9, 3);
    auto res = std::from_chars(code.data(), code.data() + code.size(), status);
    return res.ec == std::errc{} && res.ptr == code.data() + code.size();
  }

  bool engine::parse_request_line(
    std::string_view line,
    std::string_view& method,
    std::string_view& target)
  {
    // "METHOD SP request-target SP HTTP/1.x"
    auto sp1 = line.find(' ');
    if (sp1 == std::string_view::npos || sp1 == 0) {
      return false;
    }
    auto sp2 = line.find(' ', sp1 + 1);
    if (sp2 == std::string_view::npos) {
      return false;
    }
    auto version = line.substr(sp2 + 1);
    if (version.size() != 8 || version.substr(0, 7) != "HTTP/1.") {
      return false;
    }
    char minor = version[7];
    if (minor < '0' || minor > '9') {
      return false;
    }
    http_minor_ = minor - '0';
    method = line.substr(0, sp1);
    target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    return !target.empty();
  }

  bool engine::parse_content_length(std::string_view value, size_t& out)
  {
    uint64_t len = 0;
    auto res = std::from_chars(value.data(), value.data() + value.size(), len);
    if (res.ec != std::errc{} || res.ptr != value.data() + value.size()) {
      return false;
    }
    out = static_cast<size_t>(len);
    return true;
  }

  void engine::send_100_continue()
  {
    if (iequals(find_header_ci(message_headers_, "expect"), "100-continue")) {
      append_pending("HTTP/1.1 100 Continue\r\n\r\n");
    }
  }

  std::string_view engine::trim_ows(std::string_view s)
  {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
      s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
      s.remove_suffix(1);
    }
    return s;
  }

  bool engine::iequals(std::string_view a, std::string_view b)
  {
    if (a.size() != b.size()) {
      return false;
    }
    for (size_t i = 0; i < a.size(); ++i)
    {
      if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
        return false;
      }
    }
    return true;
  }

  std::string engine::find_header_ci(const headers& h, std::string_view name)
  {
    for (const auto& hdr : h.get())
    {
      if (iequals(hdr.name, name)) {
        return hdr.value;
      }
    }
    return {};
  }

  std::string engine::reason_phrase(int status_code)
  {
    // Reuse the error category's status-code message table.
    auto msg = http_error_category::instance().message(status_code);
    // Its fallback for unknown codes is "HTTP <n>"; emit an empty
    // reason phrase instead (legal per RFC 7230 3.1.2).
    if (msg == "HTTP " + std::to_string(status_code)) {
      return {};
    }
    return msg;
  }
} // namespace http::v1
