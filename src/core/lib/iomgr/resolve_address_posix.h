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

#ifndef GRPC_CORE_LIB_IOMGR_RESOLVE_ADDRESS_POSIX_H
#define GRPC_CORE_LIB_IOMGR_RESOLVE_ADDRESS_POSIX_H

#include <grpc/support/port_platform.h>

#include <string.h>
#include <sys/types.h>

#include <functional>

#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>
#include <grpc/support/time.h>

#include "src/core/lib/gpr/string.h"
#include "src/core/lib/gpr/useful.h"
#include "src/core/lib/gprpp/host_port.h"
#include "src/core/lib/gprpp/thd.h"
#include "src/core/lib/iomgr/block_annotate.h"
#include "src/core/lib/iomgr/executor.h"
#include "src/core/lib/iomgr/iomgr_internal.h"
#include "src/core/lib/iomgr/port.h"
#include "src/core/lib/iomgr/resolve_address.h"
#include "src/core/lib/iomgr/sockaddr.h"
#include "src/core/lib/iomgr/unix_sockets_posix.h"

namespace grpc_core {

class NativeDNSResolver : public DNSResolver {
 public:
  // Gets the singleton instance, creating it first if it doesn't exist
  static NativeDNSResolver* GetOrCreate();

  OrphanablePtr<DNSRequest> CreateDNSRequest(
      absl::string_view name, absl::string_view default_port,
      grpc_pollset_set* /* interested_parties */,
      std::function<void(absl::StatusOr<grpc_resolved_addresses*>)> on_done)
      override;

  absl::StatusOr<grpc_resolved_addresses*> BlockingResolveAddress(
      absl::string_view name, absl::string_view default_port) override;
};

}  // namespace grpc_core

#endif  // GRPC_CORE_LIB_IOMGR_RESOLVE_ADDRESS_POSIX_H
