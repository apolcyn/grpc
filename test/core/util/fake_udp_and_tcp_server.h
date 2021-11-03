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

#include <grpc/support/port_platform.h>

#include <functional>
#include <set>
#include <string>
#include <thread>

#include "absl/memory/memory.h"

#include <grpc/slice.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>
#include <grpc/support/time.h>

namespace grpc_core {
namespace testing {

class FakeUdpAndTcpServer {
 public:
  enum class ProcessReadResult {
    kContinueReading = 0,
    kCloseSocket,
  };

  enum class AcceptMode {
    kWaitForClientToSendFirstBytes,  // useful for emulating ALTS based
                                     // grpc servers
    kEagerlySendSettings,  // useful for emulating insecure grpc servers (e.g.
                           // ALTS handshake servers)
  };

  explicit FakeUdpAndTcpServer(
      AcceptMode accept_mode,
      const std::function<ProcessReadResult(int, int, int)>& process_read_cb);

  ~FakeUdpAndTcpServer();

  const char* address() { return address_.c_str(); }

  int port() { return port_; };

  static ProcessReadResult CloseSocketUponReceivingBytesFromPeer(
      int bytes_received_size, int read_error, int s);

  static ProcessReadResult CloseSocketUponCloseFromPeer(int bytes_received_size,
                                                        int read_error, int s);

  class FakeUdpAndTcpServerPeer {
   public:
    explicit FakeUdpAndTcpServerPeer(int fd);

    ~FakeUdpAndTcpServerPeer();

    void MaybeContinueSendingSettings();

    int fd() { return fd_; }

   private:
    int fd_;
    int total_bytes_sent_ = 0;
  };

  void ReadFromUdpSocket();

  // Run a loop that periodically, every 10 ms:
  //   1) Checks if there are any new TCP connections to accept.
  //   2) Checks if any data has arrived yet on established connections,
  //      and reads from them if so, processing the sockets as configured.
  static void RunServerLoop(FakeUdpAndTcpServer* self);

 private:
  int accept_socket_;
  int udp_socket_;
  int port_;
  gpr_event stop_ev_;
  std::string address_;
  std::unique_ptr<std::thread> run_server_loop_thd_;
  const AcceptMode accept_mode_;
  std::function<ProcessReadResult(int, int, int)> process_read_cb_;
};

}  // namespace testing
}  // namespace grpc_core
