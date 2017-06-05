#!/bin/bash
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

# Creates a DNS managed zone on GCE.

set -ex

cd $(dirname $0)

ZONE_DNS='test.apolcyntest.'
ZONE_NAME='apolcyn-zone'

TTL='2100'

class DnsRecord(object):
  def __init__(self, record_type, record_name, record_data)
    self.record_type = record_type
    self.record_name = record_name
    self.record_data = record_data

ipv4_single_target_dns = 'ipv4-single-target.%s' % ZONE_DNS
ipv6_single_target_dns = 'ipv6-single-target.%s' % ZONE_DNS

ipv4_multi_target_dns = 'ipv4-multi-target.%s' % ZONE_DNS
ipv6_multi_target_dns = 'ipv6-multi-target.%s' % ZONE_DNS

records = [
    DnsRecord('A', ipv4_single_target_dns, '1.2.3.4'),
    DnsRecord('A', ipv4_multi_target_dns, '100.1.1.1,100.2.2.2,100.3.3.3'),
    DnsRecord('AAAA', ipv6_single_target_dns, '2607:f8b0:400a:801::1005'),
    DnsRecord('AAAA', ipv6_multi_target_dns, ','.join(['2607:f8b0:400a:801::1001',
                                                       '2607:f8b0:400a:801::1002',
                                                       '2607:f8b0:400a:801::1003']),
    DnsRecord('SRV', 'srv-%s' ipv4_single_target_dns, ipv4_single_target_dns),
    DnsRecord('SRV', 'srv-%s' % ipv4_multi_target_dns, ipv4_multi_target_dns),
    DnsRecord('SRV', 'srv-%s' % ipv6_single_target_dns, ipv6_single_target_dns),
    DnsRecord('SRV', 'srv-%s' % ipv6_multi_target_dns, ipv6_multi_target_dns),
]

cmds = []

cmds.append('gcloud dns record-sets transaction start -z=\"%s\"' % ZONE_NAME)

for r in records:
  cmds.append(('gcloud dns record-sets transaction add '
               '-z=\"%s\" '
               '--name=\"%s\" '
               '--type=\"%s\" '
               '--ttl=\"%s\" '
               '\"%s\"') % ZONE_NAME,
                           r.record_name,
                           r.record_type,
                           TTL,
                           r.record_data)

cmds.append('gcloud dns record-sets transaction describe -z=\"%s\"' % ZONE_NAME)
cmds.append('gcloud dns record-sets transaction execute -z=\"%s\"' % ZONE_NAME)
cmds.append('gcloud dns record-sets list -z=\"%s\"' % ZONE_NAME)
