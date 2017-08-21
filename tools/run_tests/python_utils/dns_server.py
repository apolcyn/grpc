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
import sys
import time
import threading
import dnslib
from dnslib import *
from dnslib.server import DNSServer
import yaml

_all_records = {}
_record_name_type_to_ttl = {}

class Resolver:
  def resolve(self, request_record, _handler):
    global _all_records
    reply = DNSRecord(DNSHeader(id=request_record.header.id, qr=1, aa=1, ra=1), q=request_record.q)
    requested_name = str(request_record.q.qname)
    qtype = request_record.q.qtype
    rr_list = _all_records.get(requested_name)
    if rr_list is not None:
      for rr_data in rr_list:
        if QTYPE[qtype] == rr_data.__class__.__name__:
          ttl = _record_name_type_to_ttl['%s:%s' % (requested_name, QTYPE[qtype])]
          reply.add_answer(RR(rname=requested_name, rtype=qtype, rclass=1, ttl=ttl, rdata=rr_data))

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
  txt_data_list = []
  while len(txt_data[start:]) > 0:
    next_read = len(txt_data[start:])
    if next_read > 255:
      next_read = 255
    txt_data_list.append(txt_data[start:start+next_read])
    start += next_read

  _push_record(name, TXT(txt_data_list), ttl)

def start_local_dns_server_in_background(dns_server_port):
  with open('tools/run_tests/name_resolution/resolver_test_record_groups.yaml', 'r') as config:
    test_records_config = yaml.load(config)
  common_zone_name = test_records_config['resolver_component_tests_common_zone_name']

  for group in test_records_config['resolver_component_tests']:
    for name in group['records'].keys():
      for record in group['records'][name]:
        r_type = record['type']
        r_data = record['data']
        r_ttl = int(record['TTL'])
        record_full_name = '%s.%s' % (name, common_zone_name)
        if r_type == 'A':
          _push_record(record_full_name, A(r_data), r_ttl)
        if r_type == 'AAAA':
          _push_record(record_full_name, AAAA(r_data), r_ttl)
        if r_type == 'SRV':
          p, w, port, target = r_data.split(' ')
          p = int(p)
          w = int(w)
          port = int(port)
          target_full_name = '%s.%s' % (target, common_zone_name)
          _push_record(record_full_name, SRV(target=target_full_name, priority=p, weight=w, port=port), r_ttl)
        if r_type == 'TXT':
          _maybe_split_up_txt_data(record_full_name, r_data, r_ttl)

  resolver = Resolver()
  print('starting local dns server on 127.0.0.1:%s' % dns_server_port)
  DNSServer(resolver, port=dns_server_port, address='127.0.0.1', tcp=False).start_thread()
  DNSServer(resolver, port=dns_server_port, address='127.0.0.1', tcp=True).start_thread()

if __name__ == '__main__':
  start_local_dns_server_in_background(15353)
  while True:
    time.sleep(1)
    sys.stderr.flush()
    sys.stdout.flush()
