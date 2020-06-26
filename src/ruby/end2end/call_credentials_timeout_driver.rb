#!/usr/bin/env ruby
#
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

this_dir = File.expand_path(File.dirname(__FILE__))
protos_lib_dir = File.join(this_dir, 'lib')
grpc_lib_dir = File.join(File.dirname(this_dir), 'lib')
$LOAD_PATH.unshift(grpc_lib_dir) unless $LOAD_PATH.include?(grpc_lib_dir)
$LOAD_PATH.unshift(protos_lib_dir) unless $LOAD_PATH.include?(protos_lib_dir)
$LOAD_PATH.unshift(this_dir) unless $LOAD_PATH.include?(this_dir)

require 'grpc'
require 'end2end_common'

def create_channel_creds
  test_root = File.join(File.dirname(__FILE__), '..', 'spec', 'testdata')
  files = ['ca.pem', 'client.key', 'client.pem']
  creds = files.map { |f| File.open(File.join(test_root, f)).read }
  GRPC::Core::ChannelCredentials.new(creds[0], creds[1], creds[2])
end

def client_cert
  test_root = File.join(File.dirname(__FILE__), '..', 'spec', 'testdata')
  cert = File.open(File.join(test_root, 'client.pem')).read
  fail unless cert.is_a?(String)
  cert
end

def create_server_creds
  test_root = File.join(File.dirname(__FILE__), '..', 'spec', 'testdata')
  GRPC.logger.info("test root: #{test_root}")
  files = ['ca.pem', 'server1.key', 'server1.pem']
  creds = files.map { |f| File.open(File.join(test_root, f)).read }
  GRPC::Core::ServerCredentials.new(
    creds[0],
    [{ private_key: creds[1], cert_chain: creds[2] }],
    true) # force client auth
end

class MutableValue
  attr_accessor :value

  def initialize(value)
    @value = value
  end
end

def main
  Thread.abort_on_exception = true
  server_runner = ServerRunner.new(EchoServerImpl)
  server_runner.server_creds = create_server_creds
  server_port = server_runner.run
  channel_args = {
    GRPC::Core::Channel::SSL_TARGET => 'foo.test.google.fr'
  }
  token_fetch_attempts = MutableValue.new(0)
  token_fetch_attempts_mu = Mutex.new
  times_out_first_time_auth_proc = proc do |args|
    token_fetch_attempts_mu.synchronize do
      old_val = token_fetch_attempts.value
      token_fetch_attempts.value += 1
      if old_val == 0
        STDERR.puts "call creds plugin sleeping for 6 seconds"
        sleep 6
        STDERR.puts "call creds plugin done with 6 second sleep"
        raise "test exception thrown purposely from call creds plugin"
      end
    end
    { 'authorization' => 'fake_val' }.merge(args)
  end
  channel_creds = create_channel_creds.compose(
    GRPC::Core::CallCredentials.new(times_out_first_time_auth_proc))
  stub = Echo::EchoServer::Stub.new("localhost:#{server_port}",
                                    channel_creds,
                                    channel_args: channel_args)
  STDERR.puts "perform a first few RPCs to try to get things into a bad state..."
  threads = []
  2000.times do
    threads << Thread.new do
      got_failure = false
      begin
        stub.echo(Echo::EchoRequest.new(request: 'hello'), deadline: Time.now + 3)
      rescue GRPC::BadStatus => e
        got_failure = true
      end
      unless got_failure
        fail 'expected RPC to fail'
      end
    end
  end
  threads.each { |t| t.join }
  # Expect three more RPCs to succeed
  STDERR.puts "now perform another RPC and expect OK..."
  stub.echo(Echo::EchoRequest.new(request: 'hello'), deadline: Time.now + 10)
  STDERR.puts "now perform another RPC and expect OK..."
  stub.echo(Echo::EchoRequest.new(request: 'hello'), deadline: Time.now + 10)
  STDERR.puts "now perform another RPC and expect OK..."
  stub.echo(Echo::EchoRequest.new(request: 'hello'), deadline: Time.now + 10)
  server_runner.stop
end

main
