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

def main():
  #json = {
  #    '#': 'generated with test/cpp/naming/gen_build_json.py',
  #    'core_naming_end2end_tests': [
  #        { 'name': 'test1', 'record_type_to_resolve': 'SRV' },
  #        { 'name': 'test2', 'record_type_to_resolve': 'SRV' },
  #    ]
  #}
  #print yaml.dump(json)

  with open('tools/run_tests/name_resolution/resolver_test_record_groups.yaml') as f:
    data = yaml.load(f)
    print(yaml.dump({'naming_end2end_tests': data}))

if __name__ == '__main__':
  main()
#import json
#
#class DnsRecord(object):
#  def __init__(self, record_type, record_name, record_data):
#    self.record_type = record_type
#    self.record_name = record_name
#    self.record_data = record_data
#    self.record_class = 'IN'
#    self.ttl = TTL
#
#  def uploadable_data(self):
#    return self.record_data.split(',')
#
#ZONE_DNS = 'grpc.com.'
#SRV_PORT = '1234'
#
#def a_record(name, ips):
#  return {
#      'record_type': 'A',
#      'name': '%s.%s' % (name, ZONE_DNS),
#      'data': '%s' % ','.join(ips),
#  }
#
#def aaaa_record(name, ips):
#  return {
#      'record_type': 'AAAA',
#      'name': '%s.%s' % (name, ZONE_DNS),
#      'data': '%s' % ','.join(ips),
#  }
#
#def srv_record(name, target):
#  return {
#      'record_type': 'SRV',
#      'name': '_grpclb._tcp.%s.%s' % (name, ZONE_DNS),
#      'data': '0 0 %s %s' % (SRV_PORT, target),
#  }
#
#def txt_record(name, grpc_config):
#  return {
#      'record_type': 'TXT',
#      'name': '%s.%s' % (name, ZONE_DNS),
#      'data': 'grpc_config=%s' % grpc_config,
#  }
#
#def _test_group(record_type_to_resolve, records, expected_addrs, expected_config):
#  if record_type_to_resolve not in ['A', 'AAAA', 'SRV']:
#    raise Exception('bad record type to resolve')
#  return {
#      'record_type_to_resolve': record_type_to_resolve,
#      'records': records,
#      'expected_addrs': expected_addrs,
#      'expected_config': expected_config,
#  }
#
#def _create_ipv4_and_srv_record_group(ip_name, ip_addrs):
#  records = [
#      a_record(ip_name, ip_addrs),
#      srv_record('srv-%s' % ip_name, ip_name),
#  ]
#  return _test_group('A', records, ip_addrs, None)
#
#def _create_ipv6_and_srv_record_group(ip_name, ip_addrs):
#  records = [
#      aaaa_record(ip_name, ip_addrs),
#      srv_record('srv-%s' % ip_name, ip_name),
#  ]
#  return _test_group('AAAA', records, ip_addrs, None)
#
#def _create_ipv4_and_srv_and_txt_record_group(ip_name, ip_addrs, grpc_config, expected_config):
#  records = [
#      a_record(ip_name, ip_addrs),
#      srv_record('srv-%s' % ip_name, ip_name),
#      txt_record('srv-%s' % ip_name, grpc_config),
#  ]
#  return _test_group('A', records, ip_addrs, expected_config)
#
#def _create_ipv4_and_txt_record_group(ip_name, ip_addrs, grpc_config, expected_config):
#  records = [
#      aaaa_record(ip_name, ip_addrs),
#      txt_record(ip_name, grpc_config),
#  ]
#  return _test_group('A', records, ip_addrs, expected_config)
#
#def _create_method_config(service_name):
#  return [{
#    'name': [{
#      'service': service_name,
#      'method': 'Foo',
#      'waitForReady': True,
#    }],
#  }]
#
#def _create_service_config(method_config=[], percentage=None, client_language=None):
#  config = {
#    'loadBalancingPolicy': 'round_robin',
#    'methodConfig': method_config,
#  }
#  if percentage is not None:
#    config['percentage'] = percentage
#  if client_language is not None:
#    config['clientLanguage'] = client_language
#
#  return config
#
#def _create_records_for_testing():
#  records = []
#  records.append(_create_ipv4_and_srv_record_group('ipv4-single-target', ['1.2.3.4']))
#  records.append(_create_ipv4_and_srv_record_group('ipv4-multi-target', ['1.2.3.5',
#                                                                         '1.2.3.6',
#                                                                         '1.2.3.7']))
#  records.append(_create_ipv6_and_srv_record_group('ipv6-single-target', ['2607:f8b0:400a:801::1001']))
#  records.append(_create_ipv6_and_srv_record_group('ipv6-multi-target', ['2607:f8b0:400a:801::1002',
#                                                                         '2607:f8b0:400a:801::1003',
#                                                                         '2607:f8b0:400a:801::1004']))
#
#  records.append(_create_ipv4_and_srv_and_txt_record_group('ipv4-simple-service-config', ['1.2.3.4'],
#                                                           [_create_service_config(_create_method_config('SimpleService'))],
#                                                           0))
#
#  records.append(_create_ipv4_and_txt_record_group('ipv4-no-srv-simple-service-config', ['1.2.3.4'],
#                                                   [_create_service_config(_create_method_config('NoSrvSimpleService'))],
#                                                   0))
#
#  records.append(_create_ipv4_and_txt_record_group('ipv4-second-language-is-cpp', ['1.2.3.4'],
#                                                   [_create_service_config(
#                                                     _create_method_config('GoService'),
#                                                     client_language=['go']),
#                                                    _create_service_config(
#                                                      _create_method_config('CppService'),
#                                                      client_language=['c++'])],
#                                                   None))
#
#  records.append(_create_ipv4_and_txt_record_group('ipv4-no-config-for-cpp', ['1.2.3.4'],
#                                                   [_create_service_config(
#                                                     _create_method_config('PythonService'),
#                                                     client_language=['python'])],
#                                                   None))
#
#  records.append(_create_ipv4_and_txt_record_group('ipv4-config-with-percentages', ['1.2.3.4'],
#                                                   [_create_service_config(
#                                                      _create_method_config('NeverPickedService'),
#                                                      percentage=0),
#                                                    _create_service_config(
#                                                      _create_method_config('AlwaysPickedService'),
#                                                      percentage=100)],
#                                                   1))
#
#  records.append(_create_ipv4_and_txt_record_group('ipv4-cpp-config-has-zero-percentage', ['1.2.3.4'],
#                                                   [_create_service_config(
#                                                      _create_method_config('CppService'),
#                                                      percentage=0)],
#                                                   None))
#
#  return records
#
#test_groups = _create_records_for_testing()
#for group in test_groups:
#  print json.dumps(group)
#  if record_type_to_resolve in ['A', 'AAAA']:
#    # make test based on name of record to resolve, expected IP addr(s) with
#    # ports, and expected service config, if any
#  if record_type_to_resolve == 'SRV':
#    # make test based on name of record to resolve, expected IP addr(s) with
#    # ports, and expected service config, if any
#
#print json.dumps(test_groups)
