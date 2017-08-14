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

#include <vector>

extern "C" {
  #include "src/core/ext/filters/client_channel/client_channel.h"
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

#include "test/cpp/naming/naming_end2end_test_util.h"

namespace grpc {
namespace testing {

static gpr_timespec test_deadline(void) {
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
  int expect_is_balancer;
  const char *target_name;
  vector<std::string> expected_addrs;
  const char *expected_service_config_string;
} args_struct;

int matches_any(vector<std::string> expected_addrs, const char* addr) {
  for (auto it = expected_addrs.begin(); it != expected_addrs.end(); it++) {
    if (it->compare(addr) == 0) {
      gpr_log(GPR_INFO, "found a match for expected addresss: %s", addr);
      return 1;
    } else {
      gpr_log(GPR_INFO, "expected addresss: %s didn't match found address: %s", it->c_str(), addr);
    }
  }
  gpr_log(GPR_ERROR, "no match found for found address: %s", addr);
  return 0;
}

static void do_nothing(grpc_exec_ctx *exec_ctx, void *arg, grpc_error *error) {}

void args_init(grpc_exec_ctx *exec_ctx, args_struct *args) {
  gpr_event_init(&args->ev);
  args->pollset = (grpc_pollset*)gpr_zalloc(grpc_pollset_size());
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
  grpc_exec_ctx_flush(exec_ctx);
  grpc_pollset_destroy(exec_ctx, args->pollset);
  gpr_free(args->pollset);
}

static gpr_timespec n_sec_deadline(int seconds) {
  return gpr_time_add(gpr_now(GPR_CLOCK_REALTIME),
                      gpr_time_from_seconds(seconds, GPR_TIMESPAN));
}

static void poll_pollset_until_request_done(args_struct *args) {
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

static void check_service_config_result_locked(grpc_channel_args *channel_args, args_struct *args) {
  const grpc_arg *service_config_arg =
      grpc_channel_args_find(channel_args, GRPC_ARG_SERVICE_CONFIG);
  if (args->expected_service_config_string != NULL) {
    GPR_ASSERT(service_config_arg != NULL);
    GPR_ASSERT(service_config_arg->type == GRPC_ARG_STRING);
    char* service_config_string = service_config_arg->value.string;
    if (gpr_stricmp(service_config_string, args->expected_service_config_string) != 0) {
      gpr_log(GPR_ERROR, "expected service config string: |%s|", args->expected_service_config_string);
      gpr_log(GPR_ERROR, "got service config string: |%s|", service_config_string);
      GPR_ASSERT(0);
    }
  } else {
    GPR_ASSERT(service_config_arg == NULL);
  }
}

static void check_channel_arg_srv_result_locked(grpc_exec_ctx *exec_ctx,
                                                void *argsp, grpc_error *err) {
  args_struct *args = (args_struct*)argsp;
  grpc_channel_args *channel_args = args->channel_args;
  const grpc_arg *channel_arg =
      grpc_channel_args_find(channel_args, GRPC_ARG_LB_ADDRESSES);
  GPR_ASSERT(channel_arg != NULL);
  GPR_ASSERT(channel_arg->type == GRPC_ARG_POINTER);
  grpc_lb_addresses *addresses = (grpc_lb_addresses*)channel_arg->value.pointer.p;
  gpr_log(GPR_INFO, "num addrs found: %d. expected %" PRIdPTR,
          (int)addresses->num_addresses, args->expected_addrs.size());

  GPR_ASSERT(addresses->num_addresses == args->expected_addrs.size());
  for (size_t i = 0; i < addresses->num_addresses; i++) {
    grpc_lb_address addr = addresses->addresses[i];
    char *str;
    grpc_sockaddr_to_string(&str, &addr.address, 1 /* normalize */);
    gpr_log(GPR_INFO, "%s", str);
    GPR_ASSERT(addr.is_balancer == args->expect_is_balancer);
    GPR_ASSERT(matches_any(args->expected_addrs, str));
  }

  check_service_config_result_locked(channel_args, args);

  gpr_atm_rel_store(&args->done_atm, 1);
  gpr_mu_lock(args->mu);
  GRPC_LOG_IF_ERROR("pollset_kick", grpc_pollset_kick(args->pollset, NULL));
  gpr_mu_unlock(args->mu);
}

static void test_resolves(grpc_exec_ctx *exec_ctx, args_struct *args) {
  char* whole_uri = NULL;
  const char* authority = gpr_getenv("GRPC_DNS_AUTHORITY_TESTING_OVERRIDE");
  if (authority != NULL && strlen(authority) > 0) {
    gpr_log(GPR_INFO, "Specifying authority in uris to: %s", authority);
  } else{
    authority = "";
  }
  grpc_arg new_arg;
  new_arg.type = GRPC_ARG_STRING;
  new_arg.key = (char*)GRPC_ARG_SERVER_URI;
  new_arg.value.string = (char*)args->target_name;
  GPR_ASSERT(asprintf(&whole_uri, "dns://%s/%s", authority, new_arg.value.string));

  args->channel_args = grpc_channel_args_copy_and_add(NULL, &new_arg, 0);

  grpc_resolver *resolver =
      grpc_resolver_create(exec_ctx, whole_uri, args->channel_args,
                           args->pollset_set, args->lock);

  grpc_closure on_resolver_result_changed;
  GRPC_CLOSURE_INIT(&on_resolver_result_changed,
                    check_channel_arg_srv_result_locked, (void *)args,
                    grpc_combiner_scheduler(args->lock));

  grpc_resolver_next_locked(exec_ctx, resolver, &args->channel_args,
                            &on_resolver_result_changed);

  grpc_exec_ctx_flush(exec_ctx);
  poll_pollset_until_request_done(args);
}

void naming_end2end_test_resolves_backend(const char *name, std::vector<std::string> expected_addrs, const char *expected_service_config) {
  grpc_init();
  gpr_log(GPR_INFO, "e a size: %d", (int)expected_addrs.size());
  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
  args_struct args;
  args_init(&exec_ctx, &args);
  args.expect_is_balancer = 0;
  args.target_name = name;
  args.expected_addrs = expected_addrs;
  args.expected_service_config_string = expected_service_config;

  test_resolves(&exec_ctx, &args);
  args_finish(&exec_ctx, &args);
  grpc_exec_ctx_finish(&exec_ctx);
  grpc_shutdown();
}

void naming_end2end_test_resolves_balancer(const char *name, std::vector<std::string> expected_addrs, const char *expected_service_config) {
  grpc_init();
  gpr_log(GPR_INFO, "e a size: %d", (int)expected_addrs.size());
  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
  args_struct args;
  args_init(&exec_ctx, &args);
  args.expect_is_balancer = 1;
  args.target_name = name;
  args.expected_addrs = expected_addrs;
  args.expected_service_config_string = expected_service_config;

  test_resolves(&exec_ctx, &args);
  args_finish(&exec_ctx, &args);
  grpc_exec_ctx_finish(&exec_ctx);
  grpc_shutdown();
}

}  // namespace
}  // namespace grpc
