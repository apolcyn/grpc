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

GRPC_FORKING_TEST_MAX_FORK_COUNT = 5

def fork_one_child_and_wait(stub, current_fork_depth)
  current_fork_depth == GRPC_FORKING_TEST_MAX_FORK_COUNT && exit(0)

  GRPC::Core::ForkingContext.prefork

  child_pid = fork do
    GRPC::Core::ForkingContext.postfork_child

    STDERR.puts 'attempt RPC from child'
    stub.echo(Echo::EchoRequest.new(request: 'hello'))
    STDERR.puts 'finished RPC from child'

    fork_one_child_and_wait(stub, current_fork_depth + 1)
  end

  GRPC::Core::ForkingContext.postfork_parent

  begin
    Timeout.timeout(10) do
      Process.wait(child_pid)
    end
  rescue Timeout::Error
    STDERR.puts "timeout waiting for forked process #{p}"
    Process.kill('SIGKILL', p)
    Process.wait(p)
    raise 'Timed out waiting for client process.'
  end

  fail "forked process failed: #{$CHILD_STATUS.to_i}" if $CHILD_STATUS.to_i != 0
end

def main
  server_port = ''

  OptionParser.new do |opts|
    opts.on('--client_control_port=P', String) do
      STDERR.puts 'client control port not used'
    end
    opts.on('--server_port=P', String) do |p|
      server_port = p
    end
  end.parse!

  client_opts = {
    channel_args: {
      GRPC::Core::Channel::SSL_TARGET => 'foo.test.google.fr'
    }
  }

  stub = Echo::EchoServer::Stub.new("localhost:#{server_port}",
                                    create_channel_creds,
                                    **client_opts)

  STDERR.puts 'attempt RPC from parent'
  stub.echo(Echo::EchoRequest.new(request: 'hello'))
  STDERR.puts 'finished RPC from parent'

  fork_one_child_and_wait(stub, 0)
end

main
