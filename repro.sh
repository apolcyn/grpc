#!/bin/bash
curl localhost:32766/quitquitquit || echo "curl failed"
sleep 3
python tools/run_tests/python_utils/start_port_server.py
logfile=$(curl localhost:32766/logfile)
echo "logfile: $logfile"
python tools/run_tests/run_tests.py -l c -c dbg -t --iomgr_platform native -j 8 --disable_auto_set_flakes > k 2>&1
cat k | grep FAILED || echo "no tests failed"
cat k | grep 'port server'
cat $logfile | grep 'timed out'
