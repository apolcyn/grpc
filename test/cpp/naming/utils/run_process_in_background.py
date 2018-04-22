import sys
import subprocess
import tempfile

dns_server_log_file = tempfile.mktemp()
with open(dns_server_log_file, 'w') as log:
  p = subprocess.Popen(sys.argv[1:], stdin=subprocess.PIPE, stdout=log, stderr=log)
# Print the pid and log file path so that a driver script can parse them
print('%s %s' % (p.pid, dns_server_log_file))
