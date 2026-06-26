#include "http/error_codes.h"
#include <string>

namespace http {

const char* http_error_category::name() const noexcept {
  return "http";
}

std::string http_error_category::message(int ev) const {
  switch (static_cast<error_code>(ev)) {
    // Transport errors
    case error_code::no_error: return "No error";
    case error_code::protocol_error: return "Protocol error";
    case error_code::internal_error: return "Internal error";
    case error_code::flow_control_error: return "Flow control error";
    case error_code::connection_reset: return "Connection reset";
    case error_code::timeout: return "Timeout";

    // Stream errors
    case error_code::stream_closed: return "Stream closed";
    case error_code::refused_stream: return "Refused stream";
    case error_code::cancel: return "Cancelled";

    // HTTP/2 specific
    case error_code::settings_timeout: return "Settings timeout";
    case error_code::frame_size_error: return "Frame size error";
    case error_code::compression_error: return "Compression error";
    case error_code::connect_error: return "Connect error";
    case error_code::enhance_your_calm: return "Enhance your calm";
    case error_code::inadequate_security: return "Inadequate security";
    case error_code::http_1_1_required: return "HTTP/1.1 required";

    // HTTP/3 specific
    case error_code::quic_error: return "QUIC error";

    // Informational 1xx
    case error_code::continue_: return "Continue";
    case error_code::switching_protocols: return "Switching Protocols";
    case error_code::processing: return "Processing";
    case error_code::early_hints: return "Early Hints";

    // Successful 2xx
    case error_code::ok: return "OK";
    case error_code::created: return "Created";
    case error_code::accepted: return "Accepted";
    case error_code::non_authoritative_information: return "Non-Authoritative Information";
    case error_code::no_content: return "No Content";
    case error_code::reset_content: return "Reset Content";
    case error_code::partial_content: return "Partial Content";
    case error_code::multi_status: return "Multi-Status";
    case error_code::already_reported: return "Already Reported";
    case error_code::im_used: return "IM Used";

    // Redirection 3xx
    case error_code::multiple_choices: return "Multiple Choices";
    case error_code::moved_permanently: return "Moved Permanently";
    case error_code::found: return "Found";
    case error_code::see_other: return "See Other";
    case error_code::not_modified: return "Not Modified";
    case error_code::use_proxy: return "Use Proxy";
    case error_code::temporary_redirect: return "Temporary Redirect";
    case error_code::permanent_redirect: return "Permanent Redirect";

    // Client Error 4xx
    case error_code::bad_request: return "Bad Request";
    case error_code::unauthorized: return "Unauthorized";
    case error_code::payment_required: return "Payment Required";
    case error_code::forbidden: return "Forbidden";
    case error_code::not_found: return "Not Found";
    case error_code::method_not_allowed: return "Method Not Allowed";
    case error_code::not_acceptable: return "Not Acceptable";
    case error_code::proxy_authentication_required: return "Proxy Authentication Required";
    case error_code::request_timeout: return "Request Timeout";
    case error_code::conflict: return "Conflict";
    case error_code::gone: return "Gone";
    case error_code::length_required: return "Length Required";
    case error_code::precondition_failed: return "Precondition Failed";
    case error_code::payload_too_large: return "Payload Too Large";
    case error_code::uri_too_long: return "URI Too Long";
    case error_code::unsupported_media_type: return "Unsupported Media Type";
    case error_code::range_not_satisfiable: return "Range Not Satisfiable";
    case error_code::expectation_failed: return "Expectation Failed";
    case error_code::im_a_teapot: return "I'm a teapot";
    case error_code::unprocessable_entity: return "Unprocessable Entity";
    case error_code::too_early: return "Too Early";
    case error_code::upgrade_required: return "Upgrade Required";
    case error_code::precondition_required: return "Precondition Required";
    case error_code::too_many_requests: return "Too Many Requests";
    case error_code::request_header_fields_too_large: return "Request Header Fields Too Large";
    case error_code::unavailable_for_legal_reasons: return "Unavailable For Legal Reasons";

    // Server Error 5xx
    case error_code::internal_server_error: return "Internal Server Error";
    case error_code::not_implemented: return "Not Implemented";
    case error_code::bad_gateway: return "Bad Gateway";
    case error_code::service_unavailable: return "Service Unavailable";
    case error_code::gateway_timeout: return "Gateway Timeout";
    case error_code::http_version_not_supported: return "HTTP Version Not Supported";
    case error_code::variant_also_negotiates: return "Variant Also Negotiates";
    case error_code::insufficient_storage: return "Insufficient Storage";
    case error_code::loop_detected: return "Loop Detected";
    case error_code::not_extended: return "Not Extended";
    case error_code::network_authentication_required: return "Network Authentication Required";

    default:
      if (is_http_status_code(static_cast<error_code>(ev))) {
        return "HTTP " + std::to_string(ev);
      }
      return "Unknown HTTP error";
  }
}

const http_error_category& http_error_category::instance() {
  static const http_error_category category;
  return category;
}

std::error_code make_error_code(error_code ec) {
  return std::error_code(static_cast<int>(ec), http_error_category::instance());
}

std::error_condition make_error_condition(error_code ec) {
  return std::error_condition(static_cast<int>(ec), http_error_category::instance());
}

} // namespace http