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

class FdNodeWindows;

struct SockToNodeMapEntry {
  ares_socket_t s;
  FdNodeWindows* node;
  SockToNodeMapEntry *next;
};

class SockToNodeMap {
 public:
  SockToNodeMap() : head_(nullptr) {
    gpr_mu_init(&mu_);
  }
  ~SockToNodeMap() {
    gpr_mu_lock(&mu_);
    while (head_ != nullptr) {
      SockToNodeMapEntry* to_delete = head_;
      head_ = head_->next;
      grpc_core::Delete(to_delete);
    }
    gpr_mu_unlock(&mu_);
    gpr_mu_destroy(&mu_);
  }
  void Add(ares_socket_t s, FdNodeWindows* node) {
    gpr_mu_lock(&mu_);
    SockToNodeMapEntry* item = grpc_core::New<SockToNodeMapEntry>();
    item->s = s;
    item->node = node;
    item->next = head_;
    head_ = item;
    gpr_mu_unlock(&mu_);
  }
  FdNodeWindows* LookupNode(ares_socket_t s) {
    gpr_mu_lock(&mu_);
    SockToNodeMapEntry *cur = head_;
    while (cur != nullptr) {
      if (cur->s == s) {
        gpr_mu_unlock(&mu_);
        return cur->node;
      }
      cur = cur->next;
    }
    gpr_mu_unlock(&mu_);
    return nullptr;
  }

  SockToNodeMapEntry* head_;
  gpr_mu mu_;
};

// Max UDP datagram size we can read.
// This needs to be large enough to cover the largest UDP response
// that both a server will send and that c-ares is willing to accept
// (4K should be enough).
// It's find for TCP responses to be larger than this.
int kOurMaxUdpResponseSize = 540;

class AresEvDriverWindows;

class FdNodeWindows final : public FdNode {
 public:
  enum ReadState {
    READ_EMPTY,
    READ_DONE,
  };
  enum WriteState {
    WRITE_EMPTY,
    REQUESTING_WRITE,
    WRITE_DONE,
  };

  FdNodeWindows(grpc_winsocket* winsocket, ares_socket_t as) : FdNode(as) {
    winsocket_ = winsocket;
    GRPC_CLOSURE_INIT(&on_readable_outer_, &FdNodeWindows::OnIocpReadable, this, grpc_schedule_on_exec_ctx);
    GRPC_CLOSURE_INIT(&on_writeable_outer_, &FdNodeWindows::OnIocpWriteable, this, grpc_schedule_on_exec_ctx);
    read_buf_ = grpc_empty_slice();
    write_buf_ = grpc_empty_slice();
    read_state_ = READ_EMPTY;
    write_state_ = WRITE_EMPTY;
    socket_already_closed_ = false;
  }
  
  ~FdNodeWindows() {
    gpr_log(GPR_DEBUG, "FD NODE WINDOWS DESTRUCTOR IS CALLED");
    if (!socket_already_closed_) {
      closesocket(grpc_winsocket_wrapped_socket(winsocket_));
    }
    grpc_winsocket_destroy(winsocket_);
    grpc_slice_unref(read_buf_);
    grpc_slice_unref(write_buf_);
  }
  
  static ares_ssize_t RecvFrom(ares_socket_t sock, void* data, size_t data_len, int flags, struct sockaddr* from, ares_socklen_t *from_len, void* user_data) {
    SockToNodeMap *map = reinterpret_cast<SockToNodeMap*>(user_data);
    gpr_log(GPR_DEBUG, "custom_recvfrom called on socket %" PRIdPTR ". data_len: %" PRIdPTR, (uintptr_t)sock, data_len);
    FdNodeWindows *lookup_result = map->LookupNode(sock);
    if (lookup_result == nullptr) {
      gpr_log(GPR_DEBUG, "Unexpected: socket %" PRIdPTR " not in socket to node map");
      abort();
    }
    return lookup_result->RecvFromInner(data, data_len, flags, from, from_len);
  }
  
  ares_ssize_t RecvFromInner(void* data, size_t data_len, int flags, struct sockaddr* from, ares_socklen_t *from_len) {
    gpr_mu_lock(&mu_);
    gpr_log(GPR_DEBUG, "notify read on %" PRIdPTR, (uintptr_t)grpc_winsocket_wrapped_socket(winsocket_));
    ares_ssize_t bytes_read = 0;
    switch (read_state_) {
    case READ_EMPTY:
      bytes_read = -1;
      WSASetLastError(EWOULDBLOCK);
      break;
    case READ_DONE:
      SOCKET sock = grpc_winsocket_wrapped_socket(winsocket_);
      GPR_ASSERT(GRPC_SLICE_LENGTH(read_buf_) > 0);
      gpr_log(GPR_DEBUG, "RecvFromInner for socket %" PRIdPTR ": available data length %" PRIdPTR, (uintptr_t)sock, GRPC_SLICE_LENGTH(read_buf_));
      for (size_t i = 0; i < data_len && i < GRPC_SLICE_LENGTH(read_buf_); i++) {
        ((char*)data)[i] = GRPC_SLICE_START_PTR(read_buf_)[i];
        bytes_read++;
      }
      read_buf_ = grpc_slice_sub_no_ref(read_buf_, bytes_read, GRPC_SLICE_LENGTH(read_buf_));
      if (from != nullptr) {
        GPR_ASSERT(*from_len <= recvfrom_source_addr_len_);
        memcpy(from, &recvfrom_source_addr_, recvfrom_source_addr_len_);
        *from_len = recvfrom_source_addr_len_;
      }
      gpr_log(GPR_DEBUG, "RecvFromInner for socket %" PRIdPTR ": bytes read: %" PRIdPTR, (uintptr_t)sock, bytes_read);
      gpr_log(GPR_DEBUG, "RecvFromInner for socket %" PRIdPTR ": read_buf_ len now: %" PRIdPTR, (uintptr_t)sock, GRPC_SLICE_LENGTH(read_buf_));
      if (GRPC_SLICE_LENGTH(read_buf_) == 0) {
        read_state_ = READ_EMPTY;
      }
      break;
    }
    gpr_mu_unlock(&mu_);
    return bytes_read;
  }

  static ares_ssize_t SendV(ares_socket_t as, const struct iovec *iov, int iov_count, void *user_data) {
    gpr_log(GPR_DEBUG, "custom Sendv called on socket %" PRIdPTR ". iov_count: %" PRIdPTR, (uintptr_t)as, iov_count);
    SockToNodeMap *map = reinterpret_cast<SockToNodeMap*>(user_data);
    FdNodeWindows *lookup_result = map->LookupNode(as);
    GPR_ASSERT(lookup_result != nullptr);
    return lookup_result->SendVInner(iov, iov_count);
  }

  grpc_slice FlattenIovec(const struct iovec *iov, int iov_count) {
    int total = 0;
    for (int i = 0; i < iov_count; i++) {
      total += iov[i].iov_len;
    }
    GPR_ASSERT(GRPC_SLICE_LENGTH(write_buf_) == 0);
    grpc_slice out = GRPC_SLICE_MALLOC(total);
    size_t cur = 0;
    for (int i = 0; i < iov_count; i++) {
      for (int k = 0; k < iov[i].iov_len; k++) {
        GRPC_SLICE_START_PTR(out)[cur++] = ((char*)iov[i].iov_base)[k];
      }
    }
    return out;
  }

  int AresWrapperWSASendWriteBuf(LPWSAOVERLAPPED overlapped) {
    WSABUF buf;
    buf.len = GRPC_SLICE_LENGTH(write_buf_);
    buf.buf = (char*)GRPC_SLICE_START_PTR(write_buf_);
    DWORD bytes_sent = 0;
    DWORD flags = 0;
    LPDWORD bytes_sent_ptr = &bytes_sent;
    if (overlapped != nullptr)  {
      bytes_sent_ptr = nullptr;
    }
    WSASend(grpc_winsocket_wrapped_socket(winsocket_), &buf, 1, bytes_sent_ptr, flags, overlapped, nullptr);
    return (int)bytes_sent;
  }

  ares_ssize_t AresWrapperAttemptSendWriteBufSyncNonBlocking() {
    ares_ssize_t total_sent;
    total_sent = AresWrapperWSASendWriteBuf(nullptr);
    write_buf_ = grpc_slice_sub_no_ref(write_buf_, total_sent, GRPC_SLICE_LENGTH(write_buf_));
    if (GRPC_SLICE_LENGTH(write_buf_) > 0) {
      write_state_ = REQUESTING_WRITE;
      WSASetLastError(EWOULDBLOCK);
    }
    return total_sent;
  }

  ares_ssize_t SendVInner(const struct iovec *iov, int iov_count) {
    gpr_mu_lock(&mu_);
    ares_ssize_t total_sent = 0;
    switch (write_state_) {
      case WRITE_EMPTY:
        GPR_ASSERT(GRPC_SLICE_LENGTH(write_buf_) == 0);
        grpc_slice_unref(write_buf_);
        write_buf_ = FlattenIovec(iov, iov_count);
        total_sent = AresWrapperAttemptSendWriteBufSyncNonBlocking();
        break;
      case REQUESTING_WRITE:
        WSASetLastError(EWOULDBLOCK);
        total_sent = -1;
        break;
      case WRITE_DONE:
        size_t cur = 0;
        grpc_slice desired = FlattenIovec(iov, iov_count);
        GPR_ASSERT(GRPC_SLICE_LENGTH(desired) >= GRPC_SLICE_LENGTH(write_buf_));
        for (size_t i = 0; i < GRPC_SLICE_LENGTH(write_buf_); i++) {
          GPR_ASSERT(GRPC_SLICE_START_PTR(desired)[i] == GRPC_SLICE_START_PTR(write_buf_)[i]);
        }
        write_buf_ = grpc_slice_sub_no_ref(desired, GRPC_SLICE_LENGTH(write_buf_), GRPC_SLICE_LENGTH(desired));
        total_sent = AresWrapperAttemptSendWriteBufSyncNonBlocking();
    }
    gpr_mu_unlock(&mu_);
    return total_sent;
  }

  static ares_socket_t Socket(int af, int type, int protocol, void* user_data) {
    SockToNodeMap *map = reinterpret_cast<SockToNodeMap*>(user_data);
    SOCKET s = WSASocket(af, type, protocol, nullptr, 0, WSA_FLAG_OVERLAPPED);
    // TODO: this is abuse of tcp_prepare_socket (socket might be TCP but is likely UDP)
    grpc_tcp_prepare_socket(s);
    char* fd_name;
    gpr_asprintf(&fd_name, "ares_ev_driver-socket:%" PRIuPTR, (uintptr_t)s);
    grpc_winsocket* winsocket = grpc_winsocket_create(s, fd_name);
    gpr_free(fd_name);
    map->Add(s, grpc_core::New<FdNodeWindows>(winsocket, s));
    return s;
  }

  static int CloseSocket(ares_socket_t as, void* user_data) {
    // Underyling socket is closed when either:
    //   * if/when ShutdownInnerEndpointLockedIsCalled.
    //   * the FdNode is destructed
    return 0;
  }

  static int Connect(ares_socket_t as, const struct sockaddr* target, ares_socklen_t target_len, void* user_data) {
    SockToNodeMap *map = reinterpret_cast<SockToNodeMap*>(user_data);
    FdNodeWindows *lookup_result = map->LookupNode(as);
    if (lookup_result == nullptr) {
      gpr_log(GPR_DEBUG, "attempt to connect unregistered socket %" PRIdPTR, as);
      abort();
    }
    return lookup_result->ConnectInner(target, target_len);
  }

  int ConnectInner(const struct sockaddr* target, ares_socklen_t target_len) {
    int out = WSAConnect(grpc_winsocket_wrapped_socket(winsocket_), target, target_len, nullptr, nullptr, nullptr, nullptr);
    return out;
  }
  
  void ShutdownInnerEndpointLocked() override {
    gpr_log(GPR_DEBUG, "ShutdownInnerEndpointLocked is called.");
    grpc_winsocket_shutdown(winsocket_);
    socket_already_closed_ = true;
  }

private:
  bool ShouldRepeatReadForAresProcessFd() override {
    gpr_mu_lock(&mu_);
    if (read_state_ == READ_DONE) {
      GPR_ASSERT(GRPC_SLICE_LENGTH(read_buf_) > 0);
      gpr_mu_unlock(&mu_);
      return true;
    }
    gpr_mu_unlock(&mu_);
    return false;
  }
  
  void RegisterForOnReadableLocked() override {
    SOCKET wrapped_socket = grpc_winsocket_wrapped_socket(winsocket_);
    gpr_log(GPR_DEBUG, "notify read on %" PRIdPTR, (uintptr_t)wrapped_socket);
    switch(read_state_) {
    case READ_EMPTY:
      {
        // no data ready and no pending read, so initiate one
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
            grpc_error *error = GRPC_ERROR_CREATE_FROM_COPIED_STRING(msg);
            GRPC_CLOSURE_SCHED(&on_readable_outer_, error);
            return;
          }
        }
        grpc_socket_notify_on_read(winsocket_, &on_readable_outer_);
      }
      break;
    case READ_DONE:
      // data is still available (wasn't all read on last ares_process)
      gpr_log(GPR_ERROR, "invalid state");
      abort();
      break;
    }
  }
  
  void RegisterForOnWriteableLocked() override {
    SOCKET s = grpc_winsocket_wrapped_socket(winsocket_);
    gpr_log(GPR_DEBUG, "notify write on %" PRIdPTR, (uintptr_t)s);
    switch (write_state_) {
    case WRITE_EMPTY:
      // Either:
      //   * a previous non-blocking write attempt fully succeeded 
      //   * no writes have been attempted so far
      GRPC_CLOSURE_SCHED(&on_writeable_outer_, GRPC_ERROR_NONE);
      break;
    case REQUESTING_WRITE:
      GPR_ASSERT(GRPC_SLICE_LENGTH(write_buf_) == 0);
      // Initiate an async write of data from previous ares_process
      gpr_log(GPR_DEBUG, "RegisterForOnWriteable socket: " PRIdPTR ".state: REQUESTING_WRITE", grpc_winsocket_wrapped_socket(winsocket_));
      AresWrapperWSASendWriteBuf(&winsocket_->write_info.overlapped);
      grpc_socket_notify_on_write(winsocket_, &on_writeable_outer_);
      break;
    case WRITE_DONE:
      // A write finished but it wasn't fully consumed.
      gpr_log(GPR_ERROR, "invalid state for socket: " PRIdPTR ": WRITE_DONE", grpc_winsocket_wrapped_socket(winsocket_));
      abort();
      break;
    }
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
        // out to read. This almost certainly means that the socket is TCP, and thus that data 
        // hasn't been dropped by the socket's buffers. So let c-ares read what's available
        // and then read the remainders later. Note that receiving a UDP response larger
        // than kOurMaxUDPResponseSize is not handled by this code.
        if (winsocket_->read_info.wsa_error == WSAEMSGSIZE) {
          error = GRPC_ERROR_NONE;
        } else {
          error = GRPC_ERROR_CREATE_FROM_COPIED_STRING(msg);
        }
        gpr_free(msg);
      }
    }
    GPR_ASSERT(read_state_ == READ_EMPTY);
    read_state_ = READ_DONE;
    gpr_log(GPR_DEBUG, "iocp on readable inner called: bytes transfered: %d", winsocket_->read_info.bytes_transfered);
    read_buf_ = grpc_slice_sub_no_ref(read_buf_, 0, winsocket_->read_info.bytes_transfered);
    gpr_log(GPR_DEBUG, "iocp on readable inner called: recvd buf len now: %d", GRPC_SLICE_LENGTH(read_buf_));
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
      }
    }
    GPR_ASSERT(GRPC_SLICE_LENGTH(write_buf_) > 0);
    GPR_ASSERT(write_state_ == REQUESTING_WRITE);
    write_state_ = WRITE_DONE;
    gpr_log(GPR_DEBUG, "iocp on readable inner called: bytes transfered: %d", winsocket_->read_info.bytes_transfered);
    write_buf_ = grpc_slice_sub_no_ref(write_buf_, 0, winsocket_->write_info.bytes_transfered);
    gpr_log(GPR_DEBUG, "iocp on readable inner called: recvd buf len now: %d", GRPC_SLICE_LENGTH(read_buf_));
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
  ReadState read_state_;
  WriteState write_state_;
  bool socket_already_closed_;
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
  AresEvDriverWindows() : AresEvDriver(),
    socket_to_node_map_(grpc_core::New<SockToNodeMap>()) {}
 private:
  FdNode* CreateFdNode(ares_socket_t as, const char* name) override {
    FdNodeWindows* fdn = socket_to_node_map_->LookupNode(as);
    GPR_ASSERT(fdn != nullptr);
    return fdn;
  }
  void MaybeOverrideSockFuncs(ares_channel chan) override {
    ares_set_socket_functions(chan, &custom_ares_sock_funcs, socket_to_node_map_.get());
  }
  UniquePtr<SockToNodeMap> socket_to_node_map_;
};

AresEvDriver* AresEvDriver::Create(grpc_pollset_set* pollset_set) {
  // Don't care about pollset_set on windows since poller is global
  return grpc_core::New<AresEvDriverWindows>();
}

}  // namespace grpc_core

#endif /* GRPC_ARES == 1 && defined(GPR_WINDOWS) */
