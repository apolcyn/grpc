#!/usr/bin/env python
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

# This file is auto-generated

import argparse
import sys
import subprocess
import tempfile
import os
import time
import signal
import platform


argp = argparse.ArgumentParser(description='Run c-ares resolver tests')
argp.add_argument('--test_bin_path', default=None, type=str,
                  help='Path to gtest test binary to invoke.')
argp.add_argument('--dns_server_bin_path', default=None, type=str,
                  help='Path to local DNS server python script.')
argp.add_argument('--records_config_path', default=None, type=str,
                  help=('Path to DNS records yaml file that '
                        'specifies records for the DNS sever. '))
argp.add_argument('--dns_server_port', default=None, type=int,
                  help=('Port that local DNS server is listening on.'))
argp.add_argument('--dns_resolver_bin_path', default=None, type=str,
                  help=('Path to the DNS health check utility.'))
argp.add_argument('--tcp_connect_bin_path', default=None, type=str,
                  help=('Path to the TCP health check utility.'))
args = argp.parse_args()

def test_runner_log(msg):
  sys.stderr.write('\n%s: %s\n' % (__file__, msg))

def python_args(arg_list):
  if platform.system() == 'Windows':
    return [sys.executable] + arg_list
  return arg_list

cur_resolver = os.environ.get('GRPC_DNS_RESOLVER')
if cur_resolver and cur_resolver != 'ares':
  test_runner_log(('WARNING: cur resolver set to %s. This set of tests '
      'needs to use GRPC_DNS_RESOLVER=ares.'))
  test_runner_log('Exit 1 without running tests.')
  sys.exit(1)
os.environ.update({'GRPC_TRACE': 'cares_resolver'})

def wait_until_dns_server_is_up(args,
                                dns_server_subprocess,
                                dns_server_subprocess_output):
  for i in range(0, 30):
    test_runner_log('Health check: attempt to connect to DNS server over TCP.')
    tcp_connect_subprocess = subprocess.Popen(python_args([
        args.tcp_connect_bin_path,
        '--server_host', '127.0.0.1',
        '--server_port', str(args.dns_server_port),
        '--timeout', str(1)]))
    tcp_connect_subprocess.communicate()
    if tcp_connect_subprocess.returncode == 0:
      test_runner_log(('Health check: attempt to make an A-record '
                       'query to DNS server.'))
      dns_resolver_subprocess = subprocess.Popen(python_args([
          args.dns_resolver_bin_path,
          '--qname', 'health-check-local-dns-server-is-alive.resolver-tests.grpctestingexp',
          '--server_host', '127.0.0.1',
          '--server_port', str(args.dns_server_port)]),
          stdout=subprocess.PIPE)
      dns_resolver_stdout, _ = dns_resolver_subprocess.communicate()
      if dns_resolver_subprocess.returncode == 0:
        if '123.123.123.123' in dns_resolver_stdout:
          test_runner_log(('DNS server is up! '
                           'Successfully reached it over UDP and TCP.'))
        return
    time.sleep(0.1)
  dns_server_subprocess.kill()
  dns_server_subprocess.wait()
  test_runner_log(('Failed to reach DNS server over TCP and/or UDP. '
                   'Exitting without running tests.'))
  test_runner_log('======= DNS server stdout '
                  '(merged stdout and stderr) =============')
  with open(dns_server_subprocess_output, 'r') as l:
    test_runner_log(l.read())
  test_runner_log('======= end DNS server output=========')
  sys.exit(1)

dns_server_subprocess_output = tempfile.mktemp()
with open(dns_server_subprocess_output, 'w') as l:
  dns_server_subprocess = subprocess.Popen(python_args([
      args.dns_server_bin_path,
      '--port', str(args.dns_server_port),
      '--records_config_path', args.records_config_path]),
      stdin=subprocess.PIPE,
      stdout=l,
      stderr=l)

def _quit_on_signal(signum, _frame):
  test_runner_log('Received signal: %d' % signum)
  dns_server_subprocess.kill()
  dns_server_subprocess.wait()
  sys.exit(1)

signal.signal(signal.SIGINT, _quit_on_signal)
signal.signal(signal.SIGTERM, _quit_on_signal)
wait_until_dns_server_is_up(args,
                            dns_server_subprocess,
                            dns_server_subprocess_output)
num_test_failures = 0

test_runner_log('Run test with target: %s' % 'no-srv-ipv4-single-target.resolver-tests-version-4.grpctestingexp.')
current_test_subprocess = subprocess.Popen([
  args.test_bin_path,
  '--target_name', 'no-srv-ipv4-single-target.resolver-tests-version-4.grpctestingexp.',
  '--expected_addrs', '5.5.5.5:443,False',
  '--expected_chosen_service_config', '',
  '--expected_service_config_error', '',
  '--expected_lb_policy', '',
  '--enable_srv_queries', 'True',
  '--enable_txt_queries', 'True',
  '--inject_broken_nameserver_list', 'False',
  '--local_dns_server_address', '127.0.0.1:%d' % args.dns_server_port])
current_test_subprocess.communicate()
if current_test_subprocess.returncode != 0:
  num_test_failures += 1

test_runner_log('Run test with target: %s' % 'srv-ipv4-single-target.resolver-tests-version-4.grpctestingexp.')
current_test_subprocess = subprocess.Popen([
  args.test_bin_path,
  '--target_name', 'srv-ipv4-single-target.resolver-tests-version-4.grpctestingexp.',
  '--expected_addrs', '1.2.3.4:1234,True',
  '--expected_chosen_service_config', '',
  '--expected_service_config_error', '',
  '--expected_lb_policy', '',
  '--enable_srv_queries', 'True',
  '--enable_txt_queries', 'True',
  '--inject_broken_nameserver_list', 'False',
  '--local_dns_server_address', '127.0.0.1:%d' % args.dns_server_port])
current_test_subprocess.communicate()
if current_test_subprocess.returncode != 0:
  num_test_failures += 1

test_runner_log('Run test with target: %s' % 'srv-ipv4-multi-target.resolver-tests-version-4.grpctestingexp.')
current_test_subprocess = subprocess.Popen([
  args.test_bin_path,
  '--target_name', 'srv-ipv4-multi-target.resolver-tests-version-4.grpctestingexp.',
  '--expected_addrs', '1.2.3.5:1234,True;1.2.3.6:1234,True;1.2.3.7:1234,True',
  '--expected_chosen_service_config', '',
  '--expected_service_config_error', '',
  '--expected_lb_policy', '',
  '--enable_srv_queries', 'True',
  '--enable_txt_queries', 'True',
  '--inject_broken_nameserver_list', 'False',
  '--local_dns_server_address', '127.0.0.1:%d' % args.dns_server_port])
current_test_subprocess.communicate()
if current_test_subprocess.returncode != 0:
  num_test_failures += 1

test_runner_log('Run test with target: %s' % 'srv-ipv6-single-target.resolver-tests-version-4.grpctestingexp.')
current_test_subprocess = subprocess.Popen([
  args.test_bin_path,
  '--target_name', 'srv-ipv6-single-target.resolver-tests-version-4.grpctestingexp.',
  '--expected_addrs', '[2607:f8b0:400a:801::1001]:1234,True',
  '--expected_chosen_service_config', '',
  '--expected_service_config_error', '',
  '--expected_lb_policy', '',
  '--enable_srv_queries', 'True',
  '--enable_txt_queries', 'True',
  '--inject_broken_nameserver_list', 'False',
  '--local_dns_server_address', '127.0.0.1:%d' % args.dns_server_port])
current_test_subprocess.communicate()
if current_test_subprocess.returncode != 0:
  num_test_failures += 1

test_runner_log('Run test with target: %s' % 'srv-ipv6-multi-target.resolver-tests-version-4.grpctestingexp.')
current_test_subprocess = subprocess.Popen([
  args.test_bin_path,
  '--target_name', 'srv-ipv6-multi-target.resolver-tests-version-4.grpctestingexp.',
  '--expected_addrs', '[2607:f8b0:400a:801::1002]:1234,True;[2607:f8b0:400a:801::1003]:1234,True;[2607:f8b0:400a:801::1004]:1234,True',
  '--expected_chosen_service_config', '',
  '--expected_service_config_error', '',
  '--expected_lb_policy', '',
  '--enable_srv_queries', 'True',
  '--enable_txt_queries', 'True',
  '--inject_broken_nameserver_list', 'False',
  '--local_dns_server_address', '127.0.0.1:%d' % args.dns_server_port])
current_test_subprocess.communicate()
if current_test_subprocess.returncode != 0:
  num_test_failures += 1

test_runner_log('Run test with target: %s' % 'srv-ipv4-simple-service-config.resolver-tests-version-4.grpctestingexp.')
current_test_subprocess = subprocess.Popen([
  args.test_bin_path,
  '--target_name', 'srv-ipv4-simple-service-config.resolver-tests-version-4.grpctestingexp.',
  '--expected_addrs', '1.2.3.4:1234,True',
  '--expected_chosen_service_config', '{"loadBalancingPolicy":"round_robin","methodConfig":[{"name":[{"method":"Foo","service":"SimpleService"}],"waitForReady":true}]}',
  '--expected_service_config_error', '',
  '--expected_lb_policy', 'round_robin',
  '--enable_srv_queries', 'True',
  '--enable_txt_queries', 'True',
  '--inject_broken_nameserver_list', 'False',
  '--local_dns_server_address', '127.0.0.1:%d' % args.dns_server_port])
current_test_subprocess.communicate()
if current_test_subprocess.returncode != 0:
  num_test_failures += 1

test_runner_log('Run test with target: %s' % 'ipv4-no-srv-simple-service-config.resolver-tests-version-4.grpctestingexp.')
current_test_subprocess = subprocess.Popen([
  args.test_bin_path,
  '--target_name', 'ipv4-no-srv-simple-service-config.resolver-tests-version-4.grpctestingexp.',
  '--expected_addrs', '1.2.3.4:443,False',
  '--expected_chosen_service_config', '{"loadBalancingPolicy":"round_robin","methodConfig":[{"name":[{"method":"Foo","service":"NoSrvSimpleService"}],"waitForReady":true}]}',
  '--expected_service_config_error', '',
  '--expected_lb_policy', 'round_robin',
  '--enable_srv_queries', 'True',
  '--enable_txt_queries', 'True',
  '--inject_broken_nameserver_list', 'False',
  '--local_dns_server_address', '127.0.0.1:%d' % args.dns_server_port])
current_test_subprocess.communicate()
if current_test_subprocess.returncode != 0:
  num_test_failures += 1

test_runner_log('Run test with target: %s' % 'ipv4-no-config-for-cpp.resolver-tests-version-4.grpctestingexp.')
current_test_subprocess = subprocess.Popen([
  args.test_bin_path,
  '--target_name', 'ipv4-no-config-for-cpp.resolver-tests-version-4.grpctestingexp.',
  '--expected_addrs', '1.2.3.4:443,False',
  '--expected_chosen_service_config', '',
  '--expected_service_config_error', '',
  '--expected_lb_policy', '',
  '--enable_srv_queries', 'True',
  '--enable_txt_queries', 'True',
  '--inject_broken_nameserver_list', 'False',
  '--local_dns_server_address', '127.0.0.1:%d' % args.dns_server_port])
current_test_subprocess.communicate()
if current_test_subprocess.returncode != 0:
  num_test_failures += 1

test_runner_log('Run test with target: %s' % 'ipv4-cpp-config-has-zero-percentage.resolver-tests-version-4.grpctestingexp.')
current_test_subprocess = subprocess.Popen([
  args.test_bin_path,
  '--target_name', 'ipv4-cpp-config-has-zero-percentage.resolver-tests-version-4.grpctestingexp.',
  '--expected_addrs', '1.2.3.4:443,False',
  '--expected_chosen_service_config', '',
  '--expected_service_config_error', '',
  '--expected_lb_policy', '',
  '--enable_srv_queries', 'True',
  '--enable_txt_queries', 'True',
  '--inject_broken_nameserver_list', 'False',
  '--local_dns_server_address', '127.0.0.1:%d' % args.dns_server_port])
current_test_subprocess.communicate()
if current_test_subprocess.returncode != 0:
  num_test_failures += 1

test_runner_log('Run test with target: %s' % 'ipv4-second-language-is-cpp.resolver-tests-version-4.grpctestingexp.')
current_test_subprocess = subprocess.Popen([
  args.test_bin_path,
  '--target_name', 'ipv4-second-language-is-cpp.resolver-tests-version-4.grpctestingexp.',
  '--expected_addrs', '1.2.3.4:443,False',
  '--expected_chosen_service_config', '{"loadBalancingPolicy":"round_robin","methodConfig":[{"name":[{"method":"Foo","service":"CppService"}],"waitForReady":true}]}',
  '--expected_service_config_error', '',
  '--expected_lb_policy', 'round_robin',
  '--enable_srv_queries', 'True',
  '--enable_txt_queries', 'True',
  '--inject_broken_nameserver_list', 'False',
  '--local_dns_server_address', '127.0.0.1:%d' % args.dns_server_port])
current_test_subprocess.communicate()
if current_test_subprocess.returncode != 0:
  num_test_failures += 1

test_runner_log('Run test with target: %s' % 'ipv4-config-with-percentages.resolver-tests-version-4.grpctestingexp.')
current_test_subprocess = subprocess.Popen([
  args.test_bin_path,
  '--target_name', 'ipv4-config-with-percentages.resolver-tests-version-4.grpctestingexp.',
  '--expected_addrs', '1.2.3.4:443,False',
  '--expected_chosen_service_config', '{"loadBalancingPolicy":"round_robin","methodConfig":[{"name":[{"method":"Foo","service":"AlwaysPickedService"}],"waitForReady":true}]}',
  '--expected_service_config_error', '',
  '--expected_lb_policy', 'round_robin',
  '--enable_srv_queries', 'True',
  '--enable_txt_queries', 'True',
  '--inject_broken_nameserver_list', 'False',
  '--local_dns_server_address', '127.0.0.1:%d' % args.dns_server_port])
current_test_subprocess.communicate()
if current_test_subprocess.returncode != 0:
  num_test_failures += 1

test_runner_log('Run test with target: %s' % 'srv-ipv4-target-has-backend-and-balancer.resolver-tests-version-4.grpctestingexp.')
current_test_subprocess = subprocess.Popen([
  args.test_bin_path,
  '--target_name', 'srv-ipv4-target-has-backend-and-balancer.resolver-tests-version-4.grpctestingexp.',
  '--expected_addrs', '1.2.3.4:1234,True;1.2.3.4:443,False',
  '--expected_chosen_service_config', '',
  '--expected_service_config_error', '',
  '--expected_lb_policy', '',
  '--enable_srv_queries', 'True',
  '--enable_txt_queries', 'True',
  '--inject_broken_nameserver_list', 'False',
  '--local_dns_server_address', '127.0.0.1:%d' % args.dns_server_port])
current_test_subprocess.communicate()
if current_test_subprocess.returncode != 0:
  num_test_failures += 1

test_runner_log('Run test with target: %s' % 'srv-ipv6-target-has-backend-and-balancer.resolver-tests-version-4.grpctestingexp.')
current_test_subprocess = subprocess.Popen([
  args.test_bin_path,
  '--target_name', 'srv-ipv6-target-has-backend-and-balancer.resolver-tests-version-4.grpctestingexp.',
  '--expected_addrs', '[2607:f8b0:400a:801::1002]:1234,True;[2607:f8b0:400a:801::1002]:443,False',
  '--expected_chosen_service_config', '',
  '--expected_service_config_error', '',
  '--expected_lb_policy', '',
  '--enable_srv_queries', 'True',
  '--enable_txt_queries', 'True',
  '--inject_broken_nameserver_list', 'False',
  '--local_dns_server_address', '127.0.0.1:%d' % args.dns_server_port])
current_test_subprocess.communicate()
if current_test_subprocess.returncode != 0:
  num_test_failures += 1

test_runner_log('Run test with target: %s' % 'ipv4-config-causing-fallback-to-tcp.resolver-tests-version-4.grpctestingexp.')
current_test_subprocess = subprocess.Popen([
  args.test_bin_path,
  '--target_name', 'ipv4-config-causing-fallback-to-tcp.resolver-tests-version-4.grpctestingexp.',
  '--expected_addrs', '1.2.3.4:443,False',
  '--expected_chosen_service_config', '{"loadBalancingPolicy":"round_robin","methodConfig":[{"name":[{"method":"Foo","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"FooTwo","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"FooThree","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"FooFour","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"FooFive","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"FooSix","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"FooSeven","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"FooEight","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"FooNine","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"FooTen","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"FooEleven","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"FooTwelve","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"FooTwelve","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"FooTwelve","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"FooTwelve","service":"SimpleService"}],"waitForReady":true}]}',
  '--expected_service_config_error', '',
  '--expected_lb_policy', '',
  '--enable_srv_queries', 'True',
  '--enable_txt_queries', 'True',
  '--inject_broken_nameserver_list', 'False',
  '--local_dns_server_address', '127.0.0.1:%d' % args.dns_server_port])
current_test_subprocess.communicate()
if current_test_subprocess.returncode != 0:
  num_test_failures += 1

test_runner_log('Run test with target: %s' % 'ipv4-8k-service-config.resolver-tests-version-4.grpctestingexp.')
current_test_subprocess = subprocess.Popen([
  args.test_bin_path,
  '--target_name', 'ipv4-8k-service-config.resolver-tests-version-4.grpctestingexp.',
  '--expected_addrs', '1.2.3.4:443,False',
  '--expected_chosen_service_config', '{"loadBalancingPolicy":"round_robin","methodConfig":[{"name":[{"method":"Foo_0","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_1","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_2","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_3","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_4","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_5","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_6","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_7","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_8","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_9","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_10","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_11","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_12","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_13","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_14","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_15","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_16","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_17","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_18","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_19","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_20","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_21","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_22","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_23","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_24","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_25","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_26","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_27","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_28","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_29","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_30","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_31","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_32","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_33","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_34","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_35","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_36","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_37","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_38","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_39","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_40","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_41","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_42","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_43","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_44","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_45","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_46","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_47","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_48","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_49","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_50","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_51","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_52","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_53","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_54","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_55","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_56","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_57","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_58","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_59","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_60","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_61","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_62","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_63","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_64","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_65","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_66","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_67","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_68","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_69","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_70","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_71","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_72","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_73","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_74","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_75","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_76","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_77","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_78","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_79","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_80","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_81","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_82","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_83","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_84","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_85","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_86","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_87","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_88","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_89","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_90","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_91","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_92","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_93","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_94","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_95","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_96","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_97","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_98","service":"SimpleService"}],"waitForReady":true},{"name":[{"method":"Foo_99","service":"SimpleService"}],"waitForReady":true}]}',
  '--expected_service_config_error', '',
  '--expected_lb_policy', '',
  '--enable_srv_queries', 'True',
  '--enable_txt_queries', 'True',
  '--inject_broken_nameserver_list', 'False',
  '--local_dns_server_address', '127.0.0.1:%d' % args.dns_server_port])
current_test_subprocess.communicate()
if current_test_subprocess.returncode != 0:
  num_test_failures += 1

test_runner_log('Run test with target: %s' % 'srv-ipv4-single-target-srv-disabled.resolver-tests-version-4.grpctestingexp.')
current_test_subprocess = subprocess.Popen([
  args.test_bin_path,
  '--target_name', 'srv-ipv4-single-target-srv-disabled.resolver-tests-version-4.grpctestingexp.',
  '--expected_addrs', '2.3.4.5:443,False',
  '--expected_chosen_service_config', '',
  '--expected_service_config_error', '',
  '--expected_lb_policy', '',
  '--enable_srv_queries', 'False',
  '--enable_txt_queries', 'True',
  '--inject_broken_nameserver_list', 'False',
  '--local_dns_server_address', '127.0.0.1:%d' % args.dns_server_port])
current_test_subprocess.communicate()
if current_test_subprocess.returncode != 0:
  num_test_failures += 1

test_runner_log('Run test with target: %s' % 'srv-ipv4-multi-target-srv-disabled.resolver-tests-version-4.grpctestingexp.')
current_test_subprocess = subprocess.Popen([
  args.test_bin_path,
  '--target_name', 'srv-ipv4-multi-target-srv-disabled.resolver-tests-version-4.grpctestingexp.',
  '--expected_addrs', '9.2.3.5:443,False;9.2.3.6:443,False;9.2.3.7:443,False',
  '--expected_chosen_service_config', '',
  '--expected_service_config_error', '',
  '--expected_lb_policy', '',
  '--enable_srv_queries', 'False',
  '--enable_txt_queries', 'True',
  '--inject_broken_nameserver_list', 'False',
  '--local_dns_server_address', '127.0.0.1:%d' % args.dns_server_port])
current_test_subprocess.communicate()
if current_test_subprocess.returncode != 0:
  num_test_failures += 1

test_runner_log('Run test with target: %s' % 'srv-ipv6-single-target-srv-disabled.resolver-tests-version-4.grpctestingexp.')
current_test_subprocess = subprocess.Popen([
  args.test_bin_path,
  '--target_name', 'srv-ipv6-single-target-srv-disabled.resolver-tests-version-4.grpctestingexp.',
  '--expected_addrs', '[2600::1001]:443,False',
  '--expected_chosen_service_config', '',
  '--expected_service_config_error', '',
  '--expected_lb_policy', '',
  '--enable_srv_queries', 'False',
  '--enable_txt_queries', 'True',
  '--inject_broken_nameserver_list', 'False',
  '--local_dns_server_address', '127.0.0.1:%d' % args.dns_server_port])
current_test_subprocess.communicate()
if current_test_subprocess.returncode != 0:
  num_test_failures += 1

test_runner_log('Run test with target: %s' % 'srv-ipv6-multi-target-srv-disabled.resolver-tests-version-4.grpctestingexp.')
current_test_subprocess = subprocess.Popen([
  args.test_bin_path,
  '--target_name', 'srv-ipv6-multi-target-srv-disabled.resolver-tests-version-4.grpctestingexp.',
  '--expected_addrs', '[2600::1002]:443,False;[2600::1003]:443,False;[2600::1004]:443,False',
  '--expected_chosen_service_config', '',
  '--expected_service_config_error', '',
  '--expected_lb_policy', '',
  '--enable_srv_queries', 'False',
  '--enable_txt_queries', 'True',
  '--inject_broken_nameserver_list', 'False',
  '--local_dns_server_address', '127.0.0.1:%d' % args.dns_server_port])
current_test_subprocess.communicate()
if current_test_subprocess.returncode != 0:
  num_test_failures += 1

test_runner_log('Run test with target: %s' % 'srv-ipv4-simple-service-config-srv-disabled.resolver-tests-version-4.grpctestingexp.')
current_test_subprocess = subprocess.Popen([
  args.test_bin_path,
  '--target_name', 'srv-ipv4-simple-service-config-srv-disabled.resolver-tests-version-4.grpctestingexp.',
  '--expected_addrs', '5.5.3.4:443,False',
  '--expected_chosen_service_config', '{"loadBalancingPolicy":"round_robin","methodConfig":[{"name":[{"method":"Foo","service":"SimpleService"}],"waitForReady":true}]}',
  '--expected_service_config_error', '',
  '--expected_lb_policy', 'round_robin',
  '--enable_srv_queries', 'False',
  '--enable_txt_queries', 'True',
  '--inject_broken_nameserver_list', 'False',
  '--local_dns_server_address', '127.0.0.1:%d' % args.dns_server_port])
current_test_subprocess.communicate()
if current_test_subprocess.returncode != 0:
  num_test_failures += 1

test_runner_log('Run test with target: %s' % 'srv-ipv4-simple-service-config-txt-disabled.resolver-tests-version-4.grpctestingexp.')
current_test_subprocess = subprocess.Popen([
  args.test_bin_path,
  '--target_name', 'srv-ipv4-simple-service-config-txt-disabled.resolver-tests-version-4.grpctestingexp.',
  '--expected_addrs', '1.2.3.4:1234,True',
  '--expected_chosen_service_config', '',
  '--expected_service_config_error', '',
  '--expected_lb_policy', '',
  '--enable_srv_queries', 'True',
  '--enable_txt_queries', 'False',
  '--inject_broken_nameserver_list', 'False',
  '--local_dns_server_address', '127.0.0.1:%d' % args.dns_server_port])
current_test_subprocess.communicate()
if current_test_subprocess.returncode != 0:
  num_test_failures += 1

test_runner_log('Run test with target: %s' % 'ipv4-cpp-config-has-zero-percentage-txt-disabled.resolver-tests-version-4.grpctestingexp.')
current_test_subprocess = subprocess.Popen([
  args.test_bin_path,
  '--target_name', 'ipv4-cpp-config-has-zero-percentage-txt-disabled.resolver-tests-version-4.grpctestingexp.',
  '--expected_addrs', '1.2.3.4:443,False',
  '--expected_chosen_service_config', '',
  '--expected_service_config_error', '',
  '--expected_lb_policy', '',
  '--enable_srv_queries', 'True',
  '--enable_txt_queries', 'False',
  '--inject_broken_nameserver_list', 'False',
  '--local_dns_server_address', '127.0.0.1:%d' % args.dns_server_port])
current_test_subprocess.communicate()
if current_test_subprocess.returncode != 0:
  num_test_failures += 1

test_runner_log('Run test with target: %s' % 'ipv4-second-language-is-cpp-txt-disabled.resolver-tests-version-4.grpctestingexp.')
current_test_subprocess = subprocess.Popen([
  args.test_bin_path,
  '--target_name', 'ipv4-second-language-is-cpp-txt-disabled.resolver-tests-version-4.grpctestingexp.',
  '--expected_addrs', '1.2.3.4:443,False',
  '--expected_chosen_service_config', '',
  '--expected_service_config_error', '',
  '--expected_lb_policy', '',
  '--enable_srv_queries', 'True',
  '--enable_txt_queries', 'False',
  '--inject_broken_nameserver_list', 'False',
  '--local_dns_server_address', '127.0.0.1:%d' % args.dns_server_port])
current_test_subprocess.communicate()
if current_test_subprocess.returncode != 0:
  num_test_failures += 1

test_runner_log('Run test with target: %s' % 'ipv4-svc_cfg_bad_json.resolver-tests-version-4.grpctestingexp.')
current_test_subprocess = subprocess.Popen([
  args.test_bin_path,
  '--target_name', 'ipv4-svc_cfg_bad_json.resolver-tests-version-4.grpctestingexp.',
  '--expected_addrs', '1.2.3.4:443,False',
  '--expected_chosen_service_config', '',
  '--expected_service_config_error', 'could not parse',
  '--expected_lb_policy', '',
  '--enable_srv_queries', 'True',
  '--enable_txt_queries', 'True',
  '--inject_broken_nameserver_list', 'False',
  '--local_dns_server_address', '127.0.0.1:%d' % args.dns_server_port])
current_test_subprocess.communicate()
if current_test_subprocess.returncode != 0:
  num_test_failures += 1

test_runner_log('Run test with target: %s' % 'ipv4-svc_cfg_bad_client_language.resolver-tests-version-4.grpctestingexp.')
current_test_subprocess = subprocess.Popen([
  args.test_bin_path,
  '--target_name', 'ipv4-svc_cfg_bad_client_language.resolver-tests-version-4.grpctestingexp.',
  '--expected_addrs', '1.2.3.4:443,False',
  '--expected_chosen_service_config', '',
  '--expected_service_config_error', 'field:clientLanguage error:should be of type array',
  '--expected_lb_policy', '',
  '--enable_srv_queries', 'True',
  '--enable_txt_queries', 'True',
  '--inject_broken_nameserver_list', 'False',
  '--local_dns_server_address', '127.0.0.1:%d' % args.dns_server_port])
current_test_subprocess.communicate()
if current_test_subprocess.returncode != 0:
  num_test_failures += 1

test_runner_log('Run test with target: %s' % 'ipv4-svc_cfg_bad_percentage.resolver-tests-version-4.grpctestingexp.')
current_test_subprocess = subprocess.Popen([
  args.test_bin_path,
  '--target_name', 'ipv4-svc_cfg_bad_percentage.resolver-tests-version-4.grpctestingexp.',
  '--expected_addrs', '1.2.3.4:443,False',
  '--expected_chosen_service_config', '',
  '--expected_service_config_error', 'field:percentage error:should be of type number',
  '--expected_lb_policy', '',
  '--enable_srv_queries', 'True',
  '--enable_txt_queries', 'True',
  '--inject_broken_nameserver_list', 'False',
  '--local_dns_server_address', '127.0.0.1:%d' % args.dns_server_port])
current_test_subprocess.communicate()
if current_test_subprocess.returncode != 0:
  num_test_failures += 1

test_runner_log('Run test with target: %s' % 'ipv4-svc_cfg_bad_wait_for_ready.resolver-tests-version-4.grpctestingexp.')
current_test_subprocess = subprocess.Popen([
  args.test_bin_path,
  '--target_name', 'ipv4-svc_cfg_bad_wait_for_ready.resolver-tests-version-4.grpctestingexp.',
  '--expected_addrs', '1.2.3.4:443,False',
  '--expected_chosen_service_config', '',
  '--expected_service_config_error', 'field:waitForReady error:Type should be true/false',
  '--expected_lb_policy', '',
  '--enable_srv_queries', 'True',
  '--enable_txt_queries', 'True',
  '--inject_broken_nameserver_list', 'False',
  '--local_dns_server_address', '127.0.0.1:%d' % args.dns_server_port])
current_test_subprocess.communicate()
if current_test_subprocess.returncode != 0:
  num_test_failures += 1

test_runner_log('Run test with target: %s' % 'no-srv-ipv4-single-target-inject-broken-nameservers.resolver-tests-version-4.grpctestingexp.')
current_test_subprocess = subprocess.Popen([
  args.test_bin_path,
  '--target_name', 'no-srv-ipv4-single-target-inject-broken-nameservers.resolver-tests-version-4.grpctestingexp.',
  '--expected_addrs', '5.5.5.5:443,False',
  '--expected_chosen_service_config', '',
  '--expected_service_config_error', '',
  '--expected_lb_policy', '',
  '--enable_srv_queries', 'True',
  '--enable_txt_queries', 'True',
  '--inject_broken_nameserver_list', 'True',
  '--local_dns_server_address', '127.0.0.1:%d' % args.dns_server_port])
current_test_subprocess.communicate()
if current_test_subprocess.returncode != 0:
  num_test_failures += 1

test_runner_log('Run test with target: %s' % 'ipv4-config-causing-fallback-to-tcp-inject-broken-nameservers.resolver-tests-version-4.grpctestingexp.')
current_test_subprocess = subprocess.Popen([
  args.test_bin_path,
  '--target_name', 'ipv4-config-causing-fallback-to-tcp-inject-broken-nameservers.resolver-tests-version-4.grpctestingexp.',
  '--expected_addrs', '1.2.3.4:443,False',
  '--expected_chosen_service_config', '{"loadBalancingPolicy":"round_robin","methodConfig":[{"name":[{"method":"Foo","service":"SimpleService","waitForReady":true}]},{"name":[{"method":"FooTwo","service":"SimpleService","waitForReady":true}]},{"name":[{"method":"FooThree","service":"SimpleService","waitForReady":true}]},{"name":[{"method":"FooFour","service":"SimpleService","waitForReady":true}]},{"name":[{"method":"FooFive","service":"SimpleService","waitForReady":true}]},{"name":[{"method":"FooSix","service":"SimpleService","waitForReady":true}]},{"name":[{"method":"FooSeven","service":"SimpleService","waitForReady":true}]},{"name":[{"method":"FooEight","service":"SimpleService","waitForReady":true}]},{"name":[{"method":"FooNine","service":"SimpleService","waitForReady":true}]},{"name":[{"method":"FooTen","service":"SimpleService","waitForReady":true}]},{"name":[{"method":"FooEleven","service":"SimpleService","waitForReady":true}]},{"name":[{"method":"FooTwelve","service":"SimpleService","waitForReady":true}]},{"name":[{"method":"FooTwelve","service":"SimpleService","waitForReady":true}]},{"name":[{"method":"FooTwelve","service":"SimpleService","waitForReady":true}]},{"name":[{"method":"FooTwelve","service":"SimpleService","waitForReady":true}]}]}',
  '--expected_service_config_error', 'Service config parsing error',
  '--expected_lb_policy', '',
  '--enable_srv_queries', 'True',
  '--enable_txt_queries', 'True',
  '--inject_broken_nameserver_list', 'True',
  '--local_dns_server_address', '127.0.0.1:%d' % args.dns_server_port])
current_test_subprocess.communicate()
if current_test_subprocess.returncode != 0:
  num_test_failures += 1

test_runner_log('now kill DNS server')
dns_server_subprocess.kill()
dns_server_subprocess.wait()
test_runner_log('%d tests failed.' % num_test_failures)
sys.exit(num_test_failures)
