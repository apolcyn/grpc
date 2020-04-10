import sys
import re

def timestamp_to_ms(ts):
  m = re.match(r'([0-9]+):([0-9]+):([0-9]+).([0-9]+)', ts)
  assert m is not None, ('bad ts: %s' % ts)
  h = float(m.group(1))
  mi = float(m.group(2))
  s = float(m.group(3))
  ms = float(m.group(4)[:3])
  return ms + s * 1e3 + mi * 60 * 1e3 + h * 24 * 60 * 1e3

def get_thread_and_elapsed(key, line):
  idx = 0
  for token in line.split():
    if token.startswith(key):
      subtokens = token.split('-')
      thread = '-'.join(subtokens[1:])
      m = re.match(r'.*elapsed us: (.*)', line)
      if m is None:
        us = None
      else:
        us = float(m.group(1))
      return thread, us
    idx += 1
  raise Exception('no thread found in line %s with key %s'  % (line, key))

pending = {}
latencies = []
latencies = sorted(latencies)
register_elapsed = {}
unregister_elapsed = {}
start_time_ms = None
pending_register_starts = {}
pending_unregister_starts = {}
last_ts_ms = None
reached_end = False
timeout = None
with open('k', 'r') as f:
  for l in f.readlines():
    if 'apolcyn main begin grpc_init' in l:
      assert start_time_ms is None
      start_time_ms = timestamp_to_ms(l.split()[1])
    if '================================================================================' in l and start_time_ms is not None:
      reached_end = True
    if start_time_ms is not None and not reached_end and re.match(r'([0-9]+):([0-9]+):([0-9]+).([0-9]+)', l.split()[1]) is not None:
      last_ts_ms = timestamp_to_ms(l.split()[1])
    if 'apolcyn time counter' in l and 'RegisterSubchannel' in l and 'elapsed us' not in l:
      thread, _ = get_thread_and_elapsed('RegisterSubchannel', l)
      assert not pending_register_starts.get(thread)
      pending_register_starts[thread] = timestamp_to_ms(l.split()[1])
    elif 'apolcyn time counter' in l and 'UnregisterSubchannel' in l and 'elapsed us' not in l:
      thread, _ = get_thread_and_elapsed('UnregisterSubchannel', l)
      assert not pending_unregister_starts.get(thread)
      pending_unregister_starts[thread] = timestamp_to_ms(l.split()[1])
    elif 'apolcyn time counter' in l and 'RegisterSubchannel' in l and 'elapsed us' in l:
      thread, us = get_thread_and_elapsed('RegisterSubchannel', l)
      if register_elapsed.get(thread) is None:
        register_elapsed[thread] = 0
      register_elapsed[thread] += us
      assert pending_register_starts[thread]
      pending_register_starts[thread] = False
    elif 'apolcyn time counter' in l and 'UnregisterSubchannel' in l and 'elapsed us' in l:
      thread, us = get_thread_and_elapsed('UnregisterSubchannel', l)
      if unregister_elapsed.get(thread) is None:
        unregister_elapsed[thread] = 0
      unregister_elapsed[thread] += us
      assert pending_unregister_starts[thread]
      pending_unregister_starts[thread] = False
    elif 'TIMEOUT in' in l:
      m = re.match(r'.*TIMEOUT in (.+?)s', l)
      timeout = float(m.group(1))
for k in register_elapsed.keys():
  print('thread %s spent %lf ms in RegisterSubchannel' % (k, register_elapsed[k] / 1e3))
for k in unregister_elapsed.keys():
  print('thread %s spent %lf ms in UnregisterSubchannel' % (k, unregister_elapsed[k] / 1e3))
for k in pending_register_starts.keys():
  if pending_register_starts[k]:
    print('thread %s started RegisterSubchannel %lf ms ago' % (k, last_ts_ms - pending_register_starts[k]))
for k in pending_unregister_starts.keys():
  if pending_unregister_starts[k]:
    print('thread %s started UnregisterSubchannel %ld ms ago' % (k, last_ts_ms - pending_unregister_starts[k]))
print('last log printed by test was %lf ms in' % (last_ts_ms - start_time_ms))
print('test timed out in %lf seconds' % timeout)
print('start time ms: %lf last time ms: %lf' % (start_time_ms, last_ts_ms))
