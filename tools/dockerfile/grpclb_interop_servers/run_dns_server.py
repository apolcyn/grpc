#!/usr/bin/env python2.7

import argparse
import subprocess
import os
import tempfile
import sys
import time
import signal
import yaml

balancer_a_records = []
grpclb_ips = os.environ.get('GRPCLB_IPS').split(',')
if grpclb_ips[0]:
    for ip in grpclb_ips:
        balancer_a_records.append({
            'TTL': '2100',
            'data': ip,
            'type': 'A',
        })
fallback_a_records = []
fallback_ips = os.environ.get('FALLBACK_IPS').split(',')
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
subprocess.check_output([
    '/var/local/git/grpc/test/cpp/naming/utils/dns_server.py', '--port=53',
    '--records_config_path', records_config_path
])
