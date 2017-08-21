#!/usr/bin/env python2.7
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


"""Generates the appropriate build.json data for all the end2end tests."""


import yaml
import collections
import hashlib
import json

_LOCAL_DNS_SERVER_ADDRESS = '127.0.0.1:15353'

def _expected_chosen_service_config(test_case):
  txt_count = 0
  for name in test_case['records'].keys():
    for record in test_case['records'].get(name):
      if record['type'] == 'TXT':
        txt_count += 1
        txt_data = record['data']
  if txt_count == 0:
    return ''
  assert(txt_count == 1)
  split_index = txt_data.find('=')
  assert(split_index)
  json_data = json.loads(txt_data[split_index+1:])
  if test_case['expected_config_index'] != None:
    expected_json_data = json_data[test_case['expected_config_index']]
    return json.dumps(expected_json_data['serviceConfig'], separators=(',', ':'))
  return ''

def _append_zone_name(name, zone_name):
  return '%s.%s' % (name, zone_name)

def _use_underscores(record_name):
  return record_name.replace('.', '_').replace('-', '_'),

def _build_expected_addrs_cmd_arg(expected_addrs):
  out = []
  for addr in expected_addrs:
    out.append(addr['address'])
    out.append(str(addr['is_balancer']))
  return ','.join(out)

def _expected_lb_policy(test_case):
  out = test_case['expected_lb_policy']
  if not out:
    return ''
  return out

def main():
  resolver_component_data = ''
  with open('tools/run_tests/name_resolution/resolver_test_record_groups.yaml') as f:
    resolver_component_data = yaml.load(f)

  json = {
      'targets': [
          {
              'name': 'resolver_component_test_%s' % _use_underscores(test_case['record_to_resolve']),
              'build': 'test',
              'language': 'c++',
              'gtest': False,
              'run': True,
              'src': ['test/cpp/naming/resolver_component_test.cc'],
              'platforms': ['linux', 'posix', 'mac'],
              'deps': [
                  'grpc++_test_util',
                  'grpc_test_util',
                  'gpr_test_util',
                  'grpc++',
                  'grpc',
                  'gpr',
                  'grpc++_test_config',
              ],
              'args': [
                  '--target_name=%s' % _append_zone_name(test_case['record_to_resolve'],
                                                         resolver_component_data['resolver_component_tests_common_zone_name']),
                  '--expected_addrs=%s' % _build_expected_addrs_cmd_arg(test_case['expected_addrs']),
                  '--expected_chosen_service_config=%s' % _expected_chosen_service_config(test_case),
                  '--local_dns_server_address=%s' % _LOCAL_DNS_SERVER_ADDRESS,
                  '--expected_lb_policy=%s' % _expected_lb_policy(test_case),
              ]
          } for test_case in resolver_component_data['resolver_component_tests']
      ],
  }

  print(yaml.dump(json))

if __name__ == '__main__':
  main()
