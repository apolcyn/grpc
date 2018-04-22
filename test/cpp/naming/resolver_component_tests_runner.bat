@rem Copyright 2015 gRPC authors.
@rem
@rem Licensed under the Apache License, Version 2.0 (the "License");
@rem you may not use this file except in compliance with the License.
@rem You may obtain a copy of the License at
@rem
@rem     http://www.apache.org/licenses/LICENSE-2.0
@rem
@rem Unless required by applicable law or agreed to in writing, software
@rem distributed under the License is distributed on an "AS IS" BASIS,
@rem WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
@rem See the License for the specific language governing permissions and
@rem limitations under the License.
@rem
@rem This file is auto-generated

@rem TODO: need this? cd /d %~dp0\..\..\..

set FLAGS_test_bin_path="UNSET ARG"
set FLAGS_dns_server_bin_path="UNSET ARG"
set FLAGS_records_config_path="UNSET ARG"
set FLAGS_dns_server_port="UNSET ARG"
set FLAGS_dns_resolver_bin_path="UNSET ARG"
set FLAGS_tcp_connect_bin_path="UNSET ARG"
if "%1" == "--test_bin_path" (
  set FLAGS_test_bin_path=%2
)
shift
shift
if "%1" == "--dns_server_bin_path" (
  set FLAGS_dns_server_bin_path=%2
)
shift
shift
if "%1" == "--records_config_path" (
  set FLAGS_records_config_path=%2
)
shift
shift
if "%1" == "--dns_server_port" (
  set FLAGS_dns_server_port=%2
)
shift
shift
if "%1" == "--dns_resolver_bin_path" (
  set FLAGS_dns_resolver_bin_path=%2
)
shift
shift
if "%1" == "--tcp_connect_bin_path" (
  set FLAGS_tcp_connect_bin_path=%2
)
shift
shift

set $Env:GRPC_DNS_RESOLVER='ares'
set $Env:GRPC_VERBOSITY='DEBUG'

DNS_SERVER_CMD=python test/cpp/naming/utils/run_process_in_background.py ^
  %FLAGS_dns_server_bin_path ^
  -p %FLAGS_dns_server_port ^
  -r %FLAGS_records_config_path
echo %DNS_SERVER_CMD%
exit 0

%FLAGS_test_bin_path% ^
  --target_name="no-srv-ipv4-single-target.resolver-tests-version-4.grpctestingexp." ^
  --expected_addrs="5.5.5.5:443,False" ^
  --expected_chosen_service_config="" ^
  --expected_lb_policy="" ^
  --local_dns_server_address="127.0.0.1:%FLAGS_dns_server_port%"

%FLAGS_test_bin_path% ^
  --target_name="srv-ipv4-single-target.resolver-tests-version-4.grpctestingexp." ^
  --expected_addrs="1.2.3.4:1234,True" ^
  --expected_chosen_service_config="" ^
  --expected_lb_policy="" ^
  --local_dns_server_address="127.0.0.1:%FLAGS_dns_server_port%"

%FLAGS_test_bin_path% ^
  --target_name="srv-ipv4-multi-target.resolver-tests-version-4.grpctestingexp." ^
  --expected_addrs="1.2.3.5:1234,True;1.2.3.6:1234,True;1.2.3.7:1234,True" ^
  --expected_chosen_service_config="" ^
  --expected_lb_policy="" ^
  --local_dns_server_address="127.0.0.1:%FLAGS_dns_server_port%"

%FLAGS_test_bin_path% ^
  --target_name="srv-ipv6-single-target.resolver-tests-version-4.grpctestingexp." ^
  --expected_addrs="[2607:f8b0:400a:801::1001]:1234,True" ^
  --expected_chosen_service_config="" ^
  --expected_lb_policy="" ^
  --local_dns_server_address="127.0.0.1:%FLAGS_dns_server_port%"

%FLAGS_test_bin_path% ^
  --target_name="srv-ipv6-multi-target.resolver-tests-version-4.grpctestingexp." ^
  --expected_addrs="[2607:f8b0:400a:801::1002]:1234,True;[2607:f8b0:400a:801::1003]:1234,True;[2607:f8b0:400a:801::1004]:1234,True" ^
  --expected_chosen_service_config="" ^
  --expected_lb_policy="" ^
  --local_dns_server_address="127.0.0.1:%FLAGS_dns_server_port%"

%FLAGS_test_bin_path% ^
  --target_name="srv-ipv4-simple-service-config.resolver-tests-version-4.grpctestingexp." ^
  --expected_addrs="1.2.3.4:1234,True" ^
  --expected_chosen_service_config="{\"loadBalancingPolicy\":\"round_robin\",\"methodConfig\":[{\"name\":[{\"method\":\"Foo\",\"service\":\"SimpleService\",\"waitForReady\":true}]}]}" ^
  --expected_lb_policy="round_robin" ^
  --local_dns_server_address="127.0.0.1:%FLAGS_dns_server_port%"

%FLAGS_test_bin_path% ^
  --target_name="ipv4-no-srv-simple-service-config.resolver-tests-version-4.grpctestingexp." ^
  --expected_addrs="1.2.3.4:443,False" ^
  --expected_chosen_service_config="{\"loadBalancingPolicy\":\"round_robin\",\"methodConfig\":[{\"name\":[{\"method\":\"Foo\",\"service\":\"NoSrvSimpleService\",\"waitForReady\":true}]}]}" ^
  --expected_lb_policy="round_robin" ^
  --local_dns_server_address="127.0.0.1:%FLAGS_dns_server_port%"

%FLAGS_test_bin_path% ^
  --target_name="ipv4-no-config-for-cpp.resolver-tests-version-4.grpctestingexp." ^
  --expected_addrs="1.2.3.4:443,False" ^
  --expected_chosen_service_config="" ^
  --expected_lb_policy="" ^
  --local_dns_server_address="127.0.0.1:%FLAGS_dns_server_port%"

%FLAGS_test_bin_path% ^
  --target_name="ipv4-cpp-config-has-zero-percentage.resolver-tests-version-4.grpctestingexp." ^
  --expected_addrs="1.2.3.4:443,False" ^
  --expected_chosen_service_config="" ^
  --expected_lb_policy="" ^
  --local_dns_server_address="127.0.0.1:%FLAGS_dns_server_port%"

%FLAGS_test_bin_path% ^
  --target_name="ipv4-second-language-is-cpp.resolver-tests-version-4.grpctestingexp." ^
  --expected_addrs="1.2.3.4:443,False" ^
  --expected_chosen_service_config="{\"loadBalancingPolicy\":\"round_robin\",\"methodConfig\":[{\"name\":[{\"method\":\"Foo\",\"service\":\"CppService\",\"waitForReady\":true}]}]}" ^
  --expected_lb_policy="round_robin" ^
  --local_dns_server_address="127.0.0.1:%FLAGS_dns_server_port%"

%FLAGS_test_bin_path% ^
  --target_name="ipv4-config-with-percentages.resolver-tests-version-4.grpctestingexp." ^
  --expected_addrs="1.2.3.4:443,False" ^
  --expected_chosen_service_config="{\"loadBalancingPolicy\":\"round_robin\",\"methodConfig\":[{\"name\":[{\"method\":\"Foo\",\"service\":\"AlwaysPickedService\",\"waitForReady\":true}]}]}" ^
  --expected_lb_policy="round_robin" ^
  --local_dns_server_address="127.0.0.1:%FLAGS_dns_server_port%"

%FLAGS_test_bin_path% ^
  --target_name="srv-ipv4-target-has-backend-and-balancer.resolver-tests-version-4.grpctestingexp." ^
  --expected_addrs="1.2.3.4:1234,True;1.2.3.4:443,False" ^
  --expected_chosen_service_config="" ^
  --expected_lb_policy="" ^
  --local_dns_server_address="127.0.0.1:%FLAGS_dns_server_port%"

%FLAGS_test_bin_path% ^
  --target_name="srv-ipv6-target-has-backend-and-balancer.resolver-tests-version-4.grpctestingexp." ^
  --expected_addrs="[2607:f8b0:400a:801::1002]:1234,True;[2607:f8b0:400a:801::1002]:443,False" ^
  --expected_chosen_service_config="" ^
  --expected_lb_policy="" ^
  --local_dns_server_address="127.0.0.1:%FLAGS_dns_server_port%"

%FLAGS_test_bin_path% ^
  --target_name="ipv4-config-causing-fallback-to-tcp.resolver-tests-version-4.grpctestingexp." ^
  --expected_addrs="1.2.3.4:443,False" ^
  --expected_chosen_service_config="{\"loadBalancingPolicy\":\"round_robin\",\"methodConfig\":[{\"name\":[{\"method\":\"Foo\",\"service\":\"SimpleService\",\"waitForReady\":true}]},{\"name\":[{\"method\":\"FooTwo\",\"service\":\"SimpleService\",\"waitForReady\":true}]},{\"name\":[{\"method\":\"FooThree\",\"service\":\"SimpleService\",\"waitForReady\":true}]},{\"name\":[{\"method\":\"FooFour\",\"service\":\"SimpleService\",\"waitForReady\":true}]},{\"name\":[{\"method\":\"FooFive\",\"service\":\"SimpleService\",\"waitForReady\":true}]},{\"name\":[{\"method\":\"FooSix\",\"service\":\"SimpleService\",\"waitForReady\":true}]},{\"name\":[{\"method\":\"FooSeven\",\"service\":\"SimpleService\",\"waitForReady\":true}]},{\"name\":[{\"method\":\"FooEight\",\"service\":\"SimpleService\",\"waitForReady\":true}]},{\"name\":[{\"method\":\"FooNine\",\"service\":\"SimpleService\",\"waitForReady\":true}]},{\"name\":[{\"method\":\"FooTen\",\"service\":\"SimpleService\",\"waitForReady\":true}]},{\"name\":[{\"method\":\"FooEleven\",\"service\":\"SimpleService\",\"waitForReady\":true}]},{\"name\":[{\"method\":\"FooTwelve\",\"service\":\"SimpleService\",\"waitForReady\":true}]},{\"name\":[{\"method\":\"FooTwelve\",\"service\":\"SimpleService\",\"waitForReady\":true}]},{\"name\":[{\"method\":\"FooTwelve\",\"service\":\"SimpleService\",\"waitForReady\":true}]},{\"name\":[{\"method\":\"FooTwelve\",\"service\":\"SimpleService\",\"waitForReady\":true}]}]}" ^
  --expected_lb_policy="" ^
  --local_dns_server_address="127.0.0.1:%FLAGS_dns_server_port%"


