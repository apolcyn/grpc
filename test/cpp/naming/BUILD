# Copyright 2017 gRPC authors.
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

package(
    default_visibility = ["//visibility:public"],
    features = [
        "-layering_check",
        "-parse_headers",
    ],
)

licenses(["notice"])  # Apache v2

load("//bazel:grpc_build_system.bzl", "grpc_cc_test", "grpc_sh_test", "grpc_py_binary")

grpc_cc_test(
    name = "resolver_component_test",
    srcs = [
        "resolver_component_test.cc",
    ],
    external_deps = [
        "gtest",
    ],
    deps = [
        "//test/cpp/util:test_util",
        "//test/core/util:grpc_test_util",
        "//test/core/util:gpr_test_util",
        "//:grpc++",
        "//:grpc",
        "//:gpr",
        "//test/cpp/util:test_config",
    ],
)

grpc_py_binary(
     name = "dns_server",
     srcs = ["dns_server.py"],
     data = [
         "resolver_test_record_groups.yaml",
     ],
     deps = [
         "//twisted:TwistedCore",
         "//yaml"
     ],
)

grpc_sh_test(
    name = "resolver_component_tests_with_bazel",
    srcs = [
        "resolver_component_tests_with_bazel.sh",
    ],
    data = [
        "//test/core/util:pick_port_main",
        ":resolver_component_test",
        ":dns_server",
        "resolver_test_record_groups.yaml", # include the transitive dependency so that the py binary can locate this
    ],
)
