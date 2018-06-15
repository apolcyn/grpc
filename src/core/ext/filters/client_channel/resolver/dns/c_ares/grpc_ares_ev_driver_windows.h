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
#ifndef GRPC_ARES_EV_DRIVER_WINDOWS_H
#define GRPC_ARES_EV_DRIVER_WINDOWS_H 1

#include <grpc/support/port_platform.h>

#include "src/core/lib/iomgr/port.h"
#if GRPC_ARES == 1 && defined(GPR_WINDOWS)

#include <ares.h>

#include "src/core/ext/filters/client_channel/resolver/dns/c_ares/grpc_ares_ev_driver.h"

#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>
#include <grpc/support/time.h>
#include "src/core/ext/filters/client_channel/resolver/dns/c_ares/grpc_ares_wrapper.h"
#include "src/core/lib/gpr/string.h"
#include "src/core/lib/iomgr/iomgr_internal.h"
#include "src/core/lib/iomgr/sockaddr_utils.h"
#include "src/core/lib/iomgr/socket_windows.h"

namespace grpc_core {

class AresEvDriverWindows;

class FdNodeWindows final : public FdNode {
 public:
  FdNodeWindows(grpc_winsocket* winsocket, ares_socket_t as);
  ~FdNodeWindows();
  static ares_ssize_t RecvFrom(ares_socket_t sock, void* data, size_t data_len, int flags, struct sockaddr* from, ares_socklen_t *from_len, void* user_data);
  ares_ssize_t RecvFromInner(ares_socket_t sock, void* data, size_t data_len, int flags, struct sockaddr* from, ares_socklen_t *from_len, void* user_data);
 private:
  static void OnIocpReadable(void *arg, grpc_error *error);
  void OnIocpReadableInner(void *arg, grpc_error *error);
  gpr_mu read_mu_;
  grpc_slice read_buf_;
  grpc_closure on_readable_outer_;
  grpc_winsocket* winsocket_;
};

}  // namespace grpc_core

#endif /* GRPC_ARES == 1 && defined(GPR_WINDOWS) */

#endif // GRPC_ARES_EV_DRIVER_WINDOWS_H
