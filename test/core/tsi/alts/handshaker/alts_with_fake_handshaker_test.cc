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

#include <fcntl.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <set>

#include <grpc/grpc.h>
#include <grpc/grpc_security.h>
#include <grpc/slice.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>
#include <grpc/support/time.h>

#include <grpcpp/impl/codegen/service_type.h>
#include <grpcpp/server_builder.h>

#include "src/core/lib/gprpp/host_port.h"
#include "src/core/lib/gprpp/thd.h"
#include "src/core/lib/iomgr/error.h"
#include "src/core/lib/security/credentials/alts/alts_credentials.h"
#include "src/core/lib/security/credentials/credentials.h"
#include "src/core/lib/slice/slice_string_helpers.h"

#include "test/core/tsi/alts/fake_handshaker/fake_handshaker_server.h"
#include "test/core/util/memory_counters.h"
#include "test/core/util/port.h"
#include "test/core/util/test_config.h"

#include "test/core/end2end/cq_verifier.h"

namespace {

void* tag(int i) { return (void*)static_cast<intptr_t>(i); }

void drain_cq(grpc_completion_queue* cq) {
  grpc_event ev;
  do {
    ev = grpc_completion_queue_next(
        cq, grpc_timeout_milliseconds_to_deadline(5000), nullptr);
  } while (ev.type != GRPC_QUEUE_SHUTDOWN);
}

class FakeHandshakeServer {
 public:
  FakeHandshakeServer(int max_concurrent_streams) {
    int port = grpc_pick_unused_port_or_die();
    grpc_core::JoinHostPort(&address_, "localhost",
                            port);
    service_ = grpc::gcp::CreateFakeHandshakerService();
    grpc::ServerBuilder builder;
    if (max_concurrent_streams != 0) {
      builder.AddChannelArgument(GRPC_ARG_MAX_CONCURRENT_STREAMS, max_concurrent_streams);
    }
    builder.AddListeningPort(address_.get(),
                             grpc::InsecureServerCredentials());
    builder.RegisterService(service_.get());
    server_ = builder.BuildAndStart();
    gpr_log(GPR_INFO, "Fake handshaker server listening on %s",
            address_.get());
  }

  ~FakeHandshakeServer() {
    server_->Shutdown(grpc_timeout_milliseconds_to_deadline(0));
  }

  const char* Address() {
    return address_.get();
  }

 private:
  grpc_core::UniquePtr<char> address_;
  std::unique_ptr<grpc::Service> service_;
  std::unique_ptr<grpc::Server> server_;
};

grpc_core::UniquePtr<grpc::Server> build_and_start_fake_handshaker_server(grpc_core::UniquePtr<char>* address) {
}

grpc_status_code perform_rpc_and_get_status(
    const char* server_address, const char* fake_handshaker_service_address, const void* debug_id) {
  gpr_log(GPR_DEBUG, "debug_id:%p perform_rpc_and_get_status BEGIN", debug_id);
  grpc_alts_credentials_options* alts_options =
      grpc_alts_credentials_client_options_create();
  grpc_channel_credentials* channel_creds =
      grpc_alts_credentials_create_customized(alts_options,
                                              fake_handshaker_service_address,
                                              true /* enable_untrusted_alts */);
  grpc_alts_credentials_options_destroy(alts_options);
  grpc_completion_queue* cq = grpc_completion_queue_create_for_next(nullptr);
  // Create a new channel and call
  grpc_channel* channel = grpc_secure_channel_create(
      channel_creds, server_address, nullptr, nullptr);
  grpc_call* c;
  cq_verifier* cqv = cq_verifier_create(cq);
  grpc_op ops[6];
  grpc_op* op;
  grpc_metadata_array initial_metadata_recv;
  grpc_metadata_array trailing_metadata_recv;
  grpc_call_details call_details;
  grpc_status_code status;
  grpc_call_error error;
  grpc_slice details;
  gpr_timespec deadline = grpc_timeout_seconds_to_deadline(10);
  grpc_slice request_payload_slice = grpc_slice_from_copied_string("request");
  grpc_byte_buffer* request_payload =
      grpc_raw_byte_buffer_create(&request_payload_slice, 1);
  grpc_byte_buffer* response_payload_recv = nullptr;
  // Perform an RPC
  c = grpc_channel_create_call(channel, nullptr, GRPC_PROPAGATE_DEFAULTS, cq,
                               grpc_slice_from_static_string("/foo"), nullptr,
                               deadline, nullptr);
  GPR_ASSERT(c);
  grpc_metadata_array_init(&initial_metadata_recv);
  grpc_metadata_array_init(&trailing_metadata_recv);
  grpc_call_details_init(&call_details);
  memset(ops, 0, sizeof(ops));
  op = ops;
  op->op = GRPC_OP_SEND_INITIAL_METADATA;
  op->data.send_initial_metadata.count = 0;
  op->flags = 0;
  op->reserved = nullptr;
  op++;
  op->op = GRPC_OP_SEND_MESSAGE;
  op->data.send_message.send_message = request_payload;
  op->flags = 0;
  op->reserved = nullptr;
  op->op = GRPC_OP_SEND_CLOSE_FROM_CLIENT;
  op->flags = 0;
  op->reserved = nullptr;
  op++;
  op->op = GRPC_OP_RECV_INITIAL_METADATA;
  op->data.recv_initial_metadata.recv_initial_metadata = &initial_metadata_recv;
  op->flags = 0;
  op->reserved = nullptr;
  op++;
  op->op = GRPC_OP_RECV_MESSAGE;
  op->data.recv_message.recv_message = &response_payload_recv;
  op->op = GRPC_OP_RECV_STATUS_ON_CLIENT;
  op->data.recv_status_on_client.trailing_metadata = &trailing_metadata_recv;
  op->data.recv_status_on_client.status = &status;
  op->data.recv_status_on_client.status_details = &details;
  op->flags = 0;
  op->reserved = nullptr;
  op++;
  error = grpc_call_start_batch(c, ops, static_cast<size_t>(op - ops), tag(1),
                                nullptr);
  GPR_ASSERT(GRPC_CALL_OK == error);
  CQ_EXPECT_COMPLETION(cqv, tag(1), 1);
  cq_verify(cqv);
  // cleanup
  grpc_slice_unref(details);
  grpc_metadata_array_destroy(&initial_metadata_recv);
  grpc_metadata_array_destroy(&trailing_metadata_recv);
  grpc_call_details_destroy(&call_details);
  grpc_byte_buffer_destroy(request_payload);
  grpc_byte_buffer_destroy(response_payload_recv);
  grpc_call_unref(c);
  cq_verifier_destroy(cqv);
  grpc_channel_destroy(channel);
  grpc_channel_credentials_release(channel_creds);
  grpc_completion_queue_shutdown(cq);
  drain_cq(cq);
  grpc_completion_queue_destroy(cq);
  gpr_log(GPR_DEBUG, "debug_id:%p perform_rpc_and_get_status DONE status:%d", debug_id, status);
  return status;
}

struct serve_one_rpc_args {
  grpc_server* server;
  grpc_completion_queue* cq;
};

void serve_one_rpc(void* arg) {
  serve_one_rpc_args* args = static_cast<serve_one_rpc_args*>(arg);
  cq_verifier* cqv = cq_verifier_create(args->cq);
  // Request and respond to a single RPC
  grpc_call* s;
  grpc_metadata_array request_metadata_recv;
  grpc_metadata_array_init(&request_metadata_recv);
  grpc_call_details call_details;
  grpc_call_details_init(&call_details);
  grpc_slice response_payload_slice = grpc_slice_from_copied_string("response");
  grpc_byte_buffer* response_payload =
      grpc_raw_byte_buffer_create(&response_payload_slice, 1);
  grpc_byte_buffer* request_payload_recv = nullptr;
  gpr_log(GPR_DEBUG, "serve_one_rpc: request call");
  grpc_call_error error = grpc_server_request_call(
      args->server, &s, &call_details, &request_metadata_recv, args->cq,
      args->cq, tag(1));
  GPR_ASSERT(GRPC_CALL_OK == error);
  CQ_EXPECT_COMPLETION(cqv, tag(1), 1);
  gpr_log(GPR_DEBUG, "serve_one_rpc: accepted call");
  cq_verify(cqv);
  grpc_op ops[6];
  grpc_op* op;
  memset(ops, 0, sizeof(ops));
  op = ops;
  op->op = GRPC_OP_SEND_INITIAL_METADATA;
  op->data.send_initial_metadata.count = 0;
  op->flags = 0;
  op->reserved = nullptr;
  op++;
  op->op = GRPC_OP_SEND_MESSAGE;
  op->data.send_message.send_message = response_payload;
  op->flags = 0;
  op->op = GRPC_OP_SEND_STATUS_FROM_SERVER;
  op->data.send_status_from_server.trailing_metadata_count = 0;
  op->data.send_status_from_server.status = GRPC_STATUS_OK;
  op->flags = 0;
  op->reserved = nullptr;
  op++;
  op->op = GRPC_OP_RECV_MESSAGE;
  op->data.recv_message.recv_message = &request_payload_recv;
  op->op = GRPC_OP_RECV_CLOSE_ON_SERVER;
  op->flags = 0;
  op->reserved = nullptr;
  op++;
  error = grpc_call_start_batch(s, ops, static_cast<size_t>(op - ops), tag(1),
                                nullptr);
  GPR_ASSERT(GRPC_CALL_OK == error);
  CQ_EXPECT_COMPLETION(cqv, tag(1), 1);
  cq_verify(cqv);
  // Cleanup
  grpc_metadata_array_destroy(&request_metadata_recv);
  grpc_call_details_destroy(&call_details);
  grpc_byte_buffer_destroy(response_payload);
  grpc_byte_buffer_destroy(request_payload_recv);
  grpc_call_unref(s);
  cq_verifier_destroy(cqv);
  gpr_log(GPR_DEBUG, "serve_one_rpc: done");
}

void test_basic_client_server_handshake() {
  gpr_log(GPR_DEBUG, "Running test: test_basic_client_server_handshake");
  FakeHandshakeServer fake_handshake_server(0);
  // Setup
  grpc_alts_credentials_options* alts_options =
      grpc_alts_credentials_server_options_create();
  grpc_server_credentials* server_creds =
      grpc_alts_server_credentials_create_customized(
          alts_options, fake_handshake_server.Address(),
          true /* enable_untrusted_alts */);
  grpc_alts_credentials_options_destroy(alts_options);
  grpc_server* server = grpc_server_create(nullptr, nullptr);
  grpc_completion_queue* server_cq =
      grpc_completion_queue_create_for_next(nullptr);
  grpc_server_register_completion_queue(server, server_cq, nullptr);
  int server_port = grpc_pick_unused_port_or_die();
  grpc_server_register_completion_queue(server, server_cq, nullptr);
  grpc_core::UniquePtr<char> server_addr;
  grpc_core::JoinHostPort(&server_addr, "localhost", server_port);
  GPR_ASSERT(grpc_server_add_secure_http2_port(server, server_addr.get(),
                                               server_creds));
  grpc_server_credentials_release(server_creds);
  grpc_server_start(server);
  serve_one_rpc_args args;
  memset(&args, 0, sizeof(args));
  args.server = server;
  args.cq = server_cq;
  // Test
  {
    grpc_core::UniquePtr<grpc_core::Thread> server_thd =
        grpc_core::MakeUnique<grpc_core::Thread>(
            "test_basic_client_server_handshake server_thd", serve_one_rpc,
            &args);
    server_thd->Start();
    GPR_ASSERT(GRPC_STATUS_OK ==
               perform_rpc_and_get_status(server_addr.get(),
                                          fake_handshake_server.Address(), nullptr /* debug_id */));
    server_thd->Join();
  }
  // Cleanup
  grpc_completion_queue* shutdown_cq =
      grpc_completion_queue_create_for_pluck(nullptr);
  grpc_server_shutdown_and_notify(server, shutdown_cq, tag(1000));
  GPR_ASSERT(grpc_completion_queue_pluck(shutdown_cq, tag(1000),
                                         grpc_timeout_seconds_to_deadline(5),
                                         nullptr)
                 .type == GRPC_OP_COMPLETE);
  grpc_server_destroy(server);
  grpc_completion_queue_shutdown(shutdown_cq);
  grpc_completion_queue_destroy(shutdown_cq);
  grpc_completion_queue_shutdown(server_cq);
  drain_cq(server_cq);
  grpc_completion_queue_destroy(server_cq);
}

struct fake_tcp_server_args {
  int port;
  gpr_event stop_ev;
};

void run_fake_tcp_server_that_closes_connections_upon_receiving_bytes(
    void* arg) {
  fake_tcp_server_args* args = static_cast<fake_tcp_server_args*>(arg);
  int s = socket(AF_INET6, SOCK_STREAM, 0);
  GPR_ASSERT(s != -1);
  if (s == -1) {
    gpr_log(GPR_ERROR, "Failed to create socket: %d", errno);
    abort();
  }
  int val = 1;
  if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)) != 0) {
    gpr_log(GPR_ERROR,
            "Failed to set SO_REUSEADDR on socket bound to [::1]:%d : %d",
            args->port, errno);
    abort();
  }
  if (fcntl(s, F_SETFL, O_NONBLOCK) != 0) {
    gpr_log(GPR_ERROR, "Failed to set O_NONBLOCK on socket: %d", errno);
    abort();
  }
  sockaddr_in6 addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin6_family = AF_INET6;
  addr.sin6_port = htons(args->port);
  ((char*)&addr.sin6_addr)[15] = 1;
  if (bind(s, (const sockaddr*)&addr, sizeof(addr)) != 0) {
    gpr_log(GPR_ERROR, "Failed to bind socket to [::1]:%d : %d", args->port,
            errno);
    abort();
  }
  if (listen(s, 100)) {
    gpr_log(GPR_ERROR, "Failed to listen on socket bound to [::1]:%d : %d",
            args->port, errno);
    abort();
  }
  std::set<int> peers;
  while (!gpr_event_get(&args->stop_ev)) {
    int p = accept(s, nullptr, nullptr);
    if (p == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
      gpr_log(GPR_ERROR, "Failed to accept connection: %d", errno);
      abort();
    }
    if (p != -1) {
      gpr_log(GPR_DEBUG, "accepted peer socket: %d", p);
      if (fcntl(p, F_SETFL, O_NONBLOCK) != 0) {
        gpr_log(GPR_ERROR, "Failed to set O_NONBLOCK on peer socket: %d", p, errno);
        abort();
      }
      peers.insert(p);
    }
    for (auto it = peers.begin(); it != peers.end(); it++) {
      int p = *it;
      char buf[100];
      int bytes_received_size = recv(p, buf, 100, 0);
      if (bytes_received_size < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        gpr_log(GPR_ERROR, "Failed to receive from peer socket: %d. errno: %d", p, errno);
        abort();
      }
      if (bytes_received_size >= 0) {
        gpr_log(GPR_DEBUG,
                "Fake TCP server received %d bytes from peer socket: %d. Now close the "
                "connection.",
                bytes_received_size, p);
        close(p);
        peers.erase(p);
      }
    }
    gpr_sleep_until(gpr_time_add(gpr_now(GPR_CLOCK_MONOTONIC),
                                 gpr_time_from_millis(10, GPR_TIMESPAN)));
  }
  for (auto it = peers.begin(); it != peers.end(); it++) {
    close(*it);
  }
  close(s);
}

struct run_one_rpc_handshake_fails_fast_args {
  const char* fake_tcp_server_addr;
  const char* fake_handshaker_server_addr;
};

void run_one_rpc_handshake_fails_fast(void* arg) {
  const run_one_rpc_handshake_fails_fast_args *args = static_cast<const run_one_rpc_handshake_fails_fast_args*>(arg);
  void* debug_id = &args;
  grpc_status_code status = perform_rpc_and_get_status(
      args->fake_tcp_server_addr, args->fake_handshaker_server_addr, debug_id);
  if (status != GRPC_STATUS_UNAVAILABLE) {
    gpr_log(GPR_ERROR,
            "debug_id:%p test_handshake_fails_fast_when_peer_endpoint_closes_"
            "connection_after_accepting failed. Expected status: %d. Got "
            "%d.", debug_id,
            GRPC_STATUS_UNAVAILABLE, status);
    abort();
  }
}

/* This test is intended to make sure that we quickly cancel ALTS RPC's
 * when the security handshaker gets a read endpoint from the remote peer. The
 * goal is that RPC's will sharply slow down due to exceeding the number
 * of handshakes that can be outstanding at once, forcing new handshakes to be
 * queued up for longer than they should be, if that isn't done. */
void test_handshake_fails_fast_when_peer_endpoint_closes_connection_after_accepting() {
  gpr_log(GPR_DEBUG, "Running test: test_handshake_fails_fast_when_peer_endpoint_closes_connection_after_accepting");
  FakeHandshakeServer fake_handshake_server(20);
  {
    fake_tcp_server_args args;
    memset(&args, 0, sizeof(args));
    args.port = grpc_pick_unused_port_or_die();
    gpr_event_init(&args.stop_ev);
    grpc_core::UniquePtr<grpc_core::Thread> fake_tcp_server_thd =
        grpc_core::MakeUnique<grpc_core::Thread>(
            "fake tcp server that closes connections upon receiving bytes",
            run_fake_tcp_server_that_closes_connections_upon_receiving_bytes,
            &args);
    fake_tcp_server_thd->Start();
    grpc_core::UniquePtr<char> fake_tcp_server_addr;
    grpc_core::JoinHostPort(&fake_tcp_server_addr, "[::1]", args.port);
    {
      gpr_timespec test_deadline = gpr_time_add(gpr_now(GPR_CLOCK_MONOTONIC), gpr_time_from_seconds(5, GPR_TIMESPAN));
      gpr_log(GPR_DEBUG, "start performing concurrent RPCs");
      std::vector<grpc_core::UniquePtr<grpc_core::Thread>> rpc_thds;
      int num_concurrent_rpcs = 100;
      rpc_thds.reserve(num_concurrent_rpcs);
      run_one_rpc_handshake_fails_fast_args rpc_args;
      rpc_args.fake_tcp_server_addr = fake_tcp_server_addr.get();
      rpc_args.fake_handshaker_server_addr = fake_handshake_server.Address();
      for (size_t i = 0; i < num_concurrent_rpcs; i++) {
        auto new_thd = grpc_core::MakeUnique<grpc_core::Thread>("run one rpc handshake fails fast", run_one_rpc_handshake_fails_fast, &rpc_args);
        new_thd->Start();
        rpc_thds.push_back(std::move(new_thd));
      }
      for (size_t i = 0; i < num_concurrent_rpcs; i++) {
        rpc_thds[i]->Join();
      }
      gpr_event_set(&args.stop_ev, (void*)1);
      gpr_log(GPR_DEBUG, "done performing concurrent RPCs");
      if (gpr_time_cmp(gpr_now(GPR_CLOCK_MONOTONIC), test_deadline) > 0) {
        gpr_log(GPR_ERROR, "Exceeded test deadline. ALTS handshakes might not be failing fast when the peer endpoint closes the connection abruptly");
        abort();
      }
    }
    fake_tcp_server_thd->Join();
  }
}

}  // namespace

int main(int argc, char** argv) {
  grpc_init();
  {
    //test_basic_client_server_handshake();
    test_handshake_fails_fast_when_peer_endpoint_closes_connection_after_accepting();
  }
  grpc_shutdown();
  return 0;
}
