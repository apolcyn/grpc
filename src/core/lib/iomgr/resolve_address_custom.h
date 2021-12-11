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

#ifndef GRPC_CORE_LIB_IOMGR_RESOLVE_ADDRESS_CUSTOM_H
#define GRPC_CORE_LIB_IOMGR_RESOLVE_ADDRESS_CUSTOM_H

#include <grpc/support/port_platform.h>

#include <grpc/support/sync.h>

#include "src/core/lib/iomgr/port.h"
#include "src/core/lib/iomgr/resolve_address.h"
#include "src/core/lib/iomgr/sockaddr.h"

typedef struct grpc_custom_resolver_vtable grpc_custom_resolver_vtable;

namespace grpc_core {

class CustomDNSRequest : public DNSRequest {
 public:
  CustomDNSRequest(
      absl::string_view name, absl::string_view default_port,
      std::function<void(absl::StatusOr<grpc_resolved_addresses*>)> on_done,
      const grpc_custom_resolver_vtable* resolve_address_vtable)
      : name_(name),
        default_port_(default_port),
        on_done_(std::move(on_done)),
        resolve_address_vtable_(resolve_address_vtable) {}

  // Starts the resolution
  void Start() override;

  // This is a no-op for the native resolver. Note
  // that no I/O polling is required for the resolution to finish.
  void Orphan() override { Unref(); }

  // Implementations of grpc_custom_resolver_vtables must invoke this method
  // with the results of resolve_async.
  void ResolveCallback(grpc_resolved_addresses* result,
                       grpc_error_handle error);

 private:
  const std::string name_;
  const std::string default_port_;
  std::string host_;
  std::string port_;
  std::function<void(absl::StatusOr<grpc_resolved_addresses*>)> on_done_;
  // user-defined DNS methods
  const grpc_custom_resolver_vtable* resolve_address_vtable_ = nullptr;
};

class CustomDNSResolver : public DNSResolver {
 public:
  explicit CustomDNSResolver(
      grpc_custom_resolver_vtable* resolve_address_vtable)
      : resolve_address_vtable_(resolve_address_vtable) {}

  // Gets the singleton instance, creating it if it hasn't been already.
  static CustomDNSResolver* GetOrCreate(
      grpc_custom_resolver_vtable* resolve_address_vtable);

  OrphanablePtr<DNSRequest> ResolveName(
      absl::string_view name, absl::string_view default_port,
      grpc_pollset_set* /* interested_parties */,
      std::function<void(absl::StatusOr<grpc_resolved_addresses*>)> on_done)
      override {
    return MakeOrphanable<CustomDNSRequest>(
        name, default_port, std::move(on_done), resolve_address_vtable_);
  }

  absl::StatusOr<grpc_resolved_addresses*> ResolveNameBlocking(
      absl::string_view name, absl::string_view default_port) override;

 private:
  static CustomDNSResolver* instance_;

  // user-defined DNS methods
  const grpc_custom_resolver_vtable* resolve_address_vtable_;
};

}  // namespace grpc_core

/* user-configured DNS resolution functions */
typedef struct grpc_custom_resolver_vtable {
  grpc_error_handle (*resolve)(const char* host, const char* port,
                               grpc_resolved_addresses** res);
  void (*resolve_async)(grpc_core::CustomDNSRequest* request, const char* host,
                        const char* port);
} grpc_custom_resolver_vtable;

#endif /* GRPC_CORE_LIB_IOMGR_RESOLVE_ADDRESS_CUSTOM_H */
