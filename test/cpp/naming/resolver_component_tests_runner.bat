cd /d %~dp0\..\..\..
set FLAGS_test_bin_path=".\cmake\build\Debug\resolver_component_test.exe"
set FLAGS_dns_server_port=15353
%FLAGS_test_bin_path% ^
  --target_name="no-srv-ipv4-single-target.resolver-tests-version-4.grpctestingexp." ^
  --expected_addrs="5.5.5.5:443,False" ^
  --expected_chosen_service_config="" ^
  --expected_lb_policy="" ^
  --local_dns_server_address="127.0.0.1:%FLAGS_dns_server_port%"
@rem %FLAGS_test_bin_path% ^
@rem   --target_name="srv-ipv4-single-target.resolver-tests-version-4.grpctestingexp." ^
@rem   --expected_addrs="1.2.3.4:1234,True" ^
@rem   --expected_chosen_service_config="" ^
@rem   --expected_lb_policy="" ^
@rem   --local_dns_server_address="127.0.0.1:%FLAGS_dns_server_port%"
@rem %FLAGS_test_bin_path% ^
@rem   --target_name="ipv4-no-srv-simple-service-config.resolver-tests-version-4.grpctestingexp." ^
@rem   --expected_addrs="1.2.3.4:443,False" ^
@rem   --expected_chosen_service_config="{\"loadBalancingPolicy\":\"round_robin\",\"methodConfig\":[{\"name\":[{\"method\":\"Foo\",\"service\":\"NoSrvSimpleService\",\"waitForReady\":true}]}]}" ^
@rem   --expected_lb_policy="round_robin" ^
@rem   --local_dns_server_address="127.0.0.1:%FLAGS_dns_server_port%"
@rem %FLAGS_test_bin_path% ^
@rem   --target_name="srv-ipv4-multi-target.resolver-tests-version-4.grpctestingexp." ^
@rem   --expected_addrs="1.2.3.5:1234,True;1.2.3.6:1234,True;1.2.3.7:1234,True" ^
@rem   --expected_chosen_service_config="" ^
@rem   --expected_lb_policy="" ^
@rem   --local_dns_server_address="127.0.0.1:%FLAGS_dns_server_port%"
@rem %FLAGS_test_bin_path% ^
@rem   --target_name="srv-ipv6-single-target.resolver-tests-version-4.grpctestingexp." ^
@rem   --expected_addrs="[2607:f8b0:400a:801::1001]:1234,True" ^
@rem   --expected_chosen_service_config="" ^
@rem   --expected_lb_policy="" ^
@rem   --local_dns_server_address="127.0.0.1:%FLAGS_dns_server_port%"
