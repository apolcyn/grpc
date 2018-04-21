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
#include <grpc/support/port_platform.h>
#include <grpc/grpc.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>
#include <signal.h>
#include <string.h>

#include <gflags/gflags.h>
#include <string>
#include <thread>
#include <vector>

#include "test/cpp/util/subprocess.h"
#include "test/cpp/util/test_config.h"

#include "src/core/lib/gpr/env.h"
#include "test/core/util/port.h"

DEFINE_string(test_bin_name, "",
              "Name, without the preceding path, of the test binary");

using grpc::SubProcess;

#ifndef GPR_WINDOWS
static volatile sig_atomic_t abort_wait_for_child = 0;

static void sighandler(int sig) { abort_wait_for_child = 1; }

static void register_sighandler() {
  struct sigaction act;
  memset(&act, 0, sizeof(act));
  act.sa_handler = sighandler;
  sigaction(SIGINT, &act, nullptr);
  sigaction(SIGTERM, &act, nullptr);
}
#endif

namespace {

const int kTestTimeoutSeconds = 30;

void RunSigHandlingThread(SubProcess* test_driver, gpr_mu* test_driver_mu,
                          gpr_cv* test_driver_cv, int* test_driver_done) {
  gpr_timespec overall_deadline =
      gpr_time_add(gpr_now(GPR_CLOCK_MONOTONIC),
                   gpr_time_from_seconds(kTestTimeoutSeconds, GPR_TIMESPAN));
  while (true) {
    gpr_timespec now = gpr_now(GPR_CLOCK_MONOTONIC);
    if (gpr_time_cmp(now, overall_deadline) > 0) break;
#ifndef GPR_WINDOWS
    if (abort_wait_for_child) break;
#endif
    gpr_mu_lock(test_driver_mu);
    if (*test_driver_done) {
      gpr_mu_unlock(test_driver_mu);
      return;
    }
    gpr_timespec wait_deadline = gpr_time_add(
        gpr_now(GPR_CLOCK_MONOTONIC), gpr_time_from_seconds(1, GPR_TIMESPAN));
    gpr_cv_wait(test_driver_cv, test_driver_mu, wait_deadline);
    gpr_mu_unlock(test_driver_mu);
  }
  gpr_log(GPR_DEBUG,
          "Test timeout reached or received signal. Interrupting test driver "
          "child process.");
  test_driver->Interrupt();
  return;
}
}  // namespace

namespace grpc {

namespace testing {

void InvokeResolverComponentTestsRunner(std::string test_runner_bin_path,
                                        std::string test_bin_path) {
  int dns_server_port = grpc_pick_unused_port_or_die();
  SubProcess* dns_server = new SubProcess({
		  "python",
		  "test\\cpp\\naming\\utils\\dns_server.py",
		  "--port",
		  dns_server_port,
		  "--records_config",
		  "test\\cpp\\naming\\resolver_test_record_groups.yaml"});
  SubProcess* test_driver =
      new SubProcess({test_runner_bin_path, "--test_bin_path=" + test_bin_path,
                      "--dns_server_port=" + std::to_string(dns_server_port)});
  gpr_mu test_driver_mu;
  gpr_mu_init(&test_driver_mu);
  gpr_cv test_driver_cv;
  gpr_cv_init(&test_driver_cv);
  int test_driver_done = 0;
#ifndef GPR_WINDOWS
  register_sighandler();
#endif
  std::thread sig_handling_thread(RunSigHandlingThread, test_driver,
                                  &test_driver_mu, &test_driver_cv,
                                  &test_driver_done);
  gpr_log(GPR_DEBUG, "Now wait for the driver script to finish.");
  int status = test_driver->Join();
  gpr_log(GPR_DEBUG, "test_driver process status: %d. Now kill the DNS server and wait for it to finish.", status);
  dns_server->Interrupt();
  int status = dns_server->Join();
  gpr_log(GPR_DEBUG, "DNS server process status: %d.", status);
#ifndef GPR_WINDOWS
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
#endif
  gpr_mu_lock(&test_driver_mu);
  test_driver_done = 1;
  gpr_cv_signal(&test_driver_cv);
  gpr_mu_unlock(&test_driver_mu);
  sig_handling_thread.join();
  delete test_driver;
  gpr_mu_destroy(&test_driver_mu);
  gpr_cv_destroy(&test_driver_cv);
}

}  // namespace testing

}  // namespace grpc

int main(int argc, char** argv) {
  grpc::testing::InitTest(&argc, &argv, true);
  grpc_init();
  GPR_ASSERT(FLAGS_test_bin_name != "");
  std::string my_bin = argv[0];
  // Get the current binary's directory relative to repo root to invoke the
  // correct build config (asan/tsan/dbg, etc.).
  std::string const bin_dir = my_bin.substr(0, my_bin.rfind("\\"));
  // Invoke the .sh and .py scripts directly where they are in source code.
  grpc::testing::InvokeResolverComponentTestsRunner(
      "test\\cpp\\naming\\resolver_component_tests_runner.bat",
      bin_dir + "\\" + FLAGS_test_bin_name,
      "test\\cpp\\naming\\utils\\dns_server.py",
      "test\\cpp\\naming\\resolver_test_record_groups.yaml",
      "test\\cpp\\naming\\utils\\dns_resolver.py",
      "test\\cpp\\naming\\utils\\tcp_connect.py");
  }
  grpc_shutdown();
  return 0;
}
