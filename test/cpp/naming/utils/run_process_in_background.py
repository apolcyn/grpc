import sys
import subprocess

b = subprocess.Popen(sys.argv[1:])
print(b.pid)
