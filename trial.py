import json
import yaml

class DnsRecord(object):
  def __init__(self, record_type, record_name, record_data):
    self.record_type = record_type
    self.record_name = record_name
    self.record_data = record_data
    self.record_class = 'IN'
    self.ttl = TTL

  def uploadable_data(self):
    return self.record_data.split(',')

ZONE_DNS = 'test-2.grpctestingexp.'
SRV_PORT = '1234'
TTL = '2100'

def a_record(ip):
  return {
      'type': 'A',
      'data': ip,
      'TTL': TTL,
  }

def aaaa_record(ip):
  return {
      'type': 'AAAA',
      'data': ip,
      'TTL': TTL,
  }

def srv_record(target):
  return {
      'type': 'SRV',
      'data': '0 0 %s %s' % (SRV_PORT, '%s.%s' % (target, ZONE_DNS)),
      'TTL': TTL,
  }

def txt_record(grpc_config):
  return {
      'type': 'TXT',
      'data': 'grpc_config=%s' % json.dumps(grpc_config, separators=(',', ':')),
      'TTL': TTL,
  }

def _test_group(record_to_resolve, records, expected_addrs, expected_config):
  return {
      'record_to_resolve': record_to_resolve,
      'records': records,
      'expected_addrs': expected_addrs,
      'expected_config_index': expected_config,
  }

def _full_srv_record_name(name):
  return '_grpclb._tcp.srv-%s.%s' % (name, ZONE_DNS)

def _full_a_record_name(name):
  return '%s.%s' % (name, ZONE_DNS)

def _full_txt_record_name(name):
  return '%s.%s' % (name, ZONE_DNS)

def push(records, name, val):
  if name in records.keys():
    records[name].append(val)
    return
  records[name] = [val]

def _create_ipv4_and_srv_record_group(ip_name, ip_addrs):
  records = {}
  for ip in ip_addrs:
    push(records, _full_a_record_name(ip_name), a_record(ip))
  push(records, _full_srv_record_name(ip_name), srv_record(ip_name))

  expected_addrs = []
  for ip in ip_addrs:
    expected_addrs.append('%s:%s' % (ip, SRV_PORT))
  return _test_group('srv-%s.%s' % (ip_name, ZONE_DNS), records, expected_addrs, None)

def _create_ipv6_and_srv_record_group(ip_name, ip_addrs):
  records = {}
  for ip in ip_addrs:
    push(records, _full_a_record_name(ip_name), aaaa_record(ip))
  push(records, _full_srv_record_name(ip_name), srv_record(ip_name))

  expected_addrs = []
  for ip in ip_addrs:
    expected_addrs.append('[%s]:%s' % (ip, SRV_PORT))
  return _test_group('srv-%s.%s' % (ip_name, ZONE_DNS), records, expected_addrs, None)

def _create_ipv4_and_srv_and_txt_record_group(ip_name, ip_addrs, grpc_config, expected_config):
  records = {}
  for ip in ip_addrs:
    push(records, _full_a_record_name(ip_name), a_record(ip))
  push(records, _full_srv_record_name(ip_name), srv_record(ip_name))
  push(records, _full_txt_record_name('srv-%s' % ip_name), txt_record(grpc_config))

  expected_addrs = []
  for ip in ip_addrs:
    expected_addrs.append('%s:%s' % (ip, SRV_PORT))
  return _test_group('srv-%s.%s' % (ip_name, ZONE_DNS), records, expected_addrs, expected_config)

def _create_ipv4_and_txt_record_group(ip_name, ip_addrs, grpc_config, expected_config):
  records = {}
  for ip in ip_addrs:
    push(records, _full_a_record_name(ip_name), a_record(ip))
  push(records, _full_txt_record_name(ip_name), txt_record(grpc_config))

  expected_addrs = []
  for ip in ip_addrs:
    expected_addrs.append('%s:443' % ip)
  return _test_group('%s.%s' % (ip_name, ZONE_DNS), records, expected_addrs, expected_config)

def _create_method_config(service_name):
  return [{
    'name': [{
      'service': service_name,
      'method': 'Foo',
      'waitForReady': True,
    }],
  }]

def _create_service_config(method_config=[], percentage=None, client_language=None):
  config = {
    'loadBalancingPolicy': 'round_robin',
    'methodConfig': method_config,
  }
  if percentage is not None:
    config['percentage'] = percentage
  if client_language is not None:
    config['clientLanguage'] = client_language

  return config

def _config_wrapper(langs=None, percent=None, host_name=None, service_config=None):
  config = {}
  if langs is not None:
    config['clientLanguage'] = langs
  if percent is not None:
    config['percentage'] = percent
  if host_name is not None:
    config['clientHostname'] = host_name
  if service_config is None:
    raise Exception
  config['serviceConfig'] = service_config
  return config

def _create_records_for_testing():
  records = []
  records.append(_create_ipv4_and_srv_record_group('ipv4-single-target', ['1.2.3.4']))
  records.append(_create_ipv4_and_srv_record_group('ipv4-multi-target', ['1.2.3.5',
                                                                         '1.2.3.6',
                                                                         '1.2.3.7']))
  records.append(_create_ipv6_and_srv_record_group('ipv6-single-target', ['2607:f8b0:400a:801::1001']))
  records.append(_create_ipv6_and_srv_record_group('ipv6-multi-target', ['2607:f8b0:400a:801::1002',
                                                                         '2607:f8b0:400a:801::1003',
                                                                         '2607:f8b0:400a:801::1004']))

  records.append(_create_ipv4_and_srv_and_txt_record_group('ipv4-simple-service-config', ['1.2.3.4'],
                                                           [_config_wrapper(service_config=_create_service_config(_create_method_config('SimpleService')))],
                                                           0))

  records.append(_create_ipv4_and_txt_record_group('ipv4-no-srv-simple-service-config', ['1.2.3.4'],
                                                   [_config_wrapper(service_config=_create_service_config(_create_method_config('NoSrvSimpleService')))],
                                                   0))

  records.append(_create_ipv4_and_txt_record_group('ipv4-second-language-is-cpp', ['1.2.3.4'],
                                                   [_config_wrapper(langs=['go'],
                                                                    service_config=_create_service_config(
                                                                        _create_method_config('GoService'))),
                                                    _config_wrapper(langs=['c++'],
                                                                    service_config=_create_service_config(
                                                                        _create_method_config('CppService')))],
                                                   None))

  records.append(_create_ipv4_and_txt_record_group('ipv4-no-config-for-cpp', ['1.2.3.4'],
                                                   [_config_wrapper(langs=['python'],
                                                                    service_config=_create_service_config(
                                                                        _create_method_config('PythonService')))],
                                                   None))

  records.append(_create_ipv4_and_txt_record_group('ipv4-config-with-percentages', ['1.2.3.4'],
                                                   [_config_wrapper(percent=0,
                                                                    service_config=_create_service_config(
                                                                        _create_method_config('NeverPickedService'))),
                                                    _config_wrapper(percent=100,
                                                                    service_config=_create_service_config(
                                                                        _create_method_config('AlwaysPickedService')))],
                                                   1))

  records.append(_create_ipv4_and_txt_record_group('ipv4-cpp-config-has-zero-percentage', ['1.2.3.4'],
                                                   [_config_wrapper(percent=0,
                                                                    service_config=_create_service_config(
                                                                        _create_method_config('CppService')))],
                                                   None))

  return records

test_groups = _create_records_for_testing()
#for group in test_groups:
#  print json.dumps(group)
  #if record_to_resolve in ['A', 'AAAA']:
    # make test based on name of record to resolve, expected IP addr(s) with
    # ports, and expected service config, if any
  #if record_to_resolve == 'SRV':
    # make test based on name of record to resolve, expected IP addr(s) with
    # ports, and expected service config, if any

#print ========

print(yaml.dump(test_groups))
