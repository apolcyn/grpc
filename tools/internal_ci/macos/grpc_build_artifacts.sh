#!/bin/bash
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

set -ex

# change to grpc repo root
cd $(dirname $0)/../../..

source tools/internal_ci/helper_scripts/prepare_build_macos_rc

# install cython for all python versions
python2.7 -m pip install cython setuptools wheel
python3.4 -m pip install cython setuptools wheel
python3.5 -m pip install cython setuptools wheel
python3.6 -m pip install cython setuptools wheel

# Needed to build ruby artifacts.
# Rather than building all of the different versions of ruby for cross
# compililation right in here, fetch a zip file that contains all of the pre-built ruby
# versions. The script/makefiles used to build all of the different versions of
# ruby is flakey, and fetching the results of a successful run avoids that
# issue. Also, this makes the artifact builds significantly faster.
curl https://storage.googleapis.com/grpc-precompiled-binaries/ruby/macos-compilers/v1/rake-compilers.zip > ~/rake-compilers.zip
# Avoid logging too much
unzip ~/rake-compilers.zip > /dev/null && ls ~/.rake-compilers
gem install rubygems-update
update_rubygems

gem install rubygems-update
update_rubygems

tools/run_tests/task_runner.py -f artifact macos
