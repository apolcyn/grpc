#include <stdio.h>
#include "common/google.h"
#include "common/commandlineflags.h"
#include "common/logging.h"
#include "testing/googletest.h"

// int (*grpc_ares_wrapper_socket)(int domain, int type, int protocol) = socket;
// int (*grpc_ares_wrapper_getsockname)(int sockfd, struct sockaddr *addr, socklen_t *addrlen) = getsockname;
// int (*grpc_ares_wrapper_socket_destroy)(int sockfd) = close;

// void rfc_6724_sort(grpc_lb_addresses *resolved_lb_addrs) {

// struct grpc_ares_wrapper_socket_factory_vtable {
//   int (*socket)(grpc_ares_wrapper_socket_factory *factory, int domain, int type, int protocol);
//   int (*connect)(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
//   int (*getsockname)(grpc_ares_wrapper_socket_factory *factory, int sockfd, struct sockaddr *addr, socklen_t *addrlen);
//   int (*close)(grpc_ares_wrapper_socket_factory *factory, int sockfd);
// }

struct mock_ares_wrapper_socket_factory {
  const grpc_ares_wrapper_socket_factory_vtable* vtable;
  mock_address<>
}

grpc_ares_wrapper_socket_factory_vtable ares_wrapper_socket_factory = {
  {
    socket,
    connect,
    getsockname,
    close,
  }
};

struct TestAddress {
  string dest_addr;
  
}

class GrpcLBAddress final {

 public:
  GrpcLBAddress(std::string address, bool is_balancer)
      : is_balancer(is_balancer), address(address) {}

  bool operator==(const GrpcLBAddress &other) const {
    return this->is_balancer == other.is_balancer &&
           this->address == other.address;
  }

  bool operator!=(const GrpcLBAddress &other) const {
    return !(*this == other);
  }

  bool is_balancer;
  std::string address;
};

int mock_socket(int domain, int type, int protocol) {
}

int mock_getsockname(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
}

int mock_socket_destroy(int sockfd) {
}

static void TestAddressSorting() {

}

int main(int argc, char **argv) {
  FLAGS_logtostderr = true;
  InitGoogle(argv[0], &argc, &argv, true);
  grpc_init();
  TestAddressSorting();
  grpc_shutdown();
  printf("PASS\n");
  return 0;
}
