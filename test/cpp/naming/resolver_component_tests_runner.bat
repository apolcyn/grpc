cd /d %~dp0\..\..\..
set FLAGS_test_bin_path=".\cmake\build\Debug\resolver_component_test.exe"
set FLAGS_dns_server_port=15353
%FLAGS_test_bin_path% ^
  --target_name="srv-ipv4-single-target.resolver-tests-version-4.grpctestingexp." ^
  --expected_addrs="1.2.3.4:1234,True" ^
  --expected_chosen_service_config="" ^
  --expected_lb_policy="" ^
  --local_dns_server_address="127.0.0.1:%FLAGS_dns_server_port%"
