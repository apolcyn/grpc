# Generated by the protocol buffer compiler.  DO NOT EDIT!
# Source: echo.proto for package 'echo'
# Original file comments:
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

require 'grpc'
require 'echo_pb'

module Echo
  module EchoServer
    class Service

      include GRPC::GenericService

      self.marshal_class_method = :encode
      self.unmarshal_class_method = :decode
      self.service_name = 'echo.EchoServer'

      rpc :Echo, EchoRequest, EchoReply
      rpc :EchoVerifyCallCreds, EchoRequest, EchoReply
    end

    Stub = Service.rpc_stub_class
  end
end
