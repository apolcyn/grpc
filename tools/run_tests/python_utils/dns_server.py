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
import yaml


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

a_records = [dnslib.A('1.2.3.4')]
aaaa_records = [dnslib.A('1.2.3.4')]
srv_records = []

with open('tools/run_tests/name_resolution/resolver_test_record_groups.yaml', 'r') as config:
  test_groups = yaml.load(config)

all_records = {}

def _push_record(records, name, r):
  if records.get(name) is not None:
    records[name].append(r)
    return
  records[name] = [r]

for group in test_groups:
  for name in group['records'].keys():
    for record in group['records'][name]:
      assert(len(record.keys()) == 1)
      r_type = record.keys()[0]
      r_data = record[r_type]
      print('record Name is |%s|' % name)
      if r_type == 'A':
        _push_record(all_records, name, A(r_data))
      if r_type == 'AAAA':
        print('AAAA data is |%s|' % r_data)
        _push_record(all_records, name, AAAA(r_data))
      if r_type == 'SRV':
        print('R_data is |%s|' % r_data)
        p, w, port, target = r_data.split(' ')
        p = int(p)
        w = int(w)
        port = int(port)
        _push_record(all_records, name, SRV(target=target, priority=p, weight=w, port=port))
      if r_type == 'TXT':
        print('TXT Record length is %s' % len(r_data))
        if len(r_data) > 255:
          print('skipping TXT record %s' % name)
          continue
        _push_record(all_records, name, TXT(r_data))

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
