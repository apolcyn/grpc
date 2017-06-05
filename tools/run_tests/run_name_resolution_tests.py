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
import os

import python_utils.jobset as jobset
import python_utils.report_utils as report_utils
import name_resolution.dns_records_config as dns_records_config

_ROOT = os.path.abspath(os.path.join(os.path.dirname(sys.argv[0]), '../..'))
os.chdir(_ROOT)


def _shortname(name, cmd):
  return '%s - %s' % (name, ' '.join(cmd))

class CLanguage(object):
  def __init__(self):
    self.name = 'c-core'

  def build_cmd(self):
    cmd = ('python tools/run_tests/run_tests.py '
           '-l c -r resolve_address_test --build_only').split(' ')
    return [jobset.JobSpec(cmd, shortname=_shortname(l.name, cmd))]
            

  def test_runner_cmd(self):
    specs = []
    cmd = [_ROOT + '/bins/opt/resolve_address_test']
    for resolver in ['native', 'ares']:
      env = {'GRPC_DEFAULT_SSL_ROOTS_FILE_PATH':
                 _ROOT + '/src/core/tsi/test_creds/ca.pem',
             'GRPC_POLL_STRATEGY': 'all', #TODO(apolcyn) change this?
             'GRPC_VERBOSITY': 'DEBUG',
             'GRPC_DNS_RESOLVER': resolver}
      specs.append(jobset.JobSpec(cmd,
                                  shortname=_shortname(l.name, cmd),
                                  environ=env))
    return specs

class JavaLanguage(object):
  def __init__(self):
    self.name = 'java'
  
  def build_cmd(self):
    cmd = 'echo java build cmd'.split(' ')
    return [jobset.JobSpec(cmd, shortname=l.name)]

  def test_runner_cmd(self):
    cmd = 'echo java test runner cmd'.split(' ')
    return [jobset.JobSpec(cmd, shortname=_shortname(l.name, cmd))]

class GoLanguage(object):
  def __init__(self):
    self.name = 'go'

  def build_cmd(self):
    cmd = 'echo go build cmd'.split(' ')
    return [jobset.JobSpec(cmd, shortname=_shortname(l.name, cmd))]

  def test_runner_cmd(self):
    cmd = 'echo go test runner cmd'.split(' ')
    return [jobset.JobSpec(cmd, shortname=_shortname(l.name, cmd))]

languages = [CLanguage(), JavaLanguage(), GoLanguage()]

results = {}

build_jobs = []
run_jobs = []
for l in languages:
  build_jobs.extend(l.build_cmd())
  run_jobs.extend(l.test_runner_cmd())

def _build():
  num_failures, _ = jobset.run(
    build_jobs, maxjobs=3, stop_on_failure=True,
    newline_on_success=True)
  return num_failures

def _run():
  num_failures, resultset = jobset.run(
    run_jobs, maxjobs=3, stop_on_failure=True,
    newline_on_success=True)
  report_utils.render_junit_xml_report(resultset, 'naming.xml',
    suite_name='naming_test')

  return num_failures

if _build():
  jobset.message('FAILED', 'Some of the tests failed to build')
  sys.exit(1)

if _run():
  jobset.message('FAILED', 'Some tests failed (but the build succeeded)')
  sys.exit(2)
