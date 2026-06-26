#ifndef INCLUDE_HTTP_ERROR_CODES_H
#define INCLUDE_HTTP_ERROR_CODES_H

#include <cstdint>
#include <system_error>

namespace http {

// Unified error codes that make sense across all HTTP versions
enum class error_code : uint32_t {
  // === Transport/Connection Level Errors (HTTP/2, HTTP/3) ===
  no_error = 0x00,                  // HTTP/2: NO_ERROR
  protocol_error = 0x01,            // HTTP/2: PROTOCOL_ERROR
  internal_error = 0x02,            // HTTP/2: INTERNAL_ERROR
  flow_control_error = 0x03,        // HTTP/2: FLOW_CONTROL_ERROR
  connection_reset = 0x04,          // Generic connection reset
  timeout = 0x05,                   // Generic timeout

  // === Stream Level Errors ===
  stream_closed = 0x06,             // HTTP/2: STREAM_CLOSED
  refused_stream = 0x07,            // HTTP/2: REFUSED_STREAM
  cancel = 0x08,                    // HTTP/2: CANCEL

  // === HTTP Semantic Errors (map to status codes) ===
  // Informational 1xx
  continue_ = 100,
  switching_protocols = 101,
  processing = 102,
  early_hints = 103,

  // Successful 2xx
  ok = 200,
  created = 201,
  accepted = 202,
  non_authoritative_information = 203,
  no_content = 204,
  reset_content = 205,
  partial_content = 206,
  multi_status = 207,
  already_reported = 208,
  im_used = 226,

  // Redirection 3xx
  multiple_choices = 300,
  moved_permanently = 301,
  found = 302,
  see_other = 303,
  not_modified = 304,
  use_proxy = 305,
  temporary_redirect = 307,
  permanent_redirect = 308,

  // Client Error 4xx
  bad_request = 400,
  unauthorized = 401,
  payment_required = 402,
  forbidden = 403,
  not_found = 404,
  method_not_allowed = 405,
  not_acceptable = 406,
  proxy_authentication_required = 407,
  request_timeout = 408,
  conflict = 409,
  gone = 410,
  length_required = 411,
  precondition_failed = 412,
  payload_too_large = 413,
  uri_too_long = 414,
  unsupported_media_type = 415,
  range_not_satisfiable = 416,
  expectation_failed = 417,
  im_a_teapot = 418,
  unprocessable_entity = 422,
  too_early = 425,
  upgrade_required = 426,
  precondition_required = 428,
  too_many_requests = 429,
  request_header_fields_too_large = 431,
  unavailable_for_legal_reasons = 451,

  // Server Error 5xx
  internal_server_error = 500,
  not_implemented = 501,
  bad_gateway = 502,
  service_unavailable = 503,
  gateway_timeout = 504,
  http_version_not_supported = 505,
  variant_also_negotiates = 506,
  insufficient_storage = 507,
  loop_detected = 508,
  not_extended = 510,
  network_authentication_required = 511,

  // === HTTP/2 Specific ===
  settings_timeout = 0x1000,        // HTTP/2: SETTINGS_TIMEOUT
  frame_size_error = 0x1001,        // HTTP/2: FRAME_SIZE_ERROR
  compression_error = 0x1002,       // HTTP/2: COMPRESSION_ERROR
  connect_error = 0x1003,           // HTTP/2: CONNECT_ERROR
  enhance_your_calm = 0x1004,       // HTTP/2: ENHANCE_YOUR_CALM
  inadequate_security = 0x1005,     // HTTP/2: INADEQUATE_SECURITY
  http_1_1_required = 0x1006,       // HTTP/2: HTTP_1_1_REQUIRED

  // === HTTP/3 Specific ===
  quic_error = 0x2000
};

// HTTP version-specific error code ranges
constexpr uint32_t http_status_code_base = 100;
constexpr uint32_t http2_specific_base = 0x1000;
constexpr uint32_t http3_specific_base = 0x2000;

// Helper functions
constexpr bool is_http_status_code(error_code ec) {
  return static_cast<uint32_t>(ec) >= http_status_code_base &&
         static_cast<uint32_t>(ec) < http2_specific_base;
}

constexpr bool is_http2_specific(error_code ec) {
  return static_cast<uint32_t>(ec) >= http2_specific_base &&
         static_cast<uint32_t>(ec) < http3_specific_base;
}

constexpr bool is_http3_specific(error_code ec) {
  return static_cast<uint32_t>(ec) >= http3_specific_base;
}

constexpr bool is_transport_error(error_code ec) {
  return static_cast<uint32_t>(ec) < http_status_code_base;
}

// Error category for std::error_code integration
class http_error_category : public std::error_category {
public:
  const char* name() const noexcept override;
  std::string message(int ev) const override;
  static const http_error_category& instance();
};

std::error_code make_error_code(error_code ec);
std::error_condition make_error_condition(error_code ec);

} // namespace http

namespace std {
  template<>
  struct is_error_code_enum<http::error_code> : std::true_type {};
}

#endif // INCLUDE_HTTP_ERROR_CODES_H