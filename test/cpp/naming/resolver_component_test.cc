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
#include <grpc/support/sync.h>
#include <grpc/support/time.h>
#include <string.h>

#include <gflags/gflags.h>
#include <gmock/gmock.h>
#include <vector>

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
#include "test/core/util/test_config.h"
}

using std::vector;
using testing::UnorderedElementsAreArray;

DEFINE_string(target_name, "", "");
DEFINE_string(expected_addrs, "", "");
DEFINE_string(expected_chosen_service_config, "", "");
DEFINE_string(local_dns_server_address, "", "");
DEFINE_string(expected_lb_policy, "", "");

namespace {

class GrpcLBAddress final {
 public:
  GrpcLBAddress(std::string address, bool is_balancer)
      : is_balancer(is_balancer), address(address) {}

  bool operator<(const GrpcLBAddress &other) const {
    int comparison = this->address.compare(other.address);
    if (comparison != 0) {
      return comparison < 0 ? true : false;
    }
    return this->is_balancer == false && other.is_balancer == true;
  }

  bool is_balancer;
  std::string address;
};

bool convert_string_to_bool(std::string bool_str) {
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

vector<GrpcLBAddress> parse_expected_addrs(std::string expected_addrs) {
  std::vector<GrpcLBAddress> out;

  while (expected_addrs.size() != 0) {
    size_t next_comma = expected_addrs.find(",");
    std::string next_addr;
    bool next_bool;

    if (next_comma == std::string::npos) {
      gpr_log(GPR_ERROR,
              "expected_addrs arg should be a comma-separated list of "
              "<ip-port>,<bool> pairs");
      abort();
    }
    next_addr = expected_addrs.substr(0, next_comma);
    expected_addrs = expected_addrs.substr(next_comma + 1, std::string::npos);

    next_comma = expected_addrs.find(",");
    if (next_comma == std::string::npos) {
      next_bool = convert_string_to_bool(expected_addrs);
      out.emplace_back(GrpcLBAddress(next_addr, next_bool));
      break;
    }
    next_bool = convert_string_to_bool(expected_addrs.substr(0, next_comma));
    out.emplace_back(GrpcLBAddress(next_addr, next_bool));
    expected_addrs = expected_addrs.substr(next_comma + 1, std::string::npos);
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
  std::string target_name;
  vector<GrpcLBAddress> expected_addrs;
  const char *expected_service_config_string;
  const char *expected_lb_policy;
} args_struct;

void do_nothing(grpc_exec_ctx *exec_ctx, void *arg, grpc_error *error) {}

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

void poll_pollset_until_request_done(args_struct *args) {
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
  if (args->expected_service_config_string != NULL &&
      strlen(args->expected_service_config_string) > 0) {
    GPR_ASSERT(service_config_arg != NULL);
    GPR_ASSERT(service_config_arg->type == GRPC_ARG_STRING);
    char *service_config_string = service_config_arg->value.string;
    if (strcmp(service_config_string, args->expected_service_config_string) !=
        0) {
      gpr_log(GPR_ERROR, "expected service config string: |%s|",
              args->expected_service_config_string);
      gpr_log(GPR_ERROR, "got service config string: |%s|",
              service_config_string);
      GPR_ASSERT(0);
    }
  } else {
    GPR_ASSERT(service_config_arg == NULL);
  }
}

void check_lb_policy_result_locked(grpc_channel_args *channel_args,
                                   args_struct *args) {
  const grpc_arg *lb_policy_arg =
      grpc_channel_args_find(channel_args, GRPC_ARG_LB_POLICY_NAME);
  if (args->expected_lb_policy != NULL &&
      strlen(args->expected_lb_policy) > 0) {
    GPR_ASSERT(lb_policy_arg != NULL);
    GPR_ASSERT(lb_policy_arg->type == GRPC_ARG_STRING);
    if (strcmp(lb_policy_arg->value.string, args->expected_lb_policy) != 0) {
      gpr_log(GPR_ERROR, "expected lb policy: |%s|", args->expected_lb_policy);
      gpr_log(GPR_ERROR, "got lb policy: |%s|", lb_policy_arg->value.string);
      GPR_ASSERT(0);
    }
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

  std::sort(args->expected_addrs.begin(), args->expected_addrs.end());
  std::sort(found_lb_addrs.begin(), found_lb_addrs.end());

  for (size_t i = 0; i < args->expected_addrs.size(); i++) {
    if (args->expected_addrs[i].address != found_lb_addrs[i].address) {
      gpr_log(GPR_DEBUG,
              "expected lb address != found lb address. got address: %s, "
              "expected %s",
              args->expected_addrs[i].address.c_str(),
              found_lb_addrs[i].address.c_str());
      abort();
    }
    if (args->expected_addrs[i].is_balancer != found_lb_addrs[i].is_balancer) {
      gpr_log(GPR_DEBUG,
              "expected lb address %s is_balancer mismatch. expected "
              "is_balancer: %d, got %d",
              args->expected_addrs[i].address.c_str(),
              args->expected_addrs[i].is_balancer,
              found_lb_addrs[i].is_balancer);
      abort();
    }
  }

  check_service_config_result_locked(channel_args, args);
  check_lb_policy_result_locked(channel_args, args);

  gpr_atm_rel_store(&args->done_atm, 1);
  gpr_mu_lock(args->mu);
  GRPC_LOG_IF_ERROR("pollset_kick", grpc_pollset_kick(args->pollset, NULL));
  gpr_mu_unlock(args->mu);
}

void test_resolves(grpc_exec_ctx *exec_ctx, args_struct *args) {
  const char *authority = FLAGS_local_dns_server_address.c_str();
  if (authority != NULL && strlen(authority) > 0) {
    gpr_log(GPR_INFO, "Specifying authority in uris to: %s", authority);
  } else {
    authority = "";
  }

  char *whole_uri = NULL;
  GPR_ASSERT(asprintf(&whole_uri, "dns://%s/%s", authority,
                      args->target_name.c_str()));

  grpc_resolver *resolver = grpc_resolver_create(exec_ctx, whole_uri, NULL,
                                                 args->pollset_set, args->lock);
  gpr_free(whole_uri);

  grpc_closure on_resolver_result_changed;
  GRPC_CLOSURE_INIT(&on_resolver_result_changed, check_resolver_result_locked,
                    (void *)args, grpc_combiner_scheduler(args->lock));

  grpc_resolver_next_locked(exec_ctx, resolver, &args->channel_args,
                            &on_resolver_result_changed);

  grpc_exec_ctx_flush(exec_ctx);
  poll_pollset_until_request_done(args);
  GRPC_RESOLVER_UNREF(exec_ctx, resolver, NULL);
}

void resolver_component_test_resolution(
    std::string target_name, std::vector<GrpcLBAddress> expected_addrs,
    const char *expected_service_config, const char *expected_lb_policy) {
  grpc_init();
  gpr_log(GPR_INFO, "expected addresses size: %d", (int)expected_addrs.size());
  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
  args_struct args;
  args_init(&exec_ctx, &args);
  args.target_name = target_name;
  args.expected_addrs = expected_addrs;
  args.expected_service_config_string = expected_service_config;
  args.expected_lb_policy = expected_lb_policy;

  test_resolves(&exec_ctx, &args);
  args_finish(&exec_ctx, &args);
  grpc_exec_ctx_finish(&exec_ctx);
  grpc_shutdown();
}

}  // namespace

int main(int argc, char **argv) {
  grpc::testing::InitTest(&argc, &argv, true);
  if (FLAGS_target_name == "" || FLAGS_expected_addrs == "") {
    gpr_log(GPR_ERROR,
            "Missing target_name or expected_addrs params. Got %s and %s",
            FLAGS_target_name.c_str(), FLAGS_expected_addrs.c_str());
    abort();
  }
  auto expected_addrs_list = parse_expected_addrs(FLAGS_expected_addrs.c_str());

  resolver_component_test_resolution(
      FLAGS_target_name, expected_addrs_list,
      FLAGS_expected_chosen_service_config.c_str(),
      FLAGS_expected_lb_policy.c_str());
  return 0;
}
