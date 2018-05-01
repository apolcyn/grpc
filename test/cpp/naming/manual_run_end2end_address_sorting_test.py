#!/usr/bin/env python
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

import os
import subprocess

_MSBUILD_CONFIG = os.environ['CONFIG']
os.chdir(os.path.join('..', '..', os.getcwd()))
_DNS_SERVER_PORT = 15353

subprocess.call([
    'C:\\Python27\\python.exe',
    'test\\cpp\\naming\\end2end_address_sorting_test_runner.py',
    '--test_bin_path', 'cmake\\build\\%s\\end2end_address_sorting_test.exe' % _MSBUILD_CONFIG,
    '--dns_server_bin_path', 'test\\cpp\\naming\\utils\\dns_server.py',
    '--records_config_path', 'test\\cpp\\naming\\end2end_address_sorting_test_record_groups.yaml',
    '--dns_server_port', str(_DNS_SERVER_PORT),
    '--dns_resolver_bin_path', 'test\\cpp\\naming\\utils\\dns_resolver.py',
    '--tcp_connect_bin_path', 'test\\cpp\\naming\\utils\\tcp_connect.py',
])
