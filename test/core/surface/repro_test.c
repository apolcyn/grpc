/*
 *
 * Copyright 2016, Google Inc.
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

#include <grpc/grpc.h>
#include <grpc/grpc_security.h>
#include <grpc/support/alloc.h>
#include <grpc/support/host_port.h>
#include <grpc/support/log.h>
#include <grpc/support/thd.h>

#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/iomgr/exec_ctx.h"
#include "test/core/end2end/data/ssl_test_data.h"
#include "test/core/util/port.h"
#include "test/core/util/test_config.h"

typedef struct test_fixture {
  const char *name;
  grpc_channel *(*create_channel)(const char *addr);
} test_fixture;

static void shutdown_during_channel_watch_test(const test_fixture *fixture) {
  grpc_event ev;
  void* tag = (void*)1;

  grpc_init();

  grpc_channel *channel = fixture->create_channel("localhost:12345");
  grpc_completion_queue *cq = grpc_completion_queue_create_for_pluck(NULL);

  gpr_timespec connect_deadline = gpr_time_add(gpr_now(GPR_CLOCK_REALTIME), gpr_time_from_seconds(30, GPR_TIMESPAN));
  GPR_ASSERT(grpc_channel_check_connectivity_state(channel, 0) ==
             GRPC_CHANNEL_IDLE);

  grpc_channel_watch_connectivity_state(channel, GRPC_CHANNEL_IDLE,
                                        connect_deadline, cq, tag);

  grpc_channel_destroy(channel);

  grpc_completion_queue_shutdown(cq);
  for(;;) {
    ev = grpc_completion_queue_pluck(cq, NULL, gpr_inf_future(GPR_CLOCK_REALTIME), NULL);
    if (ev.type == GRPC_QUEUE_SHUTDOWN) {
      break;
    }
  }
  grpc_completion_queue_destroy(cq);

  grpc_shutdown();
}

static grpc_channel *insecure_test_create_channel(const char *addr) {
  return grpc_insecure_channel_create(addr, NULL, NULL);
}

static const test_fixture insecure_test = {
    "insecure", insecure_test_create_channel,
};

static grpc_channel *secure_test_create_channel(const char *addr) {
  grpc_channel_credentials *ssl_creds =
      grpc_ssl_credentials_create(test_root_cert, NULL, NULL);
  grpc_arg ssl_name_override = {GRPC_ARG_STRING,
                                GRPC_SSL_TARGET_NAME_OVERRIDE_ARG,
                                {"foo.test.google.fr"}};
  grpc_channel_args *new_client_args =
      grpc_channel_args_copy_and_add(NULL, &ssl_name_override, 1);
  grpc_channel *channel =
      grpc_secure_channel_create(ssl_creds, addr, new_client_args, NULL);
  {
    grpc_exec_ctx exec_ctx = GRPC_EXEC_CTX_INIT;
    grpc_channel_args_destroy(&exec_ctx, new_client_args);
    grpc_exec_ctx_finish(&exec_ctx);
  }
  grpc_channel_credentials_release(ssl_creds);
  return channel;
}

static const test_fixture secure_test = {
    "secure", secure_test_create_channel,
};

int main(int argc, char **argv) {
  grpc_test_init(argc, argv);

  shutdown_during_channel_watch_test(&insecure_test);
}
