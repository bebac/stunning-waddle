#ifndef INCLUDE_HTTP_CLIENT_CONTEXT_H
#define INCLUDE_HTTP_CLIENT_CONTEXT_H

#include "http/context_base.h"

namespace http
{
  class client_context : public context_base
  {
  public:
    client_context();

    void set_protocol_version(protocol_version v);

    // Client opens streams
    auto open_stream() -> stream;

  protected:
    void dispatch_headers(uint32_t id, const headers& h) override;
  };
}

#endif // INCLUDE_HTTP_CLIENT_CONTEXT_H
