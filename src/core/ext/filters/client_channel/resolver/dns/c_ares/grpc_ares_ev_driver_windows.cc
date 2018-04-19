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
#if GRPC_ARES == 1 && defined(GPR_WINDOWS)

#include <ares.h>
#include <string.h>
#include <sys/ioctl.h>

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

class AresEvDriverWindows;

class FdNodeWindows final : public FdNode {
 public:
  FdNodeWindows(AresEvDriver* ev_driver, grpc_winsocket* winsocket) : FdNode(ev_driver) {
    winsocket_ = winsocket;
  }

 private:
  void DestroyInnerEndpoint() override {
    // NOOP for windows
  }

  void ShutdownInnerEndpoint() override {
    grpc_winsocket_shutdown(fn->grpc_winsocket);
  }

  ares_socket_t GetInnerEndpoint() override { return grpc_winsocket_wrapped_socket(winsocket_); }

  bool IsInnerEndpointStillReadable() override {
    SOCKET winsocket = grpc_winsocket_wrapped_socket(winsocket_);
    size_t bytes_available = 0;
    return WSAIoctl(winsocket, FIONREAD, &bytes_available) == 0 && bytes_available > 0;
  }

  void ScheduleNotifyOnRead() override {
    GRPC_CLOSURE_SCHED(&read_closure_, GRPC_ERROR_NONE);
  }
  void ScheduleNotifyOnWrite() override {
    GRPC_CLOSURE_SCHED(&write_closure_, GRPC_ERROR_NONE);
  }

 private:
  grpc_winsocket* winsocket_;
};

class AresEvDriverWindows final : public AresEvDriver {
 public:
  AresEvDriverWindows() : AresEvDriver() {
  }

  ~AresEvDriverWindows() {
    gpr_log(GPR_DEBUG, "AresEvDriverWindows destructor called");
  }

 private:
  FdNode* CreateFdNode(ares_socket_t as, const char* name) override {
    grpc_winsocket* winsocket = grpc_winsocket_create(as, name);
    // Note that we don't add the socket to a pollset because this
    // c-ares resolver doesn't use one on windows, and instead we
    // rely on our own busyloop.
    return grpc_core::New<FdNodeWindows>(this, fd);
  }
};

AresEvDriver* AresEvDriver::Create(grpc_pollset_set* pollset_set) {
  (void*)pollset_set;
  return grpc_core::New<AresEvDriverWindows>();
}

}  // namespace grpc_core

#endif /* GRPC_ARES == 1 && defined(GPR_WINDOWS) */
