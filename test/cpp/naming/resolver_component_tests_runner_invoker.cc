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

#include "test/core/util/cmdline.h"
#include <string>
#include "src/core/lib/gprpp/thd.h"
#include "src/core/lib/gprpp/inlined_vector.h"


#include "test/core/util/subprocess.h"
#include "src/core/lib/gpr/env.h"
#include "test/core/util/port.h"
#include "test/core/util/test_config.h"

static volatile sig_atomic_t abort_wait_for_child = 0;

static void sighandler(int sig) { abort_wait_for_child = 1; }

static void register_sighandler() {
  struct sigaction act;
  memset(&act, 0, sizeof(act));
  act.sa_handler = sighandler;
  sigaction(SIGINT, &act, nullptr);
  sigaction(SIGTERM, &act, nullptr);
}

namespace {

const int kTestTimeoutSeconds = 60 * 2;

struct SigHandlingThreadArgs {
  gpr_subprocess *test_driver;
  gpr_mu* test_driver_mu;
  gpr_cv* test_driver_cv;
  int* test_driver_done;
};

void RunSigHandlingThread(void *args) {
  SigHandlingThreadArgs *sig_args = reinterpret_cast<SigHandlingThreadArgs*>(args);
  gpr_subprocess *test_driver = sig_args->test_driver;
  gpr_mu* test_driver_mu = sig_args->test_driver_mu;
  gpr_cv* test_driver_cv = sig_args->test_driver_cv;
  int *test_driver_done = sig_args->test_driver_done;
  gpr_timespec overall_deadline =
      gpr_time_add(gpr_now(GPR_CLOCK_MONOTONIC),
                   gpr_time_from_seconds(kTestTimeoutSeconds, GPR_TIMESPAN));
  while (true) {
    gpr_timespec now = gpr_now(GPR_CLOCK_MONOTONIC);
    if (gpr_time_cmp(now, overall_deadline) > 0 || abort_wait_for_child) break;
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
  gpr_subprocess_interrupt(test_driver);
  return;
}
}  // namespace

namespace grpc {

namespace testing {

void InvokeResolverComponentTestsRunner(std::string test_runner_bin_path,
                                        std::string test_bin_path,
                                        std::string dns_server_bin_path,
                                        std::string records_config_path,
                                        std::string dns_resolver_bin_path,
                                        std::string tcp_connect_bin_path) {
  int dns_server_port = grpc_pick_unused_port_or_die();

  char* test_driver_argv[7];
  int num_args = 0;
  test_driver_argv[num_args++] = strdup(test_runner_bin_path.c_str());
  GPR_ASSERT(gpr_asprintf(&test_driver_argv[num_args++], "--test_bin_path=%s", test_bin_path.c_str()));
  GPR_ASSERT(gpr_asprintf(&test_driver_argv[num_args++], "--dns_server_bin_path=%s", dns_server_bin_path.c_str()));
  GPR_ASSERT(gpr_asprintf(&test_driver_argv[num_args++], "--records_config_path=%s", records_config_path.c_str()));
  GPR_ASSERT(gpr_asprintf(&test_driver_argv[num_args++], "--dns_server_port=%s", std::to_string(dns_server_port).c_str()));
  GPR_ASSERT(gpr_asprintf(&test_driver_argv[num_args++], "--dns_resolver_bin_path=%s", dns_resolver_bin_path.c_str()));
  GPR_ASSERT(gpr_asprintf(&test_driver_argv[num_args++], "--tcp_connect_bin_path=%s", tcp_connect_bin_path.c_str()));
  for (int i = 0; i < num_args; i++) {
    gpr_log(GPR_DEBUG, "test_driver_arg[%d]: %s", i, test_driver_argv[i]);
  }
  gpr_subprocess* test_driver = gpr_subprocess_create(num_args, (const char**)test_driver_argv);
  gpr_mu test_driver_mu;
  gpr_mu_init(&test_driver_mu);
  gpr_cv test_driver_cv;
  gpr_cv_init(&test_driver_cv);
  int test_driver_done = 0;
  register_sighandler();
  SigHandlingThreadArgs sig_handling_thread_args;
  sig_handling_thread_args.test_driver = test_driver;
  sig_handling_thread_args.test_driver_mu = &test_driver_mu;
  sig_handling_thread_args.test_driver_cv = &test_driver_cv;
  sig_handling_thread_args.test_driver_done = &test_driver_done;
  grpc_core::Thread sig_handling_thread("signal handling threading",
                                        RunSigHandlingThread,
                                        &sig_handling_thread_args);
  sig_handling_thread.Start();
  int status = gpr_subprocess_join(test_driver);
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
  sig_handling_thread.Join();
  gpr_subprocess_destroy(test_driver);
  gpr_mu_destroy(&test_driver_mu);
  gpr_cv_destroy(&test_driver_cv);
  for (int i = 0; i < num_args; i++) {
    gpr_free(test_driver_argv[i]);
  }
}

}  // namespace testing

}  // namespace grpc

int main(int argc, char** argv) {
  grpc_test_init(argc, argv);
  gpr_cmdline *cl = gpr_cmdline_create("My cool tool");
  int running_under_bazel = 0;
  gpr_cmdline_add_int(cl, "running_under_bazel",
    "True if this test is running under bazel. "
    "False indicates that this test is running under run_tests.py. "
    "Child process test binaries are located differently based on this flag. ",
    &running_under_bazel);
  const char* test_bin_name = nullptr;
  gpr_cmdline_add_string(cl, "test_bin_name",
    "Name, without the preceding path, of the test binary",
    &test_bin_name);
  const char* test_runner_name = nullptr;
  gpr_cmdline_add_string(cl, "test_runner_name",
    "Name, without the preceding path, of the test runner python script",
    &test_runner_name);
  const char* records_config_name = nullptr;
  gpr_cmdline_add_string(cl, "records_config_name",
     "Name, without the preceding path, of the yaml file that configures the DNS records that the local DNS server should serve.",
     &records_config_name);
  const char* grpc_test_directory_relative_to_test_srcdir = "/com_github_grpc_grpc";
  gpr_cmdline_add_string(cl, "grpc_test_directory_relative_to_test_srcdir",
     "This flag only applies if runner_under_bazel is true. This "
     "flag is ignored if runner_under_bazel is false. "
     "Directory of the <repo-root>/test directory relative to bazel's "
     "TEST_SRCDIR environment variable",
     &grpc_test_directory_relative_to_test_srcdir);
  gpr_cmdline_parse(cl, argc, argv);
  gpr_cmdline_destroy(cl);

  grpc_init();
  GPR_ASSERT(strlen(test_bin_name) > 0);
  std::string my_bin = argv[0];
  if (running_under_bazel) {
    GPR_ASSERT(strlen(grpc_test_directory_relative_to_test_srcdir) > 0);
    // Use bazel's TEST_SRCDIR environment variable to locate the "test data"
    // binaries.
    char* test_srcdir = gpr_getenv("TEST_SRCDIR");
    std::string const bin_dir =
        std::string(test_srcdir) + std::string(grpc_test_directory_relative_to_test_srcdir) +
        std::string("/test/cpp/naming");
    // Invoke bazel's executeable links to the .sh and .py scripts (don't use
    // the .sh and .py suffixes) to make
    // sure that we're using bazel's test environment.
    grpc::testing::InvokeResolverComponentTestsRunner(
        std::string(bin_dir) + std::string(test_runner_name),
        std::string(bin_dir) + "/" + std::string(test_bin_name), std::string(bin_dir) + "/utils/dns_server",
        std::string(bin_dir) + std::string(records_config_name),
        std::string(bin_dir) + "/utils/dns_resolver", std::string(bin_dir) + "/utils/tcp_connect");
    gpr_free(test_srcdir);
  } else {
    // Get the current binary's directory relative to repo root to invoke the
    // correct build config (asan/tsan/dbg, etc.).
    std::string const bin_dir = my_bin.substr(0, my_bin.rfind('/'));
    // Invoke the .sh and .py scripts directly where they are in source code.
    grpc::testing::InvokeResolverComponentTestsRunner(
        "test/cpp/naming/" + std::string(test_runner_name),
        std::string(bin_dir) + "/" + std::string(test_bin_name),
        "test/cpp/naming/utils/dns_server.py",
        "test/cpp/naming/" + std::string(records_config_name),
        "test/cpp/naming/utils/dns_resolver.py",
        "test/cpp/naming/utils/tcp_connect.py");
  }
  grpc_shutdown();
  return 0;
}
