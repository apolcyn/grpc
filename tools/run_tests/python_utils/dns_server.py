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

def _maybe_split_up_txt_data(all_records, name, txt_data):
  start = 0
  while len(txt_data[start:]) > 0:
    next_read = len(txt_data[start:])
    if next_read > 255:
      print('%s NEEDS CHUNKING' % name)
      next_read = 255
    _push_record(all_records, name, TXT(txt_data[start:start+next_read]))
    start += next_read

for group in test_groups:
  for name in group['records'].keys():
    for record in group['records'][name]:
      r_type = record['type']
      r_data = record['data']
      # ignore TTL
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
        _maybe_split_up_txt_data(all_records, name, r_data.replace('"', '\\"'))

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
