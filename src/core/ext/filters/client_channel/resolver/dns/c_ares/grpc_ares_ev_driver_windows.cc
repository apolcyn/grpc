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
#include <ares_writev.h>

#include "src/core/ext/filters/client_channel/resolver/dns/c_ares/grpc_ares_ev_driver.h"

#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>
#include <grpc/support/time.h>
#include <grpc/support/log_windows.h>
#include "src/core/ext/filters/client_channel/resolver/dns/c_ares/grpc_ares_wrapper.h"
#include "src/core/lib/gpr/string.h"
#include "src/core/lib/iomgr/iomgr_internal.h"
#include "src/core/lib/iomgr/sockaddr_utils.h"
#include "src/core/lib/iomgr/socket_windows.h"

namespace grpc_core {

class AresEvDriverWindows;

class FdNodeWindows final : public FdNode {
 public:
  FdNodeWindows(grpc_winsocket* winsocket, ares_socket_t as) : FdNode(as) {
    winsocket_ = winsocket;
    gpr_mu_init(&read_mu_);
    GRPC_CLOSURE_INIT(&on_readable_outer_, &FdNodeWindows::OnIocpReadable, this, grpc_schedule_on_exec_ctx);
    read_buf_ = grpc_empty_slice();
  }
  
  ~FdNodeWindows() {
    grpc_winsocket_destroy(winsocket_);
    grpc_slice_unref(read_buf_);
  }
  
  static ares_ssize_t RecvFrom(ares_socket_t sock, void* data, size_t data_len, int flags, struct sockaddr* from, ares_socklen_t *from_len, void* user_data) {
    AresEvDriver *ev_driver = reinterpret_cast<AresEvDriver*>(user_data);
    gpr_log(GPR_DEBUG, "custom_recvfrom called on socket %" PRIdPTR ". data_len: %" PRIdPTR, (uintptr_t)sock, data_len);
    FdNode *lookup_result = ev_driver->LookupFdNode(sock);
    if (lookup_result == nullptr) {
      gpr_log(GPR_DEBUG, "socket %" PRIdPTR " not yet in driver's list");
      WSASetLastError(EWOULDBLOCK);
    }
    FdNodeWindows *fdn = reinterpret_cast<FdNodeWindows*>(lookup_result);
    return fdn->RecvFromInner(sock, data, data_len, flags, from, from_len, user_data);
  }
  
  ares_ssize_t RecvFromInner(ares_socket_t sock, void* data, size_t data_len, int flags, struct sockaddr* from, ares_socklen_t *from_len, void* user_data) {
    gpr_mu_lock(&read_mu_);
    ares_ssize_t bytes_read = 0;
    if (GRPC_SLICE_LENGTH(read_buf_) > 0) {
      gpr_log(GPR_DEBUG, "RecvFromInner for socket %" PRIdPTR ": returning data length %" PRIdPTR, (uintptr_t)sock, GRPC_SLICE_LENGTH(read_buf_));
      for (size_t i = 0; i < data_len && i < GRPC_SLICE_LENGTH(read_buf_); i++) {
        ((char*)data)[i] = GRPC_SLICE_START_PTR(read_buf_)[i];
        bytes_read++;
      }
      read_buf_ = grpc_slice_sub_no_ref(read_buf_, bytes_read, GRPC_SLICE_LENGTH(read_buf_));
      GPR_ASSERT(*from_len < sizeof(recvfrom_source_addr_));
      memcpy(from, &recvfrom_source_addr_, *from_len);
      *from_len = recvfrom_source_addr_len_;
      gpr_log(GPR_DEBUG, "RecvFromInner for socket %" PRIdPTR ": bytes read: %" PRIdPTR, (uintptr_t)sock, bytes_read);
      gpr_log(GPR_DEBUG, "RecvFromInner for socket %" PRIdPTR ": read_buf_ len now: %" PRIdPTR, (uintptr_t)sock, GRPC_SLICE_LENGTH(read_buf_));
    } else {
      gpr_log(GPR_DEBUG, "RecvFromInner for socket %" PRIdPTR ": no data available", (uintptr_t)sock);
      bytes_read = -1;
      WSASetLastError(EWOULDBLOCK);
    }
    gpr_mu_unlock(&read_mu_);
    return bytes_read;
  }

  static ares_ssize_t SendV(ares_socket_t as, const struct iovec *iov, int iov_count, void *user_data) {
    AresEvDriver *ev_driver = reinterpret_cast<AresEvDriver*>(user_data);
    gpr_log(GPR_DEBUG, "custom Sendv called on socket %" PRIdPTR ". iov_count: %" PRIdPTR, (uintptr_t)as, iov_count);
    // TODO: implement async writes.
    int total = 0;
    for (int i = 0; i < iov_count; i++) {
      total += iov[i].iov_len;
    }
    grpc_slice buf = GRPC_SLICE_MALLOC(total);
    size_t cur = 0;
    for (int i = 0; i < iov_count; i++) {
      for (int k = 0; k < iov[i].iov_len; k++) {
        GRPC_SLICE_START_PTR(buf)[cur++] = ((char*)iov[i].iov_base)[k];
      }
    }
    int num_written = send((SOCKET)as, (const char*)GRPC_SLICE_START_PTR(buf), total, 0);
    if (num_written != total) {
      gpr_log(GPR_DEBUG, "socket %" PRIdPTR ". writev wrote %" PRIdPTR "/%" PRIdPTR "bytes. TODO: handle async writes", num_written, total);
      abort();
    }
  }

  static ares_socket_t Socket(int af, int type, int protocol, void* user_data) {
    return WSASocket(af, type, protocol, nullptr, 0, WSA_FLAG_OVERLAPPED);
  }

  static int CloseSocket(ares_socket_t as, void* user_data) {
    return closesocket((SOCKET)as);
  }

  static int Connect(ares_socket_t as, const struct sockaddr* target, ares_socklen_t target_len, void* user_data) {
    return WSAConnect((SOCKET)as, target, target_len, nullptr, nullptr, nullptr, nullptr);
  }
  
  void ShutdownInnerEndpointLocked() override {
    gpr_log(GPR_DEBUG, "ShutdownInnerEndpointLocked is called.");
    grpc_winsocket_shutdown_without_close(winsocket_);
  }

private:
  bool ShouldRepeatReadForAresProcessFd() override {
    return GRPC_SLICE_LENGTH(read_buf_) > 0;
  }
  
  void RegisterForOnReadable() override {
    SOCKET wrapped_socket = grpc_winsocket_wrapped_socket(winsocket_);
    gpr_log(GPR_DEBUG, "notify read on %" PRIdPTR, (uintptr_t)wrapped_socket);
    WSABUF buffer;
    GPR_ASSERT(GRPC_SLICE_LENGTH(read_buf_) == 0);
    grpc_slice_unref(read_buf_);
    read_buf_ = GRPC_SLICE_MALLOC(8192);
    buffer.buf= (char*)GRPC_SLICE_START_PTR(read_buf_);
    buffer.len = GRPC_SLICE_LENGTH(read_buf_);
    memset(&winsocket_->read_info.overlapped, 0, sizeof(OVERLAPPED));
    DWORD flags = 0;
    memset(&recvfrom_source_addr_, 0, sizeof(recvfrom_source_addr_));
    recvfrom_source_addr_len_ = sizeof(recvfrom_source_addr_);
    if (WSARecvFrom(wrapped_socket,
                    &buffer,
		    1,
		    nullptr,
		    &flags,
		    (sockaddr*)&recvfrom_source_addr_,
		    &recvfrom_source_addr_len_,
		    &winsocket_->read_info.overlapped,
		    nullptr) != 0) {
      gpr_log(GPR_DEBUG, "Error registering async read on %" PRIdPTR ". error %" PRIdPTR ": %s",
              (uintptr_t)wrapped_socket, WSAGetLastError(), gpr_format_message(WSAGetLastError()));
      if (WSAGetLastError() != ERROR_IO_PENDING) {
        abort(); // TODO: remove this abort, replace with error
      }
    }
    grpc_socket_notify_on_read(winsocket_, &on_readable_outer_);
  }
  
  void RegisterForOnWriteable() override {
    GRPC_CLOSURE_SCHED(&write_closure_, GRPC_ERROR_NONE);
  }
  
  static void OnIocpReadable(void *arg, grpc_error *error) {
    FdNodeWindows* fdn = reinterpret_cast<FdNodeWindows*>(arg);
    fdn->OnIocpReadableInner(error);
  }
  
  void OnIocpReadableInner(grpc_error *error) {
    if (error == GRPC_ERROR_NONE) {
      if (winsocket_->read_info.wsa_error != 0) {
        char* msg = gpr_format_message(winsocket_->read_info.wsa_error);
        error = GRPC_ERROR_CREATE_FROM_COPIED_STRING(msg);
      } else {
        gpr_log(GPR_DEBUG, "iocp on readable inner called: bytes transfered: %d", winsocket_->read_info.bytes_transfered);
        read_buf_ = grpc_slice_sub_no_ref(read_buf_, 0, winsocket_->read_info.bytes_transfered);
        gpr_log(GPR_DEBUG, "iocp on readable inner called: recvd buf len now: %d", GRPC_SLICE_LENGTH(read_buf_));
      }
    }
    if (error != GRPC_ERROR_NONE) {
      gpr_log(GPR_DEBUG, "iocpreadableinner error occurred");
      grpc_slice_unref(read_buf_);
      read_buf_ = grpc_empty_slice();
    }
    GRPC_CLOSURE_SCHED(&read_closure_, error);
  }

  char recvfrom_source_addr_[200];
  ares_socklen_t recvfrom_source_addr_len_;
  gpr_mu read_mu_;
  grpc_slice read_buf_;
  grpc_closure on_readable_outer_;
  grpc_winsocket* winsocket_;
};

struct ares_socket_functions custom_ares_sock_funcs = {
	&FdNodeWindows::Socket /* socket */,
	&FdNodeWindows::CloseSocket /* close */, 
	&FdNodeWindows::Connect /* connect */, 
        &FdNodeWindows::RecvFrom /* recvfrom */, 
	&FdNodeWindows::SendV /* sendv */,
};

class AresEvDriverWindows final : public AresEvDriver {
 public:
  AresEvDriverWindows() : AresEvDriver() {}

 private:
  FdNode* CreateFdNode(ares_socket_t as, const char* name) override {
    grpc_winsocket* winsocket = grpc_winsocket_create((SOCKET)as, name);
    return grpc_core::New<FdNodeWindows>(winsocket, as);
  }
  void MaybeOverrideSockFuncs(ares_channel chan) override {
    ares_set_socket_functions(chan, &custom_ares_sock_funcs, this);
  }
};

AresEvDriver* AresEvDriver::Create(grpc_pollset_set* pollset_set) {
  // Don't care about pollset_set on windows since poller is global
  return grpc_core::New<AresEvDriverWindows>();
}

}  // namespace grpc_core

#endif /* GRPC_ARES == 1 && defined(GPR_WINDOWS) */
