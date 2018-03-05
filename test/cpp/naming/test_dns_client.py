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

"""Starts a local DNS server for use in tests"""

import argparse
import sys
import yaml
import signal
import os

import twisted
import twisted.internet
import twisted.internet.reactor
import twisted.internet.threads
import twisted.internet.defer
import twisted.internet.protocol
import twisted.internet.task as task
import twisted.names
import twisted.names.client
import twisted.names.dns
import twisted.names.server
from twisted.names import client, server, common, authority, dns
import argparse

def main():
  argp = argparse.ArgumentParser(description='Local DNS Server for resolver tests')
  argp.add_argument('-s', '--server_host', default='127.0.0.1', type=str,
                    help='Host for DNS server to listen on for TCP and UDP.')
  argp.add_argument('-p', '--server_port', default=53, type=int,
                    help='Port for DNS server to listen on for TCP and UDP.')
  argp.add_argument('-t', '--qtype', default=None, type=str,
                    help=('Directory of resolver_test_record_groups.yaml file. '
                          'Defauls to path needed when the test is invoked as part of run_tests.py.'))
  argp.add_argument('-n', '--qname', default=None, type=str,
                    help=('Directory of resolver_test_record_groups.yaml file. '
                          'Defauls to path needed when the test is invoked as part of run_tests.py.'))
  args = argp.parse_args()
  def OnResolverResultAvailable(result):
    answers, authority, additional = result
    for a in answers:
      print(a.payload)
  def BeginQuery(reactor, qname):
    servers = [(args.server_host, args.server_port)]
    resolver = client.Resolver(servers=servers)
    lookups = {
      'A': resolver.lookupAddress,
      'TXT': resolver.lookupText,
    }
    lookup_func = lookups.get(args.qtype)
    assert lookup_func, ('Unsupported qtype: %s' % args.qtype)
    deferred_result = lookup_func(qname)
    deferred_result.addCallback(OnResolverResultAvailable)
    return deferred_result
  task.react(BeginQuery, [args.qname])

if __name__ == '__main__':
  main()
