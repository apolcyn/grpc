
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

#include "test/core/naming_end2end/name_end2end_test_util.h"

#include <stdbool.h>
#include <string.h>

#include <grpc/support/log.h>

#include "test/core/util/debugger_macros.h"

static bool g_pre_init_called = false;

void test1_test() {
  naming_end2end_test_resolves_balancer();
}
void test2_test() {
  naming_end2end_test_resolves_balancer();
}

int main(int argc, char **argv) {
  test1_test();
  test2_test();
}

