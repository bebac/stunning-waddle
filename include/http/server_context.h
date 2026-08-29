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

  protected:
    void on_new_stream_event(uint32_t id) override;

  private:
    auto create_server_stream(uint32_t id) -> stream;
  };
}

#endif // INCLUDE_HTTP_SERVER_CONTEXT_H