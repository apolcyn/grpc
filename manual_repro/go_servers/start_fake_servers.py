import argparse
import subprocess
import os
import tempfile
import sys
import time

argp = argparse.ArgumentParser(description='Local DNS Server for resolver tests')
argp.add_argument('--grpclb_args', required=True, type=str,
                  help='grpclb args.')
argp.add_argument('--backend_args', required=True, type=str,
                  help=('Backend args'))
argp.add_argument('--fallback_args', required=True, type=str,
                  help=('Fallback args'))
argp.add_argument('--output_dir', required=True, type=str,
                  help=('Output directory for logs'))
argp.add_argument('--timeout', required=True, type=int,
                  help=('timeout in seconds'))
args = argp.parse_args()
# Start the fake grpclb server
grpclb_invoke_args = [
        os.path.join(os.sep, 'go', 'src', 'google.golang.org', 'fake_grpclb', 'fake_grpclb'),
    ] + args.grpclb_args.split(' ')
sys.stderr.write('grpclb invocation:|%s|\n' % grpclb_invoke_args)
grpclb_log_stdout = tempfile.mktemp(dir=args.output_dir, suffix='_grpclb_stdout')
grpclb_log_stderr = tempfile.mktemp(dir=args.output_dir, suffix='_grpclb_stderr')
_ENVIRON = {'GRPC_LOG_VERBOSITY_LEVEL': '3', 'GRPC_LOG_SEVERITY_LEVEL': 'INFO'}
with open(grpclb_log_stdout, 'w') as stdout_log:
    with open(grpclb_log_stderr, 'w') as stderr_log:
        subprocess.Popen(grpclb_invoke_args, stdin=subprocess.PIPE, stdout=stdout_log, stderr=stderr_log, env=_ENVIRON)
# Start the fake backend
backend_invoke_args = [
        os.path.join(os.sep, 'go', 'src', 'google.golang.org', 'grpc', 'interop', 'server', 'server'),
    ] + args.backend_args.split(' ')
sys.stderr.write('backend invocation:|%s|\n' % backend_invoke_args)
backend_log_stdout = tempfile.mktemp(dir=args.output_dir, suffix='_backend_stdout')
backend_log_stderr = tempfile.mktemp(dir=args.output_dir, suffix='_backend_stderr')
with open(backend_log_stderr  , 'w') as stdout_log:
    with open(backend_log_stderr, 'w') as stderr_log:
        subprocess.Popen(backend_invoke_args, stdin=subprocess.PIPE, stdout=stdout_log, stderr=stderr_log, env=_ENVIRON)
# Start the fake fallback server
fallback_invoke_args = [
        os.path.join(os.sep, 'go', 'src', 'google.golang.org', 'grpc', 'interop', 'server', 'server'),
    ] + args.fallback_args.split(' ')
fallback_log_stdout = tempfile.mktemp(dir=args.output_dir, suffix='_fallback_sdout')
fallback_log_stderr = tempfile.mktemp(dir=args.output_dir, suffix='_fallback_stderr')
with open(fallback_log_stdout, 'w') as stdout_log:
    with open(fallback_log_stderr, 'w') as stderr_log:
        subprocess.Popen(fallback_invoke_args, stdin=subprocess.PIPE, stdout=stdout_log, stderr=stderr_log, env=_ENVIRON)
sys.stderr.write('fallback invocation:|%s|\n' % fallback_invoke_args)
sys.stdout.flush()
sys.stderr.flush()
time.sleep(args.timeout)
