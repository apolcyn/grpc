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

#include "test/core/end2end/end2end_tests.h"

#include <stdio.h>
#include <string.h>

#include <grpc/byte_buffer.h>
#include <grpc/grpc.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/time.h>
#include "src/core/lib/debug/stats.h"
#include "src/core/lib/gpr/string.h"
#include "include/grpc/support/string_util.h"
#include "test/core/end2end/cq_verifier.h"
#include "src/core/lib/gpr/host_port.h"
#include "test/core/util/port.h"
#include "test/core/util/test_config.h"
#include "test/core/util/cmdline.h"

static void* tag(intptr_t t) { return (void*)t; }

static gpr_timespec n_seconds_from_now(int n) {
  return grpc_timeout_seconds_to_deadline(n);
}

static gpr_timespec five_seconds_from_now(void) {
  return n_seconds_from_now(5);
}

static void drain_cq(grpc_completion_queue* cq) {
  grpc_event ev;
  do {
    ev = grpc_completion_queue_next(cq, five_seconds_from_now(), nullptr);
  } while (ev.type != GRPC_QUEUE_SHUTDOWN);
}

static void shutdown_server(grpc_server *server) {
  grpc_completion_queue* shutdown_cq = grpc_completion_queue_create_for_pluck(nullptr);
  grpc_server_shutdown_and_notify(server, shutdown_cq, tag(1000));
  GPR_ASSERT(grpc_completion_queue_pluck(shutdown_cq, tag(1000),
                                         grpc_timeout_seconds_to_deadline(5),
                                         nullptr)
                 .type == GRPC_OP_COMPLETE);
  grpc_server_destroy(server);
  grpc_completion_queue_shutdown(shutdown_cq);
  grpc_completion_queue_destroy(shutdown_cq);
}

static void shutdown_client(grpc_channel* client) {
  grpc_channel_destroy(client);
}

static void end_test(grpc_channel *client, grpc_server *server, grpc_completion_queue *cq) {
  shutdown_server(server);
  shutdown_client(client);

  grpc_completion_queue_shutdown(cq);
  drain_cq(cq);
  grpc_completion_queue_destroy(cq);
}

static void simple_request_body(const char* local_dns_server_address) {
  grpc_call* c;
  grpc_call* s;

  int port = grpc_pick_unused_port_or_die();
  char* localaddr = nullptr;
  // Broken-on-ipv4-only
  gpr_join_host_port(&localaddr, "::1", port);

  char* client_target = nullptr;
  GPR_ASSERT(gpr_asprintf(&client_target, "dns://%s/server.end2end_address_sorting_test.com:%d", local_dns_server_address, port));
  grpc_channel* client =
      grpc_insecure_channel_create(client_target, /* client_args */nullptr, nullptr);
  grpc_completion_queue* cq = grpc_completion_queue_create_for_next(nullptr);
  cq_verifier* cqv = cq_verifier_create(cq);
  grpc_server *server = grpc_server_create(nullptr /* server_args */, nullptr);
  grpc_server_register_completion_queue(server, cq, nullptr);
  GPR_ASSERT(grpc_server_add_insecure_http2_port(server, localaddr));
  grpc_server_start(server);

  grpc_op ops[6];
  grpc_op* op;
  grpc_metadata_array initial_metadata_recv;
  grpc_metadata_array trailing_metadata_recv;
  grpc_metadata_array request_metadata_recv;
  grpc_call_details call_details;
  grpc_status_code status;
  const char* error_string;
  grpc_call_error error;
  grpc_slice details;
  int was_cancelled = 2;
  char* peer;

  gpr_timespec deadline = five_seconds_from_now();
  c = grpc_channel_create_call(client, nullptr, GRPC_PROPAGATE_DEFAULTS, cq,
                               grpc_slice_from_static_string("/foo"), nullptr,
                               deadline, nullptr);
  GPR_ASSERT(c);

  peer = grpc_call_get_peer(c);
  GPR_ASSERT(peer != nullptr);
  gpr_log(GPR_DEBUG, "client_peer_before_call=%s", peer);
  gpr_free(peer);

  grpc_metadata_array_init(&initial_metadata_recv);
  grpc_metadata_array_init(&trailing_metadata_recv);
  grpc_metadata_array_init(&request_metadata_recv);
  grpc_call_details_init(&call_details);

  memset(ops, 0, sizeof(ops));
  op = ops;
  op->op = GRPC_OP_SEND_INITIAL_METADATA;
  op->data.send_initial_metadata.count = 0;
  op->flags = 0;
  op->reserved = nullptr;
  op++;
  op->op = GRPC_OP_SEND_CLOSE_FROM_CLIENT;
  op->flags = 0;
  op->reserved = nullptr;
  op++;
  op->op = GRPC_OP_RECV_INITIAL_METADATA;
  op->data.recv_initial_metadata.recv_initial_metadata = &initial_metadata_recv;
  op->flags = 0;
  op->reserved = nullptr;
  op++;
  op->op = GRPC_OP_RECV_STATUS_ON_CLIENT;
  op->data.recv_status_on_client.trailing_metadata = &trailing_metadata_recv;
  op->data.recv_status_on_client.status = &status;
  op->data.recv_status_on_client.status_details = &details;
  op->data.recv_status_on_client.error_string = &error_string;
  op->flags = 0;
  op->reserved = nullptr;
  op++;
  error = grpc_call_start_batch(c, ops, static_cast<size_t>(op - ops), tag(1),
                                nullptr);
  GPR_ASSERT(GRPC_CALL_OK == error);

  error =
      grpc_server_request_call(server, &s, &call_details,
                               &request_metadata_recv, cq, cq, tag(101));
  GPR_ASSERT(GRPC_CALL_OK == error);
  CQ_EXPECT_COMPLETION(cqv, tag(101), 1);
  cq_verify(cqv);

  peer = grpc_call_get_peer(s);
  GPR_ASSERT(peer != nullptr);
  gpr_log(GPR_DEBUG, "server_peer=%s", peer);
  gpr_free(peer);
  peer = grpc_call_get_peer(c);
  GPR_ASSERT(peer != nullptr);
  gpr_log(GPR_DEBUG, "client_peer=%s", peer);
  gpr_free(peer);

  memset(ops, 0, sizeof(ops));
  op = ops;
  op->op = GRPC_OP_SEND_INITIAL_METADATA;
  op->data.send_initial_metadata.count = 0;
  op->flags = 0;
  op->reserved = nullptr;
  op++;
  op->op = GRPC_OP_SEND_STATUS_FROM_SERVER;
  op->data.send_status_from_server.trailing_metadata_count = 0;
  op->data.send_status_from_server.status = GRPC_STATUS_UNIMPLEMENTED;
  grpc_slice status_details = grpc_slice_from_static_string("xyz");
  op->data.send_status_from_server.status_details = &status_details;
  op->flags = 0;
  op->reserved = nullptr;
  op++;
  op->op = GRPC_OP_RECV_CLOSE_ON_SERVER;
  op->data.recv_close_on_server.cancelled = &was_cancelled;
  op->flags = 0;
  op->reserved = nullptr;
  op++;
  error = grpc_call_start_batch(s, ops, static_cast<size_t>(op - ops), tag(102),
                                nullptr);
  GPR_ASSERT(GRPC_CALL_OK == error);

  CQ_EXPECT_COMPLETION(cqv, tag(102), 1);
  CQ_EXPECT_COMPLETION(cqv, tag(1), 1);
  cq_verify(cqv);

  GPR_ASSERT(status == GRPC_STATUS_UNIMPLEMENTED);
  GPR_ASSERT(0 == grpc_slice_str_cmp(details, "xyz"));
  // the following sanity check makes sure that the requested error string is
  // correctly populated by the core. It looks for certain substrings that are
  // not likely to change much. Some parts of the error, like time created,
  // obviously are not checked.
  GPR_ASSERT(nullptr != strstr(error_string, "xyz"));
  GPR_ASSERT(nullptr != strstr(error_string, "description"));
  GPR_ASSERT(nullptr != strstr(error_string, "Error received from peer"));
  GPR_ASSERT(nullptr != strstr(error_string, "grpc_message"));
  GPR_ASSERT(nullptr != strstr(error_string, "grpc_status"));
  GPR_ASSERT(0 == grpc_slice_str_cmp(call_details.method, "/foo"));
  GPR_ASSERT(0 == call_details.flags);
  GPR_ASSERT(was_cancelled == 1);

  grpc_slice_unref(details);
  gpr_free((void*)error_string);
  grpc_metadata_array_destroy(&initial_metadata_recv);
  grpc_metadata_array_destroy(&trailing_metadata_recv);
  grpc_metadata_array_destroy(&request_metadata_recv);
  grpc_call_details_destroy(&call_details);

  grpc_call_unref(c);
  grpc_call_unref(s);

  cq_verifier_destroy(cqv);
  end_test(client, server, cq);
}

int main(int argc, char** argv) {
  grpc_test_init(argc, argv);

  gpr_cmdline *cl;
  cl = gpr_cmdline_create("My cool tool");
  const char* local_dns_server_address = nullptr;
  gpr_cmdline_add_string(cl, "local_dns_server_address", "IP-port of local DNS server.", &local_dns_server_address);
  gpr_cmdline_parse(cl, argc, argv);
  gpr_cmdline_destroy(cl);
  gpr_log(GPR_INFO, "Local DNS server address: %s", local_dns_server_address);

  grpc_init();
  simple_request_body(local_dns_server_address);
  grpc_shutdown();
}
