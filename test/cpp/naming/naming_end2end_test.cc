
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

#include "test/cpp/naming/naming_end2end_test_util.h"

#include <stdbool.h>
#include <string.h>
#include "test/core/util/test_config.h"

#include <grpc/support/log.h>

#include <gtest/gtest.h>

#include "test/core/util/debugger_macros.h"

//  Example yaml test config:
//
//  - expected_addrs: ['1.2.3.4:1234']
//    expected_config_index: null
//    record_to_resolve: ipv4-single-target.grpc.com.
//    records:
//      _grpclb._tcp.srv-ipv4-single-target.grpc.com.:
//      - {SRV: 0 0 1234 ipv4-single-target}
//      ipv4-single-target.grpc.com.:
//      - {A: 1.2.3.4}


TEST(NamingEnd2EndTest, srv_ipv4_single_target_grpc_com__test) {
  naming_end2end_test_resolves_balancer(srv-ipv4-single-target.grpc.com., 1.2.3.4:1234, NULL);
}

TEST(NamingEnd2EndTest, srv_ipv4_simple_service_config_grpc_com__test) {
  naming_end2end_test_resolves_balancer(srv-ipv4-simple-service-config.grpc.com., 1.2.3.4:1234, "[{\"loadBalancingPolicy\":\"round_robin\",\"methodConfig\":[{\"name\":[{\"method\":\"Foo\",\"service\":\"SimpleService\",\"waitForReady\":true}]}]}]");
}

TEST(NamingEnd2EndTest, ipv4_cpp_config_has_zero_percentage_grpc_com__test) {
  naming_end2end_test_resolves_backend(ipv4-cpp-config-has-zero-percentage.grpc.com., 1.2.3.4:443, "[{\"percentage\":0,\"loadBalancingPolicy\":\"round_robin\",\"methodConfig\":[{\"name\":[{\"method\":\"Foo\",\"service\":\"CppService\",\"waitForReady\":true}]}]}]");
}

TEST(NamingEnd2EndTest, ipv4_no_config_for_cpp_grpc_com__test) {
  naming_end2end_test_resolves_backend(ipv4-no-config-for-cpp.grpc.com., 1.2.3.4:443, "[{\"loadBalancingPolicy\":\"round_robin\",\"methodConfig\":[{\"name\":[{\"method\":\"Foo\",\"service\":\"PythonService\",\"waitForReady\":true}]}],\"clientLanguage\":[\"python\"]}]");
}

TEST(NamingEnd2EndTest, ipv4_second_language_is_cpp_grpc_com__test) {
  naming_end2end_test_resolves_backend(ipv4-second-language-is-cpp.grpc.com., 1.2.3.4:443, "[{\"loadBalancingPolicy\":\"round_robin\",\"methodConfig\":[{\"name\":[{\"method\":\"Foo\",\"service\":\"GoService\",\"waitForReady\":true}]}],\"clientLanguage\":[\"go\"]},{\"loadBalancingPolicy\":\"round_robin\",\"methodConfig\":[{\"name\":[{\"method\":\"Foo\",\"service\":\"CppService\",\"waitForReady\":true}]}],\"clientLanguage\":[\"c++\"]}]");
}

TEST(NamingEnd2EndTest, ipv4_no_srv_simple_service_config_grpc_com__test) {
  naming_end2end_test_resolves_backend(ipv4-no-srv-simple-service-config.grpc.com., 1.2.3.4:443, "[{\"loadBalancingPolicy\":\"round_robin\",\"methodConfig\":[{\"name\":[{\"method\":\"Foo\",\"service\":\"NoSrvSimpleService\",\"waitForReady\":true}]}]}]");
}

TEST(NamingEnd2EndTest, ipv4_config_with_percentages_grpc_com__test) {
  naming_end2end_test_resolves_backend(ipv4-config-with-percentages.grpc.com., 1.2.3.4:443, "[{\"percentage\":0,\"loadBalancingPolicy\":\"round_robin\",\"methodConfig\":[{\"name\":[{\"method\":\"Foo\",\"service\":\"NeverPickedService\",\"waitForReady\":true}]}]},{\"percentage\":100,\"loadBalancingPolicy\":\"round_robin\",\"methodConfig\":[{\"name\":[{\"method\":\"Foo\",\"service\":\"AlwaysPickedService\",\"waitForReady\":true}]}]}]");
}

TEST(NamingEnd2EndTest, srv_ipv4_multi_target_grpc_com__test) {
  naming_end2end_test_resolves_balancer(srv-ipv4-multi-target.grpc.com., 1.2.3.5:1234,1.2.3.6:1234,1.2.3.7:1234, NULL);
}

TEST(NamingEnd2EndTest, srv_ipv6_single_target_grpc_com__test) {
  naming_end2end_test_resolves_balancer(srv-ipv6-single-target.grpc.com., [2607:f8b0:400a:801::1001]:1234, NULL);
}

TEST(NamingEnd2EndTest, srv_ipv6_multi_target_grpc_com__test) {
  naming_end2end_test_resolves_balancer(srv-ipv6-multi-target.grpc.com., [2607:f8b0:400a:801::1002]:1234,[2607:f8b0:400a:801::1003]:1234,[2607:f8b0:400a:801::1004]:1234, NULL);
}

int main(int argc, char **argv) {
  grpc_test_init(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  int ret = RUN_ALL_TESTS();
  return ret;
}

