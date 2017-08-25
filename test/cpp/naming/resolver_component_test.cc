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

#include <grpc/grpc.h>
#include <grpc/support/alloc.h>
#include <grpc/support/host_port.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>
#include <grpc/support/sync.h>
#include <grpc/support/time.h>
#include <string.h>

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

using std::vector;
using grpc::SubProcess;
using testing::UnorderedElementsAreArray;

DEFINE_string(target_name, "", "Target name to resolve.");
DEFINE_string(expected_addrs, "",
              "Comma-separated list of expected "
              "'<ip0:port0>,<is_balancer0>;<ip1:port1>,<is_balancer1>;...' "
              "addresses of "
              "backend and/or balancers. 'is_balancer' should be bool, i.e. "
              "true of false.");
DEFINE_string(expected_chosen_service_config, "",
              "Expected service config json string that gets chosen (no "
              "whitespace). Empty for none.");
DEFINE_string(
    local_dns_server_address, "",
    "Optional. This address is placed as the uri authority if present.");
DEFINE_bool(start_local_dns_server, false,
            "Start and use a local DNS server as a subprocess.");
DEFINE_string(expected_lb_policy, "",
              "Expected lb policy name that appears in resolver result channel "
              "arg. Empty for none.");

namespace {

int local_dns_server_port = 0;

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

bool ConvertStringToBool(std::string bool_str) {
  if (gpr_stricmp(bool_str.c_str(), "true") == 0) {
    return true;
  } else if (gpr_stricmp(bool_str.c_str(), "false") == 0) {
    return false;
  } else {
    gpr_log(GPR_ERROR,
            "invalid expected_addrs_list entry: expected false or true, got %s",
            bool_str.c_str());
    abort();
  }
}

void ExpectToken(const char *token, std::string expected_addrs) {
  size_t next_occurrence = expected_addrs.find(token);

  if (next_occurrence != 0) {
    gpr_log(
        GPR_ERROR,
        "Missing %s. Expected_addrs arg should be a comma-separated list of "
        "(<ip-port>,<bool>) pairs",
        token);
    abort();
  }
}

vector<GrpcLBAddress> ParseExpectedAddrs(std::string expected_addrs) {
  std::vector<GrpcLBAddress> out;

  while (expected_addrs.size() != 0) {
    // get the next <ip>:<port> (v4 or v6)
    size_t next_comma = expected_addrs.find(",");
    if (next_comma == std::string::npos) {
      gpr_log(GPR_ERROR,
              "expected_addrs arg should be a comma-separated list of "
              "<ip-port>,<bool> pairs");
      abort();
    }
    std::string next_addr = expected_addrs.substr(0, next_comma);
    expected_addrs = expected_addrs.substr(next_comma + 1, std::string::npos);
    // get the next is_balancer 'bool' associated with this address
    size_t next_semicolon = expected_addrs.find(";");
    bool is_balancer =
        ConvertStringToBool(expected_addrs.substr(0, next_semicolon));
    out.emplace_back(GrpcLBAddress(next_addr, is_balancer));
    if (next_semicolon == std::string::npos) {
      break;
    }
    expected_addrs =
        expected_addrs.substr(next_semicolon + 1, std::string::npos);
  }
  if (out.size() == 0) {
    gpr_log(GPR_ERROR,
            "expected_addrs arg should be a comma-separated list of "
            "<ip-port>,<bool> pairs");
    abort();
  }
  return out;
}

gpr_timespec test_deadline(void) {
  return grpc_timeout_seconds_to_deadline(100);
}

typedef struct args_struct {
  gpr_event ev;
  gpr_atm done_atm;
  gpr_mu *mu;
  grpc_pollset *pollset;
  grpc_pollset_set *pollset_set;
  grpc_combiner *lock;
  grpc_channel_args *channel_args;
  vector<GrpcLBAddress> expected_addrs;
  std::string expected_service_config_string;
  std::string expected_lb_policy;
} args_struct;

void args_init(grpc_exec_ctx *exec_ctx, args_struct *args) {
  gpr_event_init(&args->ev);
  args->pollset = (grpc_pollset *)gpr_zalloc(grpc_pollset_size());
  grpc_pollset_init(args->pollset, &args->mu);
  args->pollset_set = grpc_pollset_set_create();
  grpc_pollset_set_add_pollset(exec_ctx, args->pollset_set, args->pollset);
  args->lock = grpc_combiner_create();
  gpr_atm_rel_store(&args->done_atm, 0);
  args->channel_args = NULL;
}

void do_nothing(grpc_exec_ctx *exec_ctx, void *arg, grpc_error *error) {}

void args_finish(grpc_exec_ctx *exec_ctx, args_struct *args) {
  GPR_ASSERT(gpr_event_wait(&args->ev, test_deadline()));
  grpc_pollset_set_del_pollset(exec_ctx, args->pollset_set, args->pollset);
  grpc_pollset_set_destroy(exec_ctx, args->pollset_set);
  grpc_closure do_nothing_cb;
  GRPC_CLOSURE_INIT(&do_nothing_cb, do_nothing, NULL,
                    grpc_schedule_on_exec_ctx);
  grpc_pollset_shutdown(exec_ctx, args->pollset, &do_nothing_cb);
  // exec_ctx needs to be flushed before calling grpc_pollset_destroy()
  grpc_channel_args_destroy(exec_ctx, args->channel_args);
  grpc_exec_ctx_flush(exec_ctx);
  grpc_pollset_destroy(exec_ctx, args->pollset);
  gpr_free(args->pollset);
  GRPC_COMBINER_UNREF(exec_ctx, args->lock, NULL);
}

gpr_timespec n_sec_deadline(int seconds) {
  return gpr_time_add(gpr_now(GPR_CLOCK_REALTIME),
                      gpr_time_from_seconds(seconds, GPR_TIMESPAN));
}

void PollPollsetUntilRequestDone(args_struct *args) {
  gpr_timespec deadline = n_sec_deadline(10);
  while (true) {
    bool done = gpr_atm_acq_load(&args->done_atm) != 0;
    if (done) {
      break;
    }
    gpr_timespec time_left =
        gpr_time_sub(deadline, gpr_now(GPR_CLOCK_REALTIME));
    gpr_log(GPR_DEBUG, "done=%d, time_left=%" PRId64 ".%09d", done,
            time_left.tv_sec, time_left.tv_nsec);
    GPR_ASSERT(gpr_time_cmp(time_left, gpr_time_0(GPR_TIMESPAN)) >= 0);
    grpc_pollset_worker *worker = NULL;
    grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
    gpr_mu_lock(args->mu);
    GRPC_LOG_IF_ERROR(
        "pollset_work",
        grpc_pollset_work(&exec_ctx, args->pollset, &worker,
                          gpr_now(GPR_CLOCK_REALTIME), n_sec_deadline(1)));
    gpr_mu_unlock(args->mu);
    grpc_exec_ctx_finish(&exec_ctx);
  }
  gpr_event_set(&args->ev, (void *)1);
}

void check_service_config_result_locked(grpc_channel_args *channel_args,
                                        args_struct *args) {
  const grpc_arg *service_config_arg =
      grpc_channel_args_find(channel_args, GRPC_ARG_SERVICE_CONFIG);
  if (args->expected_service_config_string != "") {
    GPR_ASSERT(service_config_arg != NULL);
    GPR_ASSERT(service_config_arg->type == GRPC_ARG_STRING);
    EXPECT_EQ(service_config_arg->value.string,
              args->expected_service_config_string);
  } else {
    GPR_ASSERT(service_config_arg == NULL);
  }
}

void check_lb_policy_result_locked(grpc_channel_args *channel_args,
                                   args_struct *args) {
  const grpc_arg *lb_policy_arg =
      grpc_channel_args_find(channel_args, GRPC_ARG_LB_POLICY_NAME);
  if (args->expected_lb_policy != "") {
    GPR_ASSERT(lb_policy_arg != NULL);
    GPR_ASSERT(lb_policy_arg->type == GRPC_ARG_STRING);
    EXPECT_EQ(lb_policy_arg->value.string, args->expected_lb_policy);
  } else {
    GPR_ASSERT(lb_policy_arg == NULL);
  }
}

void check_resolver_result_locked(grpc_exec_ctx *exec_ctx, void *argsp,
                                  grpc_error *err) {
  args_struct *args = (args_struct *)argsp;
  grpc_channel_args *channel_args = args->channel_args;
  const grpc_arg *channel_arg =
      grpc_channel_args_find(channel_args, GRPC_ARG_LB_ADDRESSES);
  GPR_ASSERT(channel_arg != NULL);
  GPR_ASSERT(channel_arg->type == GRPC_ARG_POINTER);
  grpc_lb_addresses *addresses =
      (grpc_lb_addresses *)channel_arg->value.pointer.p;
  gpr_log(GPR_INFO, "num addrs found: %" PRIdPTR ". expected %" PRIdPTR,
          addresses->num_addresses, args->expected_addrs.size());

  GPR_ASSERT(addresses->num_addresses == args->expected_addrs.size());
  std::vector<GrpcLBAddress> found_lb_addrs;
  for (size_t i = 0; i < addresses->num_addresses; i++) {
    grpc_lb_address addr = addresses->addresses[i];
    char *str;
    grpc_sockaddr_to_string(&str, &addr.address, 1 /* normalize */);
    gpr_log(GPR_INFO, "%s", str);

    found_lb_addrs.emplace_back(
        GrpcLBAddress(std::string(str), addr.is_balancer));
    gpr_free(str);
  }

  if (args->expected_addrs.size() != found_lb_addrs.size()) {
    gpr_log(GPR_DEBUG, "found lb addrs size is: %" PRIdPTR
                       ". expected addrs size is %" PRIdPTR,
            found_lb_addrs.size(), args->expected_addrs.size());
    abort();
  }

  EXPECT_THAT(args->expected_addrs, UnorderedElementsAreArray(found_lb_addrs));

  check_service_config_result_locked(channel_args, args);
  check_lb_policy_result_locked(channel_args, args);

  gpr_atm_rel_store(&args->done_atm, 1);
  gpr_mu_lock(args->mu);
  GRPC_LOG_IF_ERROR("pollset_kick", grpc_pollset_kick(args->pollset, NULL));
  gpr_mu_unlock(args->mu);
}

void TestResolves(grpc_exec_ctx *exec_ctx, args_struct *args) {
  // sanity check flags
  if (FLAGS_local_dns_server_address != "" && FLAGS_start_local_dns_server) {
    gpr_log(GPR_ERROR,
            "Cant set local DNS server address and start a new DNS server");
    abort();
  }
  if (FLAGS_target_name == "") {
    gpr_log(GPR_ERROR, "Missing target_name param.");
    abort();
  }

  // maybe build the address with an authority
  std::string authority = FLAGS_local_dns_server_address;
  if (FLAGS_start_local_dns_server) {
    GPR_ASSERT(local_dns_server_port != 0);
    authority = "127.0.0.1:" + std::to_string(local_dns_server_port);
  }
  if (authority.size() > 0) {
    gpr_log(GPR_INFO, "Specifying authority in uris to: %s", authority.c_str());
  }
  char *whole_uri = NULL;
  GPR_ASSERT(asprintf(&whole_uri, "dns://%s/%s", authority.c_str(),
                      FLAGS_target_name.c_str()));
  // create resolver and resolve
  grpc_resolver *resolver = grpc_resolver_create(exec_ctx, whole_uri, NULL,
                                                 args->pollset_set, args->lock);
  gpr_free(whole_uri);
  grpc_closure on_resolver_result_changed;
  GRPC_CLOSURE_INIT(&on_resolver_result_changed, check_resolver_result_locked,
                    (void *)args, grpc_combiner_scheduler(args->lock));

  grpc_resolver_next_locked(exec_ctx, resolver, &args->channel_args,
                            &on_resolver_result_changed);

  grpc_exec_ctx_flush(exec_ctx);
  PollPollsetUntilRequestDone(args);
  GRPC_RESOLVER_UNREF(exec_ctx, resolver, NULL);
}

TEST(ResolverTest, ResolvesRelevantRecords) {
  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
  args_struct args;
  args_init(&exec_ctx, &args);
  args.expected_addrs = ParseExpectedAddrs(FLAGS_expected_addrs);
  args.expected_service_config_string = FLAGS_expected_chosen_service_config;
  args.expected_lb_policy = FLAGS_expected_lb_policy;

  TestResolves(&exec_ctx, &args);
  args_finish(&exec_ctx, &args);
  grpc_exec_ctx_finish(&exec_ctx);
}

}  // namespace

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  grpc::testing::InitTest(&argc, &argv, true);

  grpc_init();

  SubProcess *dns_server_subprocess = NULL;

  if (FLAGS_start_local_dns_server == true) {
    /* spawn a DNS server subprocess*/
    local_dns_server_port = grpc_pick_unused_port_or_die();
    std::string my_bin = argv[0];
    std::string bin_dir = my_bin.substr(0, my_bin.rfind('/'));
    std::vector<std::string> args = {
        "tools/run_tests/python_utils/dns_server.py",
        "--dns_port=" + std::to_string(local_dns_server_port)};
    dns_server_subprocess = new SubProcess(args);

    // Wait for the DNS server to stand up.
    // TODO: can we get rid of the need to sleep here?
    // Without this sleep, some polling engines timeout and others
    // fail fast.
    gpr_sleep_until(gpr_time_add(gpr_now(GPR_CLOCK_REALTIME),
                                 gpr_time_from_seconds(1, GPR_TIMESPAN)));
    gpr_log(GPR_INFO, "started local DNS server subprocess: |%s %s|",
            args[0].c_str(), args[1].c_str());
  }

  int test_return_val = RUN_ALL_TESTS();
  if (test_return_val) {
    gpr_log(GPR_ERROR, "DNS RESOLVER TEST FAILED.");
  }

  if (FLAGS_start_local_dns_server == true) {
    /* wait for DNS server subprocess*/
    gpr_log(GPR_INFO, "Interrupt DNS server subprocess and wait for join.");
    dns_server_subprocess->Interrupt();
    const int dns_server_return_val = dns_server_subprocess->Join();
    delete dns_server_subprocess;
    if (dns_server_return_val != 0) {
      test_return_val |= dns_server_return_val;
      gpr_log(GPR_ERROR,
              "DNS server subprocess exited with non-zero status: %d",
              dns_server_return_val);
    }
    grpc_recycle_unused_port(local_dns_server_port);
  }

  grpc_shutdown();
  return test_return_val;
}
