/*
 *
 * Copyright 2018 gRPC authors.
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

#ifndef GRPC_CORE_EXT_FILTERS_CLIENT_CHANNEL_LOCAL_SUBCHANNEL_POOL_H
#define GRPC_CORE_EXT_FILTERS_CLIENT_CHANNEL_LOCAL_SUBCHANNEL_POOL_H

#include <grpc/support/port_platform.h>

#include "absl/container/btree_map.h"

#include "src/core/ext/filters/client_channel/subchannel_pool_interface.h"
#include "src/core/ext/filters/client_channel/subchannel.h"

namespace grpc_core {

// The local subchannel pool that is owned by a single channel. It doesn't
// support subchannel sharing with other channels by nature. Nor does it support
// subchannel retention when a subchannel is not used. The only real purpose of
// using this subchannel pool is to allow subchannel reuse within the channel
// when an incoming resolver update contains some addresses for which the
// channel has already created subchannels.
// Thread-unsafe.
class LocalSubchannelPool final : public SubchannelPoolInterface {
 public:
  LocalSubchannelPool() {}
  ~LocalSubchannelPool() override {}

  // Implements interface methods.
  // Thread-unsafe. Intended to be invoked within the client_channel work
  // serializer.
  std::unique_ptr<SubchannelRef> RegisterSubchannel(const SubchannelKey &key,
                                 RefCountedPtr<Subchannel> constructed) override;

 private:
  class LocalSubchannelPoolSubchannelRef : public SubchannelRef {
   public:
    LocalSubchannelPoolSubchannelRef(RefCountedPtr<LocalSubchannelPool> parent, RefCountedPtr<Subchannel> subchannel, const SubchannelKey &key);
    ~LocalSubchannelPoolSubchannelRef() override;
    Subchannel* subchannel() override { return subchannel_.get(); }
   private:
    RefCountedPtr<LocalSubchannelPool> parent_;
    RefCountedPtr<Subchannel> subchannel_;
    const SubchannelKey key_;
  };

  // A map from subchannel key to subchannel.
  absl::btree_map<SubchannelKey, WeakRefCountedPtr<Subchannel>> subchannel_map_;
};

}  // namespace grpc_core

#endif /* GRPC_CORE_EXT_FILTERS_CLIENT_CHANNEL_LOCAL_SUBCHANNEL_POOL_H */
