/*
 *
 * Copyright 2020 gRPC authors.
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

#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include <functional>
#include <set>
#include <thread>

#include <gmock/gmock.h>

#include <grpc/grpc.h>
#include <grpc/grpc_security.h>
#include <grpc/impl/codegen/grpc_types.h>
#include <grpc/slice.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>
#include <grpc/support/time.h>

#include "absl/types/optional.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_cat.h"

#include "src/core/lib/gpr/useful.h"
#include "src/core/lib/gprpp/host_port.h"
#include "src/core/lib/gprpp/thd.h"
#include "src/core/lib/iomgr/error.h"
#include "src/core/lib/security/credentials/alts/alts_credentials.h"
#include "src/core/lib/security/credentials/credentials.h"
#include "src/core/lib/security/security_connector/alts/alts_security_connector.h"
#include "src/core/ext/filters/client_channel/parse_address.h"
#include "src/core/lib/uri/uri_parser.h"
#include "src/core/ext/filters/client_channel/resolver/fake/fake_resolver.h"
#include "src/core/lib/slice/slice_string_helpers.h"

#include "test/core/util/memory_counters.h"
#include "test/core/util/port.h"
#include "test/core/util/test_config.h"

#include "test/core/end2end/cq_verifier.h"

namespace {

struct TestCall {
  explicit TestCall(grpc_channel* channel, grpc_call* call, grpc_completion_queue* cq, std::string server_address)
      : channel(channel), call(call), cq(cq), server_address(server_address) {}

  TestCall(const TestCall& other) = delete;
  TestCall& operator=(const TestCall& other) = delete;

  ~TestCall() {
    grpc_channel_destroy(channel);
    grpc_completion_queue_shutdown(cq);
    while (grpc_completion_queue_next(cq, gpr_inf_future(GPR_CLOCK_REALTIME),
                                      nullptr)
           .type != GRPC_QUEUE_SHUTDOWN)
      ;
    grpc_completion_queue_destroy(cq);
    grpc_call_unref(call);
  }

  grpc_channel* channel;
  grpc_call* call;
  grpc_completion_queue* cq;
  std::string server_address;
  absl::optional<grpc_status_code> status; // filled in when the call is finished
};

void StartCall(TestCall* test_call) {
  grpc_op ops[6];
  grpc_op* op;
  memset(ops, 0, sizeof(ops));
  op = ops;
  op->op = GRPC_OP_SEND_INITIAL_METADATA;
  op->data.send_initial_metadata.count = 0;
  op->flags = GRPC_INITIAL_METADATA_WAIT_FOR_READY;
  op->reserved = nullptr;
  op++;
  void* tag = test_call;
  grpc_call_error error = grpc_call_start_batch(test_call->call, ops, static_cast<size_t>(op - ops), tag,
                                nullptr);
  GPR_ASSERT(GRPC_CALL_OK == error);
  cq_verifier* cqv = cq_verifier_create(test_call->cq);
  CQ_EXPECT_COMPLETION(cqv, tag, 1);
  cq_verify(cqv);
}

void ReceiveInitialMetadata(TestCall* test_call, gpr_timespec deadline) {
  grpc_metadata_array initial_metadata_recv;
  grpc_metadata_array_init(&initial_metadata_recv);
  grpc_op ops[6];
  grpc_op* op;
  memset(ops, 0, sizeof(ops));
  op = ops;
  op->op = GRPC_OP_RECV_INITIAL_METADATA;
  op->data.recv_initial_metadata.recv_initial_metadata = &initial_metadata_recv;
  op->reserved = nullptr;
  op++;
  void* tag = test_call;
  grpc_call_error error = grpc_call_start_batch(test_call->call, ops, static_cast<size_t>(op - ops), tag,
                                nullptr);
  GPR_ASSERT(GRPC_CALL_OK == error);
  grpc_event event = grpc_completion_queue_next(test_call->cq, deadline, nullptr);
  if (event.type != GRPC_OP_COMPLETE || !event.success) {
    gpr_log(GPR_ERROR, "Wanted op complete with success, got op type:%d success:%d", event.type, event.success);
    GPR_ASSERT(0);
  }
  GPR_ASSERT(event.tag == tag);
}

void FinishCall(TestCall* test_call) {
  grpc_op ops[6];
  grpc_op* op;
  grpc_metadata_array trailing_metadata_recv;
  grpc_status_code status;
  grpc_slice details;
  grpc_metadata_array_init(&trailing_metadata_recv);
  memset(ops, 0, sizeof(ops));
  op = ops;
  op->op = GRPC_OP_RECV_STATUS_ON_CLIENT;
  op->data.recv_status_on_client.trailing_metadata = &trailing_metadata_recv;
  op->data.recv_status_on_client.status = &status;
  op->data.recv_status_on_client.status_details = &details;
  op->flags = 0;
  op->reserved = nullptr;
  op++;
  void* tag = test_call;
  grpc_call_error error = grpc_call_start_batch(test_call->call, ops, static_cast<size_t>(op - ops), tag,
                                nullptr);
  GPR_ASSERT(GRPC_CALL_OK == error);
  grpc_event event = grpc_completion_queue_next(test_call->cq, gpr_inf_future(GPR_CLOCK_REALTIME), nullptr);
  GPR_ASSERT(event.type == GRPC_OP_COMPLETE);
  GPR_ASSERT(event.success);
  GPR_ASSERT(event.tag == tag);
  test_call->status = status;
}

class TestServer {
 public:
  explicit TestServer(gpr_event* send_initial_metadata_event, gpr_event* send_status_event) : send_initial_metadata_event_(send_initial_metadata_event), send_status_event_(send_status_event) {
    cq_ = grpc_completion_queue_create_for_next(nullptr);
    cqv_ = cq_verifier_create(cq_);
    server_ = grpc_server_create(nullptr, nullptr);
    address_ =
      grpc_core::JoinHostPort("127.0.0.1", grpc_pick_unused_port_or_die());
    grpc_server_register_completion_queue(server_, cq_, nullptr);
    GPR_ASSERT(
      grpc_server_add_insecure_http2_port(server_, address_.c_str()));
    grpc_server_start(server_);
    thread_ = std::thread(std::bind(&TestServer::AcceptThread, this));
  }

  ~TestServer() {
    if (gpr_event_get(send_initial_metadata_event_) != reinterpret_cast<void*>(1)) {
      gpr_log(GPR_ERROR, "TestServer being dtor'd without SetServerThreadsReadyToRespond having been called");
      GPR_ASSERT(0);
    }
    grpc_server_shutdown_and_notify(server_, cq_, nullptr);
    thread_.join();
    grpc_completion_queue_shutdown(cq_);
    while (grpc_completion_queue_next(cq_, gpr_inf_future(GPR_CLOCK_REALTIME),
                                      nullptr)
           .type != GRPC_QUEUE_SHUTDOWN)
      ;
    grpc_server_destroy(server_);
    grpc_completion_queue_destroy(cq_);
  }

  std::string address() const {
    return address_;
  }

 private:
  void AcceptThread() {
    grpc_call_details call_details;
    grpc_metadata_array request_metadata_recv;
    void* tag = this;
    grpc_call_error error = grpc_server_request_call(
        server_, &call_, &call_details,
        &request_metadata_recv, cq_, cq_, tag);
    GPR_ASSERT(error == GRPC_CALL_OK);
    grpc_event event = grpc_completion_queue_next(cq_, gpr_inf_future(GPR_CLOCK_REALTIME), nullptr);
    GPR_ASSERT(event.type == GRPC_OP_COMPLETE);
    GPR_ASSERT(event.success);
    GPR_ASSERT(event.tag == tag);
    // send initial metadata after getting the signal to do so
    gpr_event_wait(send_initial_metadata_event_, gpr_inf_future(GPR_CLOCK_REALTIME));
    grpc_op ops[6];
    grpc_op* op;
    memset(ops, 0, sizeof(ops));
    op = ops;
    op->op = GRPC_OP_SEND_INITIAL_METADATA;
    op->data.send_initial_metadata.count = 0;
    op->reserved = nullptr;
    op++;
    error = grpc_call_start_batch(call_, ops, static_cast<size_t>(op - ops), tag,
                                  nullptr);
    GPR_ASSERT(GRPC_CALL_OK == error);
    event = grpc_completion_queue_next(cq_, gpr_inf_future(GPR_CLOCK_REALTIME), nullptr);
    GPR_ASSERT(event.type == GRPC_OP_COMPLETE);
    GPR_ASSERT(event.success);
    GPR_ASSERT(event.tag == tag);
    // wait until ok to finish the call
    gpr_event_wait(send_status_event_, gpr_inf_future(GPR_CLOCK_REALTIME));
    grpc_call_cancel_with_status(call_, GRPC_STATUS_PERMISSION_DENIED, "test status",
                                 nullptr);
  }

  grpc_server* server_;
  grpc_completion_queue* cq_;
  cq_verifier *cqv_;
  gpr_event* send_initial_metadata_event_;
  gpr_event* send_status_event_;
  std::string address_;
  std::thread thread_;
  grpc_call* call_;
};

grpc_core::Resolver::Result BuildResolverResponse(
      const std::vector<std::string>& addresses) {
  grpc_core::Resolver::Result result;
  for (const auto& address_str: addresses) {
    grpc_uri* uri = grpc_uri_parse(address_str.c_str(), true);
    if (uri == nullptr) {
      gpr_log(GPR_ERROR, "Failed to parse uri:%s", address_str.c_str());
      GPR_ASSERT(0);
    }
    grpc_resolved_address address;
    GPR_ASSERT(grpc_parse_uri(uri, &address));
    result.addresses.emplace_back(address.addr, address.len, nullptr);
    grpc_uri_destroy(uri);
  }
  return result;
}

void ReceiveInitialMetadataOnCallsDivisibleByAndStartingFrom(int start, int stop, int jump, std::vector<std::unique_ptr<TestCall>> &test_calls) {
  std::vector<std::thread> threads;
  for (int i = start; i < stop; i += jump) {
    threads.push_back(std::thread([&test_calls, i]() {
      ReceiveInitialMetadata(test_calls[i].get(), grpc_timeout_seconds_to_deadline(30));
    }));
  }
  for (auto& thread : threads) {
    thread.join();
  }
}

// Perform a simple RPC where the server cancels the request with
// grpc_call_cancel_with_status
TEST(Pollers, TestReadabilityNotificationsDontGetStrandedOnOneCq) {
  gpr_log(GPR_DEBUG, "test thread");
  const int kNumCalls = 64;
  gpr_event send_initial_metadata_event;
  gpr_event_init(&send_initial_metadata_event);
  gpr_event send_status_event;
  gpr_event_init(&send_status_event);
  size_t num_initial_metadata_received = 0;
  size_t num_status_received = 0;
  grpc_core::Mutex call_phase_counters_mu;
  grpc_core::CondVar call_phase_counters_cv;
  const std::string kSharedUnconnectableAddress = grpc_core::JoinHostPort("127.0.0.1", grpc_pick_unused_port_or_die());
  gpr_log(GPR_DEBUG, "created unconnectable address:%s", kSharedUnconnectableAddress.c_str());
  std::vector<std::thread> threads;
  for (int i = 0; i < kNumCalls; i++) {
    threads.push_back(std::thread([
			    kSharedUnconnectableAddress,
			    &send_initial_metadata_event,
			    &send_status_event,
			    &num_initial_metadata_received,
			    &num_status_received,
			    &call_phase_counters_mu,
			    &call_phase_counters_cv]() {
      auto test_server = absl::make_unique<TestServer>(&send_initial_metadata_event, &send_status_event);
      gpr_log(GPR_DEBUG, "created test_server with address:%s", test_server->address().c_str());
      grpc_arg service_config_arg;
      service_config_arg.type = GRPC_ARG_STRING;
      service_config_arg.key = const_cast<char*>(GRPC_ARG_SERVICE_CONFIG);
      service_config_arg.value.string = const_cast<char*>("{\"loadBalancingConfig\":[{\"round_robin\":{}}]}");
      auto fake_resolver_response_generator = grpc_core::MakeRefCounted<grpc_core::FakeResolverResponseGenerator>();
      {
        grpc_core::ExecCtx exec_ctx;
        fake_resolver_response_generator->SetResponse(
            BuildResolverResponse({
              absl::StrCat("ipv4:", kSharedUnconnectableAddress),
              absl::StrCat("ipv4:", test_server->address())
            }));
      }
      grpc_arg fake_resolver_arg = grpc_core::FakeResolverResponseGenerator::MakeChannelArg(fake_resolver_response_generator.get());
      grpc_channel_args* args = grpc_channel_args_copy_and_add(nullptr, &service_config_arg, 1);
      args = grpc_channel_args_copy_and_add(args, &fake_resolver_arg, 1);
      grpc_channel* channel = grpc_insecure_channel_create("fake:///test.server.com", args, nullptr);
      grpc_completion_queue* cq = grpc_completion_queue_create_for_next(nullptr);
      grpc_call* call = grpc_channel_create_call(channel, nullptr, GRPC_PROPAGATE_DEFAULTS, cq,
        grpc_slice_from_static_string("/foo"), nullptr,
        grpc_timeout_seconds_to_deadline(60), nullptr);
      auto test_call = absl::make_unique<TestCall>(channel, call, cq, test_server->address());
      // Start a call, and ensure that round_robin load balancing is configured
      StartCall(test_call.get());
      // Make sure the test is doing what it's meant to be doing
      grpc_channel_info channel_info;
      memset(&channel_info, 0, sizeof(channel_info));
      char* lb_policy_name = nullptr;
      channel_info.lb_policy_name = &lb_policy_name;
      grpc_channel_get_info(channel, &channel_info);
      EXPECT_EQ(std::string(lb_policy_name), "round_robin") << "not using round robin; this test has a low chance of hitting the bug that it's meant to try to hit";
      gpr_free(lb_policy_name);
      //// Flush out any potentially pending events
      //gpr_log(GPR_DEBUG, "now flush pending events on call with server address:%s", test_server->address().c_str());
      //grpc_event event = grpc_completion_queue_next(test_call->cq, gpr_time_add(gpr_now(GPR_CLOCK_MONOTONIC), gpr_time_from_millis(100, GPR_TIMESPAN)),
      //                                              nullptr);
      //GPR_ASSERT(event.type == GRPC_QUEUE_TIMEOUT);
      // Receive initial metadata
      gpr_log(GPR_DEBUG, "now receive initial metadata on call with server address:%s", test_server->address().c_str());
      ReceiveInitialMetadata(test_call.get(), grpc_timeout_seconds_to_deadline(30));
      {
        grpc_core::MutexLock lock(&call_phase_counters_mu);
        num_initial_metadata_received++;
	call_phase_counters_cv.Broadcast();
	while (num_initial_metadata_received < kNumCalls) {
          call_phase_counters_cv.Wait(&call_phase_counters_mu);
	}
      }
      //// Flush out any potentially pending events
      //gpr_log(GPR_DEBUG, "now flush pending events on call with server address:%s", test_server->address().c_str());
      //grpc_event event = grpc_completion_queue_next(test_call->cq, gpr_time_add(gpr_now(GPR_CLOCK_MONOTONIC), gpr_time_from_millis(100, GPR_TIMESPAN)),
      //                                              nullptr);
      //GPR_ASSERT(event.type == GRPC_QUEUE_TIMEOUT);
      gpr_log(GPR_DEBUG, "now receive status on call with server address:%s", test_server->address().c_str());
      FinishCall(test_call.get());
      GPR_ASSERT(test_call->status.has_value());
      GPR_ASSERT(test_call->status.value() == GRPC_STATUS_PERMISSION_DENIED);
      gpr_log(GPR_DEBUG, "now wait for other calls to received status, this one has server address:%s", test_server->address().c_str());
      {
        grpc_core::MutexLock lock(&call_phase_counters_mu);
        num_status_received++;
	call_phase_counters_cv.Broadcast();
	while (num_status_received < kNumCalls) {
          call_phase_counters_cv.Wait(&call_phase_counters_mu);
	}
      }
      gpr_log(GPR_DEBUG, "now destruct call with server address:%s", test_server->address().c_str());
    }));
  }
  gpr_sleep_until(grpc_timeout_seconds_to_deadline(1));
  gpr_log(GPR_DEBUG, "now let servers send initial metadata");
  gpr_event_set(&send_initial_metadata_event, (void*)1);
  {
    grpc_core::MutexLock lock(&call_phase_counters_mu);
    while (num_initial_metadata_received != kNumCalls) {
      gpr_log(GPR_DEBUG, "now wait for %ld more calls to receive initial metadata", kNumCalls - num_initial_metadata_received);
      call_phase_counters_cv.Wait(&call_phase_counters_mu);
    }
  }
  gpr_sleep_until(grpc_timeout_seconds_to_deadline(1));
  gpr_log(GPR_DEBUG, "now let servers send statuses");
  gpr_event_set(&send_status_event, (void*)1);
  for (auto& thread : threads) {
    thread.join();
  }
  gpr_log(GPR_DEBUG, "All, RPCs completed!");
}

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  grpc::testing::TestEnvironment env(argc, argv);
  grpc_init();
  auto result = RUN_ALL_TESTS();
  grpc_shutdown();
  return result;
}
