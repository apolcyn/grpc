#!/usr/bin/env python2.7

import argparse
import subprocess
import os
import tempfile
import sys
import time
import signal

_CONVENTIONAL_DNS_SERVER_PORT = 53

# Read in args passed in as env variables
port = int(os.environ.get('PORT'))
records_config_template_path = os.environ.get('RECORDS_CONFIG_TEMPLATE_PATH')
grpclb_port = int(os.environ.get('GRPCLB_PORT'))
# Generate the actual DNS server records config file
with open(records_config_template_path, 'r') as template:
  records_config = template.read() % { 'grpclb_port': grpclb_port }
records_config_path = tempfile.mktemp()
with open(records_config_path, 'w') as records_config_generated:
  records_config_generated.write(records_config)

sys.stderr.write('===== DNS server records config: =====\n')
sys.stderr.write(records_config)
sys.stderr.write('======================================\n')

# Run the DNS server
subprocess.check_output([
  '/var/local/git/grpc/test/cpp/naming/utils/dns_server.py',
  '--port=%d' % _CONVENTIONAL_DNS_SERVER_PORT,
  '--records_config_path', records_config_path])
