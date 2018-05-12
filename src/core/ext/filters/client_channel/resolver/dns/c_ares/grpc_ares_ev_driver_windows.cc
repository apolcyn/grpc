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
#include "src/core/lib/iomgr/tcp_windows.h"

namespace grpc_core {

// Max UDP datagram size we can read.
// This needs to be large enough to cover the largest UDP response
// that both a server will send and that c-ares is willing to accept
// (4K should be generous for that).
// It's find for TCP responses to be larger than this.
int kOurMaxUdpResponseSize = 540;

class AresEvDriverWindows;

class FdNodeWindows final : public FdNode {
 public:
  FdNodeWindows(grpc_winsocket* winsocket, ares_socket_t as) : FdNode(as) {
    winsocket_ = winsocket;
    GRPC_CLOSURE_INIT(&on_readable_outer_, &FdNodeWindows::OnIocpReadable, this, grpc_schedule_on_exec_ctx);
    GRPC_CLOSURE_INIT(&on_writeable_outer_, &FdNodeWindows::OnIocpWriteable, this, grpc_schedule_on_exec_ctx);
    read_buf_ = grpc_empty_slice();
    write_buf_ = grpc_empty_slice();
    connected_ = false;
  }
  
  ~FdNodeWindows() {
    gpr_log(GPR_DEBUG, "FD NODE WINDOWS DESTRUCTOR IS CALLED");
    grpc_winsocket_destroy(winsocket_);
    grpc_slice_unref(read_buf_);
    grpc_slice_unref(write_buf_);
  }
  
  static ares_ssize_t RecvFrom(ares_socket_t sock, void* data, size_t data_len, int flags, struct sockaddr* from, ares_socklen_t *from_len, void* user_data) {
    AresEvDriver *ev_driver = reinterpret_cast<AresEvDriver*>(user_data);
    gpr_log(GPR_DEBUG, "custom_recvfrom called on socket %" PRIdPTR ". data_len: %" PRIdPTR, (uintptr_t)sock, data_len);
    FdNode *lookup_result = ev_driver->LookupFdNode(sock);
    if (lookup_result == nullptr) {
      gpr_log(GPR_DEBUG, "Unexpected: socket %" PRIdPTR " not yet in driver's list");
      abort();
    }
    FdNodeWindows *fdn = reinterpret_cast<FdNodeWindows*>(lookup_result);
    return fdn->RecvFromInner(data, data_len, flags, from, from_len);
  }
  
  ares_ssize_t RecvFromInner(void* data, size_t data_len, int flags, struct sockaddr* from, ares_socklen_t *from_len) {
    gpr_mu_lock(&mu_);
    SOCKET sock = grpc_winsocket_wrapped_socket(winsocket_);
    ares_ssize_t bytes_read = 0;
    if (GRPC_SLICE_LENGTH(read_buf_) > 0) {
      gpr_log(GPR_DEBUG, "RecvFromInner for socket %" PRIdPTR ": returning data length %" PRIdPTR, (uintptr_t)sock, GRPC_SLICE_LENGTH(read_buf_));
      for (size_t i = 0; i < data_len && i < GRPC_SLICE_LENGTH(read_buf_); i++) {
        ((char*)data)[i] = GRPC_SLICE_START_PTR(read_buf_)[i];
        bytes_read++;
      }
      read_buf_ = grpc_slice_sub_no_ref(read_buf_, bytes_read, GRPC_SLICE_LENGTH(read_buf_));
      // c-ares sets "from" to NULL for TCP reads.
      if (from != nullptr) {
        GPR_ASSERT(*from_len <= recvfrom_source_addr_len_);
        memcpy(from, &recvfrom_source_addr_, recvfrom_source_addr_len_);
        *from_len = recvfrom_source_addr_len_;
      }
      gpr_log(GPR_DEBUG, "RecvFromInner for socket %" PRIdPTR ": bytes read: %" PRIdPTR, (uintptr_t)sock, bytes_read);
      gpr_log(GPR_DEBUG, "RecvFromInner for socket %" PRIdPTR ": read_buf_ len now: %" PRIdPTR, (uintptr_t)sock, GRPC_SLICE_LENGTH(read_buf_));
    } else {
      gpr_log(GPR_DEBUG, "RecvFromInner for socket %" PRIdPTR ": no data available", (uintptr_t)sock);
      bytes_read = -1;
      WSASetLastError(EWOULDBLOCK);
    }
    gpr_mu_unlock(&mu_);
    return bytes_read;
  }

  static ares_ssize_t SendV(ares_socket_t as, const struct iovec *iov, int iov_count, void *user_data) {
    gpr_log(GPR_DEBUG, "custom Sendv called on socket %" PRIdPTR ". iov_count: %" PRIdPTR, (uintptr_t)as, iov_count);
    AresEvDriver *ev_driver = reinterpret_cast<AresEvDriver*>(user_data);
    FdNode *lookup_result = ev_driver->LookupFdNode(as);
    // The fact that we make "connect" succeed asynchronously guarantees that the event driver
    // knows about this socket by the time c-ares tries to write to it.
    GPR_ASSERT(lookup_result != nullptr);
    return lookup_result->SendVInner(iov, iov_count);
  }

  ares_ssize_t SendVInner(const struct iovec *iov, int iov_count) {
    gpr_mu_lock(&mu_);
    int total = 0;
    for (int i = 0; i < iov_count; i++) {
      total += iov[i].iov_len;
    }
    GPR_ASSERT(GRPC_SLICE_LENGTH(write_buf_) == 0);
    grpc_slice_unref(write_buf_);
    write_buf_ = GRPC_SLICE_MALLOC(total);
    size_t cur = 0;
    for (int i = 0; i < iov_count; i++) {
      for (int k = 0; k < iov[i].iov_len; k++) {
        GRPC_SLICE_START_PTR(write_buf_)[cur++] = ((char*)iov[i].iov_base)[k];
      }
    }
    WSABUF buf;
    buf.len = GRPC_SLICE_LENGTH(write_buf_);
    buf.buffer = GRPC_SLICE_START_PTR(write_buf_);
    LPDWORD bytes_sent;
    DWORD flags = 0;
    if (WSASend(grpc_winsocket_wrapped_socket(winsocket_), &buf, 1, &bytes_sent, flags, nullptr, nullptr) == 0) {
      GPR_ASSERT(bytes_sent == GRPC_SLICE_LENGTH(write_buf_));
      grpc_slice_unref(write_buf_);
      write_buf_ = grpc_empty_slice();
      gpr_mu_unlock(&mu_);
      return;
    }
    gpr_log(GPR_DEBUG, "TODO: handle case when socket is not immediately writeable. Last error code: %" PRIdPTR ". message: %s", WSAGetLastError(), gpr_format_message(WSAGetLastError()));
    abort();
  }

  static ares_socket_t Socket(int af, int type, int protocol, void* user_data) {
    SOCKET s = WSASocket(af, type, protocol, nullptr, 0, WSA_FLAG_OVERLAPPED);
    // TODO: this is abuse of tcp_prepare_socket (socket might be TCP but is likely UDP)
    grpc_tcp_prepare_socket(s);
    return s;
  }

  static int CloseSocket(ares_socket_t as, void* user_data) {
    AresEvDriver *ev_driver = reinterpret_cast<AresEvDriver*>(user_data);
    FdNode *lookup_result = ev_driver->LookupFdNode(as);
    if (lookup_result == nullptr) {
      gpr_log(GPR_DEBUG, "Closing socket %" PRIdPTR " without having worked on it.");
      return closesocket((SOCKET)as);
    }
    // Don't close the socket here if it ever made it into the driver's list.
    // (ShutdownInnerEndpointLocked is responsible for closing it in that case).
    return 0;
  }

  static int Connect(ares_socket_t as, const struct sockaddr* target, ares_socklen_t target_len, void* user_data) {
    AresEvDriver *ev_driver = reinterpret_cast<AresEvDriver*>(user_data);
    FdNode *lookup_result = ev_driver->LookupFdNode(as);
    if (lookup_result == nullptr) {
      WSASetLastError(EWOULDBLOCK);
      return -1;
    }
    return lookup_result->ConnectInner(target, target_len) {
  }

  int ConnectInner(const struct sockaddr* target, ares_socklen_t target_len) {
    gpr_mu_lock(&mu_);
    GPR_ASSERT(connected_);
    gpr_mu_unlock(&mu_);
    return 0;
  }
  
  void ShutdownInnerEndpointLocked() override {
    gpr_log(GPR_DEBUG, "ShutdownInnerEndpointLocked is called.");
    grpc_winsocket_shutdown(winsocket_);
  }

private:
  bool ShouldRepeatReadForAresProcessFd() override {
    return GRPC_SLICE_LENGTH(read_buf_) > 0;
  }
  
  void RegisterForOnReadable() override {
    gpr_mu_lock(&mu_);
    SOCKET wrapped_socket = grpc_winsocket_wrapped_socket(winsocket_);
    gpr_log(GPR_DEBUG, "notify read on %" PRIdPTR, (uintptr_t)wrapped_socket);
    WSABUF buffer;
    GPR_ASSERT(GRPC_SLICE_LENGTH(read_buf_) == 0);
    grpc_slice_unref(read_buf_);
    read_buf_ = GRPC_SLICE_MALLOC(kOurMaxUdpResponseSize);
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
      if (WSAGetLastError() != ERROR_IO_PENDING) {
        char* msg = gpr_format_message(WSAGetLastError());
        grpc_error *error = GRPC_ERROR_FROM_COPIED_STRING(msg);
        GRPC_CLOSURE_SCHED(&on_readable_outer_, msg);
        gpr_mu_unlock(&mu_);
        return;
      }
    }
    grpc_socket_notify_on_read(winsocket_, &on_readable_outer_);
    gpr_mu_unlock(&mu_);
  }
  
  void RegisterForOnWriteable() override {
    gpr_mu_lock(&mu_);
    SOCKET s = grpc_winsocket_wrapped_socket(winsocket_);
    gpr_log(GPR_DEBUG, "notify write on %" PRIdPTR, (uintptr_t)s);
    if (!connected_) {
      LPFD_CONNECTEX ConnectEx;
      GUID guid = WSAID_CONNECTEX;
      DWORD ioctl_numbytes;
      if (WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid), ConnectEx, sizeof(ConnectEx),  &ioctl_numbytes, nullptr, nullptr) != 0) {
        grpc_error *error = GRPC_ERROR_FROM_COPIED_STRING(gpr_format_msg(WSAGetLastError()));
        GRPC_CLOSURE_SCHED(&on_writeable_outer_, error);
      } else if (!ConnectEx(s, target, target_len, nullptr, 0, nullptr, &winsocket_->write_info.overlapped)) {
        if (WSAGetLastError() != ERROR_IO_PENDING) {
          grpc_error *error = GRPC_ERROR_FROM_COPIED_STRING(gpr_format_msg(WSAGetLastError()));
          GRPC_CLOSURE_SCHED(&on_writeable_outer_, error);
        }
      }
      gpr_mu_unlock(&mu_);
      return;
    }
    GPR_ASSERT(GRPC_SLICE_LENGTH(write_buf_) == 0);
    GRPC_CLOSURE_SCHED(&on_writeable_outer_, GRPC_ERROR_NONE);
    gpr_mu_unlock(&mu_);
  }
  
  static void OnIocpReadable(void *arg, grpc_error *error) {
    FdNodeWindows* fdn = reinterpret_cast<FdNodeWindows*>(arg);
    fdn->OnIocpReadableInner(error);
  }
  
  void OnIocpReadableInner(grpc_error *error) {
    gpr_mu_lock(&mu_);
    if (error == GRPC_ERROR_NONE) {
      if (winsocket_->read_info.wsa_error != 0) {
        char* msg = gpr_format_message(winsocket_->read_info.wsa_error);
        gpr_log(GPR_DEBUG, "c-ares on iocp readable. Error: %s. Code: %d", msg, winsocket_->read_info.wsa_error);
        GRPC_ERROR_UNREF(error);
        // WSAEMSGSIZE would be caused by more data arriving in the socket's buffer than we set
        // out to read. This almost certainly means that the socket is TCP, whic means that data 
        // hasn't been dropped by the socket's buffers. So let c-ares read what's available
        // and then read the remainders later.
        if (winsocket_->read_info.wsa_error == WSAEMSGSIZE) {
          error = GRPC_ERROR_NONE;
        } else {
          error = GRPC_ERROR_CREATE_FROM_COPIED_STRING(msg);
        }
      } else {
        gpr_log(GPR_DEBUG, "iocp on readable inner called: bytes transfered: %d", winsocket_->read_info.bytes_transfered);
        read_buf_ = grpc_slice_sub_no_ref(read_buf_, 0, winsocket_->read_info.bytes_transfered);
        gpr_log(GPR_DEBUG, "iocp on readable inner called: recvd buf len now: %d", GRPC_SLICE_LENGTH(read_buf_));
      }
    }
    if (error != GRPC_ERROR_NONE) {
      gpr_log(GPR_DEBUG, "iocp readable inner error occurred");
      grpc_slice_unref(read_buf_);
      read_buf_ = grpc_empty_slice();
    }
    GRPC_CLOSURE_SCHED(&read_closure_, error);
    gpr_mu_unlock(&mu_);
  }

  static void OnIocpWriteable(void *arg, grpc_error *error) {
    FdNodeWindows* fdn = reinterpret_cast<FdNodeWindows*>(arg);
    fdn->OnIocpReadableInner(error);
  }
  
  void OnIocpWriteableInner(grpc_error *error) {
    gpr_mu_lock(&mu_);
    if (error == GRPC_ERROR_NONE) {
      if (winsocket_->write_info.wsa_error != 0) {
        char* msg = gpr_format_message(winsocket_->write_info.wsa_error);
        gpr_log(GPR_DEBUG, "c-ares on iocp writeable. Error: %s. Code: %d", msg, winsocket_->write_info.wsa_error);
        GRPC_ERROR_UNREF(error);
        error = GRPC_ERROR_CREATE_FROM_COPIED_STRING(msg);
      } else {
        connected_ = true;
      }
    }
    GPR_ASSERT(GRPC_SLICE_LENGTH(write_buf_) == 0);
    if (error != GRPC_ERROR_NONE) {
      gpr_log(GPR_DEBUG, "iocp writeable inner error occurred");
    }
    GRPC_CLOSURE_SCHED(&write_closure_, error);
    gpr_mu_unlock(&mu_);
  }

  char recvfrom_source_addr_[200];
  ares_socklen_t recvfrom_source_addr_len_;
  grpc_slice read_buf_;
  grpc_slice write_buf_;
  grpc_closure on_readable_outer_;
  grpc_closure on_writeable_outer_;
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
