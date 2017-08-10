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

"""Manage TCP ports for unit tests; started by run_tests.py"""

import argparse
import hashlib
import os
import socket
import sys
import time
import random
from SocketServer import BaseRequestHandler
from SocketServer import ThreadingMixIn
import SocketServer
import threading
import platform
import dnslib
from dnslib import *
from dnslib.server import DNSServer
import datetime
import traceback


# increment this number whenever making a change to ensure that
# the changes are picked up by running CI servers
# note that all changes must be backwards compatible
_MY_VERSION = 20

#ZONE_DNS = 'test.grpctestingexp.'
#ZONE_NAME = 'exp-grpc-testing'
#TTL = '2100'
#
#SRV_PORT='1234'
#
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
#def _create_records_for_testing():
#  ipv4_single_target_dns = 'ipv4-single-target.%s' % ZONE_DNS
#  ipv6_single_target_dns = 'ipv6-single-target.%s' % ZONE_DNS
#  ipv4_multi_target_dns = 'ipv4-multi-target.%s' % ZONE_DNS
#  ipv6_multi_target_dns = 'ipv6-multi-target.%s' % ZONE_DNS
#
#  records = [
#      DnsRecord('A', ipv4_single_target_dns, '1.2.3.4'),
#      DnsRecord('A', ipv4_multi_target_dns, ','.join(['1.2.3.5',
#                                                      '1.2.3.6',
#                                                      '1.2.3.7'])),
#      DnsRecord('AAAA', ipv6_single_target_dns, '2607:f8b0:400a:801::1001'),
#      DnsRecord('AAAA', ipv6_multi_target_dns, ','.join(['2607:f8b0:400a:801::1002',
#                                                         '2607:f8b0:400a:801::1003',
#                                                         '2607:f8b0:400a:801::1004'])),
#      DnsRecord('SRV', '_grpclb._tcp.srv-%s' % ipv4_single_target_dns, '0 0 %s %s' % (SRV_PORT, ipv4_single_target_dns)),
#      DnsRecord('SRV', '_grpclb._tcp.srv-%s' % ipv4_multi_target_dns, '0 0 %s %s' % (SRV_PORT, ipv4_multi_target_dns)),
#      DnsRecord('SRV', '_grpclb._tcp.srv-%s' % ipv6_single_target_dns, '0 0 %s %s' % (SRV_PORT, ipv6_single_target_dns)),
#      DnsRecord('SRV', '_grpclb._tcp.srv-%s' % ipv6_multi_target_dns, '0 0 %s %s' % (SRV_PORT, ipv6_multi_target_dns)),
#  ]
#  return records

ZONE_DNS = 'test.grpctestingexp.'
TTL = 2100
SRV_PORT='1234'

class DnsRecord(object):
  def __init__(self, record_type, record_name, record_data):
    self.record_type = record_type
    self.record_name = record_name
    self.record_data = record_data
    self.record_class = 'IN'
    self.ttl = TTL

  def uploadable_data(self):
    return self.record_data.split(',')

def _create_records_for_testing():
  ipv4_single_target_dns = 'ipv4-single-target.%s' % ZONE_DNS
  ipv6_single_target_dns = 'ipv6-single-target.%s' % ZONE_DNS

a_records = [dnslib.A('1.2.3.4')]
aaaa_records = [dnslib.A('1.2.3.4')]
srv_records = []

all_records = {
    'ipv4-single-target.grpc.com.': [A('1.2.3.4')],
    'ipv6-single-target.grpc.com.': [AAAA('2607:f8b0:400a:801::1001')],
    'ipv4-multi-target.grpc.com.': [A('1.2.3.5'),
                                           A('1.2.3.6'),
                                           A('1.2.3.7')],
    'ipv6-multi-target.grpc.com.': [AAAA('2607:f8b0:400a:801::1001'),
                                    AAAA('2607:f8b0:400a:801::1003'),
                                    AAAA('2607:f8b0:400a:801::1004')],

    # SRV -> A/AAAA, without a service config
    '_grpclb._tcp.srv-ipv4-single-target.grpc.com.': [SRV(target='ipv4-single-target.grpc.com.', priority=0, weight=0, port=1234)],
    '_grpclb._tcp.srv-ipv6-single-target.grpc.com.': [SRV(target='ipv6-single-target.grpc.com.', priority=0, weight=0, port=1234)],
    '_grpclb._tcp.srv-ipv4-multi-target.grpc.com.': [SRV(target='ipv4-multi-target.grpc.com.', priority=0, weight=0, port=1234)],
    '_grpclb._tcp.srv-ipv6-multi-target.grpc.com.': [SRV(target='ipv6-multi-target.grpc.com.', priority=0, weight=0, port=1234)],

    # A record with a service config
    'no-srv-simple-service-config.grpc.com.': [A('1.2.3.4'), TXT("grpc_config=[{\"serviceConfig\":{\"loadBalancingPolicy\":\"round_robin\",\"methodConfig\":[{\"name\":[{\"service\":\"MyService\",\"method\":\"Foo\"}],\"waitForReady\":true}]}}]")],

    # SRV -> A record with a service config
    '_grpclb._tcp.srv-for-simple-service-config.grpc.com.': [SRV(target='simple-service-config.grpc.com.', priority=0, weight=0, port=1234)],
    'srv-for-simple-service-config.grpc.com.': [TXT("grpc_config=[{\"serviceConfig\":{\"loadBalancingPolicy\":\"round_robin\",\"methodConfig\":[{\"name\":[{\"service\":\"MyService\",\"method\":\"Foo\"}],\"waitForReady\":true}]}}]")],
    'simple-service-config.grpc.com.': [A('1.2.3.4')],

    # A record with a service config having c++ as second client language
    'second-language-cpp.grpc.com.': [A('1.2.3.4'), TXT("grpc_config=[{\"clientLanguage\":[\"go\"],\"serviceConfig\":{}},{\"clientLanguage\":[\"c++\"],\"serviceConfig\":{\"methodConfig\":[{\"name\":[{\"service\":\"SecondLanguageCppService\"}],\"waitForReady\":true}]}}]")],

    # A record with a service config having only java and go as client languages
    'no-cpp-config.grpc.com.': [A('1.2.3.4'), TXT("grpc_config=[{\"clientLanguage\":[\"go\"],\"serviceConfig\":{\"loadBalancingPolicy\":\"round_robin\"}},{\"clientLanguage\":[\"java\"],\"serviceConfig\":{\"loadBalancingPolicy\":\"round_robin\"}}]")],

    # A record with a service config having with extreme percentages chosen
    'config-with-percentages.grpc.com.': [A('1.2.3.4'), TXT("grpc_config=[{\"percentage\":0,\"serviceConfig\":{\"methodConfig\":[{\"name\":[{\"service\":\"NeverChosenService\"}],\"waitForReady\":true}]}},{\"percentage\":100,\"serviceConfig\":{\"methodConfig\":[{\"name\":[{\"service\":\"AlwaysChosenService\"}],\"waitForReady\":false}]}}]")],

    # A record with a service config having an empty-list clientHostname
    # TODO: should this test pass? does an empty list count mean no client langs
    # or all client langs pass?
    # 'empty-list-client-hostname.grpc.com.': [A('1.2.3.4'), TXT("grpc_config=[{\"clientHostname\":[],\"serviceConfig\":{\"loadBalancingPolicy\":\"round_robin\"}}]")],

    # service config with matching client language but zero percentage
    'cpp-language-zero-picks.grpc.com.': [A('1.2.3.4'), TXT("grpc_config=[{\"percentage\":\"0\",\"clientLanguage\":\"c++\",\"serviceConfig\":{\"loadBalancingPolicy\":\"round_robin\"}}]")],

    # TODO: test service configs in which clientHostname is specified
}

TYPE_LOOKUP = {
  A: QTYPE.A,
  AAAA: QTYPE.AAAA,
  SRV: QTYPE.SRV,
  TXT: QTYPE.TXT,
}

class Resolver:
  def resolve(self, request_record, _handler):
    global all_records
    reply = DNSRecord(DNSHeader(id=request_record.header.id, qr=1, aa=1, ra=1), q=request_record.q)
    qt = QTYPE[request_record.q.qtype]
    for name, resource_record_set in all_records.iteritems():
      if name == str(request_record.q.qname):
        for resource_data in resource_record_set:
          resource_query_type = resource_data.__class__.__name__
          if QTYPE[request_record.q.qtype] == resource_query_type:
            rtype = TYPE_LOOKUP[resource_data.__class__]
            reply.add_answer(RR(rname=str(request_record.q.qname), rtype=rtype, rclass=1, ttl=TTL, rdata=resource_data))

    print "---- Reply:\n", reply
    return reply

resolver = Resolver()
servers = [
  DNSServer(resolver, port=15353, address='127.0.0.1', tcp=False),
  DNSServer(resolver, port=15353, address='127.0.0.1', tcp=True)
]
for s in servers:
  s.start_thread()

try:
  while True:
    time.sleep(1)
    sys.stderr.flush()
    sys.stdout.flush()
except KeyboardInterrupt:
  pass
finally:
  for s in servers:
    s.stop()
