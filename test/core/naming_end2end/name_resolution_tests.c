
/*
 *
 * Copyright 2015 gRPC authors.
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

/* This file is auto-generated */

#include "test/core/naming_end2end/name_resolution_tests.h"

#include <stdbool.h>
#include <string.h>

#include <grpc/support/log.h>

#include "test/core/util/debugger_macros.h"

static bool g_pre_init_called = false;

extern void some_test1(grpc_end2end_test_config config);
extern void some_test1_pre_init(void);
extern void some_test2(grpc_end2end_test_config config);
extern void some_test2_pre_init(void);
extern void some_test3(grpc_end2end_test_config config);
extern void some_test3_pre_init(void);

void grpc_end2end_tests_pre_init(void) {
  GPR_ASSERT(!g_pre_init_called);
  g_pre_init_called = true;
  grpc_summon_debugger_macros();
  some_test1_pre_init();
  some_test2_pre_init();
  some_test3_pre_init();
}

void grpc_end2end_tests(int argc, char **argv,
                        grpc_end2end_test_config config) {
  int i;

  GPR_ASSERT(g_pre_init_called);

  if (argc <= 1) {
    some_test1(config);
    some_test2(config);
    some_test3(config);
    return;
  }

  for (i = 1; i < argc; i++) {
    if (0 == strcmp("some_test1", argv[i])) {
      some_test1(config);
      continue;
    }
    if (0 == strcmp("some_test2", argv[i])) {
      some_test2(config);
      continue;
    }
    if (0 == strcmp("some_test3", argv[i])) {
      some_test3(config);
      continue;
    }
    gpr_log(GPR_DEBUG, "not a test: '%s'", argv[i]);
    abort();
  }
}
