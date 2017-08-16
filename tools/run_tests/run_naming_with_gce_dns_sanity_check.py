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
argp.add_argument('-p', '--dns_server_port', default=0, type=int)
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


def _dig_output_line_matches(found_line, expected_line):
  if len(found_line) != len(expected_line):
    return False
  matches = True
  for i in range(len(expected_line)):
    if found_line[i] != expected_line[i]:
      matches = False
  if matches:
    return True


def _check_dns_record(command, expected_lines_in_answer_section):
  dig_output = subprocess.check_output(command.split(' ')).splitlines()

  begin_answer_section = -1
  i = 0
  for line in dig_output:
    line = line.strip()
    if line == ';; ANSWER SECTION:':
      begin_answer_section = i + 1
      break
    i += 1

  if begin_answer_section == -1:
    _fail_error(command, found, expected_lines_in_answer_section)
  if len(expected_lines_in_answer_section) > len(dig_output) - begin_answer_section:
    _fail_error(command, found, expected_lines_in_answer_section)

  i = 0
  for line in dig_output[begin_answer_section:begin_answer_section+len(expected_lines_in_answer_section)]:
    parsed = re.split('\s+', line)
    if not _dig_output_line_matches(parsed, expected_lines_in_answer_section[i]):
      _fail_error(command, parsed, expected_lines_in_answer_section)
    i += 1


def _maybe_massage_and_split_up_expected_data(record_type, record_data):
  if record_type != 'TXT':
    return [record_data]
  global args

  if args.dns_server_host is None:
    # With GCE DNS record uploads, backslashes used to escape inner-string
    # double-quotes appear to be counted towards the 255 character limit
    # for single strings. The local DNS server doesn't do this though.
    # So we need to add backslashes before chunking rather than after.
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
        for data_chunk in _maybe_massage_and_split_up_expected_data(record_type,
                                                                    record.record_data):
          line = '%s %s %s %s %s' % (record.record_name,
                                     record.ttl,
                                     record.record_class,
                                     record.record_type,
                                     data_chunk)
          expected_lines.append(line.split(' '))
      cmd = 'dig %s %s' % (record_type, name)
      if args.dns_server_host:
        assert args.dns_server_port > 0
        cmd += ' -p %s @%s' % (args.dns_server_port, args.dns_server_host)

      _check_dns_record(
        command=cmd,
        expected_lines_in_answer_section=expected_lines)


if __name__ == '__main__':
  sanity_check_dns_records(dns_records_config.RECORDS_BY_NAME)
