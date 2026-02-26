#ifndef INCLUDE_HTTP_SERVER_CONTEXT_H
#define INCLUDE_HTTP_SERVER_CONTEXT_H

#include "http/context_base.h"

namespace http
{
  class server_context : public context_base
  {
  public:
    server_context();

    void set_protocol_version(protocol_version v);

    // New stream callback - fired when client opens a stream
    void on_new_stream(std::function<void(stream, const headers&)> cb);

  protected:
    void dispatch_headers(uint32_t id, const headers& h) override;

  private:
    void register_server_callbacks();
    stream create_server_stream(uint32_t id);
    void dispatch_new_stream(uint32_t id);

  private:
    std::function<void(stream, const headers&)> on_new_stream_;
  };
}

#endif // INCLUDE_HTTP_SERVER_CONTEXT_H
