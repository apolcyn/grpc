#!/usr/bin/env bash
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
#
# This script is invoked by Jenkins and runs interop test suite.
set -ex

export LANG=en_US.UTF-8

CPP_BUILD_CONFIG=${CPP_BUILD_CONFIG:-dbg}

# Enter the gRPC repo root
cd $(dirname $0)/../..

tools/run_tests/run_naming_with_gce_dns_sanity_check.py

# use an empty authority in test uri's
export GRPC_DNS_AUTHORITY_TESTING_OVERRIDE=''
tools/run_tests/run_tests.py -l c++ -c $CPP_BUILD_CONFIG -r naming_end2end --use_docker || true
