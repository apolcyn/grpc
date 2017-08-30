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
#include <unistd.h>

#include <string>
#include <vector>

#include "test/cpp/util/test_config.h"

int main(int argc, char **argv) {
  grpc::testing::InitTest(&argc, &argv, true);
  grpc_init();
  char *exec_args[3];
  exec_args[0] =
      (char *)"test/cpp/naming/resolver_component_tests_with_run_tests.sh";
  // pass the current binary's directory relative to repo root
  std::string my_bin = argv[0];
  std::string const bin_dir = my_bin.substr(0, my_bin.rfind('/'));
  gpr_log(GPR_INFO, "passing %s as relative dir. my bin is %s", bin_dir.c_str(),
          my_bin.c_str());
  exec_args[1] = (char *)bin_dir.c_str();
  exec_args[2] = NULL;
  execv(exec_args[0], exec_args);
  gpr_log(GPR_ERROR, "exec %s failed.", exec_args[0]);
  abort();
}
