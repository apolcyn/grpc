#!/usr/bin/env ruby

# Copyright 2016 gRPC authors.
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

require_relative './end2end_common'

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

  parent_stub = Echo::EchoServer::Stub.new("localhost:#{server_port}",
                                    :this_channel_is_insecure)
  p "PARENT got: #{parent_stub.echo(Echo::EchoRequest.new(request: 'hello')).response}"

  GRPC::Core::ForkingContext.prefork()

  p = fork do
    GRPC::Core::ForkingContext.postfork_child()

    stub = Echo::EchoServer::Stub.new("localhost:#{server_port}",
                                      :this_channel_is_insecure)
    p "CHILD got: #{stub.echo(Echo::EchoRequest.new(request: 'hello'))}"

    p "CHILD WITH PARENT STUB got: #{parent_stub.echo(Echo::EchoRequest.new(request: 'hello')).response}"
  end

  GRPC::Core::ForkingContext.postfork_parent()

  p "PARENT AFTER FORK got: #{parent_stub.echo(Echo::EchoRequest.new(request: 'hello')).response}"

end

main
