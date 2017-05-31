/*
 *
 * Copyright 2015, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "src/core/ext/filters/client_channel/resolver/dns/c_ares/grpc_ares_wrapper.h"
#include "src/core/lib/iomgr/resolve_address.h"
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/sync.h>
#include <grpc/support/time.h>
#include <grpc/support/host_port.h>

#include "src/core/lib/iomgr/executor.h"
#include "src/core/lib/iomgr/iomgr.h"
#include "test/core/util/test_config.h"
#include "src/core/lib/support/string.h"
#include "src/core/lib/support/env.h"
#include "./src/core/lib/iomgr/sockaddr_utils.h"

extern void grpc_resolver_dns_ares_init();
extern void grpc_resolver_dns_ares_shutdown();

static gpr_timespec test_deadline(void) {
  return grpc_timeout_seconds_to_deadline(100);
}

typedef struct args_struct {
  gpr_event ev;
  grpc_resolved_addresses *addrs;
  grpc_lb_addresses *lb_addrs;
  gpr_atm done_atm;
  gpr_mu *mu;
  grpc_pollset *pollset;
  grpc_pollset_set *pollset_set;
} args_struct;

static void do_nothing(grpc_exec_ctx *exec_ctx, void *arg, grpc_error *error) {}

void args_init(grpc_exec_ctx *exec_ctx, args_struct *args) {
  gpr_event_init(&args->ev);
  args->pollset = gpr_zalloc(grpc_pollset_size());
  grpc_pollset_init(args->pollset, &args->mu);
  args->pollset_set = grpc_pollset_set_create();
  grpc_pollset_set_add_pollset(exec_ctx, args->pollset_set, args->pollset);
  args->addrs = NULL;
  args->lb_addrs = NULL;
  gpr_atm_rel_store(&args->done_atm, 0);
}

void args_finish(grpc_exec_ctx *exec_ctx, args_struct *args) {
  GPR_ASSERT(gpr_event_wait(&args->ev, test_deadline()));
  grpc_resolved_addresses_destroy(args->addrs);
  grpc_pollset_set_del_pollset(exec_ctx, args->pollset_set, args->pollset);
  grpc_pollset_set_destroy(exec_ctx, args->pollset_set);
  grpc_closure do_nothing_cb;
  grpc_closure_init(&do_nothing_cb, do_nothing, NULL,
                    grpc_schedule_on_exec_ctx);
  grpc_pollset_shutdown(exec_ctx, args->pollset, &do_nothing_cb);
  // exec_ctx needs to be flushed before calling grpc_pollset_destroy()
  grpc_exec_ctx_flush(exec_ctx);
  grpc_pollset_destroy(exec_ctx, args->pollset);
  gpr_free(args->pollset);
  if (args->lb_addrs) {
    grpc_lb_addresses_destroy(exec_ctx, args->lb_addrs);
  }
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

//static void must_succeed(grpc_exec_ctx *exec_ctx, void *argsp,
//                         grpc_error *err) {
//  args_struct *args = argsp;
//  GPR_ASSERT(err == GRPC_ERROR_NONE);
//  GPR_ASSERT(args->addrs != NULL);
//  GPR_ASSERT(args->addrs->naddrs > 0);
//  gpr_atm_rel_store(&args->done_atm, 1);
//  gpr_mu_lock(args->mu);
//  GRPC_LOG_IF_ERROR("pollset_kick", grpc_pollset_kick(args->pollset, NULL));
//  gpr_mu_unlock(args->mu);
//}
//
static void check_srv_result(grpc_exec_ctx *exec_ctx, void *args, grpc_error *err) {
  grpc_lb_addresses *addresses = *((grpc_lb_addresses**)lb_addrs);
  GPR_ASSERT(addresses);
  gpr_log(GPR_INFO, "num addrs: %d", (int)addresses->num_addresses);
  GPR_ASSERT(addresses->num_addresses == 1);
  grpc_lb_address addr = addresses->addresses[0];
  char *str;
  grpc_sockaddr_to_string(&str, &addr.address, 1 /* normalize */);
  gpr_log(GPR_INFO, "%s", str);
  char *host;
  char *port;
  gpr_split_host_port(str, &host, &port);
  // TODO(apolcyn) figure out what to do with the port
  GPR_ASSERT(gpr_stricmp(host, "5.6.7.8") == 0);
  GPR_ASSERT(addr.is_balancer);
  gpr_atm_rel_store(&args->done_atm, 1);
  gpr_mu_lock(args->mu);
  GRPC_LOG_IF_ERROR("pollset_kick", grpc_pollset_kick(args->pollset, NULL));
  gpr_mu_unlock(args->mu);
}

//static void must_fail(grpc_exec_ctx *exec_ctx, void *argsp, grpc_error *err) {
//  args_struct *args = argsp;
//  GPR_ASSERT(err != GRPC_ERROR_NONE);
//  gpr_atm_rel_store(&args->done_atm, 1);
//  gpr_mu_lock(args->mu);
//  GRPC_LOG_IF_ERROR("pollset_kick", grpc_pollset_kick(args->pollset, NULL));
//  gpr_mu_unlock(args->mu);
//}
//
//static void test_localhost(void) {
//  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
//  args_struct args;
//  args_init(&exec_ctx, &args);
//  grpc_resolve_address(
//      &exec_ctx, "localhost:1", NULL, args.pollset_set,
//      grpc_closure_create(must_succeed, &args, grpc_schedule_on_exec_ctx),
//      &args.addrs);
//  grpc_exec_ctx_flush(&exec_ctx);
//  poll_pollset_until_request_done(&args);
//  args_finish(&exec_ctx, &args);
//  grpc_exec_ctx_finish(&exec_ctx);
//}
//
static void test_resolves_srv(void) {
  if (gpr_stricmp("ares", gpr_getenv("GRPC_DNS_RESOLVER")) != 0) {
    gpr_log(GPR_INFO, "skipping test");
    return;
  }
  char *resolve_srv = gpr_getenv("GRPC_RESOLVE_SRV");
  if (!resolve_srv || gpr_stricmp("", resolve_srv) == 0) {
    gpr_log(GPR_INFO, "skipping test");
  }
  gpr_log(GPR_INFO, "start resolves srv test");
  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
  args_struct args;
  args_init(&exec_ctx, &args);
  grpc_dns_lookup_ares(
      &exec_ctx, NULL, /* _grpclb._tcp. */ "mylbtest.test.apolcyntest" /* "mytestlb.test.apolcyntest" */, "443", args.pollset_set,
      grpc_closure_create(check_srv_result, &args.lb_addrs, grpc_schedule_on_exec_ctx),
      &args.lb_addrs, true /* check lb */);
  grpc_exec_ctx_flush(&exec_ctx);
  poll_pollset_until_request_done(&args);
  args_finish(&exec_ctx, &args);
  grpc_exec_ctx_finish(&exec_ctx);
  gpr_log(GPR_INFO, "end resolves srv test");
}

//static void test_default_port(void) {
//  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
//  args_struct args;
//  args_init(&exec_ctx, &args);
//  grpc_resolve_address(
//      &exec_ctx, "localhost", "1", args.pollset_set,
//      grpc_closure_create(must_succeed, &args, grpc_schedule_on_exec_ctx),
//      &args.addrs);
//  grpc_exec_ctx_flush(&exec_ctx);
//  poll_pollset_until_request_done(&args);
//  args_finish(&exec_ctx, &args);
//  grpc_exec_ctx_finish(&exec_ctx);
//}
//
//static void test_non_numeric_default_port(void) {
//  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
//  args_struct args;
//  args_init(&exec_ctx, &args);
//  grpc_resolve_address(
//      &exec_ctx, "localhost", "https", args.pollset_set,
//      grpc_closure_create(must_succeed, &args, grpc_schedule_on_exec_ctx),
//      &args.addrs);
//  grpc_exec_ctx_flush(&exec_ctx);
//  poll_pollset_until_request_done(&args);
//  args_finish(&exec_ctx, &args);
//  grpc_exec_ctx_finish(&exec_ctx);
//}
//
//static void test_missing_default_port(void) {
//  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
//  args_struct args;
//  args_init(&exec_ctx, &args);
//  grpc_resolve_address(
//      &exec_ctx, "localhost", NULL, args.pollset_set,
//      grpc_closure_create(must_fail, &args, grpc_schedule_on_exec_ctx),
//      &args.addrs);
//  grpc_exec_ctx_flush(&exec_ctx);
//  poll_pollset_until_request_done(&args);
//  args_finish(&exec_ctx, &args);
//  grpc_exec_ctx_finish(&exec_ctx);
//}
//
//static void test_ipv6_with_port(void) {
//  grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
//  args_struct args;
//  args_init(&exec_ctx, &args);
//  grpc_resolve_address(
//      &exec_ctx, "[2001:db8::1]:1", NULL, args.pollset_set,
//      grpc_closure_create(must_succeed, &args, grpc_schedule_on_exec_ctx),
//      &args.addrs);
//  grpc_exec_ctx_flush(&exec_ctx);
//  poll_pollset_until_request_done(&args);
//  args_finish(&exec_ctx, &args);
//  grpc_exec_ctx_finish(&exec_ctx);
//}
//
//static void test_ipv6_without_port(void) {
//  const char *const kCases[] = {
//      "2001:db8::1", "2001:db8::1.2.3.4", "[2001:db8::1]",
//  };
//  unsigned i;
//  for (i = 0; i < sizeof(kCases) / sizeof(*kCases); i++) {
//    grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
//    args_struct args;
//    args_init(&exec_ctx, &args);
//    grpc_resolve_address(
//        &exec_ctx, kCases[i], "80", args.pollset_set,
//        grpc_closure_create(must_succeed, &args, grpc_schedule_on_exec_ctx),
//        &args.addrs);
//    grpc_exec_ctx_flush(&exec_ctx);
//    poll_pollset_until_request_done(&args);
//    args_finish(&exec_ctx, &args);
//    grpc_exec_ctx_finish(&exec_ctx);
//  }
//}
//
//static void test_invalid_ip_addresses(void) {
//  const char *const kCases[] = {
//      "293.283.1238.3:1", "[2001:db8::11111]:1",
//  };
//  unsigned i;
//  for (i = 0; i < sizeof(kCases) / sizeof(*kCases); i++) {
//    grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
//    args_struct args;
//    args_init(&exec_ctx, &args);
//    grpc_resolve_address(
//        &exec_ctx, kCases[i], NULL, args.pollset_set,
//        grpc_closure_create(must_fail, &args, grpc_schedule_on_exec_ctx),
//        &args.addrs);
//    grpc_exec_ctx_flush(&exec_ctx);
//    poll_pollset_until_request_done(&args);
//    args_finish(&exec_ctx, &args);
//    grpc_exec_ctx_finish(&exec_ctx);
//  }
//}
//
//static void test_unparseable_hostports(void) {
//  const char *const kCases[] = {
//      "[", "[::1", "[::1]bad", "[1.2.3.4]", "[localhost]", "[localhost]:1",
//  };
//  unsigned i;
//  for (i = 0; i < sizeof(kCases) / sizeof(*kCases); i++) {
//    grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
//    args_struct args;
//    args_init(&exec_ctx, &args);
//    grpc_resolve_address(
//        &exec_ctx, kCases[i], "1", args.pollset_set,
//        grpc_closure_create(must_fail, &args, grpc_schedule_on_exec_ctx),
//        &args.addrs);
//    grpc_exec_ctx_flush(&exec_ctx);
//    poll_pollset_until_request_done(&args);
//    args_finish(&exec_ctx, &args);
//    grpc_exec_ctx_finish(&exec_ctx);
//  }
//}
//
int main(int argc, char **argv) {
  grpc_test_init(argc, argv);
  grpc_executor_init();
  grpc_iomgr_init();
  grpc_iomgr_start();
  grpc_resolver_dns_ares_init();
  //test_localhost();
  //test_default_port();
  //test_non_numeric_default_port();
  //test_missing_default_port();
  //test_ipv6_with_port();
  //test_ipv6_without_port();
  //test_invalid_ip_addresses();
  //test_unparseable_hostports();
  gpr_setenv("GRPC_DNS_RESOLVER", "ares");
  test_resolves_srv();
  {
    grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
    grpc_executor_shutdown(&exec_ctx);
    grpc_iomgr_shutdown(&exec_ctx);
    grpc_resolver_dns_ares_shutdown(&exec_ctx);
    grpc_exec_ctx_finish(&exec_ctx);
  }
  return 0;
}
