#include "usr/local/google/home/apolcyn/grpc/test/core/transport/chttp2/stream_leak_with_queued_flow_control_update.h"

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"

namespace {

class TestServer {
 public:
  explicit TestServer(grpc_completion_queue* cq, grpc_channel_args* channel_args) : cq_(cq) {
    server_ = grpc_server_create(channel_args, nullptr);
    address_ = grpc_core::JoinHostPort("[::1]", grpc_pick_unused_port_or_die());
    grpc_server_register_completion_queue(server_, cq_, nullptr);
    grpc_server_credentials* server_creds =
        grpc_insecure_server_credentials_create();
    GPR_ASSERT(
        grpc_server_add_http2_port(server_, address_.c_str(), server_creds));
    grpc_server_credentials_release(server_creds);
    grpc_server_start(server_);
  }

  ~TestServer() {
    grpc_server_shutdown_and_notify(server_, cq_, this /* tag */);
    grpc_server_destroy(server_);
  }

  void HandleOneRpc() {
    grpc_call_details call_details;
    grpc_call_details_init(&call_details);
    grpc_call* call = nullptr;
    grpc_metadata_array request_metadata_recv;
    grpc_metadata_array_init(&request_metadata_recv);
    // request a call
    grpc_call_error error = grpc_server_request_call(
      server_, &call, &call_details, &request_metadata_recv, cq_,
      cq_, this /* tag */);
    GPR_ASSERT(error == GRPC_CALL_OK);
    grpc_event event = grpc_completion_queue_next(
          cq_, gpr_inf_future(GPR_CLOCK_REALTIME), nullptr);
    GPR_ASSERT(event.type == GRPC_OP_COMPLETE);
    grpc_call_details_destroy(&call_details);
    grpc_metadata_array_destroy(&request_metadata_recv);
    GPR_ASSERT(event.success);
    GPR_ASSERT(event.tag == this);
    // send a response
    void* tag = this;
    grpc_op ops[2];
    grpc_op* op;
    memset(ops, 0, sizeof(ops));
    op = ops;
    op->op = GRPC_OP_SEND_INITIAL_METADATA;
    op++;
    op->op = GRPC_OP_SEND_STATUS_FROM_SERVER;
    op->data.send_status_from_server.status = GRPC_STATUS_OK;
    grpc_slice status_details = grpc_slice_from_static_string("xyz");
    op->data.send_status_from_server.status_details = &status_details;
    grpc_call_error error = grpc_call_start_batch(
        call, ops, static_cast<size_t>(op - ops), tag, nullptr);
    grpc_call_unref(call);
  }

  std::string address() const { return address_; }

 private:
  grpc_server* server_;
  grpc_completion_queue* cq_;
  std::string address_;
};

void StartCallAndCloseWrites(grpc_call* call, grpc_completion_queue *cq) {
  grpc_op ops[2];
  grpc_op* op;
  memset(ops, 0, sizeof(ops));
  op = ops;
  op->op = GRPC_OP_SEND_INITIAL_METADATA;
  op++;
  op->op = GRPC_OP_SEND_CLOSE_FROM_CLIENT;
  op++;
  void* tag = call;
  grpc_call_error error = grpc_call_start_batch(
      call, ops, static_cast<size_t>(op - ops), tag, nullptr);
  GPR_ASSERT(GRPC_CALL_OK == error);
  grpc_event event = grpc_completion_queue_next(
      cq, gpr_inf_future(GPR_CLOCK_REALTIME), nullptr);
  GPR_ASSERT(event.type == GRPC_OP_COMPLETE);
  GPR_ASSERT(event.success);
  GPR_ASSERT(event.tag == tag);
}

void ReceiveResponse(grpc_call* cal, grpc_completion_queue *cq) {
  grpc_op ops[3];
  grpc_op* op;
  grpc_metadata_array initial_metadata_recv;
  grpc_metadata_array_init(&initial_metadata_recv);
  grpc_metadata_array trailing_metadata_recv;
  grpc_metadata_array_init(&trailing_metadata_recv);
  grpc_status_code status;
  grpc_slice details;
  grpc_byte_buffer* recv_payload = nullptr;
  memset(ops, 0, sizeof(ops));
  op = ops;
  op->op = GRPC_OP_RECV_INITIAL_METADATA;
  op->data.recv_initial_metadata.recv_initial_metadata = &initial_metadata_recv;
  op++;
  op->op = GRPC_OP_RECV_MESSAGE;
  op->data.recv_message.recv_message = &recv_payload;
  op++;
  op->op = GRPC_OP_RECV_STATUS_ON_CLIENT;
  op->data.recv_status_on_client.trailing_metadata = &trailing_metadata_recv;
  op->data.recv_status_on_client.status = &status;
  op->data.recv_status_on_client.status_details = &details;
  op++;
  void* tag = call;
  grpc_call_error error = grpc_call_start_batch(
      call, ops, static_cast<size_t>(op - ops), tag, nullptr);
  GPR_ASSERT(GRPC_CALL_OK == error);
  grpc_event event = grpc_completion_queue_next(
      cq, gpr_inf_future(GPR_CLOCK_REALTIME), nullptr);
  GPR_ASSERT(event.type == GRPC_OP_COMPLETE);
  GPR_ASSERT(event.success);
  GPR_ASSERT(event.tag == tag);
  grpc_metadata_array_destroy(&initial_metadata_recv);
  grpc_metadata_array_destroy(&trailing_metadata_recv);
  grpc_byte_buffer_destroy(recv_payload);
  grpc_slice_unref(details);
  GPR_ASSERT(status == GRPC_STATUS_OK);
}

TEST(Chttp2, TestStreamDoesntLeakWhenItsWriteClosedAndThenReadClosedWhileReadingMessage) {
  grpc_completion_queue* cq = grpc_completion_queue_create_for_next(nullptr);
  {
    // Prevent pings from client to server and server to client, since they can
    // cause chttp2 to initiate a write and so dodge the bug we're trying to
    // repro.
    grpc_arg args[] = {
      grpc_channel_arg_integer_create(
          const_cast<char*>(GRPC_ARG_KEEPALIVE_TIME_MS), 0),
      grpc_channel_arg_integer_create(
          const_cast<char*>(GRPC_ARG_HTTP2_BDP_PROBE), 0)
    };
    grpc_channel_args channel_args = {GPR_ARRAY_SIZE(args), args};
    TestServer server(cq, &channel_args);
    grpc_channel_credentials* creds = grpc_insecure_credentials_create();
    grpc_channel* channel = grpc_channel_create(
        absl::StrCat("ipv6:", server.address()).c_str(), creds, &channel_args);
    grpc_channel_credentials_release(creds);
    grpc_call* call = grpc_channel_create_call(
        channel, nullptr, GRPC_PROPAGATE_DEFAULTS, cq,
        grpc_slice_from_static_string("/foo"), nullptr,
        gpr_inf_future(GPR_CLOCK_REALTIME), nullptr);
    // Start the call. It's important for our repro to close writes before
    // reading the response.
    StartCallAndCloseWrites(call, cq);
    server.HandleOneRpc()
    FinishCall(call, cq);
    grpc_call_unref(call);
    grpc_channel_destroy(channel);
    gpr_log(GPR_INFO, "apolcyn now sleep forever. test server address: %s", server.address().c_str());
    sleep(100000);
  }
  grpc_completion_queue_shutdown(cq);
  while (grpc_completion_queue_next(
                 call_cq, gpr_inf_future(GPR_CLOCK_REALTIME), nullptr)
                 .type != GRPC_QUEUE_SHUTDOWN) {
  }
  grpc_completion_queue_destroy(cq);
}

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  // Disable the backup poller to be certain it won't clean up a leaked
  // file descriptor (which we shouldn't need it for). For this test, we
  // want a leaked file descriptor to show up as a loud failure like a
  // memory leak.
  GPR_GLOBAL_CONFIG_SET(grpc_client_channel_backup_poll_interval_ms, 0);
  grpc::testing::TestEnvironment env(&argc, argv);
  grpc_init();
  auto result = RUN_ALL_TESTS();
  grpc_shutdown();
  return result;
}
