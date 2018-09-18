#!/bin/bash
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
#
# Builds Go interop server and client in a base image.
set -ex

if [[ "$GRPCLB_PORT" == "" ]]; then
  echo "GRPCLB_PORT unset."
  exit 1
fi

THIS_DIR="$(dirname $0)"
python "$THIS_DIR"/run_fake_dns_server.py
