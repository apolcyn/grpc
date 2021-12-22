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

#include <grpc/support/port_platform.h>

#include "src/core/lib/http/httpcli.h"

#include <string.h>

#include <string>

#include "absl/functional/bind_front.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"

#include <grpc/grpc.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>

#include "src/core/lib/address_utils/sockaddr_utils.h"
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/gpr/string.h"
#include "src/core/lib/gprpp/memory.h"
#include "src/core/lib/http/format_request.h"
#include "src/core/lib/http/parser.h"
#include "src/core/lib/iomgr/endpoint.h"
#include "src/core/lib/iomgr/iomgr_internal.h"
#include "src/core/lib/iomgr/resolve_address.h"
#include "src/core/lib/iomgr/tcp_client.h"
#include "src/core/lib/resource_quota/api.h"
#include "src/core/lib/slice/slice_internal.h"
#include "src/core/lib/transport/error_utils.h"


namespace grpc_core {

namespace {

grpc_httpcli_get_override g_get_override;
grpc_httpcli_post_override g_post_override;

}  // namespace

OrphanablePtr<HttpCliRequest> HttpCliRequest::Get(
      grpc_polling_entity* pollent, ResourceQuotaRefPtr resource_quota,
      const char* host, const char* ssl_override, const grpc_http_request* request,
      std::unique_ptr<HttpCliRequest::HandshakerFactory> handshaker_factory,
      grpc_millis deadline,
      grpc_closure* on_done,
      grpc_httpcli_response* response) {
  if (g_get_override && g_get_override(request, deadline, on_done, response)) {
    return nullptr;
  }
  std::string name =
      absl::StrFormat("HTTP:GET:%s:%s", request->host, request->http.path);
  return MakeOrphanable<HttpCliRequest>(
      grpc_httpcli_format_get_request(request), response,
      std::move(resource_quota), request->host, request->ssl_host_override,
      deadline,
      std::move(handshaker_factory),
      on_done, pollent, name.c_str());
}

OrphanablePtr<HttpCliRequest> HttpCliRequest::Post(
      grpc_polling_entity* pollent, ResourceQuotaRefPtr resource_quota,
      const char* host, const char* ssl_override, const grpc_http_request* request,
      std::unique_ptr<HttpCliRequest::HandshakerFactory> handshaker_factory,
      grpc_millis deadline,
      grpc_closure* on_done,
      grpc_httpcli_response* response) {
  if (g_post_override && g_post_override(request, body_bytes, body_size,
                                         deadline, on_done, response)) {
    return nullptr;
  }
  std::string name =
      absl::StrFormat("HTTP:POST:%s:%s", request->host, request->http.path);
  return MakeOrphanable<HttpCliRequest>(
      grpc_httpcli_format_post_request(request, body_bytes, body_size),
      response, std::move(resource_quota), request->host,
      request->ssl_host_override, deadline,
      std::move(handshaker_factory),
      on_done, pollent, name.c_str());
}

void HttpCliRequest::SetOverride(grpc_httpcli_get_override get,
                                 grpc_httpcli_post_override post) {
  g_get_override = get;
  g_post_override = post;
}

HttpCliRequest::HttpCliRequest(
    const grpc_slice& request_text, grpc_httpcli_response* response,
    ResourceQuotaRefPtr resource_quota, absl::string_view host,
    absl::string_view ssl_host_override, grpc_millis deadline,
    std::unique_ptr<HandshakerFactory> handshaker_factory, grpc_closure* on_done,
    grpc_polling_entity* pollent, const char* name)
    : handshaker_factory_(std::move(handshaker_factory)),
      resource_quota_(std::move(resource_quota)),
      host_(host),
      deadline_(deadline),
      handshaker_(handshaker),
      on_done_(on_done),
      pollset_set_(grpc_pollset_set_create()) {
  grpc_http_parser_init(&parser_, GRPC_HTTP_RESPONSE, response);
  grpc_slice_buffer_init(&incoming_);
  grpc_slice_buffer_init(&outgoing_);
  grpc_iomgr_register_object(&iomgr_obj_, name);

  GRPC_CLOSURE_INIT(&on_read_, OnRead, this, grpc_schedule_on_exec_ctx);
  GRPC_CLOSURE_INIT(&continue_on_read_after_schedule_on_exec_ctx_,
                    ContinueOnReadAfterScheduleOnExecCtx, this,
                    grpc_schedule_on_exec_ctx);
  GRPC_CLOSURE_INIT(&done_write_, DoneWrite, this, grpc_schedule_on_exec_ctx);
  GRPC_CLOSURE_INIT(&continue_done_write_after_schedule_on_exec_ctx_,
                    ContinueDoneWriteAfterScheduleOnExecCtx, this,
                    grpc_schedule_on_exec_ctx);
  GPR_ASSERT(pollent);
  grpc_polling_entity_add_to_pollset_set(pollent, pollset_set_);
  dns_request_ = GetDNSResolver()->ResolveName(
      host_.c_str(), handshaker_factory_->default_port(), pollset_set_,
      absl::BindFront(&HttpCliRequest::OnResolved, this));
}

HttpCliRequest::~HttpCliRequest() {
  grpc_http_parser_destroy(&parser_);
  if (ep_ != nullptr) {
    grpc_endpoint_destroy(ep_);
  }
  grpc_slice_unref_internal(request_text_);
  grpc_iomgr_unregister_object(&iomgr_obj_);
  grpc_slice_buffer_destroy_internal(&incoming_);
  grpc_slice_buffer_destroy_internal(&outgoing_);
  GRPC_ERROR_UNREF(overall_error_);
  grpc_pollset_set_destroy(pollset_set_);
}

void HttpCliRequest::Start() {
  grpc_core::MutexLock lock(&mu_);
  Ref().release();  // ref held by pending request
  dns_request_->Start();
}

void HttpCliRequest::Orphan() {
  {
    grpc_core::MutexLock lock(&mu_);
    cancelled_ = true;
    dns_request_.reset();  // cancel potentially pending DNS resolution
    if (own_endpoint_ && ep_ != nullptr) {
      grpc_endpoint_shutdown(
          ep_, GRPC_ERROR_CREATE_FROM_STATIC_STRING("HTTP request cancelled"));
    }
    handshaker_.reset();  // cancel potentially pending handshake
  }
  Unref();
}

void HttpCliRequest::AppendError(grpc_error_handle error)
    ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
  if (overall_error_ == GRPC_ERROR_NONE) {
    overall_error_ =
        GRPC_ERROR_CREATE_FROM_STATIC_STRING("Failed HTTP/1 client request");
  }
  const grpc_resolved_address* addr = &addresses_[next_address_ - 1];
  std::string addr_text = grpc_sockaddr_to_uri(addr);
  overall_error_ = grpc_error_add_child(
      overall_error_,
      grpc_error_set_str(error, GRPC_ERROR_STR_TARGET_ADDRESS, addr_text));
}

void HttpCliRequest::OnReadInternal(grpc_error_handle error)
    ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
  size_t i;

  for (i = 0; i < incoming_.count; i++) {
    if (GRPC_SLICE_LENGTH(incoming_.slices[i])) {
      have_read_byte_ = 1;
      grpc_error_handle err =
          grpc_http_parser_parse(&parser_, incoming_.slices[i], nullptr);
      if (err != GRPC_ERROR_NONE) {
        Finish(err);
        return;
      }
    }
  }

  if (error == GRPC_ERROR_NONE) {
    DoRead();
  } else if (!have_read_byte_) {
    NextAddress(GRPC_ERROR_REF(error));
  } else {
    Finish(grpc_http_parser_eof(&parser_));
  }
}

void HttpCliRequest::ContinueDoneWriteAfterScheduleOnExecCtx(
    void* arg, grpc_error_handle error) {
  HttpCliRequest* req = static_cast<HttpCliRequest*>(arg);
  grpc_core::MutexLock lock(&req->mu_);
  if (error == GRPC_ERROR_NONE) {
    req->OnWritten();
  } else {
    req->NextAddress(GRPC_ERROR_REF(error));
  }
}

void HttpCliRequest::StartWrite() ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
  grpc_slice_ref_internal(request_text_);
  grpc_slice_buffer_add(&outgoing_, request_text_);
  grpc_endpoint_write(ep_, &outgoing_, &done_write_, nullptr);
}

void HttpCliRequest::OnHandshakeDone(grpc_endpoint* ep) {
  grpc_core::MutexLock lock(mu_);
  own_endpoint_ = true;
  if (!ep) {
    NextAddress(
        GRPC_ERROR_CREATE_FROM_STATIC_STRING("Unexplained handshake failure"));
    return;
  }
  ep_ = ep;
  StartWrite();
}

void HttpCliRequest::OnConnected(void* arg, grpc_error_handle error) {
  HttpCliRequest* req = static_cast<HttpCliRequest*>(arg);
  grpc_endpoint* ep;
  const char* host;
  grpc_millis deadline;
  {
    grpc_core::MutexLock lock(&req->mu_);
    if (!req->ep_) {
      req->NextAddress(GRPC_ERROR_REF(error));
      return;
    }
    ep = req->ep_;
    req->handshaker_ = req->handshaker_factory_->CreateHandshaker(req->ep_, req->ssl_host_override_.empty() ? req_->host_, req_->ssl_host_override_, req->deadline_,
                                               absl::BindFront(&HttpCliRequest::OnHandshakeDone, req));
    req->handshaker_->Start();
  }
}

void HttpCliRequest::NextAddress(grpc_error_handle error)
    ABSL_EXCLUSIVE_LOCKS_REQUIRED(mu_) {
  if (error != GRPC_ERROR_NONE) {
    AppendError(error);
  }
  if (next_address_ == addresses_.size()) {
    Finish(GRPC_ERROR_CREATE_REFERENCING_FROM_STATIC_STRING(
        "Failed HTTP requests to all targets", &overall_error_, 1));
    return;
  }
  const grpc_resolved_address* addr = &addresses_[next_address_++];
  GRPC_CLOSURE_INIT(&connected_, OnConnected, this, grpc_schedule_on_exec_ctx);
  grpc_arg rq_arg = grpc_channel_arg_pointer_create(
      const_cast<char*>(GRPC_ARG_RESOURCE_QUOTA), resource_quota_->c_ptr(),
      grpc_resource_quota_arg_vtable());
  grpc_channel_args channel_args{1, &rq_arg};
  auto* args = CoreConfiguration::Get()
                   .channel_args_preconditioning()
                   .PreconditionChannelArgs(&channel_args);
  own_endpoint_ = false;
  grpc_tcp_client_connect(&connected_, &ep_, pollset_set_, args, addr,
                          deadline_);
  grpc_channel_args_destroy(args);
}

void HttpCliRequest::OnResolved(
    absl::StatusOr<std::vector<grpc_resolved_address>> addresses_or) {
  grpc_core::MutexLock lock(&mu_);
  dns_request_.reset();
  if (!addresses_or.ok()) {
    Finish(absl_status_to_grpc_error(addresses_or.status()));
    return;
  }
  if (cancelled_) {
    Finish(GRPC_ERROR_CREATE_FROM_STATIC_STRING(
        "cancelled during DNS resolution"));
    return;
  }
  addresses_ = std::move(*addresses_or);
  next_address_ = 0;
  NextAddress(GRPC_ERROR_NONE);
}

}  // namespace grpc_core
