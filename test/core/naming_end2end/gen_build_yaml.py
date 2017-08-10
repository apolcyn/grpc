#!/usr/bin/env python2.7
# Copyright 2015 gRPC authors.
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


"""Generates the appropriate build.json data for all the end2end tests."""


import yaml
import collections
import hashlib

def main():
  json = {
      '#': 'generated with test/core/naming_end2end/gen_build_json.py',
      'libs': [
          {
              'name': 'naming_end2end_test',
              'build': 'private',
              'language': 'c',
              'secure': False,
              'src': ['test/core/naming_end2end/naming_end2end_test_util.c']
              'headers': ['test/core/naming_end2end/naming_end2end_test_util.h']
              'deps': [
                  'grpc_test_util',
                  'grpc'
                  'gpr_test_util'
                  'gpr',
              ],
              'vs_proj_dir': 'test/naming_end2end/tests',
          }
      ],
      'targets': [
          {
              'name': 'naming_end2end_test',
              'build': 'test',
              'language': 'c',
              'run': False,
              'src': ['test/core/naming_end2end/naming_end2end_test.c'],
              'deps': [
                  'naming_end2end_tests'
              ],
              'vs_proj_dir': 'test/naming_end2end/tests',
          }
      ],
      'core_naming_end2end_tests': {
          'test1': 'test1_val',
          'test2': 'test2_val',
      }
          #(t, END2END_TESTS[t].secure)
          #for t in END2END_TESTS.keys()
      #)
  }
  print yaml.dump(json)


if __name__ == '__main__':
  main()
