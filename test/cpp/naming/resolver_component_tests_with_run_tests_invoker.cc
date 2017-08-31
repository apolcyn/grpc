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

#include <gflags/gflags.h>
#include <grpc/grpc.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "test/cpp/util/test_config.h"

DEFINE_bool(unsecure, false, "use the unsecure grpc build");

int main(int argc, char **argv) {
  grpc::testing::InitTest(&argc, &argv, true);
  std::string test_binary_name = "resolver_component_test";
  if (FLAGS_unsecure) {
    test_binary_name.append("_unsecure");
  }
  grpc_init();
  char *exec_args[4];
  exec_args[0] =
      (char *)"test/cpp/naming/resolver_component_tests_with_run_tests.sh";
  // pass the current binary's directory relative to repo root
  std::string my_bin = argv[0];
  std::string bin_dir = my_bin.substr(0, my_bin.rfind('/'));
  std::string const test_binary_path = bin_dir + "/" + test_binary_name;
  std::string const pick_port_binary_path = bin_dir + "/pick_port_main";
  gpr_log(GPR_INFO,
          "passing %s as test binary path, and %s as pick port binary path. my "
          "bin is %s",
          test_binary_path.c_str(), pick_port_binary_path.c_str(),
          my_bin.c_str());
  exec_args[1] = (char *)test_binary_path.c_str();
  exec_args[2] = (char *)pick_port_binary_path.c_str();
  exec_args[3] = NULL;
  execv(exec_args[0], exec_args);
  gpr_log(GPR_ERROR, "exec %s failed.", exec_args[0]);
  abort();
}
