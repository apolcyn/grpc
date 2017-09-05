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
#include <unistd.h>

#include <gflags/gflags.h>
#include <string>
#include <vector>

#include "test/cpp/util/subprocess.h"
#include "test/cpp/util/test_config.h"

#include "test/cpp/naming/resolver_component_tests_runner_invoker_common.h"

extern "C" {
#include "test/core/util/port.h"
}

using grpc::SubProcess;

volatile sig_atomic_t abort = 0;

namespace {
  gpr_mu
  std::mutex g_test_driver_mu;
  std::condition_variable g_test_driver_cv;
  int g_test_driver_done;
  SubProcess *g_test_driver = nullptr;

  void RunSigHandlingThread(SubProcess *test_driver, gpr_mu *test_driver_mu, gpr_cv *test_driver_cv, int *test_driver_done) {
    for (;;) {
      if (abort) {
        gpr_mu_lock(test_driver_mu);
        if (test_driver != nullptr) {
          test_driver->Interrupt();
        }
        gpr_mu_unlock(test_driver_mu);
        return;
      }
      gpr_mu_lock(test_driver_mu);
      if (*test_driver_done) {
        gpr_mu_unlock(test_driver_mu);
        return;
      }
      gpr_cv_wait(test_driver_mu, test_driver_cv, gpr_time_add(gpr_now(GPR_CLOCK_REALTIME),
                                                               gpr_time_from_seconds(1, GPR_TIMESPAN)));
      gpr_mu_unlock(test_driver_mu);
      gpr_mu_lock(test_driver_mu);
    }
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
  std::thread sig_handling_thread = SigHandlingThread(test_driver, &test_driver_mu, &test_driver_cv, &test_driver_done);

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
  delete test_driver;
  gpr_mu_destroy(&test_driver_mu);
  gpr_cv_destroy(&test_driver_);
  grpc_shutdown();
}

}  // namespace testing

}  // namespace grpc
