/*
 *
 * Copyright 2016 gRPC authors.
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

#ifndef GRPC_CORE_EXT_FILTERS_CLIENT_CHANNEL_RESOLVER_DNS_C_ARES_GRPC_ARES_EV_DRIVER_H
#define GRPC_CORE_EXT_FILTERS_CLIENT_CHANNEL_RESOLVER_DNS_C_ARES_GRPC_ARES_EV_DRIVER_H

#include <grpc/support/port_platform.h>

#include <ares.h>
#include "src/core/lib/iomgr/pollset_set.h"
#include "src/core/lib/gprpp/abstract.h"

namespace grpc_core {

class AresEvDriver;

class FdNode {
 public:
  explicit FdNode(AresEvDriver*);
  explicit FdNode();
  virtual void DestroyInnerEndpoint() GRPC_ABSTRACT;
  virtual void ShutdownInnerEndpoint() GRPC_ABSTRACT;
  virtual ares_socket_t GetInnerEndpoint() GRPC_ABSTRACT;
  virtual bool IsInnerEndpointStillReadable() GRPC_ABSTRACT;
  virtual void AttachInnerEndpoint(const ares_socket_t&, const char*) GRPC_ABSTRACT;
  virtual void ScheduleNotifyOnRead() GRPC_ABSTRACT;
  virtual void ScheduleNotifyOnWrite() GRPC_ABSTRACT;
  void MaybeRegisterForReadsAndWrites(int socks_bitmask, size_t idx);
  void Destroy();
  void Shutdown();
  static void OnReadable(void* arg, grpc_error* error);
  static void OnWriteable(void* arg, grpc_error* error);
  void OnReadableInner(grpc_error* error);
  void OnWriteableInner(grpc_error* error);
  void SetNext(FdNode* other);
  FdNode* GetNext();

 protected:
  /** a closure wrapping on_readable_cb, which should be invoked when the
      grpc_fd in this node becomes readable. */
  grpc_closure read_closure_;
  /** a closure wrapping on_writable_cb, which should be invoked when the
      grpc_fd in this node becomes writable. */
  grpc_closure write_closure_;

 private:
  /** the owner of this fd node */
  AresEvDriver* ev_driver_;
  /** next fd node in the list */
  FdNode* next_;
  /** mutex guarding the rest of the state */
  gpr_mu mu_;
  /** if the readable closure has been registered */
  bool readable_registered_;
  /** if the writable closure has been registered */
  bool writable_registered_;
  /** if the fd is being shut down */
  bool shutting_down_;
};

class AresEvDriver {
 public:
  explicit AresEvDriver();
  virtual void AddInnerEndpointToPollsetSet(FdNode* fdn) GRPC_ABSTRACT;
  virtual FdNode* CreateFdNode() GRPC_ABSTRACT;
  void Start();
  void Ref();
  void Unref();
  void Destroy();
  void Shutdown();
  ares_channel GetChannel();
  ares_channel* GetChannelPointer();
  void NotifyOnEvent();
   /* Creates a new grpc_ares_ev_driver. Returns GRPC_ERROR_NONE if \a ev_driver is
      created successfully. */
  static grpc_error* Create(AresEvDriver** ev_driver, grpc_pollset_set* pollset_set);

 private:
  void NotifyOnEventLocked();
  FdNode* PopFdNode(ares_socket_t as);
  FdNode* fds_;
  ares_channel channel_;
  gpr_mu mu_;
  gpr_refcount refs_;
  bool working_;
  bool shutting_down_;
};

}  // namespace grpc_core

grpc_error* grpc_ares_ev_driver_create(grpc_core::AresEvDriver** ev_driver,
                                       grpc_pollset_set* pollset_set);

#endif /* GRPC_CORE_EXT_FILTERS_CLIENT_CHANNEL_RESOLVER_DNS_C_ARES_GRPC_ARES_EV_DRIVER_H \
        */
