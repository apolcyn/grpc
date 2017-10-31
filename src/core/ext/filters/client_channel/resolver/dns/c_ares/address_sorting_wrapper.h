//
// Created by apolcyn on 10/30/17.
//

#ifndef GRPC_ADDRESS_SORTING_WRAPPER_H
#define GRPC_ADDRESS_SORTING_WRAPPER_H

#include "third_party/address_sorting/address_sorting.h"

void grpc_ares_wrapper_set_socket_factory(grpc_ares_wrapper_socket_factory *factory);

void grpc_ares_wrapper_rfc_6724_sort(grpc_lb_addresses *resolved_lb_addrs);

#endif //GRPC_ADDRESS_SORTING_WRAPPER_H
