#ifndef INCLUDE_HTTP_V1_ENGINE_H
#define INCLUDE_HTTP_V1_ENGINE_H

#include "http/protocol_engine.h"
#include "http/event.h"
#include "http/headers.h"
#include "http/error_codes.h"

#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace http::v1
{
  // HTTP/1.1 protocol engine.
  //
  // HTTP/1.1 has no multiplexing: requests on a connection are serial
  // and responses arrive in request order. This engine keeps the
  // uniform stream-oriented protocol_engine API by treating each
  // request/response exchange as a stream:
  //
  //   - Stream IDs follow the HTTP/2 client-initiated convention
  //     (odd, starting at 1) on both roles; the server assigns IDs to
  //     incoming requests.
  //   - Client: pipelined requests are serialized in open order. A
  //     request head is staged until all earlier exchanges have
  //     completed, then flushed. Responses are demuxed to stream IDs
  //     strictly in request order (the only order HTTP/1.1 guarantees).
  //   - Server: one request is parsed at a time; the next (pipelined)
  //     request is not parsed until the current response completes.
  //
  // Event mapping (uniform http::event vocabulary):
  //   - new_stream       server: request line parsed
  //   - headers          response: ":status" synthesized from the status
  //                      line; request: ":method", ":path", ":authority"
  //                      synthesized from the request line and Host.
  //                      Header names are lowercased on receive.
  //   - data             body bytes (dechunked)
  //   - stream_closed    message complete (end of body)
  //   - connection_error premature EOF mid-message (connection_reset)
  //                      or malformed message (protocol_error)
  //   - goaway           connection done: after a "Connection: close"
  //                      exchange, or EOF while no exchange is in flight
  //   - stream_reset / window events never fire; send_reset is a no-op
  //
  // Flow control: HTTP/1.1 has none. Send windows report "unlimited"
  // (INT64_MAX) and send_data always accepts the full buffer.
  //
  // Framing: on send, a caller-supplied Content-Length is passed
  // through (identity); otherwise a body is sent with
  // Transfer-Encoding: chunked. On receive, Content-Length, chunked,
  // and close-delimited bodies are supported. Responses to HEAD and
  // 1xx/204/304 statuses carry no body.
  //
  // EOF: call eof() when the peer closes the connection (read == 0).
  class engine : public protocol_engine
  {
  public:
    explicit engine(connection_role role = connection_role::client);

    // --- Sans-I/O buffers ---
    auto input_begin() -> std::span<std::byte> override;
    void input_end(size_t n) override;
    auto output_begin() -> std::span<const std::byte> override;
    void output_end(size_t n) override;
    bool has_output() const override { return !pending_out_.empty(); }
    void on_output_ready(output_ready_callback cb) override { output_ready_cb_ = std::move(cb); }

    // --- Events ---
    bool has_events() const override { return !event_queue_.empty(); }
    auto next_event() -> std::optional<event> override;

    // --- EOF ---
    void eof() override;

    // --- Streams ---
    uint32_t open_stream() override;

    void send_request_headers(
      uint32_t stream_id,
      std::string_view method,
      std::string_view path,
      std::string_view authority,
      const headers& headers,
      bool end_stream
    ) override;

    void send_response_headers(
      uint32_t stream_id,
      int status_code,
      const headers& headers,
      bool end_stream
    ) override;

    auto send_data(uint32_t stream_id, std::span<const std::byte> data, bool end_stream) -> size_t override;
    void send_reset(uint32_t stream_id, std::error_code ec) override;

    // --- Flow control: HTTP/1.1 has none ---
    auto connection_send_window() const -> int64_t override;
    auto stream_send_window(uint32_t stream_id) const -> int64_t override;

  private:
    enum class parse_state
    {
      start_line,        // waiting for request/status line
      skip_interim,      // client: skipping an interim 1xx response's headers
      headers,           // waiting for header lines
      body_length,       // Content-Length delimited body
      chunk_size,        // waiting for chunk size line
      chunk_data,        // inside chunk data
      chunk_crlf,        // waiting for CRLF after chunk data
      chunk_trailer,     // waiting for trailer lines after the 0-chunk
      body_until_close,  // close-delimited body (responses only)
      awaiting_response, // server: request complete, response not yet sent
      done               // connection finished
    };

    struct stream_info
    {
      uint32_t id = 0;
      std::string method;             // request method (HEAD detection)
      std::string head;               // client: staged request head, empty once flushed
      std::string body_staged;        // client: staged body bytes (pipelined)
      bool head_sent = false;         // start line + headers serialized
      bool finished = false;          // exchange complete
      bool chunked = false;           // send-side: chunked framing
      bool no_body = false;           // response carries no body (HEAD/204/304/1xx)
      bool request_body_done = false; // server: request body fully read
      bool request_complete = false;  // client: request fully sent
      bool close_after = false;       // connection closes after this exchange
    };

    // Receive-side parser
    void parse_available();
    void compact(size_t offset);
    auto find_line(size_t offset) const -> std::optional<std::pair<std::string_view, size_t>>;
    auto take_line(size_t& offset, std::string_view& out) -> bool;
    void handle_headers_complete();
    void finish_message();
    void emit_data(size_t offset, size_t n);

    // Exchange lifecycle
    void complete_request(uint32_t id);   // server: request body complete
    void complete_response(uint32_t id);  // server: response complete
    void complete_exchange(uint32_t id);  // client: response complete
    void mark_connection_done();
    void connection_error(error_code ec);
    void erase_stream_order(uint32_t id);

    // Send-side
    void flush_pending();

    auto serialize_request_head(
      stream_info& info,
      std::string_view method,
      std::string_view path,
      std::string_view authority,
      const headers& h,
      bool end_stream
    ) -> std::string;

    auto serialize_response_head(
      stream_info& info,
      int status_code,
      const headers& h,
      bool end_stream
    ) -> std::string;

    void append_pending(std::string_view bytes);
    void maybe_invoke_output_ready();

    // Parsing helpers
    auto parse_status_line(std::string_view line, int& status) -> bool;
    auto parse_request_line(std::string_view line, std::string_view& method, std::string_view& target) -> bool;
    auto parse_content_length(std::string_view value, size_t& out) -> bool;
    void send_100_continue();

    // Header helpers
    static auto trim_ows(std::string_view s) -> std::string_view;
    static auto iequals(std::string_view a, std::string_view b) -> bool;
    static auto find_header_ci(const headers& h, std::string_view name) -> std::string;
    static auto reason_phrase(int status_code) -> std::string;

  private:
    connection_role role_;
    parse_state state_ = parse_state::start_line;
    std::array<std::byte, 16384> input_buffer_;
    size_t input_buffered_ = 0;
    std::vector<std::byte> pending_out_;
    output_ready_callback output_ready_cb_;
    bool had_output_ = false;

    std::unordered_map<uint32_t, stream_info> streams_;
    std::deque<uint32_t> stream_order_;  // opened streams in ID order
    uint32_t next_stream_id_ = 1;
    std::optional<uint32_t> active_stream_;

    // Receive-side parser state
    int status_code_ = 0;
    int http_minor_ = 1;      // peer's HTTP version (0 = HTTP/1.0)
    size_t body_remaining_ = 0;
    headers message_headers_; // headers of the message being parsed
    std::string pending_target_;

    // Connection lifecycle
    bool connection_done_ = false;
    bool goaway_sent_ = false;
    bool eof_seen_ = false;
    uint32_t last_stream_id_ = 0;

    std::deque<event> event_queue_;
  };
} // namespace http::v1

#endif // INCLUDE_HTTP_V1_ENGINE_H
