#!/usr/bin/env python2.7

import argparse
import subprocess
import os
import tempfile
import sys
import time
import signal

grpclb_port = int(os.environ.get('GRPCLB_PORT'))
records_config_template_path = os.path.join(os.path.dirname(os.path.realpath(__file__)), 'records_config.yaml.template')
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
  '--port=53',
  '--records_config_path', records_config_path])
