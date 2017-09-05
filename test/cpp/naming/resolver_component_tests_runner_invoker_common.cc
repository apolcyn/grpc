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
  grpc_shutdown();
}

}  // namespace testing

}  // namespace grpc
