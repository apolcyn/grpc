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

setlocal ENABLEDELAYEDEXPANSION
@echo off

set FLAGS_test_bin_path="BAD UNSET ARG"
set FLAGS_dns_server_bin_path="BAD UNSET ARG"
set FLAGS_records_config_path="BAD UNSET ARG"
set FLAGS_dns_server_port="BAD UNSET ARG"
set FLAGS_dns_resolver_bin_path="BAD UNSET ARG"
set FLAGS_tcp_connect_bin_path="BAD UNSET ARG"

@rem Parse the command args provided by resolver_component_tests_runner_invoker
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

set GRPC_DNS_RESOLVER=ares
set PYTHON=C:\Python27\python.exe

for /f "tokens=1 delims=" %%f in (
'%PYTHON% -c "import tempfile; print(tempfile.mktemp())"'
) do set DNS_SERVER_LAUNCHER_OUTPUT=%%f

%PYTHON% test\cpp\naming\utils\run_process_in_background.py ^
  %PYTHON% ^
  test\cpp\naming\utils\dns_server.py ^
  -p %FLAGS_dns_server_port% ^
  -r test\cpp\naming\resolver_test_record_groups.yaml ^
  > %DNS_SERVER_LAUNCHER_OUTPUT%

for /f "tokens=1 delims= " %%p in (
  'type %DNS_SERVER_LAUNCHER_OUTPUT%'
) do set DNS_SERVER_PID=%%p

for /f "tokens=2 delims= " %%p in (
  'type %DNS_SERVER_LAUNCHER_OUTPUT%'
) do set DNS_SERVER_LOG=%%p

@rem Wait until the DNS server is up and running
set DNS_SERVER_STATUS=unkown
for /l %%i in (1, 1, 10) do (
  echo "Health check: attempt to connect to DNS server over TCP."
  %PYTHON% %FLAGS_tcp_connect_bin_path% ^
    -s 127.0.0.1 ^
    -p %FLAGS_dns_server_port% ^
    -t 1
  if !ERRORLEVEL! == 0 (
    echo "Health check: attempt to make an A-record query to DNS server."
    %PYTHON% %FLAGS_dns_resolver_bin_path% ^
      -n health-check-local-dns-server-is-alive.resolver-tests.grpctestingexp ^
      -s 127.0.0.1 ^
      -p %FLAGS_dns_server_port% | findstr 123.123.123.123
    if !ERRORLEVEL! == 0 (
      echo "DNS server is up. Successfully reached it over UDP and TCP."
      set DNS_SERVER_STATUS=alive
      goto dns_server_health_check_done
    )
  )
  %PYTHON% -c "import time; time.sleep(0.2)"
)
:dns_server_health_check_done
if NOT "%DNS_SERVER_STATUS%" == "alive" (
  taskkill /pid %DNS_SERVER_PID% /f
  echo "Failed to connect to DNS server. Skipping tests, exitting 1."
  echo "======= DNS server log, %DNS_SERVER_LOG%: ============="
  type %DNS_SERVER_LOG%
  echo "======= end DNS server log ============================"
  exit 1
)

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

taskkill /pid %DNS_SERVER_PID% /f

endlocal
