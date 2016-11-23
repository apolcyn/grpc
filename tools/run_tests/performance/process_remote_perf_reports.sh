#!/bin/bash
# Copyright 2015, Google Inc.
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

mkdir -p $OUTPUT_DIR

PERF_DATA_FILE=${PERF_BASE_NAME}-perf.data
PERF_SCRIPT_OUTPUT=${PERF_BASE_NAME}-out.perf

# Generate text output from profiles
echo "creating perf text report on remote host $USER_AT_HOST"

# "perf report -i" to not wait for terminal input
ssh $USER_AT_HOST "cd ~/performance_workspace/grpc && perf report -i $PERF_DATA_FILE -v --header | gzip > ${OUTPUT_FILENAME}.gz"

echo "copying perf text report from $USER_AT_HOST to here"
scp $USER_AT_HOST:~/performance_workspace/grpc/${OUTPUT_FILENAME}.gz "${OUTPUT_DIR}/${OUTPUT_FILENAME}.gz"

gzip -d -f $OUTPUT_DIR/${OUTPUT_FILENAME}.gz
mv $OUTPUT_DIR/${OUTPUT_FILENAME} $OUTPUT_DIR/${OUTPUT_FILENAME}.txt

# Generate Flame graphs
echo "running perf script on $USER_AT_HOST with perf.data"
ssh $USER_AT_HOST "cd ~/performance_workspace/grpc && perf script -i $PERF_DATA_FILE | gzip > ${PERF_SCRIPT_OUTPUT}.gz"

scp $USER_AT_HOST:~/performance_workspace/grpc/$PERF_SCRIPT_OUTPUT.gz .

gzip -d -f $PERF_SCRIPT_OUTPUT.gz

~/FlameGraph/stackcollapse-perf.pl --kernel $PERF_SCRIPT_OUTPUT | ~/FlameGraph/flamegraph.pl --color=java --hash --minwidth 0.01 > ${OUTPUT_DIR}/${OUTPUT_FILENAME}.svg
