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
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include <gflags/gflags.h>
#include <string>
#include <thread>
#include <vector>

#include "test/cpp/util/subprocess.h"
#include "test/cpp/util/test_config.h"

#include "test/cpp/naming/resolver_component_tests_runner_invoker_common.h"

extern "C" {
#include "test/core/util/port.h"
}

using grpc::SubProcess;

static volatile sig_atomic_t abort_wait_for_child = 0;

static void sighandler(int sig) { abort_wait_for_child = 1; }

static void register_sighandler() {
  struct sigaction act;
  memset(&act, 0, sizeof(act));
  act.sa_handler = sighandler;
  sigaction(SIGINT, &act, NULL);
  sigaction(SIGTERM, &act, NULL);
}

namespace {

const int kTestTimeoutSeconds = 60 * 2;

void RunSigHandlingThread(SubProcess *test_driver, gpr_mu *test_driver_mu,
                          gpr_cv *test_driver_cv, int *test_driver_done) {
  for (size_t i = 0; i < kTestTimeoutSeconds && !abort_wait_for_child; i++) {
    gpr_mu_lock(test_driver_mu);
    if (*test_driver_done) {
      gpr_mu_unlock(test_driver_mu);
      return;
    }
    gpr_cv_wait(test_driver_cv, test_driver_mu,
                gpr_time_add(gpr_now(GPR_CLOCK_REALTIME),
                             gpr_time_from_seconds(1, GPR_TIMESPAN)));
    gpr_mu_unlock(test_driver_mu);
  }
  gpr_log(GPR_DEBUG,
          "Test timeout reached or received signal. Interrupting test driver "
          "child process.");
  test_driver->Interrupt();
  return;
}
}

namespace grpc {

namespace testing {

void InvokeResolverComponentTestsRunner(std::string test_runner_bin_path,
                                        std::string test_bin_path,
                                        std::string dns_server_bin_path,
                                        std::string records_config_path) {
  grpc_init();
  int test_dns_server_port = grpc_pick_unused_port_or_die();

  SubProcess *test_driver = new SubProcess(
      {test_runner_bin_path, "--test_bin_path=" + test_bin_path,
       "--dns_server_bin_path=" + dns_server_bin_path,
       "--records_config_path=" + records_config_path,
       "--test_dns_server_port=" + std::to_string(test_dns_server_port)});
  gpr_mu test_driver_mu;
  gpr_mu_init(&test_driver_mu);
  gpr_cv test_driver_cv;
  gpr_cv_init(&test_driver_cv);
  int test_driver_done = 0;
  register_sighandler();
  std::thread sig_handling_thread(RunSigHandlingThread, test_driver,
                                  &test_driver_mu, &test_driver_cv,
                                  &test_driver_done);
  int status = test_driver->Join();
  if (WIFEXITED(status)) {
    if (WEXITSTATUS(status)) {
      gpr_log(GPR_INFO,
              "Resolver component test test-runner exited with code %d",
              WEXITSTATUS(status));
      abort();
    }
  } else if (WIFSIGNALED(status)) {
    gpr_log(GPR_INFO,
            "Resolver component test test-runner ended from signal %d",
            WTERMSIG(status));
    abort();
  } else {
    gpr_log(GPR_INFO,
            "Resolver component test test-runner ended with unknown status %d",
            status);
    abort();
  }
  gpr_mu_lock(&test_driver_mu);
  test_driver_done = 1;
  gpr_cv_signal(&test_driver_cv);
  gpr_mu_unlock(&test_driver_mu);
  sig_handling_thread.join();
  delete test_driver;
  gpr_mu_destroy(&test_driver_mu);
  gpr_cv_destroy(&test_driver_cv);
  grpc_shutdown();
}

}  // namespace testing

}  // namespace grpc
