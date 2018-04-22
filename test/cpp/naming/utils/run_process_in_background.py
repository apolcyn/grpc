import sys
import subprocess

with open('dns_server_log.log', 'w') as log:
  print(subprocess.Popen(sys.argv[1:], stdin=subprocess.PIPE, stdout=log, stderr=log).pid)
