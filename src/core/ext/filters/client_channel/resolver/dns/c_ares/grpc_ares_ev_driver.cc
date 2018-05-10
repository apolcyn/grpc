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

#include <grpc/support/port_platform.h>

#include "src/core/lib/iomgr/port.h"
#if GRPC_ARES == 1 && !defined(GRPC_UV)

#include <ares.h>
#include <memory.h>

#include "src/core/ext/filters/client_channel/resolver/dns/c_ares/grpc_ares_ev_driver.h"

#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>
#include <grpc/support/time.h>
#include "src/core/ext/filters/client_channel/resolver/dns/c_ares/grpc_ares_ev_driver.h"
#include "src/core/ext/filters/client_channel/resolver/dns/c_ares/grpc_ares_wrapper.h"
#include "src/core/lib/gpr/string.h"
#include "src/core/lib/gprpp/memory.h"
#include "src/core/lib/iomgr/iomgr_internal.h"
#include "src/core/lib/iomgr/sockaddr_utils.h"

namespace grpc_core {

FdNode::FdNode(ares_socket_t as) : InternallyRefCounted() {
  gpr_mu_init(&mu_);
  readable_registered_ = false;
  writable_registered_ = false;
  shutting_down_ = false;
  ares_socket_ = as;
}

FdNode::~FdNode() {
  GPR_ASSERT(!readable_registered_);
  GPR_ASSERT(!writable_registered_);
  GPR_ASSERT(shutting_down_);
  gpr_mu_destroy(&mu_);
}

ares_socket_t FdNode::GetInnerEndpoint() { return ares_socket_; }

void FdNode::Shutdown() {
  gpr_mu_lock(&mu_);
  if (!shutting_down_) {
    shutting_down_ = true;
    gpr_log(GPR_DEBUG, "shutdown ares_socket: %" PRIdPTR,
            (uintptr_t)ares_socket_);
    ShutdownInnerEndpointLocked();
  }
  gpr_mu_unlock(&mu_);
}

struct FdNodeEventArg {
  FdNodeEventArg(RefCountedPtr<FdNode> fdn,
                 RefCountedPtr<AresEvDriver> ev_driver)
      : fdn(fdn), ev_driver(ev_driver){};
  RefCountedPtr<FdNode> fdn;
  RefCountedPtr<AresEvDriver> ev_driver;
};

void FdNode::MaybeRegisterForReadsAndWrites(
    RefCountedPtr<AresEvDriver> ev_driver, int socks_bitmask, size_t idx) {
  gpr_mu_lock(&mu_);
  // Register read_closure if the socket is readable and read_closure has
  // not been registered with this socket.
  if (ARES_GETSOCK_READABLE(socks_bitmask, idx) && !readable_registered_) {
    GRPC_CLOSURE_INIT(&read_closure_, &FdNode::OnReadable,
                      grpc_core::New<FdNodeEventArg>(Ref(), ev_driver),
                      grpc_schedule_on_exec_ctx);
    RegisterForOnReadable();
    readable_registered_ = true;
  }
  // Register write_closure if the socket is writable and write_closure
  // has not been registered with this socket.
  if (ARES_GETSOCK_WRITABLE(socks_bitmask, idx) && !writable_registered_) {
    GRPC_CLOSURE_INIT(&write_closure_, &FdNode::OnWriteable,
                      grpc_core::New<FdNodeEventArg>(Ref(), ev_driver),
                      grpc_schedule_on_exec_ctx);
    RegisterForOnWriteable();
    writable_registered_ = true;
  }
  gpr_mu_unlock(&mu_);
}

void FdNode::OnReadable(void* arg, grpc_error* error) {
  UniquePtr<FdNodeEventArg> event_arg(reinterpret_cast<FdNodeEventArg*>(arg));
  event_arg->fdn->OnReadableInner(event_arg->ev_driver.get(), error);
}

void FdNode::OnWriteable(void* arg, grpc_error* error) {
  UniquePtr<FdNodeEventArg> event_arg(reinterpret_cast<FdNodeEventArg*>(arg));
  event_arg->fdn->OnWriteableInner(event_arg->ev_driver.get(), error);
}

void FdNode::OnReadableInner(AresEvDriver* ev_driver, grpc_error* error) {
  gpr_log(GPR_DEBUG, "readable on %" PRIdPTR, (uintptr_t)ares_socket_);
  gpr_mu_lock(&mu_);
  readable_registered_ = false;
  gpr_mu_unlock(&mu_);
  if (error == GRPC_ERROR_NONE) {
    do {
      ares_process_fd(ev_driver->GetChannel(), ares_socket_, ARES_SOCKET_BAD);
    } while (ShouldRepeatReadForAresProcessFd());
  } else {
    // If error is not GRPC_ERROR_NONE, it means the fd has been shutdown or
    // timed out. The pending lookups made on this ev_driver will be cancelled
    // by the following ares_cancel() and the on_done callbacks will be
    // invoked with a status of ARES_ECANCELLED. The remaining file
    // descriptors in this ev_driver will be cleaned up in the follwing
    // ev_driver->NotifyOnEvent().
    ares_cancel(ev_driver->GetChannel());
  }
  ev_driver->NotifyOnEvent();
}

void FdNode::OnWriteableInner(AresEvDriver* ev_driver, grpc_error* error) {
  gpr_log(GPR_DEBUG, "writable on %" PRIdPTR, (uintptr_t)ares_socket_);
  gpr_mu_lock(&mu_);
  writable_registered_ = false;
  gpr_mu_unlock(&mu_);
  if (error == GRPC_ERROR_NONE) {
    ares_process_fd(ev_driver->GetChannel(), ARES_SOCKET_BAD, ares_socket_);
  } else {
    // If error is not GRPC_ERROR_NONE, it means the fd has been shutdown or
    // timed out. The pending lookups made on this ev_driver will be cancelled
    // by the following ares_cancel() and the on_done callbacks will be
    // invoked with a status of ARES_ECANCELLED. The remaining file
    // descriptors in this ev_driver will be cleaned up in the follwing
    // grpc_ares_notify_on_event_locked().
    ares_cancel(ev_driver->GetChannel());
  }
  ev_driver->NotifyOnEvent();
}

AresEvDriver::AresEvDriver() : InternallyRefCounted() {
  gpr_mu_init(&mu_);
  // TODO: make list size unbounded?
  fds_ = UniquePtr<InlinedVector<RefCountedPtr<FdNode>, ARES_GETSOCK_MAXNUM>>(
      grpc_core::New<
          InlinedVector<RefCountedPtr<FdNode>, ARES_GETSOCK_MAXNUM>>());
  working_ = false;
  shutting_down_ = false;
}

AresEvDriver::~AresEvDriver() {
  gpr_mu_destroy(&mu_);
  ares_destroy(channel_);
}

void AresEvDriver::Start() {
  gpr_mu_lock(&mu_);
  if (!working_) {
    working_ = true;
    NotifyOnEventLocked();
  }
  gpr_mu_unlock(&mu_);
}

void AresEvDriver::Destroy() {
  // We mark the event driver
  // as being shut down. If the event driver is working,
  // grpc_ares_notify_on_event_locked will shut down the fds; if it's not
  // working, there are no fds to shut down.
  gpr_mu_lock(&mu_);
  shutting_down_ = true;
  gpr_mu_unlock(&mu_);
  Unref();
}

void AresEvDriver::Shutdown() {
  gpr_mu_lock(&mu_);
  gpr_log(GPR_DEBUG, "AresEvDriver::Shutdown is called");
  shutting_down_ = true;
  for (size_t i = 0; i < fds_->size(); i++) {
    (*fds_)[i]->Shutdown();
  }
  gpr_mu_unlock(&mu_);
}

ares_channel AresEvDriver::GetChannel() { return channel_; }
ares_channel* AresEvDriver::GetChannelPointer() { return &channel_; }

void AresEvDriver::NotifyOnEvent() {
  gpr_mu_lock(&mu_);
  NotifyOnEventLocked();
  gpr_mu_unlock(&mu_);
}

void AresEvDriver::NotifyOnEventLocked() {
  InlinedVector<RefCountedPtr<FdNode>, ARES_GETSOCK_MAXNUM> active;
  if (!shutting_down_) {
    ares_socket_t socks[ARES_GETSOCK_MAXNUM];
    int socks_bitmask = ares_getsock(channel_, socks, ARES_GETSOCK_MAXNUM);
    for (size_t i = 0; i < ARES_GETSOCK_MAXNUM; i++) {
      if (ARES_GETSOCK_READABLE(socks_bitmask, i) ||
          ARES_GETSOCK_WRITABLE(socks_bitmask, i)) {
        int existing_index = LookupFdNodeIndexLocked(socks[i]);
        // Create a new fd_node if sock[i] is not in the fd_node list.
        if (existing_index == -1) {
          gpr_log(GPR_DEBUG, "new fd: %d", socks[i]);
          char* fd_name;
          gpr_asprintf(&fd_name, "ares_ev_driver-%" PRIuPTR " socket:%" PRIuPTR, i, (uintptr_t)socks[i]);
          auto fdn = RefCountedPtr<FdNode>(CreateFdNode(socks[i], fd_name));
          gpr_free(fd_name);
	  fds_->push_back(fdn);
          active.push_back(fdn);
        } else {
          active.push_back((*fds_)[existing_index]);
        }
        active[active.size() - 1]->MaybeRegisterForReadsAndWrites(
            Ref(), socks_bitmask, i);
      }
    }
  }
  for (size_t i = 0; i < fds_->size(); i++) {
    bool still_active = false;
    for (size_t k = 0; k < active.size(); k++) {
      if ((*fds_)[i] == active[k]) {
        still_active = true; 
      }
    }
    if (!still_active) {
      // Shut down the fd but keep it in the list; keep it
      // recorded that this ev driver has worked on this fd.
      (*fds_)[i]->Shutdown();
    }
  }
  // If the ev driver has no working fd, all the tasks are done.
  if (active.size() == 0) {
    working_ = false;
    gpr_log(GPR_DEBUG, "ev driver stop working");
  }
}

int AresEvDriver::LookupFdNodeIndexLocked(ares_socket_t as) {
  for (size_t i = 0; i < fds_->size(); i++) {
    if ((*fds_)[i] != nullptr && (*fds_)[i]->GetInnerEndpoint() == as) {
      return i;
    }
  }
  return -1;
}

FdNode* AresEvDriver::LookupFdNode(ares_socket_t as) {
  gpr_mu_lock(&mu_);
  int index = LookupFdNodeIndexLocked(as);
  if (index == -1) {
    return nullptr;
  }
  FdNode* out = (*fds_)[index].get();
  gpr_mu_unlock(&mu_);
  return out;
}


grpc_error* AresEvDriver::CreateAndInitialize(AresEvDriver** ev_driver,
                                              grpc_pollset_set* pollset_set) {
  *ev_driver = AresEvDriver::Create(pollset_set);
  int status =
      ares_init(&(*ev_driver)->channel_);
  (*ev_driver)->MaybeOverrideSockFuncs((*ev_driver)->channel_);
  gpr_log(GPR_DEBUG, "grpc_ares_ev_driver_create:%" PRIdPTR,
          (uintptr_t)*ev_driver);
  if (status != ARES_SUCCESS) {
    char* err_msg;
    gpr_asprintf(&err_msg, "Failed to init ares channel. C-ares error: %s",
                 ares_strerror(status));
    grpc_error* err = GRPC_ERROR_CREATE_FROM_COPIED_STRING(err_msg);
    gpr_free(err_msg);
    Delete(*ev_driver);
    *ev_driver = nullptr;
    return err;
  }
  return GRPC_ERROR_NONE;
}

}  // namespace grpc_core

#endif /* GRPC_ARES == 1 && !defined(GRPC_UV) */
