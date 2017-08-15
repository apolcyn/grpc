#!/usr/bin/env python
# Copyright 2017 gRPC authors.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# This script is invoked by a Jenkins pull request job and executes all
# args passed to this script if the pull request affect C/C++ code

import argparse
import subprocess
import re
import sys
import os

import python_utils.jobset as jobset
import python_utils.report_utils as report_utils
import name_resolution.dns_records_config as dns_records_config

_ROOT = os.path.abspath(os.path.join(os.path.dirname(sys.argv[0]), '../..'))
os.chdir(_ROOT)

argp = argparse.ArgumentParser(description='Sanity check existence of testing DNS records in a DNS service')
argp.add_argument('-p', '--dns_server_port', default=15353, type=int)
argp.add_argument('-s', '--dns_server_host', default=None, type=str)
args = argp.parse_args()

def _fail_error(cmd, found, expected):
  expected_str = ''
  for e in expected:
    expected_str += ' '.join(e)
    expected_str += '\n'
  jobset.message('FAILED', ('A dns records sanity check failed.\n'
                            '\"%s\" yielded bad record: \n'
                            '  Found: \n'
                            '    %s\n  Expected: \n'
                            '    %s') % (cmd, found and ' '.join(found), expected_str))
  sys.exit(1)


def _matches_any(parsed_record, candidates):
  for e in candidates:
    if len(e) == len(parsed_record):
      matches = True
      for i in range(len(e)):
        if e[i] != parsed_record[i]:
          matches = False
      if matches:
        return True
  return False


def _check_dns_record(command, expected_data):
  output = subprocess.check_output(command.split(' '))
  lines = output.splitlines()

  found = None
  i = 0
  for l in lines:
    l = l.strip()
    if l == ';; ANSWER SECTION:':
      found = i
      break
    i += 1

  if found is None or len(expected_data) > len(lines) - found:
    _fail_error(command, found, expected_data)
  for l in lines[found:found+len(expected_data)]:
    parsed = re.split('\s+', lines[i + 1])
    if not _matches_any(parsed, expected_data):
      _fail_error(command, parsed, expected_data)


def _maybe_massage_and_split_up_expected_data(record_type, record_data):
  if record_type != 'TXT':
    return [record_data]
  global args

  if args.dns_server_host is None:
    # There is a small feature with GCE DNS resolver uploader:
    # double quotes surrounding the txt record string and the backslashes
    # used to escape inner double quotes appear to be counted towards the 255 character
    # single-string limit. The local DNS server doesn't though.
    record_data = record_data.replace('"', '\\"')

  chunks = []
  start = 0
  while len(record_data[start:]) > 0:
    next_read = len(record_data[start:])
    if next_read > 255:
      next_read = 255

    data_chunk = record_data[start:start+next_read]
    if args.dns_server_host is not None:
      data_chunk = data_chunk.replace('"', '\\"')
    chunks.append('\"%s\"' % data_chunk)

    start += next_read
  return chunks


def sanity_check_dns_records(dns_records_by_name):
  for name in dns_records_by_name.keys():
    type_to_data = {}
    for record in dns_records_by_name[name]:
      if type_to_data.get(record.record_type) is not None:
        type_to_data[record.record_type] += [record]
      else:
        type_to_data[record.record_type] = [record]

    for record_type in type_to_data.keys():
      expected_lines = []
      for record in type_to_data[record_type]:
        assert record.record_type == record_type
        assert record.record_name == name
        for data_chunk in _maybe_massage_and_split_up_expected_data(record_type, record.record_data):
          line = '%s %s %s %s %s' % (record.record_name, record.ttl, record.record_class, record.record_type, data_chunk)
          expected_lines.append(line.split(' '))
      cmd = 'dig %s %s' % (record_type, name)
      if args.dns_server_host:
        cmd += ' -p %s @%s' % (args.dns_server_port, args.dns_server_host)
      _check_dns_record(
        command=cmd,
        expected_data=expected_lines)


if __name__ == '__main__':
  sanity_check_dns_records(dns_records_config.RECORDS_BY_NAME)
