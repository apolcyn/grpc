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
#if GRPC_ARES == 1 && defined(GRPC_POSIX_SOCKET)

#include <ares.h>
#include <memory.h>
#include <sys/ioctl.h>

#include "src/core/ext/filters/client_channel/resolver/dns/c_ares/grpc_ares_ev_driver.h"

#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>
#include <grpc/support/time.h>
#include "src/core/ext/filters/client_channel/resolver/dns/c_ares/grpc_ares_ev_driver.h"
#include "src/core/ext/filters/client_channel/resolver/dns/c_ares/grpc_ares_wrapper.h"
#include "src/core/lib/gpr/string.h"
#include "src/core/lib/iomgr/ev_posix.h"
#include "src/core/lib/iomgr/iomgr_internal.h"
#include "src/core/lib/iomgr/sockaddr_utils.h"

namespace grpc_core {

FdNode::FdNode(AresEvDriver* ev_driver) {
  gpr_log(GPR_DEBUG,
          "entering FdNode constructor. ev_driver:%" PRIdPTR ". this:%" PRIdPTR,
          (uintptr_t)ev_driver, (uintptr_t)this);
  ev_driver_ = ev_driver;
  gpr_mu_init(&mu_);
  readable_registered_ = false;
  writable_registered_ = false;
  shutting_down_ = false;
  GRPC_CLOSURE_INIT(&read_closure_, &FdNode::OnReadable, this,
                    grpc_schedule_on_exec_ctx);
  GRPC_CLOSURE_INIT(&write_closure_, &FdNode::OnWriteable, this,
                    grpc_schedule_on_exec_ctx);
}

void FdNode::MaybeRegisterForReadsAndWrites(int socks_bitmask, size_t idx) {
  gpr_mu_lock(&mu_);
  // Register read_closure if the socket is readable and read_closure has
  // not been registered with this socket.
  if (ARES_GETSOCK_READABLE(socks_bitmask, idx) && !readable_registered_) {
    gpr_log(GPR_DEBUG,
            "Socket readable. this:%" PRIdPTR ". ref ev_driver:%" PRIdPTR,
            (uintptr_t)this, (uintptr_t)ev_driver_);
    ev_driver_->Ref();
    ScheduleNotifyOnRead();
    readable_registered_ = true;
  }
  // Register write_closure if the socket is writable and write_closure
  // has not been registered with this socket.
  if (ARES_GETSOCK_WRITABLE(socks_bitmask, idx) && !writable_registered_) {
    gpr_log(GPR_DEBUG,
            "Socket writeable. this:%" PRIdPTR ". ref ev_driver:%" PRIdPTR,
            (uintptr_t)this, (uintptr_t)ev_driver_);
    ev_driver_->Ref();
    ScheduleNotifyOnWrite();
    writable_registered_ = true;
  }
  gpr_mu_unlock(&mu_);
}

void FdNode::Destroy(FdNode* fdn) {
  //  gpr_log(GPR_DEBUG, "delete fd: %d", grpc_fd_wrapped_fd(fdn->fd));
  GPR_ASSERT(!fdn->readable_registered_);
  GPR_ASSERT(!fdn->writable_registered_);
  gpr_mu_destroy(&fdn->mu_);
  fdn->DestroyInnerEndpoint();
  Delete(fdn);
}

void FdNode::Shutdown(FdNode* fdn) {
  gpr_mu_lock(&fdn->mu_);
  fdn->shutting_down_ = true;
  if (!fdn->readable_registered_ && !fdn->writable_registered_) {
    gpr_mu_unlock(&fdn->mu_);
    FdNode::Destroy(fdn);
  } else {
    fdn->ShutdownInnerEndpoint();
    gpr_mu_unlock(&fdn->mu_);
  }
}

void FdNode::OnReadable(void* arg, grpc_error* error) {
  FdNode* fdn = reinterpret_cast<FdNode*>(arg);
  if (!fdn->OnReadableInner(error)) {
    FdNode::Destroy(fdn);
  }
}

void FdNode::OnWriteable(void* arg, grpc_error* error) {
  FdNode* fdn = reinterpret_cast<FdNode*>(arg);
  if (!fdn->OnWriteableInner(error)) {
    FdNode::Destroy(fdn);
  }
}

bool FdNode::OnReadableInner(grpc_error* error) {
  gpr_mu_lock(&mu_);
  readable_registered_ = false;
  if (shutting_down_ && !writable_registered_) {
    gpr_mu_unlock(&mu_);
    ev_driver_->Unref();
    return false;
  }
  gpr_mu_unlock(&mu_);

  // GET THIS THERE  gpr_log(GPR_DEBUG, "readable on %d", fd);
  if (error == GRPC_ERROR_NONE) {
    do {
      ares_process_fd(ev_driver_->GetChannel(), GetInnerEndpoint(),
                      ARES_SOCKET_BAD);
    } while (IsInnerEndpointStillReadable());
  } else {
    // If error is not GRPC_ERROR_NONE, it means the fd has been shutdown or
    // timed out. The pending lookups made on this ev_driver will be cancelled
    // by the following ares_cancel() and the on_done callbacks will be
    // invoked with a status of ARES_ECANCELLED. The remaining file
    // descriptors in this ev_driver will be cleaned up in the follwing
    // grpc_ares_notify_on_event_locked().
    ares_cancel(ev_driver_->GetChannel());
  }
  ev_driver_->NotifyOnEvent();
  ev_driver_->Unref();
  return true;
}

bool FdNode::OnWriteableInner(grpc_error* error) {
  gpr_mu_lock(&mu_);
  writable_registered_ = false;
  if (shutting_down_ && !readable_registered_) {
    gpr_mu_unlock(&mu_);
    ev_driver_->Unref();
    return false;
  }
  gpr_mu_unlock(&mu_);

  //   GET THIS THERE  gpr_log(GPR_DEBUG, "writable on %d", fd);
  if (error == GRPC_ERROR_NONE) {
    ares_process_fd(ev_driver_->GetChannel(), ARES_SOCKET_BAD,
                    GetInnerEndpoint());
  } else {
    // If error is not GRPC_ERROR_NONE, it means the fd has been shutdown or
    // timed out. The pending lookups made on this ev_driver will be cancelled
    // by the following ares_cancel() and the on_done callbacks will be
    // invoked with a status of ARES_ECANCELLED. The remaining file
    // descriptors in this ev_driver will be cleaned up in the follwing
    // grpc_ares_notify_on_event_locked().
    ares_cancel(ev_driver_->GetChannel());
  }
  ev_driver_->NotifyOnEvent();
  ev_driver_->Unref();
  return true;
}

AresEvDriver::AresEvDriver() {
  gpr_log(GPR_DEBUG, "entering AresEvDriver constructor");
  gpr_mu_init(&mu_);
  gpr_ref_init(&refs_, 1);
  fds_ = nullptr;
  working_ = false;
  shutting_down_ = false;
}

void AresEvDriver::Start() {
  gpr_mu_lock(&mu_);
  if (!working_) {
    working_ = true;
    NotifyOnEventLocked();
  }
  gpr_mu_unlock(&mu_);
}

void AresEvDriver::Ref() {
  gpr_log(GPR_DEBUG, "Ref ev_driver %" PRIuPTR, (uintptr_t)this);
  gpr_ref(&refs_);
}

void AresEvDriver::Unref() {
  gpr_log(GPR_DEBUG, "Unref ev_driver %" PRIuPTR, (uintptr_t)this);
  if (gpr_unref(&refs_)) {
    gpr_log(GPR_DEBUG, "destroy ev_driver %" PRIuPTR, (uintptr_t)this);
    GPR_ASSERT(fds_ == nullptr);
    gpr_mu_destroy(&mu_);
    ares_destroy(channel_);
    // TODO: delete fix
  }
}

void AresEvDriver::Destroy() {
  // It's not safe to shut down remaining fds here directly, becauses
  // ares_host_callback does not provide an exec_ctx. We mark the event driver
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
  shutting_down_ = true;
  for (size_t i = 0; i < fds_->size(); i++) {
    (*fds_)[i]->ShutdownInnerEndpoint();
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
  UniquePtr<InlinedVector<FdNode*, ARES_GETSOCK_MAXNUM>> new_list(
      grpc_core::New<InlinedVector<FdNode*, ARES_GETSOCK_MAXNUM>>());
  if (!shutting_down_) {
    ares_socket_t socks[ARES_GETSOCK_MAXNUM];
    int socks_bitmask = ares_getsock(channel_, socks, ARES_GETSOCK_MAXNUM);
    for (size_t i = 0; i < ARES_GETSOCK_MAXNUM; i++) {
      if (ARES_GETSOCK_READABLE(socks_bitmask, i) ||
          ARES_GETSOCK_WRITABLE(socks_bitmask, i)) {
        int existing_index = LookupFdNodeIndex(socks[i]);
        // Create a new fd_node if sock[i] is not in the fd_node list.
        if (existing_index == -1) {
          gpr_log(GPR_DEBUG, "new fd: %d", socks[i]);
          char* fd_name;
          gpr_asprintf(&fd_name, "ares_ev_driver-%" PRIuPTR, i);
          auto fdn = CreateFdNode(socks[i], fd_name);
          gpr_free(fd_name);
          new_list->push_back(fdn);
        } else {
          new_list->push_back((*fds_)[existing_index]);
          (*fds_)[existing_index] = nullptr;
        }
        (*new_list)[new_list->size() - 1]->MaybeRegisterForReadsAndWrites(
            socks_bitmask, i);
      }
    }
  }
  // Any remaining fds in ev_driver->fds were not returned by ares_getsock()
  // and are therefore no longer in use, so they can be shut down and removed
  // from the list.
  for (size_t i = 0; i < fds_->size(); i++) {
    if ((*fds_)[i] != nullptr) {
      FdNode::Shutdown((*fds_)[i]);
    }
  }
  fds_ = std::move(new_list);
  // If the ev driver has no working fd, all the tasks are done.
  if (new_list->size() == 0) {
    working_ = false;
    gpr_log(GPR_DEBUG, "ev driver stop working");
  }
}

// Search fd in the fd_node list head. This is an O(n) search, the max
// possible value of n is ARES_GETSOCK_MAXNUM (16). n is typically 1 - 2 in
// our tests.
int AresEvDriver::LookupFdNodeIndex(ares_socket_t as) {
  for (size_t i = 0; i < fds_->size(); i++) {
    if ((*fds_)[i]->GetInnerEndpoint() == as) {
      return i;
    }
  }
  return -1;
}

grpc_error* AresEvDriver::CreateAndInitialize(AresEvDriver** ev_driver,
                                              grpc_pollset_set* pollset_set) {
  *ev_driver = AresEvDriver::Create(pollset_set);
  gpr_ref_init(&(*ev_driver)->refs_, 1);
  int status = ares_init(&(*ev_driver)->channel_);
  gpr_log(GPR_DEBUG, "grpc_ares_ev_driver_create:%" PRIdPTR,
          (uintptr_t)*ev_driver);
  if (status != ARES_SUCCESS) {
    char* err_msg;
    gpr_asprintf(&err_msg, "Failed to init ares channel. C-ares error: %s",
                 ares_strerror(status));
    grpc_error* err = GRPC_ERROR_CREATE_FROM_COPIED_STRING(err_msg);
    gpr_free(err_msg);
    // TODO: delete *ev_driver;
    return err;
  }
  return GRPC_ERROR_NONE;
}

}  // namespace grpc_core

#endif /* GRPC_ARES == 1 && defined(GRPC_POSIX_SOCKET) */
