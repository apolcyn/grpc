#!/usr/bin/env python
# Copyright 2015, Google Inc.
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met:
#
#     * Redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above
# copyright notice, this list of conditions and the following disclaimer
# in the documentation and/or other materials provided with the
# distribution.
#     * Neither the name of Google Inc. nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

# Source of truth for DNS records used in testing on GCE

ZONE_DNS = 'test.expgrpctesting.'
ZONE_NAME = 'exp-grcp-testing'
TTL = '2100'

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
  ipv4_multi_target_dns = 'ipv4-multi-target.%s' % ZONE_DNS
  ipv6_multi_target_dns = 'ipv6-multi-target.%s' % ZONE_DNS

  records = [
      DnsRecord('A', ipv4_single_target_dns, '1.2.3.4'),
      DnsRecord('A', ipv4_multi_target_dns, ','.join(['1.2.3.5',
                                                      '1.2.3.6',
                                                      '1.2.3.7'])),
      DnsRecord('AAAA', ipv6_single_target_dns, '2607:f8b0:400a:801::1001'),
      DnsRecord('AAAA', ipv6_multi_target_dns, ','.join(['2607:f8b0:400a:801::1002',
                                                         '2607:f8b0:400a:801::1003',
                                                         '2607:f8b0:400a:801::1004'])),
      DnsRecord('SRV', '_grpclb._tcp.srv-%s' % ipv4_single_target_dns, '0 0 %s %s' % (SRV_PORT, ipv4_single_target_dns)),
      DnsRecord('SRV', '_grpclb._tcp.srv-%s' % ipv4_multi_target_dns, '0 0 %s %s' % (SRV_PORT, ipv4_multi_target_dns)),
      DnsRecord('SRV', '_grcplb._tcp.srv-%s' % ipv6_single_target_dns, '0 0 %s %s' % (SRV_PORT, ipv6_single_target_dns)),
      DnsRecord('SRV', '_grcplb._tcp.srv-%s' % ipv6_multi_target_dns, '0 0 %s %s' % (SRV_PORT, ipv6_multi_target_dns)),
  ]
  return records

def get_expected_ip_end_results_for_srv_record(srv_record, dns_records):
  for r in dns_records:
    if r.record_name == srv_record.record_data.split(' ')[3]:
      return r.record_data
  raise(Exception('no ip record found for target of srv record: %s' % srv_record.record_name))

DNS_RECORDS = _create_records_for_testing()
