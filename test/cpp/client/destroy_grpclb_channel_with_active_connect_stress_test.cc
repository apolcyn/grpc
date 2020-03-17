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
  gpr_asprintf(&uri_str, "ipv6:[0100::1234]:443");
  grpc_uri* lb_uri = grpc_uri_parse(uri_str, true);
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
              grpc_test_slowdown_factor() * 100);
  std::ostringstream uri;
  uri << "fake:///servername_not_used";
  auto channel = ::grpc::CreateCustomChannel(
      uri.str(), grpc::InsecureChannelCredentials(), args);
  // Start connecting, and give some time for the TCP connection attempt to the
  // unreachable balancer to begin. The connection should never become ready
  // because the LB we're trying to connect to is unreachable.
  channel->GetState(true /* try_to_connect */);
  GPR_ASSERT(
      !channel->WaitForConnected(grpc_timeout_milliseconds_to_deadline(100)));
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

void BlackHoleIPv6DiscardPrefix() {
  system("echo cat /proc/net/dev");
  system("cat /proc/net/dev");
  std::string command = "cat /proc/" + std::to_string(getpid()) + "/status";
  system(command.c_str());
  system("cat /proc/ parent ppid status");
  command = "cat /proc/" + std::to_string(getppid()) + "/status";
  system(command.c_str());
  system("echo done all cat /proc/net/dev");
  // init the ifinfomsg
  struct ifinfomsg create_dummy_device_body;
  memset(&create_dummy_device_body, 0, sizeof(create_dummy_device_body));
  create_dummy_device_body.ifi_change = 0xFFFFFFFF;
  // init the dev name rtattr
  const char* dummy_str = "dummy0";
  struct rtattr dummy_device_name;
  dummy_device_name.rta_type = IFLA_IFNAME;
  dummy_device_name.rta_len = RTA_LENGTH(strlen(dummy_str));
  // init the dev type rtattr
  const char* dummy_type_str = "dummy";
  struct rtattr dummy_device_type;
  dummy_device_type.rta_type = IFLA_INFO_KIND;
  dummy_device_type.rta_len = RTA_LENGTH(strlen(dummy_type_str));
  // init the outer IFLA_LINKINFO attribute
  struct rtattr ifla_linkinfo_attr;
  ifla_linkinfo_attr.rta_type = IFLA_LINKINFO;
  ifla_linkinfo_attr.rta_len = RTA_SPACE(0) + dummy_device_type.rta_len;
  // init the nlmsghdr
  struct nlmsghdr create_dummy_device_header;
  memset(&create_dummy_device_header, 0, sizeof(create_dummy_device_header));
  create_dummy_device_header.nlmsg_len =
    NLMSG_SPACE(sizeof(struct ifinfomsg)) + RTA_ALIGN(dummy_device_name.rta_len) + RTA_ALIGN(ifla_linkinfo_attr.rta_len);
  create_dummy_device_header.nlmsg_type = RTM_NEWLINK;
  create_dummy_device_header.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_EXCL | NLM_F_CREATE;
  // construct the entire RTNETLINK message
  // RTA_LENGTH != RTA_SPACE
  void* create_dummy_device_request = gpr_zalloc(create_dummy_device_header.nlmsg_len);
  char* cur = static_cast<char*>(create_dummy_device_request);
  memcpy(cur, &create_dummy_device_header, sizeof(create_dummy_device_header));
  memcpy(NLMSG_DATA(cur), &create_dummy_device_body, sizeof(create_dummy_device_body));
  cur += NLMSG_SPACE(sizeof(create_dummy_device_body));
  memcpy(cur, &dummy_device_name, sizeof(dummy_device_name));
  memcpy(RTA_DATA(cur), dummy_str, strlen(dummy_str));
  cur += RTA_ALIGN(dummy_device_name.rta_len);
  memcpy(cur, &ifla_linkinfo_attr, sizeof(ifla_linkinfo_attr));
  cur += RTA_SPACE(0);
  memcpy(cur, &dummy_device_type, sizeof(dummy_device_type));
  memcpy(RTA_DATA(cur), dummy_type_str, strlen(dummy_type_str));
  cur += RTA_ALIGN(dummy_device_type.rta_len);
  // TODO: fill in kind rtattr
  // construct the iovec and the overall msghdr;
  struct iovec iov;
  iov.iov_base = create_dummy_device_request;
  iov.iov_len = create_dummy_device_header.nlmsg_len;
  struct msghdr create_dummy_device_msghdr;
  memset(&create_dummy_device_msghdr, 0, sizeof(create_dummy_device_msghdr));
  struct sockaddr_nl kernel_netlink_addr;
  memset(&kernel_netlink_addr, 0, sizeof(kernel_netlink_addr));
  kernel_netlink_addr.nl_family = AF_NETLINK;
  create_dummy_device_msghdr.msg_name = &kernel_netlink_addr;
  create_dummy_device_msghdr.msg_namelen = sizeof(kernel_netlink_addr);
  create_dummy_device_msghdr.msg_iov = &iov;
  create_dummy_device_msghdr.msg_iovlen = 1;
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
  // send the message msghdr out on the netlink socket
  {
    int ret = sendmsg(fd, &create_dummy_device_msghdr, 0);
    if (ret == -1) {
      gpr_log(GPR_ERROR, "got ret:%d error:%d sending netlink message", ret, errno);
      abort();
    }
  }
  {
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
  system("echo cat /proc/net/dev");
  system("cat /proc/net/dev");
  system("echo done cat /proc/net/dev");
}

}  // namespace

int main(int argc, char** argv) {
  BlackHoleIPv6DiscardPrefix();
  grpc::testing::TestEnvironment env(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  auto result = RUN_ALL_TESTS();
  abort();
  return result;
}
