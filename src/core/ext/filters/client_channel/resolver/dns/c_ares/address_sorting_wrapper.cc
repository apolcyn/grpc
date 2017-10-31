//
// Created by apolcyn on 10/30/17.
//

#include <grpc/support/port_platform.h>
#include "address_sorting_wrapper.h"
#include "third_party/address_sorting/address_sorting.h"

void grpc_ares_wrapper_set_socket_factory(grpc_ares_wrapper_socket_factory *factory) {
  grpc_ares_wrapper_set_socket_factory_internal(factory);
}

void grpc_ares_wrapper_rfc_6724_sort(grpc_lb_addresses *resolved_lb_addrs) {
  grpc_ares_wrapper_rfc_6724_sort_internal(resolved_lb_addrs);
}
