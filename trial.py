class DnsRecord(object):
  def __init__(self, record_type, record_name, record_data):
    self.record_type = record_type
    self.record_name = record_name
    self.record_data = record_data
    self.record_class = 'IN'
    self.ttl = TTL

  def uploadable_data(self):
    return self.record_data.split(',')

class A(object):
  def __init__(self, name, ip):
    self.name = '%s.%s' % (name, ZONE_DNS)
    self.ip = ip

class AAAA(object):
  def __init__(self, name, ip):
    self.name = name
    self.ip = ip

class SRV(object):
  def __init__(self, name, target):
    self.name = '%s.%s.%s' % (SRV_PREFIX, name, ZONE_DNS)
    self.target = target

class TXT(object):
  def __init__(self, name, grpc_config):
    self.name = name
    self.grpc_config = grpc_config


ZONE_DNS = 'grpc.com.'
SRV_PORT = '1234'
SRV_PREFIX = '_grpclb._tcp'

def _test_group(records, expected_addrs, expected_config):
  return {
      'records': records,
      'expected_addrs': expected_addrs,
      'expected_config': expected_config,
  }

def _create_ipv4_and_srv_record_group(ip_name, ip_addrs):
  records = [
      A(ip_name, ip_addrs),
      SRV('srv-%s' % ip_name, ip_name),
  ]
  return test_group(records, ip_addrs, None)

def _create_ipv6_and_srv_record_group(ip_name, ip_addrs):
  records = [
      AAAA(ip_name, ip_addrs),
      SRV('srv-%s' % ip_name, ip_name),
  ]
  return test_group(records, ip_addrs, None)

def _create_ipv4_and_srv_and_txt_record_group(ip_name, ip_addrs, grpc_config, expected_config):
  records = [
      A(ip_name, ip_addrs),
      SRV('srv-%s' % ip_name, ip_name),
      TXT('srv-%s' % ip_name, grpc_config),
  ]
  return test_group(records, ip_addrs, expected_config)

def _create_ipv4_and_txt_record_group(ip_name, ip_addrs, grpc_config, expected_config):
  records = [
      AAAA(ip_name, ip_addr),
      TXT(ip_name, grpc_config),
  ]
  return test_group(records, ip_addrs, expected_config)

def _create_method_config(service_name):
  return [{
    'name': [{
      'service': service_name,
      'method': 'Foo',
      'waitForReady': True,
    }],
  }]

def _create_service_config(method_config=[], percentage=None, client_language=None)
  config = {
    'loadBalancingPolicy': 'round_robin',
    'methodConfig': method_config,
  }
  if percentage is not None:
    config['percentage'] = percentage
  if client_language is not None:
    config['clientLanguage'] = client_language

def _create_grpc_config(

def _create_records_for_testing():
  records = []
  records.extend(_create_ipv4_and_srv_record_group('ipv4-single-target', ['1.2.3.4']))
  records.extend(_create_ipv4_and_srv_record_group('ipv4-multi-target', ['1.2.3.5', '1.2.3.6', '1.2.3.7']))
  records.extend(_create_ipv6_and_srv_record_group('ipv6-single-target', ['2607:f8b0:400a:801::1001']))
  records.extend(_create_ipv6_and_srv_record_group('ipv6-multi-target', ['2607:f8b0:400a:801::1002', '2607:f8b0:400a:801::1003', '2607:f8b0:400a:801::1004']))

  configs = [_create_service_config(_create_method_config('SimpleService'))]
  records.extend(_create_ipv4_and_srv_and_txt_record_group('ipv4-simple-service-config', ['1.2.3.4'], configs, configs[0])

  configs = [_create_service_config(_create_method_config('NoSrvSimpleService'))]
  records.extend(_create_ipv4_and_txt_record_group('ipv4-no-srv-simple-service-config', ['1.2.3.4'], configs, configs[0])

  configs = [_create_service_config(
              _create_method_config('GoService'),
              client_language=['go']),
             _create_service_config(
               _create_method_config('CppService'),
               client_language=['c++'])])]
  records.extend(_create_ipv4_and_txt_record_group('ipv4-second-language-is-cpp', ['1.2.3.4'], configs, configs[1])


  configs = [_create_service_config(
              _create_method_config('PythonService'),
              client_language=['python']))]
  records.extend(_create_ipv4_and_txt_record_group('ipv4-no-config-for-cpp', ['1.2.3.4'], configs, None)


  configs = [_create_service_config(
               _create_method_config('NeverPickedService'),
               percentage=0),
             _create_service_config(
               _create_method_config('AlwaysPickedService'),
               percentage=100)]
  records.extend(_create_ipv4_and_txt_record_group('ipv4-config-with-percentages', ['1.2.3.4'], configs, configs[1])

  configs = [_create_service_config(
              _create_method_config('CppService'),
              percentage=0)]
  records.extend(_create_ipv4_and_txt_record_group('ipv4-cpp-config-has-zero-percentage', ['1.2.3.4'], configs, None)
  return records
