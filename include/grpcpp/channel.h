/*
 *
 * Copyright 2015 gRPC authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#ifndef GRPCPP_CHANNEL_H
#define GRPCPP_CHANNEL_H

#include <memory>
#include <mutex>

#include <grpc/grpc.h>
#include <grpcpp/impl/call.h>
#include <grpcpp/impl/codegen/channel_interface.h>
#include <grpcpp/impl/codegen/client_interceptor.h>
#include <grpcpp/impl/codegen/config.h>
#include <grpcpp/impl/codegen/grpc_library.h>

struct grpc_channel;

namespace grpc {

namespace experimental {
/// Resets the channel's connection backoff.
/// TODO(roth): Once we see whether this proves useful, either create a gRFC
/// and change this to be a method of the Channel class, or remove it.
void ChannelResetConnectionBackoff(Channel* channel);
}  // namespace experimental

/// Channels represent a connection to an endpoint. Created by \a CreateChannel.
class Channel final : public ChannelInterface,
                      public internal::CallHook,
                      public std::enable_shared_from_this<Channel>,
                      private GrpcLibraryCodegen {
 public:
  ~Channel();

  /// Get the current channel state. If the channel is in IDLE and
  /// \a try_to_connect is set to true, try to connect.
  grpc_connectivity_state GetState(bool try_to_connect) override;

  /// Returns the LB policy name, or the empty string if not yet available.
  grpc::string GetLoadBalancingPolicyName() const;

  /// Returns the service config in JSON form, or the empty string if
  /// not available.
  grpc::string GetServiceConfigJSON() const;
  const grpc_channel* get_inner_channel();

 private:
  template <class InputMessage, class OutputMessage>
  friend class internal::BlockingUnaryCallImpl;
  friend void experimental::ChannelResetConnectionBackoff(Channel* channel);
  friend std::shared_ptr<Channel> CreateChannelInternal(
      const grpc::string& host, grpc_channel* c_channel,
      std::vector<
          std::unique_ptr<experimental::ClientInterceptorFactoryInterface>>
          interceptor_creators);
  friend class internal::InterceptedChannel;
  Channel(const grpc::string& host, grpc_channel* c_channel,
          std::vector<
              std::unique_ptr<experimental::ClientInterceptorFactoryInterface>>
              interceptor_creators);

  internal::Call CreateCall(const internal::RpcMethod& method,
                            ClientContext* context,
                            CompletionQueue* cq) override;
  void PerformOpsOnCall(internal::CallOpSetInterface* ops,
                        internal::Call* call) override;
  void* RegisterMethod(const char* method) override;

  void NotifyOnStateChangeImpl(grpc_connectivity_state last_observed,
                               gpr_timespec deadline, CompletionQueue* cq,
                               void* tag) override;
  bool WaitForStateChangeImpl(grpc_connectivity_state last_observed,
                              gpr_timespec deadline) override;

  CompletionQueue* CallbackCQ() override;

  internal::Call CreateCallInternal(const internal::RpcMethod& method,
                                    ClientContext* context, CompletionQueue* cq,
                                    size_t interceptor_pos) override;

  const grpc::string host_;
  grpc_channel* const c_channel_;  // owned

  // mu_ protects callback_cq_ (the per-channel callbackable completion queue)
  std::mutex mu_;

  // callback_cq_ references the callbackable completion queue associated
  // with this channel (if any). It is set on the first call to CallbackCQ().
  // It is _not owned_ by the channel; ownership belongs with its internal
  // shutdown callback tag (invoked when the CQ is fully shutdown).
  CompletionQueue* callback_cq_ = nullptr;

  std::vector<std::unique_ptr<experimental::ClientInterceptorFactoryInterface>>
      interceptor_creators_;
};

}  // namespace grpc

#endif  // GRPCPP_CHANNEL_H
