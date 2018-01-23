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

# Sample gRPC server that implements the Greeter::Helloworld service.
#
# Usage: $ path/to/greeter_server.rb

this_dir = File.expand_path(File.dirname(__FILE__))
this_lib_dir = File.join(this_dir, 'lib')
root_dir = File.expand_path(File.dirname(File.expand_path(File.dirname(this_dir))))
p root_dir
lib_dir = File.join(File.join(File.join(File.join(root_dir, 'src'), 'ruby'), 'lib'), 'grpc')
before_lib_dir = File.join(File.join(File.join(root_dir, 'src'), 'ruby'), 'lib')
p lib_dir
$LOAD_PATH.unshift(this_lib_dir) unless $LOAD_PATH.include?(this_lib_dir)
$LOAD_PATH.unshift(lib_dir) unless $LOAD_PATH.include?(lib_dir)
$LOAD_PATH.unshift(before_lib_dir) unless $LOAD_PATH.include?(before_lib_dir)

require_relative '../../src/ruby/lib/grpc'
require 'helloworld_services_pb'

# GreeterServer is simple server that implements the Helloworld Greeter server.
class GreeterServer < Helloworld::Greeter::Service
  # say_hello implements the SayHello rpc method.
  def say_hello(hello_req, _unused_call)
    Helloworld::HelloReply.new(message: "Hello #{hello_req.name}")
  end
end

# main starts an RpcServer that receives requests to GreeterServer at the sample
# server port.
def main
  Thread.abort_on_exception = true
  s = GRPC::RpcServer.new
  stop_server = false
  stop_server_cv = ConditionVariable.new
  stop_server_mu = Mutex.new
  stop_server_thread = Thread.new do
    loop do
      break if stop_server
      stop_server_mu.synchronize { stop_server_cv.wait(stop_server_mu, 60) }
    end
    s.stop
  end
  trap('INT') do
    stop_server = true
    stop_server_cv.broadcast
  end
  s.add_http2_port('0.0.0.0:50051', :this_port_is_insecure)
  s.handle(GreeterServer)
  s.run_till_terminated
  stop_server_thread.join
end

main
