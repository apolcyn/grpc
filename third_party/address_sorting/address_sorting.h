//
// Created by apolcyn on 10/30/17.
//

#ifndef GRPC_ADDRESS_SORTING_H
#define GRPC_ADDRESS_SORTING_H

#include <grpc/support/port_platform.h>

#include <netinet/in.h>
#include "src/core/ext/filters/client_channel/lb_policy_factory.h"

#ifdef __cplusplus
extern "C" {
#endif

void address_sorting_rfc_6724_sort(grpc_lb_addresses *resolved_lb_addrs);

void address_sorting_init();
void address_sorting_shutdown();

#ifdef __cplusplus
}
#endif

/* SocketFactory interface exposed for testing */
namespace address_sorting {

class SocketFactory {
 public:
  SocketFactory() {}
  virtual ~SocketFactory() {}
  virtual int Socket(int domain, int type, int protocol) = 0;
  virtual int Connect(int sockfd, const struct sockaddr *addr,
                      socklen_t addrlen) = 0;
  virtual int GetSockName(int sockfd, struct sockaddr *addr,
                          socklen_t *addrlen) = 0;
  virtual int Close(int sockfd) = 0;
};

void OverrideSocketFactoryForTesting(SocketFactory *factory);

}  // namespace

#endif  // GRPC_ADDRESS_SORTING_H
