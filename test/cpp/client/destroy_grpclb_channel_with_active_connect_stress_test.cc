/*
 *
 * Copyright 2017 gRPC authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <atomic>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>
#include <asm/types.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/types.h>
#include <unistd.h>
#include <net/if.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <ifaddrs.h>

#include <gmock/gmock.h>

#include <grpc/grpc.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>
#include <grpc/support/time.h>
#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/impl/codegen/sync.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>

#include "src/core/ext/filters/client_channel/parse_address.h"
#include "src/core/ext/filters/client_channel/resolver/fake/fake_resolver.h"
#include "src/core/ext/filters/client_channel/server_address.h"
#include "src/core/lib/gprpp/ref_counted_ptr.h"
#include "src/core/lib/gprpp/thd.h"
#include "src/core/lib/iomgr/sockaddr.h"

#include "test/core/util/port.h"
#include "test/core/util/test_config.h"

namespace {

std::string* g_blackhole_target = nullptr;
int g_tun_fd = -1;

void ReadTun() {
  char buffer[2000];
  for (;;) {
    int bytes = read(g_tun_fd, buffer, sizeof(buffer));
    if (bytes < 0) {
      gpr_log(GPR_ERROR, "error reading from tun device: %s", strerror(errno));
      return;
    }
    gpr_log(GPR_INFO, "read %d bytes from tun device", bytes);
  }
}

void TryConnectAndDestroy() {
  auto response_generator =
      grpc_core::MakeRefCounted<grpc_core::FakeResolverResponseGenerator>();
  // Return a grpclb address with an IP address on the IPv6 discard prefix
  // (https://tools.ietf.org/html/rfc6666). This is important because
  // the behavior we want in this test is for a TCP connect attempt to "hang",
  // i.e. we want to send SYN, and then *not* receive SYN-ACK or RST.
  // The precise behavior is dependant on the test runtime environment though,
  // since connect() attempts on this address may unfortunately result in
  // "network unreachable" errors in some test runtime environments.
  char* uri_str;
  GPR_ASSERT(g_blackhole_target != nullptr);
  gpr_asprintf(&uri_str, "ipv6:[100::1234]:443");//, g_blackhole_target->c_str());
  grpc_uri* lb_uri = grpc_uri_parse(uri_str, true);
  gpr_log(GPR_INFO, "setting lb uri string to: %s", uri_str);
  gpr_free(uri_str);
  GPR_ASSERT(lb_uri != nullptr);
  grpc_resolved_address address;
  GPR_ASSERT(grpc_parse_uri(lb_uri, &address));
  std::vector<grpc_arg> address_args_to_add = {
      grpc_channel_arg_integer_create(
          const_cast<char*>(GRPC_ARG_ADDRESS_IS_BALANCER), 1),
  };
  grpc_core::ServerAddressList addresses;
  grpc_channel_args* address_args = grpc_channel_args_copy_and_add(
      nullptr, address_args_to_add.data(), address_args_to_add.size());
  addresses.emplace_back(address.addr, address.len, address_args);
  grpc_core::Resolver::Result lb_address_result;
  lb_address_result.addresses = addresses;
  grpc_uri_destroy(lb_uri);
  response_generator->SetResponse(lb_address_result);
  grpc::ChannelArguments args;
  args.SetPointer(GRPC_ARG_FAKE_RESOLVER_RESPONSE_GENERATOR,
                  response_generator.get());
  // Explicitly set the connect deadline to the same amount of
  // time as the WaitForConnected time. The goal is to get the
  // connect timeout code to run at about the same time as when
  // the channel gets destroyed, to try to reproduce a race.
  args.SetInt("grpc.testing.fixed_reconnect_backoff_ms",
              grpc_test_slowdown_factor() * 5000);
  std::ostringstream uri;
  uri << "fake:///servername_not_used";
  auto channel = ::grpc::CreateCustomChannel(
      uri.str(), grpc::InsecureChannelCredentials(), args);
  // Start connecting, and give some time for the TCP connection attempt to the
  // unreachable balancer to begin. The connection should never become ready
  // because the LB we're trying to connect to is unreachable.
  channel->GetState(true /* try_to_connect */);
  GPR_ASSERT(
      !channel->WaitForConnected(grpc_timeout_milliseconds_to_deadline(5000)));
  GPR_ASSERT("grpclb" == channel->GetLoadBalancingPolicyName());
  channel.reset();
};

TEST(DestroyGrpclbChannelWithActiveConnectStressTest,
     LoopTryConnectAndDestroy) {
  grpc_init();
  std::vector<std::unique_ptr<std::thread>> threads;
  // 100 is picked for number of threads just
  // because it's enough to reproduce a certain crash almost 100%
  // at this time of writing.
  const int kNumThreads = 100;
  threads.reserve(kNumThreads);
  for (int i = 0; i < kNumThreads; i++) {
    threads.emplace_back(new std::thread(TryConnectAndDestroy));
  }
  for (int i = 0; i < threads.size(); i++) {
    threads[i]->join();
  }
  grpc_shutdown();
}

void wait_for_netlink_message_ack(int fd) {
  const int recv_buf_size = 8192;
  void* recv_buf = static_cast<char*>(gpr_zalloc(recv_buf_size));
  int ret = recv(fd, recv_buf, recv_buf_size, 0);
  if (ret == -1) {
    gpr_log(GPR_ERROR, "got ret:%d error:%d recving netlink message", ret, errno);
    abort();
  }
  struct nlmsghdr* response = reinterpret_cast<struct nlmsghdr*>(recv_buf);
  ASSERT_EQ(response->nlmsg_type, NLMSG_ERROR) << "unexpected nlmsghdr type";
  struct nlmsgerr* error_msg = static_cast<struct nlmsgerr*>(NLMSG_DATA(response));
  gpr_log(GPR_INFO, "received NLMSG_ERROR error:%d error str:|%s|", -error_msg->error, strerror(-error_msg->error));
  ASSERT_EQ(error_msg->error, 0);
}

int create_netlink_socket() {
  // create a netlink socket
  int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
  struct sockaddr_nl local_netlink_addr;
  memset(&local_netlink_addr, 0, sizeof(local_netlink_addr));
  local_netlink_addr.nl_family = AF_NETLINK;
  local_netlink_addr.nl_pad = 0;
  local_netlink_addr.nl_pid = getpid();
  local_netlink_addr.nl_groups = 0;
  {
    int ret = bind(fd, (struct sockaddr*) &local_netlink_addr, sizeof(local_netlink_addr));
    if (ret == -1) {
      gpr_log(GPR_ERROR, "got ret:%d error:%d binding netlink socket", ret, errno);
      abort();
    }
  }
  return fd;
}

void BlackHoleIPv6DiscardPrefix() {
  system("echo cat /proc/net/dev");
  system("cat /proc/net/dev");
  system("cat /proc/version");
  system("echo cat /proc/net/if_inet6");
  system("cat /proc/net/if_inet6");
  system("echo cat /proc/net/ipv6_route");
  system("cat /proc/net/ipv6_route");
  std::string command = "cat /proc/" + std::to_string(getpid()) + "/status";
  system(command.c_str());
  system("cat /proc/ parent ppid status");
  command = "cat /proc/" + std::to_string(getppid()) + "/status";
  system(command.c_str());
  system("echo done all cat /proc/net/dev");
  // create a tun device
  const char* tun_str = "tun0";
  {
    g_tun_fd = open("/dev/net/tun", O_RDWR);
    if (g_tun_fd < 0) {
      gpr_log(GPR_ERROR, "Error opening /dev/net/tun: |%s|", strerror(errno));
      abort();
    }
    GPR_ASSERT(g_tun_fd > 0);
    {
      struct ifreq ifr;
      memset(&ifr, 0, sizeof(ifr));
      ifr.ifr_flags = IFF_TUN;
      strncpy(ifr.ifr_name, tun_str, IFNAMSIZ);
      if (ioctl(g_tun_fd, TUNSETIFF, static_cast<void*>(&ifr)) < 0) {
        gpr_log(GPR_ERROR, "Error performing ioctl to create tun device: |%s|", strerror(errno));
        abort();
      }
      gpr_log(GPR_INFO, "created tun device: %s", ifr.ifr_name);
      system("echo cat /proc/net/dev");
      system("cat /proc/net/dev");
    }
    {
      struct ifreq ifr;
      memset(&ifr, 0, sizeof(ifr));
      ifr.ifr_flags |= IFF_TUN;
      ifr.ifr_flags |= IFF_UP;
      ifr.ifr_flags |= IFF_RUNNING;
      strncpy(ifr.ifr_name, tun_str, IFNAMSIZ);
      int sock = socket(AF_INET6, SOCK_DGRAM, 0);
      if (sock < 0) {
        gpr_log(GPR_ERROR, "error creating ipv6 udp socket: %s", strerror(errno));
        abort();
      }
      if (ioctl(sock, SIOCSIFFLAGS, static_cast<void*>(&ifr)) < 0) {
        gpr_log(GPR_ERROR, "Error performing ioctl to put tun device to UP: |%s|", strerror(errno));
        abort();
      }
      close(sock);
      gpr_log(GPR_INFO, "tun interface: %s is turned up", ifr.ifr_name);
      system("echo cat /proc/net/if_inet6");
      system("cat /proc/net/if_inet6");
      system("echo cat /proc/net/ipv6_route");
      system("cat /proc/net/ipv6_route");
    }
    char* tun_address_str = static_cast<char*>(gpr_zalloc(100));
    {
      struct ifaddrs* next = NULL;
      if (getifaddrs(&next) < 0) {
        gpr_log(GPR_ERROR, "getifaddrs failed: %s", strerror(errno));
        abort();
      }
      struct ifaddrs* head = next;
      while (next != NULL) {
        if (next->ifa_addr == nullptr) {
          gpr_log(GPR_ERROR, "getifaddrs found interface without address info: %s", next->ifa_name);
          next = next->ifa_next;
          continue;
        }
        gpr_log(GPR_INFO, "getifaddrs found address with family: %d. interface with name: %s", next->ifa_addr->sa_family, next->ifa_name);
        if (gpr_stricmp(next->ifa_name, tun_str) == 0 && next->ifa_addr->sa_family == AF_INET6 && strlen(tun_address_str) == 0) {
          struct sockaddr_in6* s_addr = reinterpret_cast<struct sockaddr_in6*>(next->ifa_addr);
          if (inet_ntop(AF_INET6, &s_addr->sin6_addr, tun_address_str, 100) == NULL) {
            gpr_log(GPR_ERROR, "inet_ntop failed: %s", strerror(errno));
            abort();
          }
        }
        next = next->ifa_next;
      }
      if (strlen(tun_address_str) == 0) {
        gpr_log(GPR_ERROR, "failed to find address of tun interface");
        abort();
      } else {
        gpr_log(GPR_ERROR, "found address of tun interface: %s", tun_address_str);
        g_blackhole_target = new std::string(tun_address_str);
      }
      freeifaddrs(head);
      system("echo cat /proc/net/if_inet6");
      system("cat /proc/net/if_inet6");
      system("echo cat /proc/net/ipv6_route");
      system("cat /proc/net/ipv6_route");
    }
  }
  // retrieve the interface index of the new tun device
  struct rtmsg create_route_body;
  memset(&create_route_body, 0, sizeof(create_route_body));
  create_route_body.rtm_family = AF_INET6;
  create_route_body.rtm_scope = RT_SCOPE_UNIVERSE;
  create_route_body.rtm_protocol = RTPROT_BOOT;
  create_route_body.rtm_type = RTN_UNICAST;
  create_route_body.rtm_table = RT_TABLE_MAIN;
  create_route_body.rtm_dst_len = 64;
  // init the destination address attibute
  struct rtattr dst_addr;
  memset(&dst_addr, 0, sizeof(dst_addr));
  dst_addr.rta_type = RTA_DST;
  dst_addr.rta_len = RTA_LENGTH(16 /* number of bytes in an ipv6 address */);
  // init the output interface index
  struct rtattr output_interface_index;
  output_interface_index.rta_type = RTA_OIF;
  output_interface_index.rta_len = RTA_LENGTH(sizeof(uint32_t));
  // init the nlmsghdr
  struct nlmsghdr create_route_header;
  memset(&create_route_header, 0, sizeof(create_route_header));
  create_route_header.nlmsg_type = RTM_NEWROUTE;
  create_route_header.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_EXCL | NLM_F_CREATE;
  create_route_header.nlmsg_len =
    NLMSG_SPACE(sizeof(create_route_body)) + RTA_ALIGN(dst_addr.rta_len) + RTA_ALIGN(output_interface_index.rta_len);
  // pack the RTNETLINK message
  void* create_route_request = gpr_zalloc(create_route_header.nlmsg_len);
  char* cur = static_cast<char*>(create_route_request);
  memcpy(cur, &create_route_header, sizeof(create_route_header));
  memcpy(NLMSG_DATA(cur), &create_route_body, sizeof(create_route_body));
  cur += NLMSG_SPACE(sizeof(create_route_body));
  memcpy(cur, &dst_addr, sizeof(dst_addr));
  ASSERT_EQ(1, inet_pton(AF_INET6, "100::", RTA_DATA(cur)));
  cur += RTA_ALIGN(dst_addr.rta_len);
  memcpy(cur, &output_interface_index, sizeof(output_interface_index));
  uint32_t interface_index = if_nametoindex(tun_str);
  ASSERT_NE(0, interface_index);
  memcpy(RTA_DATA(cur), &interface_index, sizeof(interface_index));
  cur += RTA_ALIGN(output_interface_index.rta_len);
  // construct the iovec and the overall msghdr;
  struct msghdr create_route_msghdr;
  struct iovec iov;
  memset(&iov, 0, sizeof(iov));
  iov.iov_base = create_route_request;
  iov.iov_len = create_route_header.nlmsg_len;
  memset(&create_route_msghdr, 0, sizeof(create_route_msghdr));
  struct sockaddr_nl kernel_netlink_addr;
  memset(&kernel_netlink_addr, 0, sizeof(kernel_netlink_addr));
  kernel_netlink_addr.nl_family = AF_NETLINK;
  create_route_msghdr.msg_name = &kernel_netlink_addr;
  create_route_msghdr.msg_namelen = sizeof(kernel_netlink_addr);
  create_route_msghdr.msg_iov = &iov;
  create_route_msghdr.msg_iovlen = 1;
  // send the message msghdr out on the netlink socket
  {
    int fd = create_netlink_socket();
    int ret = sendmsg(fd, &create_route_msghdr, 0);
    if (ret == -1) {
      gpr_log(GPR_ERROR, "got ret:%d error:%d (%s) sending netlink message to add a route to the tun device", ret, errno, strerror(errno));
      abort();
    }
    wait_for_netlink_message_ack(fd);
    close(fd);
  }
  system("echo cat /proc/net/if_inet6");
  system("cat /proc/net/if_inet6");
  system("echo cat /proc/net/ipv6_route");
  system("cat /proc/net/ipv6_route");
  system("echo cat /proc/net/fib_trie");
  system("cat /proc/net/fib_trie");
  system("echo cat /proc/net/dev");
  system("cat /proc/net/dev");
  system("echo donae cat /proc/net/dev");
}

}  // namespace

int main(int argc, char** argv) {
  BlackHoleIPv6DiscardPrefix();
  grpc::testing::TestEnvironment env(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  std::thread read_thread(ReadTun);
  auto result = RUN_ALL_TESTS();
  close(g_tun_fd);
  read_thread.join();
  system("echo cat /proc/net/dev");
  system("cat /proc/net/dev");
  system("echo cat /proc/net/if_inet6");
  system("cat /proc/net/if_inet6");
  system("echo cat /proc/net/ipv6_route");
  system("cat /proc/net/ipv6_route");
  abort();
  return result;
}
