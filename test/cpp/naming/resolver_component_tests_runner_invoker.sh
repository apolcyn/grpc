#!/bin/bash
# Copyright 2015 gRPC authors.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Intended for running the resolver component test under bazel

set -ex

FLAGS_test_bin_name=`echo $1 | grep '\--test_bin_name=' | cut -d "=" -f 2`

# Don't invoke .py directly, use bazel's bin target to get dependencies
exec $TEST_SRCDIR/__main__/test/cpp/naming/resolver_component_tests_runner \
  "--test_bin_path=$TEST_SRCDIR/__main__/test/cpp/naming/$FLAGS_test_bin_name" \
  "--dns_server_bin_path=$TEST_SRCDIR/__main__/test/cpp/naming/test_dns_server" \
  "--records_config_path=$TEST_SRCDIR/__main__/test/cpp/naming/resolver_test_record_groups.yaml"
