//
// Created by apolcyn on 10/30/17.
//

#ifndef GRPC_ADDRESS_SORTING_H
#define GRPC_ADDRESS_SORTING_H

#include <grpc/support/port_platform.h>

#include <netinet/in.h>
#include "src/core/ext/filters/client_channel/lb_policy_factory.h"

/* Exposed for testing */
void grpc_ares_wrapper_rfc_6724_sort(grpc_lb_addresses *resolved_lb_addrs);

struct grpc_ares_wrapper_socket_factory_vtable {
  int (*socket)(struct grpc_ares_wrapper_socket_factory *factory, int domain, int type, int protocol);
  int (*connect)(struct grpc_ares_wrapper_socket_factory *factory, int sockfd, const struct sockaddr *addr, socklen_t addrlen);
  int (*getsockname)(struct grpc_ares_wrapper_socket_factory *factory, int sockfd, struct sockaddr *addr, socklen_t *addrlen);
  int (*close)(struct grpc_ares_wrapper_socket_factory *factory, int sockfd);
};

struct grpc_ares_wrapper_socket_factory {
  const grpc_ares_wrapper_socket_factory_vtable* vtable;
};

void grpc_ares_wrapper_set_socket_factory(grpc_ares_wrapper_socket_factory *factory);

extern grpc_tracer_flag grpc_trace_cares_address_sorting;

#endif //GRPC_ADDRESS_SORTING_H
