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


import argparse
import subprocess
import os
import tempfile
import sys
import time
import signal
import yaml

argp = argparse.ArgumentParser(description='Runs a DNS server for LB interop tests')
argp.add_argument('-l', '--grpclb_ips', default=None, type=str,
                  help='Comma-separated list of IP addresses of balancers')
argp.add_argument('-f', '--fallback_ips', default=None, type=str,
                  help='Comma-separated list of IP addresses of fallback servers')
args = argp.parse_args()

balancer_a_records = []
grpclb_ips = args.grpclb_ips.split(',')
if grpclb_ips[0]:
    for ip in grpclb_ips:
        balancer_a_records.append({
            'TTL': '2100',
            'data': ip,
            'type': 'A',
        })
fallback_a_records = []
fallback_ips = args.fallback_ips.split(',')
if fallback_ips[0]:
    for ip in fallback_ips:
        fallback_a_records.append({
            'TTL': '2100',
            'data': ip,
            'type': 'A',
        })
records_config_yaml = {
    'resolver_tests_common_zone_name':
    'test.google.fr.',
    'resolver_component_tests': [{
        'records': {
            '_grpclb._tcp.server': [
                {
                    'TTL': '2100',
                    'data': '0 0 8080 balancer',
                    'type': 'SRV'
                },
            ],
            'balancer':
            balancer_a_records,
            'server':
            fallback_a_records,
        }
    }]
}
# Generate the actual DNS server records config file
records_config_path = tempfile.mktemp()
with open(records_config_path, 'w') as records_config_generated:
    records_config_generated.write(yaml.dump(records_config_yaml))

with open(records_config_path, 'r') as records_config_generated:
    sys.stderr.write('===== DNS server records config: =====\n')
    sys.stderr.write(records_config_generated.read())
    sys.stderr.write('======================================\n')

# Run the DNS server
# Note that we need to add the extra
# A record for metadata.google.internal in order for compute engine
# OAuth creds and ALTS creds to work.
# TODO(apolcyn): should metadata.google.internal always resolve
# to 169.254.169.254?
subprocess.check_output([
    '/var/local/git/grpc/test/cpp/naming/utils/dns_server.py', '--port=53',
    '--records_config_path', records_config_path,
    '--add_a_record=metadata.google.internal:169.254.169.254',
])
