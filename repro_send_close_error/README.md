# Reproduce an error with a SEND_CLOSE_FROM_CLIENT op, seen in ruby

To run this, from the repo root:

'''
$ tools/run_tests/performance/build_performance.sh ruby
$ bins/opt/qps_worker --server_port 13000
'''

In another terminal, from this directory:

'''
$ make
$ ./repro_error
'''

It should fail within a second or two, with a message at the bottom of the
saying that it failed on a SEND_CLOSE_FROM_CLIENT op
