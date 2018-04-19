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
#include <sys/ioctl.h>
#include <string.h>

#include "src/core/ext/filters/client_channel/resolver/dns/c_ares/grpc_ares_ev_driver.h"

#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>
#include <grpc/support/time.h>
#include "src/core/ext/filters/client_channel/resolver/dns/c_ares/grpc_ares_wrapper.h"
#include "src/core/lib/gpr/string.h"
#include "src/core/lib/iomgr/ev_posix.h"
#include "src/core/lib/iomgr/iomgr_internal.h"
#include "src/core/lib/iomgr/sockaddr_utils.h"

namespace grpc_core {

class AresEvDriverPosix;

class FdNodePosix final : public FdNode {
 public:
  FdNodePosix(AresEvDriver* ev_driver) : FdNode(ev_driver) {
    pollset_set_ = nullptr;
    fd_ = nullptr;
  }

  void DestroyInnerEndpoint() override {
    /* c-ares library has closed the fd inside grpc_fd. This fd may be picked up
       immediately by another thread, and should not be closed by the following
       grpc_fd_orphan. */
    grpc_fd_orphan(fd_, nullptr, nullptr, true /* already_closed */,
                   "c-ares query finished");
  }

  void ShutdownInnerEndpoint() override {
    grpc_fd_shutdown(
        fd_, GRPC_ERROR_CREATE_FROM_STATIC_STRING("c-ares fd shutdown"));
  }

  ares_socket_t GetInnerEndpoint() override { return grpc_fd_wrapped_fd(fd_); }

  bool IsInnerEndpointStillReadable() override {
    const int fd = grpc_fd_wrapped_fd(fd_);
    size_t bytes_available = 0;
    return ioctl(fd, FIONREAD, &bytes_available) == 0 && bytes_available > 0;
  }

  void AttachInnerEndpoint(const ares_socket_t& as, const char* name) override {
    fd_ = grpc_fd_create(as, name);
  }
  void ScheduleNotifyOnRead() override {
    gpr_log(GPR_DEBUG, "notify read on: %d", grpc_fd_wrapped_fd(fd_));
    grpc_fd_notify_on_read(fd_, &read_closure_);
  }
  void ScheduleNotifyOnWrite() override {
    gpr_log(GPR_DEBUG, "notify read on: %d", grpc_fd_wrapped_fd(fd_));
    grpc_fd_notify_on_write(fd_, &write_closure_);
  }

  grpc_fd* GetGrpcFd() {
    return fd_;
  }

 private:
  grpc_pollset_set* pollset_set_;
  grpc_fd* fd_;
};

class AresEvDriverPosix final : public AresEvDriver {
 public:
  AresEvDriverPosix(grpc_pollset_set* pollset_set) : AresEvDriver() {
    pollset_set_ = pollset_set;
  }
  ~AresEvDriverPosix() {
    gpr_log(GPR_DEBUG, "AresEvDriverPosix destructor called");
  }
  void AddInnerEndpointToPollsetSet(FdNode* fdn) override {
    grpc_pollset_set_add_fd(pollset_set_,
                            reinterpret_cast<FdNodePosix*>(fdn)->GetGrpcFd());
  }
  FdNode* CreateFdNode() override {
    return grpc_core::New<FdNodePosix>(this);
  }

 private:
  grpc_pollset_set* pollset_set_;
};

grpc_error* AresEvDriver::Create(AresEvDriver **ev_driver, grpc_pollset_set* pollset_set) {
  *ev_driver = grpc_core::New<AresEvDriverPosix>(pollset_set);
  gpr_ref_init(&(*ev_driver)->refs_, 1);
  int status = ares_init(&(*ev_driver)->channel_);
  gpr_log(GPR_DEBUG, "grpc_ares_ev_driver_create:%" PRIdPTR, (uintptr_t)*ev_driver);
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
