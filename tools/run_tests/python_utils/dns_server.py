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

_all_records = {}
_record_name_type_to_ttl = {}

TYPE_LOOKUP = {
  A: QTYPE.A,
  AAAA: QTYPE.AAAA,
  SRV: QTYPE.SRV,
  TXT: QTYPE.TXT,
}

class Resolver:
  def resolve(self, request_record, _handler):
    global _all_records
    reply = DNSRecord(DNSHeader(id=request_record.header.id, qr=1, aa=1, ra=1), q=request_record.q)
    qt = QTYPE[request_record.q.qtype]
    for name, resource_record_set in _all_records.iteritems():
      if name == str(request_record.q.qname):
        for resource_data in resource_record_set:
          resource_query_type = resource_data.__class__.__name__
          if QTYPE[request_record.q.qtype] == resource_query_type:
            rtype = TYPE_LOOKUP[resource_data.__class__]
            ttl = _record_name_type_to_ttl['%s:%s' % (name, resource_query_type)]
            reply.add_answer(RR(rname=str(request_record.q.qname), rtype=rtype, rclass=1, ttl=ttl, rdata=resource_data))

    print "---- Reply:\n", reply
    return reply

def _push_record(name, r, ttl):
  _record_name_type_to_ttl['%s:%s' % (name, r.__class__.__name__)] = ttl
  if _all_records.get(name) is not None:
    _all_records[name].append(r)
    return
  _all_records[name] = [r]


def _maybe_split_up_txt_data(name, txt_data, ttl):
  start = 0
  while len(txt_data[start:]) > 0:
    next_read = len(txt_data[start:])
    if next_read > 255:
      next_read = 255
    _push_record(name, TXT(txt_data[start:start+next_read]), ttl)
    start += next_read

def start_local_dns_server(dns_server_port):
  with open('tools/run_tests/name_resolution/resolver_test_record_groups.yaml', 'r') as config:
    test_groups = yaml.load(config)

  for group in test_groups:
    for name in group['records'].keys():
      for record in group['records'][name]:
        r_type = record['type']
        r_data = record['data']
        r_ttl = int(record['TTL'])
        if r_type == 'A':
          _push_record(name, A(r_data), r_ttl)
        if r_type == 'AAAA':
          _push_record(name, AAAA(r_data), r_ttl)
        if r_type == 'SRV':
          p, w, port, target = r_data.split(' ')
          p = int(p)
          w = int(w)
          port = int(port)
          _push_record(name, SRV(target=target, priority=p, weight=w, port=port), r_ttl)
        if r_type == 'TXT':
          _maybe_split_up_txt_data(name, r_data, r_ttl)

  resolver = Resolver()
  print('starting local dns server on 127.0.0.1:%s' % dns_server_port)
  DNSServer(resolver, port=dns_server_port, address='127.0.0.1', tcp=False).start_thread()
  DNSServer(resolver, port=dns_server_port, address='127.0.0.1', tcp=True).start_thread()

if __name__ == '__main__':
  start_local_dns_server(15353)
  while True:
    time.sleep(1)
    sys.stderr.flush()
    sys.stdout.flush()
