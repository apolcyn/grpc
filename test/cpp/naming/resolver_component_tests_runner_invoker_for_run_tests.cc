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

/* Intended for running the resolver component test under run_tests.py */

#include <grpc/grpc.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>
#include <unistd.h>

#include <gflags/gflags.h>
#include <string>
#include <vector>

#include "test/cpp/naming/resolver_component_tests_runner_invoker_common.h"
#include "test/cpp/util/subprocess.h"
#include "test/cpp/util/test_config.h"

extern "C" {
#include "test/core/util/port.h"
}

using grpc::SubProcess;

DEFINE_string(test_bin_name, "",
              "Name, without the preceding path, of the test binary");

int main(int argc, char **argv) {
  grpc::testing::InitTest(&argc, &argv, true);
  // get the current binary's directory relative to repo root to invoke the
  // correct build config (asan/tsan/dbg, etc.)
  GPR_ASSERT(FLAGS_test_bin_name != "");
  std::string my_bin = argv[0];
  std::string const bin_dir = my_bin.substr(0, my_bin.rfind('/'));
  gpr_log(GPR_INFO, "passing %s as relative dir. my bin is %s", bin_dir.c_str(),
          my_bin.c_str());
  grpc::testing::InvokeResolverComponentTestsRunner(
      "test/cpp/naming/resolver_component_tests_runner.sh",
      bin_dir + "/" + FLAGS_test_bin_name, "test/cpp/naming/test_dns_server.py",
      "test/cpp/naming/resolver_test_record_groups.yaml");
}
