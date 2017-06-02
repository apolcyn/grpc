#!/usr/bin/env python
# Copyright 2017, Google Inc.
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met:
#
#     * Redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above
# copyright notice, this list of conditions and the following disclaimer
# in the documentation and/or other materials provided with the
# distribution.
#     * Neither the name of Google Inc. nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

import subprocess
import re
import sys


def check_dns_record(command, expected):
  output = subprocess.check_output(re.split('\s+', command))
  lines = output.splitlines()
  expected = re.split('\s+', expected)
  
  found = None
  i = 0
  for l in lines:
    if l == ';; ANSWER SECTION:':
      found = re.split('\s+', lines[i + 1])
      break
    i += 1

  def fail_error(found, expected):
    print('bad record: %s.\n Expected %s' %
      (' '.join(found), ' '.join(expected)))
    sys.exit(1)
  
  if len(expected) != len(found):
    fail_error(found, expected)
  i = 0
  for w in expected:
    if w != found[i]:
      fail_error(found, expected)
    i += 1

check_dns_record(
  command='dig A mytestlb.test.apolcyntest.',
  expected='mytestlb.test.apolcyntest. 21000 IN	A 5.6.7.8')

check_dns_record(
  command='dig SRV _grpclb._tcp.mylbtest.test.apolcyntest.',
  expected=('_grpclb._tcp.mylbtest.test.apolcyntest. '
            '300 IN SRV 0 0 1234 mytestlb.test.apolcyntest.'))
