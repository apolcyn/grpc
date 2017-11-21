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


"""Generates the appropriate build.json data for all the naming tests."""


import yaml
import collections
import hashlib
import json
import service_config_utils

_SERVER_HEALTH_CHECK_RECORD_NAME = 'dummy-health-check'
_SERVER_HEALTH_CHECK_RECORD_DATA = '123.123.123.123'

_GCLOUD = 'gcloud'
_TWISTED = 'twisted'
_BIND9 = 'bind9'

_TARGET_RECORDS_TO_SKIP = {
  _GCLOUD: [
      # TODO: enable this once able to upload the very large TXT record
      # in this group to GCE DNS.
      'ipv4-config-causing-fallback-to-tcp',
  ],
  _TWISTED: [
      # It appears that twisted DNS server can't follow CNAME chains.
      'chained-cname-to-ipv4',
      'chained-cname-to-ipv6',
  ],
  _BIND9: [],
}

def _append_zone_name(name, zone_name):
  return '%s.%s' % (name, zone_name)

def _build_expected_addrs_cmd_arg(expected_addrs):
  out = []
  for addr in expected_addrs:
    out.append('%s,%s' % (addr['address'], str(addr['is_balancer'])))
  return ';'.join(out)

def _data_for_type(r_type,
                   r_data,
                   common_zone_name,
                   record_name,
                   zone_file_type):
  if r_type in ['A', 'AAAA']:
    return r_data
  if r_type == 'SRV':
    target = r_data.split()[3]
    uploadable_target = '%s.%s' % (target, common_zone_name)
    uploadable = r_data.split()
    uploadable[3] = uploadable_target
    return '%s' % ' '.join(uploadable)
  if r_type == 'TXT':
    assert r_data is None, ('TXT data is present in the yaml file. '
                            'It is meant to be read from json file')
    path = 'test/cpp/naming/service_configs/%s.json' % record_name
    return service_config_utils.convert_service_config_to_txt_data(
        path, zone_file_type)
  if r_type == 'CNAME':
    out = '%s.%s' % (r_data, common_zone_name)
    assert out[-1] == '.'
    if zone_file_type == _TWISTED:
      return out[:-1]
    else:
      return out

def _fill_column(data, col_width):
  return data + ' ' * (col_width - len(data))

def _create_bind_format_line(record_name, r_ttl, r_type, r_data):
  _NAME_COL_WIDTH = 128
  _TTL_COL_WIDTH = 8
  _IN_COL_WIDTH = 4
  _TYPE_COL_WIDTH = 8
  line = ''
  line += _fill_column(record_name, _NAME_COL_WIDTH)
  line += _fill_column(r_ttl, _TTL_COL_WIDTH)
  line += _fill_column('IN', _IN_COL_WIDTH)
  line += _fill_column(r_type, _TYPE_COL_WIDTH)
  line += r_data
  return line

def _bind_zone_file_lines(test_cases,
                          common_zone_name,
                          zone_file_type):
  out = []
  if zone_file_type != _TWISTED:
    # For the python DNS server's zone file, we need to avoid use of
    # the $ORIGIN keyword and instead use FQDN in all names in the
    # zone file, because older versions of twisted have a bug in which
    # use of $ORIGIN adds an an extra period at the end of names in it's
    # domain name lookup table. (see
    # https://github.com/twisted/twisted/pull/579 which fixes the issue).
    # TODO: use the $ORIGIN keyword once this doesn't have to be
    # compatible with older versions of twisted.
    out.append('$ORIGIN %s' % common_zone_name)
  for group in test_cases:
    if group['record_to_resolve'] in _TARGET_RECORDS_TO_SKIP[zone_file_type]:
      continue
    for record_name in group['records'].keys():
      r_ttl = None
      all_r_data = {}
      for r_data in group['records'][record_name]:
        # enforce records have the same TTL only for simplicity
        if r_ttl is None:
          r_ttl = r_data['TTL']
        assert r_ttl == r_data['TTL'], '%s and %s differ' % \
            (r_ttl, r_data['TTL'])
        r_type = r_data['type']
        if all_r_data.get(r_type) is None:
          all_r_data[r_type] = []
        all_r_data[r_type].append(_data_for_type(r_type,
                                                 r_data.get('data'),
                                                 common_zone_name,
                                                 record_name,
                                                 zone_file_type))
      for r_type in all_r_data.keys():
        for r_data in all_r_data[r_type]:
          name_for_line = record_name
          if zone_file_type == _TWISTED:
            name_for_line += '.%s' % common_zone_name
          out.append(_create_bind_format_line(name_for_line,
                                              r_ttl,
                                              r_type,
                                              r_data))
  return out

def _get_record_names_and_types_from_zone_file_entries(zone_file_entries):
  names = []
  for entry in zone_file_entries:
    if entry.split()[0] == '$ORIGIN':
      continue
    names.append({
        'name': entry.split()[0],
        'type': entry.split()[3],
    })
  return names

def _auxiliary_records_for_local_dns_server(common_zone_name, zone_file_type):
  if zone_file_type == _BIND9:
    soa_name = '@'
    zone_ns_name = '@'
    ns1_name = 'ns1'
    health_check_record_name = _SERVER_HEALTH_CHECK_RECORD_NAME
  else:
    assert zone_file_type == _TWISTED
    soa_name = common_zone_name
    zone_ns_name = common_zone_name
    ns1_name = 'ns1.%s' % common_zone_name
    health_check_record_name = '%s.%s' % (_SERVER_HEALTH_CHECK_RECORD_NAME, common_zone_name)
  return [
      _create_bind_format_line(
          soa_name,
          '0',
          'SOA',
          'ns1.%s dummy-admin-address.com 0 0 0 0 0' % common_zone_name),
      _create_bind_format_line(
          zone_ns_name,
          '0',
          'NS',
          'ns1.%s' % common_zone_name),
      _create_bind_format_line(
          ns1_name,
          '0',
          'A',
          '127.0.0.1'),
      _create_bind_format_line(
          ns1_name,
          '0',
          'AAAA',
          '::1'),
      _create_bind_format_line(
          health_check_record_name,
          '0',
          'A',
          _SERVER_HEALTH_CHECK_RECORD_DATA),
  ]

def _gce_dns_zone_id(resolver_component_data):
  dns_name = resolver_component_data['resolver_tests_common_zone_name']
  return dns_name.replace('.', '-') + 'zone-id'

def _resolver_test_cases(resolver_component_data, records_to_skip):
  out = []
  for test_case in resolver_component_data['resolver_component_tests']:
    if test_case['record_to_resolve'] in records_to_skip:
      continue
    out.append({
      'target_name': _append_zone_name(
          test_case['record_to_resolve'],
          resolver_component_data['resolver_tests_common_zone_name']),
      'expected_addrs': _build_expected_addrs_cmd_arg(
          test_case['expected_addrs']),
      'expected_chosen_service_config': (
          test_case['expected_chosen_service_config'] or ''),
      'expected_lb_policy': (test_case['expected_lb_policy'] or ''),
    })
  return out

def _create_zone_file_entries(zone_file_type,
                              resolver_component_data,
                              add_auxiliary_records):
  zone_file_entries = _bind_zone_file_lines(
      resolver_component_data['resolver_component_tests'],
      resolver_component_data['resolver_tests_common_zone_name'],
      zone_file_type)
  if add_auxiliary_records:
    zone_file_entries += _auxiliary_records_for_local_dns_server(
        resolver_component_data['resolver_tests_common_zone_name'],
        zone_file_type)
  return zone_file_entries

def main():
  resolver_component_data = ''
  with open('test/cpp/naming/resolver_test_record_groups.yaml') as f:
    resolver_component_data = yaml.load(f)
  integration_test_zone_file_entries = _create_zone_file_entries(
      _GCLOUD, resolver_component_data, False)
  json_out = {
      'resolver_tests_common_zone_name': \
          resolver_component_data['resolver_tests_common_zone_name'],
      'resolver_gce_integration_tests_zone_id': \
          _gce_dns_zone_id(resolver_component_data),
      'integration_test_zone_file_entries': integration_test_zone_file_entries,
      'local_dns_server_test_zone_file_entries': \
          _create_zone_file_entries(_TWISTED, resolver_component_data, True),
      'bind9_dns_server_test_zone_file_entries': \
          _create_zone_file_entries(_BIND9, resolver_component_data, True),
      'integration_test_record_names_and_types': \
           _get_record_names_and_types_from_zone_file_entries(
               integration_test_zone_file_entries),
      'resolver_gce_integration_test_cases': _resolver_test_cases(
          resolver_component_data, _TARGET_RECORDS_TO_SKIP[_GCLOUD]),
      'resolver_component_test_cases': _resolver_test_cases(
          resolver_component_data, _TARGET_RECORDS_TO_SKIP[_TWISTED]),
      'resolver_component_tests_health_check_record_name': '%s.%s' % (
          _SERVER_HEALTH_CHECK_RECORD_NAME,
          resolver_component_data['resolver_tests_common_zone_name']),
      'resolver_component_tests_health_check_record_data': \
          _SERVER_HEALTH_CHECK_RECORD_DATA,
      'targets': [
          {
              'name': 'resolver_component_test' + unsecure_build_config_suffix,
              'build': 'test',
              'language': 'c++',
              'gtest': False,
              'run': False,
              'src': ['test/cpp/naming/resolver_component_test.cc'],
              'platforms': ['linux', 'posix', 'mac'],
              'deps': [
                  'grpc++_test_util' + unsecure_build_config_suffix,
                  'grpc_test_util' + unsecure_build_config_suffix,
                  'gpr_test_util',
                  'grpc++' + unsecure_build_config_suffix,
                  'grpc' + unsecure_build_config_suffix,
                  'gpr',
                  'grpc++_test_config',
              ],
          } for unsecure_build_config_suffix in ['_unsecure', '']
      ] + [
          {
              'name': 'resolver_component_tests_runner_invoker%s' % \
                  unsecure_build_config_suffix,
              'build': 'test',
              'language': 'c++',
              'gtest': False,
              'run': True,
              'src': [
                  'test/cpp/naming/resolver_component_tests_runner_invoker.cc'
              ],
              'platforms': (use_named_server and ['linux', 'posix']) or \
                  ['linux', 'posix', 'mac'],
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
                  '--test_bin_name=resolver_component_test%s' % \
                      unsecure_build_config_suffix,
                  '--running_under_bazel=false',
                  '--use_named_server=%s' % use_named_server,
              ],
          } for unsecure_build_config_suffix in ['_unsecure', ''] \
              for use_named_server in [True, False]
      ]
  }

  print(yaml.dump(json_out))

if __name__ == '__main__':
  main()
