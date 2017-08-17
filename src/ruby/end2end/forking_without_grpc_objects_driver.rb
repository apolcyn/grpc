#!/usr/bin/env ruby

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

# Prompted by and minimal repro of https://github.com/grpc/grpc/issues/10658

require_relative './end2end_common'

def main
  STDERR.puts 'prefork from parent'
  GRPC::Core::ForkingContext.prefork
  pid = fork do
    GRPC::Core::ForkingContext.postfork_child
    STDERR.puts 'postfork done from child'
  end
  GRPC::Core::ForkingContext.postfork_parent
  STDERR.puts 'postfork done from parent'

  begin
    Timeout.timeout(10) do
      Process.wait(pid)
    end
  rescue Timeout::Error
    STDERR.puts "timeout wait for client pid #{pid}"
    Process.kill('SIGKILL', pid)
    Process.wait(pid)
    STDERR.puts 'killed client child'
    raise 'Timed out waiting for client process. ' \
      'It likely hangs when forking while no grpc ' \
      'objects have been created'
  end

  client_exit_code = $CHILD_STATUS
  fail 'forking client client failed, ' \
    "exit code #{client_exit_code}" unless client_exit_code == 0
end

main
