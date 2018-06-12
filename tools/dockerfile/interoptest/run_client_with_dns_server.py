import argparse
import subprocess
import os
import tempfile
import sys
import time

argp = argparse.ArgumentParser(description='Local DNS Server for resolver tests')
argp.add_argument('--client_args', required=True, type=str,
                  help='Client args.')
argp.add_argument('--dns_server_args', required=True, type=str,
                  help=('DNS server args'))
argp.add_argument('--output_dir', required=True, type=str,
                  help=('Output directory for logs'))
args = argp.parse_args()


# Start the fake grpclb server
grpclb_invoke_args = [
        os.path.join(os.sep, 'go', 'src', 'google.golang.org', 'fake_grpclb', 'fake_grpclb'),
    ] + args.grpclb_args.split(' ')
sys.stderr.write('grpclb invocation:|%s|\n' % grpclb_invoke_args)
grpclb_log_stdout = tempfile.mktemp(dir=args.output_dir)
grpclb_log_stderr = tempfile.mktemp(dir=args.output_dir)
with open(grpclb_log_stdout, 'w') as stdout_log:
    with open(grpclb_log_stderr, 'w') as stderr_log:
        subprocess.Popen(grpclb_invoke_args, stdout=stdout_log, stderr=stderr_log)
# Start the fake backend
backend_invoke_args = [
        os.path.join(os.sep, 'go', 'src', 'google.golang.org', 'grpc', 'interop', 'server', 'server'),
    ] + args.backend_args.split(' ')
sys.stderr.write('backend invocation:|%s|\n' % backend_invoke_args)
backend_log_stdout = tempfile.mktemp(dir=args.output_dir)
backend_log_stderr = tempfile.mktemp(dir=args.output_dir)
with open(backend_log_stderr  , 'w') as stdout_log:
    with open(backend_log_stderr, 'w') as stderr_log:
        subprocess.Popen(backend_invoke_args, stdout=stdout_log, stderr=stderr_log)
# Start the fake fallback server
fallback_invoke_args = [
        os.path.join(os.sep, 'go', 'src', 'google.golang.org', 'grpc', 'interop', 'server', 'server'),
    ] + args.fallback_args.split(' ')
fallback_log_stdout = tempfile.mktemp(dir=args.output_dir)
fallback_log_stderr = tempfile.mktemp(dir=args.output_dir)
with open(fallback_log_stdout, 'w') as stdout_log:
    with open(fallback_log_stderr, 'w') as stderr_log:
        subprocess.Popen(fallback_invoke_args, stdout=stdout_log, stderr=stderr_log)
sys.stderr.write('fallback invocation:|%s|\n' % fallback_invoke_args)
sys.stdout.flush()
sys.stderr.flush()
time.sleep(args.timeout)
