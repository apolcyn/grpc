#!/usr/bin/env python2.7

import argparse
import subprocess
import os
import tempfile
import sys
import time
import signal

argp = argparse.ArgumentParser(description='Local DNS Server for resolver tests')
argp.add_argument('--client_args', required=True, type=str,
                  help='Client args.')
argp.add_argument('--records_config_template_path', required=True, type=str,
                  help=('DNS server records config template path'))
argp.add_argument('--grpclb_port', required=True, type=int,
                  help=('Port that grpclb server is listening on'))
argp.add_argument('--dns_server_bin_path', required=True, type=str,
                  help='Path to local DNS server python script.')
argp.add_argument('--dns_resolver_bin_path', required=True, type=str,
                  help=('Path to the DNS health check utility.'))
argp.add_argument('--tcp_connect_bin_path', required=True, type=str,
                  help=('Path to the TCP health check utility.'))
args = argp.parse_args()

def test_runner_log(msg):
  sys.stderr.write('\n%s: %s\n' % (__file__, msg))

def wait_until_dns_server_is_up(dns_server_subprocess,
                                dns_server_subprocess_output):
  for i in range(0, 30):
    test_runner_log('Health check: attempt to connect to DNS server over TCP.')
    tcp_connect_subprocess = subprocess.Popen([
        args.tcp_connect_bin_path,
        '--server_host', '127.0.0.1',
        '--server_port', str(53),
        '--timeout', str(1)])
    tcp_connect_subprocess.communicate()
    if tcp_connect_subprocess.returncode == 0:
      test_runner_log(('Health check: attempt to make an A-record '
                       'query to DNS server.'))
      dns_resolver_subprocess = subprocess.Popen([
          args.dns_resolver_bin_path,
          '--qname', 'health-check-local-dns-server-is-alive.resolver-tests.grpctestingexp',
          '--server_host', '127.0.0.1',
          '--server_port', str(53)],
          stdout=subprocess.PIPE)
      dns_resolver_stdout, _ = dns_resolver_subprocess.communicate()
      if dns_resolver_subprocess.returncode == 0:
        if '123.123.123.123' in dns_resolver_stdout:
          test_runner_log(('DNS server is up! '
                           'Successfully reached it over UDP and TCP.'))
        return
    time.sleep(0.1)
  dns_server_subprocess.kill()
  dns_server_subprocess.wait()
  test_runner_log(('Failed to reach DNS server over TCP and/or UDP. '
                   'Exitting without running tests.'))
  test_runner_log('======= DNS server stdout '
                  '(merged stdout and stderr) =============')
  with open(dns_server_subprocess_output, 'r') as l:
    test_runner_log(l.read())
  test_runner_log('======= end DNS server output=========')
  sys.exit(1)

with open(args.records_config_template_path, 'r') as template:
    records_config = template.read() % { 'grpclb_port': args.grpclb_port }

sys.stderr.write('===== DNS server records config: =====\n')
sys.stderr.write(records_config)
sys.stderr.write('======================================\n')

dns_server_invoke_args = os.path.join(os.sep, 'var', 'local', 'local_dns_server', 'dns_server.py')
dns_server_subprocess_output = tempfile.mktemp()
records_config_path = tempfile.mktemp()
with open(records_config_path, 'w') as records_config_generated:
    records_config_generated.write(records_config)
# Start the DNS server subprocess
with open(dns_server_subprocess_output, 'w') as l:
  dns_server_subprocess = subprocess.Popen([
      args.dns_server_bin_path,
      '--port', str(53),
      '--records_config_path', records_config_path],
      stdin=subprocess.PIPE,
      stdout=l,
      stderr=l)

def _quit_on_signal(signum, _frame):
  test_runner_log('Received signal: %d' % signum)

signal.signal(signal.SIGINT, _quit_on_signal)
signal.signal(signal.SIGTERM, _quit_on_signal)

wait_until_dns_server_is_up(
        dns_server_subprocess,
        dns_server_subprocess_output)

# Run the client
client_invoke_args = args.client_args.split(' ')
sys.stderr.write('client invocation command:|%s|\n' % client_invoke_args)
sys.stderr.write('cwd:|%s|\n' % os.getcwd())
environ = dict(os.environ)
environ.update({'GRPC_GO_LOG_VERBOSITY_LEVEL': '3', 'GRPC_GO_LOG_SEVERITY_LEVEL': 'INFO'})
p = subprocess.Popen(client_invoke_args, env=environ)
p.communicate()
sys.stdout.flush()
sys.stderr.flush()
dns_server_subprocess.kill()
dns_server_subprocess.wait()
sys.exit(p.returncode)
