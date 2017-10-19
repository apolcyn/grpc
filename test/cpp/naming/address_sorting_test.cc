#include <grpc/grpc.h>
#include <grpc/support/alloc.h>
#include <grpc/support/host_port.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>
#include <grpc/support/sync.h>
#include <grpc/support/time.h>
#include <string.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <gflags/gflags.h>
#include <gmock/gmock.h>
#include <vector>

#include "test/cpp/util/subprocess.h"
#include "test/cpp/util/test_config.h"

extern "C" {
#include "src/core/ext/filters/client_channel/client_channel.h"
#include "src/core/ext/filters/client_channel/resolver.h"
#include "src/core/ext/filters/client_channel/resolver/dns/c_ares/grpc_ares_wrapper.h"
#include "src/core/ext/filters/client_channel/resolver_registry.h"
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/iomgr/combiner.h"
#include "src/core/lib/iomgr/executor.h"
#include "src/core/lib/iomgr/iomgr.h"
#include "src/core/lib/iomgr/resolve_address.h"
#include "src/core/lib/iomgr/sockaddr_utils.h"
#include "src/core/lib/support/env.h"
#include "src/core/lib/support/string.h"
#include "test/core/util/port.h"
#include "test/core/util/test_config.h"
}


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

namespace {

struct TestAddress {
  std::string dest_addr;
  int family;
};

struct MockAresWrapperSocketFactory {
  const grpc_ares_wrapper_socket_factory_vtable* vtable;
  // user configured test config
  bool ipv4_supported;
  bool ipv6_supported;
  std::map<std::string, TestAddress> dest_addr_to_src_addr;
  // internal state for mock
  std::map<int, TestAddress> fd_to_getsockname_return_vals;
  int cur_socket;
};

int MockSocket(grpc_ares_wrapper_socket_factory *factory, int domain, int type, int protocol) {
  MockAresWrapperSocketFactory *mock = (MockAresWrapperSocketFactory*)factory;
  gpr_log(GPR_INFO, "domain is: %d", domain);
  EXPECT_TRUE(domain == AF_INET || domain == AF_INET6);
  if ((domain == AF_INET && !mock->ipv4_supported) || (domain == AF_INET6 && !mock->ipv6_supported)) {
    errno = EAFNOSUPPORT;
    return -1;
  }
  return mock->cur_socket++;
}

int MockConnect(grpc_ares_wrapper_socket_factory *factory, int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
  MockAresWrapperSocketFactory *mock = (MockAresWrapperSocketFactory*)factory;
  if ((addr->sa_family == AF_INET && !mock->ipv4_supported) || (addr->sa_family == AF_INET6 && !mock->ipv6_supported)) {
    errno = EAFNOSUPPORT;
    return -1;
  }
  char *ip_addr_str;
  grpc_resolved_address resolved_addr;
  memcpy(&resolved_addr.addr, addr, addrlen);
  resolved_addr.len = addrlen;
  char ntop_buf[INET6_ADDRSTRLEN + 1];
  ntop_buf[INET6_ADDRSTRLEN] = 0;
  if (addr->sa_family == AF_INET) {
    gpr_log(GPR_INFO, "convert AF_INET sock to str");
    inet_ntop(AF_INET, &((sockaddr_in*)addr)->sin_addr, ntop_buf, sizeof(ntop_buf));
    gpr_log(GPR_INFO, "str: %s", ntop_buf);
  }
  if (addr->sa_family == AF_INET6) {
    gpr_log(GPR_INFO, "convert AF_INET6 sock to str");
    inet_ntop(AF_INET6, &((sockaddr_in6*)addr)->sin6_addr, ntop_buf, sizeof(ntop_buf));
    gpr_log(GPR_INFO, "str: %s", ntop_buf);
  }
  grpc_sockaddr_to_string(&ip_addr_str, &resolved_addr, false /* normalize */);
  auto it = mock->dest_addr_to_src_addr.find(ip_addr_str);
  if (it == mock->dest_addr_to_src_addr.end()) {
    gpr_log(GPR_DEBUG, "can't find |%s| in dest to src map", ip_addr_str);
    errno = ENETUNREACH;
    return -1;
  }
  mock->fd_to_getsockname_return_vals.insert(std::pair<int, TestAddress>(sockfd, it->second));
  return 0;
}

grpc_resolved_address TestAddressToGrpcResolvedAddress(TestAddress test_addr) {
  char *host;
  char *port;
  grpc_resolved_address resolved_addr;
  gpr_split_host_port(test_addr.dest_addr.c_str(), &host, &port);
  if (test_addr.family == AF_INET) {
    sockaddr_in in_dest;
    in_dest.sin_port = htons(atoi(port));;
    in_dest.sin_family = AF_INET;
    int zeros[2]{0, 0};
    memcpy(&in_dest.sin_zero, &zeros, 8);
    GPR_ASSERT(inet_pton(AF_INET, host, &in_dest.sin_addr) == 1);
    memcpy(&resolved_addr.addr, &in_dest, sizeof(sockaddr_in));
    resolved_addr.len = sizeof(sockaddr_in);
  } else {
    GPR_ASSERT(test_addr.family == AF_INET6);
    sockaddr_in6 in6_dest;
    in6_dest.sin6_port = htons(atoi(port));
    in6_dest.sin6_family = AF_INET6;
    in6_dest.sin6_flowinfo = 0;
    in6_dest.sin6_scope_id = 0;
    GPR_ASSERT(inet_pton(AF_INET6, host, &in6_dest.sin6_addr) == 1);
    memcpy(&resolved_addr.addr, &in6_dest, sizeof(sockaddr_in6));
    resolved_addr.len = sizeof(sockaddr_in6);

    char ntop_buf[INET6_ADDRSTRLEN + 1];
    ntop_buf[INET6_ADDRSTRLEN] = 0;
    gpr_log(GPR_INFO, "1st convert AF_INET6 sock to str");
    inet_ntop(AF_INET6, &in6_dest.sin6_addr, ntop_buf, sizeof(ntop_buf));
    gpr_log(GPR_INFO, "1st str: %s", ntop_buf);
  }
  return resolved_addr;
}

int MockGetSockName(grpc_ares_wrapper_socket_factory *factory, int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
  MockAresWrapperSocketFactory *mock = (MockAresWrapperSocketFactory*)factory;
  auto it = mock->fd_to_getsockname_return_vals.find(sockfd);
  EXPECT_TRUE(it != mock->fd_to_getsockname_return_vals.end());
  grpc_resolved_address resolved_addr = TestAddressToGrpcResolvedAddress(it->second);
  memcpy(addr, &resolved_addr.addr, resolved_addr.len);
  *addrlen = resolved_addr.len;
  return 0;
}

int MockClose(grpc_ares_wrapper_socket_factory *factory, int sockfd) {
  return 0;
}

grpc_ares_wrapper_socket_factory_vtable mock_ares_wrapper_socket_factory_vtable =  {
    MockSocket,
    MockConnect,
    MockGetSockName,
    MockClose,
};

grpc_lb_addresses *BuildLbAddrInputs(std::vector<TestAddress> test_addrs) {
  grpc_lb_addresses *lb_addrs = grpc_lb_addresses_create(0, NULL);
  lb_addrs->addresses = (grpc_lb_address*)gpr_zalloc(sizeof(grpc_lb_address) * test_addrs.size());
  lb_addrs->num_addresses = test_addrs.size();
  for (size_t i = 0; i < test_addrs.size(); i++) {
    lb_addrs->addresses[i].address = TestAddressToGrpcResolvedAddress(test_addrs[i]);
  }
  return lb_addrs;
}

void VerifyLbAddrOutputs(grpc_lb_addresses *lb_addrs, std::vector<std::string> expected_addrs) {
  EXPECT_EQ(lb_addrs->num_addresses, expected_addrs.size());
  for (size_t i = 0; i < lb_addrs->num_addresses; i++) {
    char *ip_addr_str;
    grpc_sockaddr_to_string(&ip_addr_str, &lb_addrs->addresses[i].address, false /* normalize */);
    EXPECT_EQ(expected_addrs[i], ip_addr_str);
    gpr_free(ip_addr_str);
  }
}

MockAresWrapperSocketFactory *NewMockAresWrapperSocketFactory() {
  MockAresWrapperSocketFactory *factory = new MockAresWrapperSocketFactory();
  factory->vtable = &mock_ares_wrapper_socket_factory_vtable;
  grpc_ares_wrapper_set_socket_factory((grpc_ares_wrapper_socket_factory*)factory);
  return factory;
}

} // namespace

TEST(AddressSortingTest, TestDepriotizesUnreachableAddresses) {
  auto mock = NewMockAresWrapperSocketFactory();
  mock->ipv4_supported = true;
  mock->ipv6_supported = true;
  mock->dest_addr_to_src_addr = {
    {"1.2.3.4:443", {"4.3.2.1:443", AF_INET}},
  };
  grpc_lb_addresses *lb_addrs = BuildLbAddrInputs({
    {"1.2.3.4:443", AF_INET},
    {"5.6.7.8:443", AF_INET},
  });
  grpc_ares_wrapper_rfc_6724_sort(lb_addrs);
  VerifyLbAddrOutputs(lb_addrs, {
    "1.2.3.4:443",
    "5.6.7.8:443",
  });
}

TEST(AddressSortingTest, TestDepriotizesUnsupportedDomainIpv6) {
  auto mock = NewMockAresWrapperSocketFactory();
  mock->ipv4_supported = true;
  mock->ipv6_supported = false;
  mock->dest_addr_to_src_addr = {
    {"1.2.3.4:443", {"4.3.2.1:0", AF_INET}},
  };
  grpc_lb_addresses *lb_addrs = BuildLbAddrInputs({
    {"[2607:f8b0:400a:801::1002]:443", AF_INET6},
    {"1.2.3.4:443", AF_INET},
  });
  grpc_ares_wrapper_rfc_6724_sort(lb_addrs);
  VerifyLbAddrOutputs(lb_addrs, {
    "1.2.3.4:443",
    "[2607:f8b0:400a:801::1002]:443",
  });
}

TEST(AddressSortingTest, TestDepriotizesUnsupportedDomainIpv4) {
  auto mock = NewMockAresWrapperSocketFactory();
  mock->ipv4_supported = false;
  mock->ipv6_supported = true;
  mock->dest_addr_to_src_addr = {
    {"1.2.3.4:443", {"4.3.2.1:0", AF_INET}},
  };
  grpc_lb_addresses *lb_addrs = BuildLbAddrInputs({
    {"[2607:f8b0:400a:801::1002]:443", AF_INET6},
    {"1.2.3.4:443", AF_INET},
  });
  grpc_ares_wrapper_rfc_6724_sort(lb_addrs);
  VerifyLbAddrOutputs(lb_addrs, {
    "[2607:f8b0:400a:801::1002]:443",
    "1.2.3.4:443",
  });
}

TEST(AddressSortingTest, TestDepriotizesNonMatchingScope) {
  auto mock = NewMockAresWrapperSocketFactory();
  mock->ipv4_supported = true;
  mock->ipv6_supported = true;
  mock->dest_addr_to_src_addr = {
    {"[2000:f8b0:400a:801::1002]:443", {"[fec0::1000]:0", AF_INET6}}, // global and site-local scope
    {"[2111:f8b0:400a:801::1002]:443", {"[2222:f8b0:400a:801::1002]:0", AF_INET6}}, // global and global scope
    {"[fec0::5000]:443", {"[fec0::5001]:0", AF_INET6}}, // site-local and site-local scope
    {"[fe80::6000]:443", {"[fe80::6001]:0", AF_INET6}}, // link-local and link-local scope
  };
  grpc_lb_addresses *lb_addrs = BuildLbAddrInputs({
    {"[2000:f8b0:400a:801::1002]:443", AF_INET6},
    {"[2111:f8b0:400a:801::1002]:443", AF_INET6},
    {"[fec0::5000]:443", AF_INET6},
    {"[fe80::6000]:443", AF_INET6},
  });
  grpc_ares_wrapper_rfc_6724_sort(lb_addrs);
  VerifyLbAddrOutputs(lb_addrs, {
    "[fec0::5000]:443",
    "[fe80::6000]:443",
    "[2111:f8b0:400a:801::1002]:443",
    "[2000:f8b0:400a:801::1002]:443",
  });
}

int main(int argc, char **argv) {
  grpc_init();
  grpc_test_init(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  auto result = RUN_ALL_TESTS();
  grpc_shutdown();
  return result;
}
