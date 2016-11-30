#include <stdio.h>
#include <string.h>
#include <pthread.h>

#include <grpc/grpc.h>
#include <grpc/byte_buffer_reader.h>
#include <grpc/support/time.h>
#include <grpc/support/log.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/time.h>

/* This is a minimal plain C repro of the bug seen in ruby, where pluck calls
 * with a SEND_CLOSE_FROM_CLIENT op sometimes fail. The problem was noticed when
 * running ruby unary unconstrained benchmarks with 64 channels and 16 streams per channel,
 * which causes 1024 concurrent ruby threads making repeated unary calls.
 * The bug happens with ruby client -> ruby server, ruby client -> c++ server,
 * and can also occur with the plain code here, against a c++ or ruby server.
 *
 * This attempts to mimick the way the ruby uses the core. It mimicks the ruby benchmark client
 * by setting up a certain number of channels and then making a certain number of
 * "outstanding calls" on each channel. So here, there are multiple threads, where each one
 * repeatedly makes unary calls on its channel, using ops in the same order that was causing
 * issues in ruby.
 *
 * The way that ops are set up and ran here attempts to mimck how it's done in ruby, with some
 * minor tweaks to be specific to this repro, where we are only making client request-response
 * calls, on an insecure channel, with hard coded service/method, and proto to send.
 * This also attempts to mimick the use of the GIL in ruby,
 * the "fake_gil" mutex is released just before
 * calling pluck, and then obtained again just after finishing the call.
 */

// The raw write format of the common SimpleRequest proto that's used in benchmarks
#define RAW_PROTO_SIMPLE_REQUEST "\x1a\x00"
#define RAW_PROTO_SIMPLE_REQUEST_LEN 2

// A local benchmark server needs to be running on port 13000
#define TARGET_HOST_PORT "localhost:13000"

// This is the service and method used in 'unconstrained unary' benchmark calls,
// where the error was noticed
#define SERVICE_AND_METHOD "/grpc.testing.BenchmarkService/UnaryCall"

/* grpc_wrapped_channel wraps a grpc_channel. */
typedef struct grpc_wrapped_channel {

  /* The actual channel */
  grpc_channel *wrapped;
  grpc_completion_queue *queue;
} grpc_wrapped_channel;

typedef struct grpc_wrapped_call {
  grpc_call *wrapped;
  grpc_completion_queue *queue;
} grpc_wrapped_call;

/* run_batch_stack holds various values used by the
 * grpc_run_batch function */
typedef struct run_batch_stack {
  /* The batch ops */
  grpc_op ops[1]; /* 1 is the maximum number of operations,
                     (normally this is 8 but for this repro, only using one op at a time */
  size_t op_num;  /* tracks the last added operation */

  /* Data being sent */
  grpc_metadata_array send_metadata;
  grpc_metadata_array send_trailing_metadata;

  /* Data being received */
  grpc_byte_buffer *recv_message;
  grpc_metadata_array recv_metadata;
  grpc_metadata_array recv_trailing_metadata;
  int recv_cancelled;
  grpc_status_code recv_status;
  char *recv_status_details;
  size_t recv_status_details_capacity;
  unsigned write_flag;
} run_batch_stack;

/* Used to allow grpc_completion_queue_next call to release the GIL */
typedef struct next_call_stack {
  grpc_completion_queue *cq;
  grpc_event event;
  gpr_timespec timeout;
  void *tag;
  volatile int interrupted;
} next_call_stack;

// Mimicks use of the GIL in ruby as best as possible. Release the mutex when we call pluck,
// but only one thread can run at a time outside of the pluck call.
gpr_mu fake_gil;

grpc_byte_buffer* gprc_s_to_byte_buffer(char *string, size_t length);
grpc_event run_completion_queue_pluck_mimick_ruby(grpc_completion_queue *queue, void *tag,
                                     gpr_timespec deadline, void *reserved);
void grpc_completion_queue_shutdown_and_destroy(grpc_completion_queue *cq);
void grpc_channel_wrapper_free(grpc_wrapped_channel *ch);
void destroy_call(grpc_wrapped_call *call);
grpc_wrapped_channel* grpc_channel_alloc_init();
grpc_wrapped_call* grpc_channel_create_wrapped_call(grpc_wrapped_channel *wrapper, char *method, char *host);
void grpc_run_batch_stack_init(run_batch_stack *st,
                                      unsigned write_flag);
void grpc_run_batch_stack_cleanup(run_batch_stack *st);
void grpc_run_batch_stack_fill_op(run_batch_stack *st, grpc_op_type this_op);
void grpc_run_batch(grpc_wrapped_call *wrapped_call, grpc_op_type this_op);
void grpc_run_request_response_mimick_ruby(grpc_wrapped_call *wrapped_call);
void create_and_run_unary_calls();

// Destroys a wrapped "call", which holds a grpc_call and a completion queue
// , just as is done in ruby
void destroy_call(grpc_wrapped_call *call) {
  /* Ensure that we only try to destroy the call once */
  if (call->wrapped != NULL) {
    grpc_call_destroy(call->wrapped);
    call->wrapped = NULL;
    grpc_completion_queue_shutdown_and_destroy(call->queue);
    call->queue = NULL;
  }
}

// Create a channel and a completion queue for it, address hard coded to localhost:13000
grpc_wrapped_channel* grpc_channel_alloc_init() {
  grpc_wrapped_channel *wrapper = gpr_malloc(sizeof(grpc_wrapped_channel));
  grpc_channel *ch = NULL;
  ch = grpc_insecure_channel_create(TARGET_HOST_PORT, NULL, NULL);
  wrapper->queue = grpc_completion_queue_create(NULL);
  wrapper->wrapped = ch;
  return wrapper;
}

// creates a call object. the call gets its own completion queue, just as is done in ruby
grpc_wrapped_call* grpc_channel_create_wrapped_call(grpc_wrapped_channel *wrapper, char *method, char *host) {
  grpc_call *call = NULL;
  grpc_wrapped_call *wrapped_call = NULL;
  grpc_channel *ch = NULL;
  grpc_completion_queue *cq = NULL;
  int flags = GRPC_PROPAGATE_DEFAULTS;

  cq = grpc_completion_queue_create(NULL);
  GPR_ASSERT(cq != NULL);
  ch = wrapper->wrapped;
  GPR_ASSERT(ch != NULL);
  call = grpc_channel_create_call(ch, NULL, flags, cq, method,
                                  host, gpr_inf_future(GPR_CLOCK_REALTIME),
                                      NULL);
  GPR_ASSERT(call != NULL);
  wrapped_call = gpr_malloc(sizeof(grpc_wrapped_call));
  wrapped_call->wrapped = call;
  wrapped_call->queue = cq;

  return wrapped_call;
}

/* grpc_run_batch_stack_init ensures the run_batch_stack is properly
 * initialized */
void grpc_run_batch_stack_init(run_batch_stack *st,
                                      unsigned write_flag) {
  memset(st, 0, sizeof(run_batch_stack));
  grpc_metadata_array_init(&st->send_metadata);
  grpc_metadata_array_init(&st->send_trailing_metadata);
  grpc_metadata_array_init(&st->recv_metadata);
  grpc_metadata_array_init(&st->recv_trailing_metadata);
  st->op_num = 0;
  st->write_flag = write_flag;
}

// A tweak from regular ruby for this repro:
// in ruby, the byte buffer is copied to a string, discard it for this repro
void grpc_byte_buffer_read_and_discard(grpc_byte_buffer *buffer) {
  grpc_byte_buffer_reader reader;
  grpc_slice next;

  // A tweak for this repro: we're expecting non-nil messages to be received
  GPR_ASSERT(buffer != NULL);

  GPR_ASSERT(grpc_byte_buffer_reader_init(&reader, buffer));

  while (grpc_byte_buffer_reader_next(&reader, &next) != 0) {
    grpc_slice_unref(next);
  }
  grpc_byte_buffer_reader_destroy(&reader);
  grpc_byte_buffer_destroy(buffer);
}

/* grpc_run_batch_stack_cleanup ensures the run_batch_stack is properly
 * cleaned up */
void grpc_run_batch_stack_cleanup(run_batch_stack *st) {
  size_t i = 0;

  grpc_metadata_array_destroy(&st->send_metadata);
  grpc_metadata_array_destroy(&st->send_trailing_metadata);
  grpc_metadata_array_destroy(&st->recv_metadata);
  grpc_metadata_array_destroy(&st->recv_trailing_metadata);

  if (st->recv_status_details != NULL) {
    gpr_free(st->recv_status_details);
  }

  for (i = 0; i < st->op_num; i++) {
    if (st->ops[i].op == GRPC_OP_SEND_MESSAGE) {
      grpc_byte_buffer_destroy(st->ops[i].data.send_message);
    }
    // A tweak for this repro: discard received messages,
    // normally they're copied to a ruby string
    if (st->ops[i].op == GRPC_OP_RECV_MESSAGE) {
      grpc_byte_buffer_read_and_discard(*st->ops[i].data.recv_message);
    }
  }
}

/* Destroys Channel instances. */
void grpc_channel_wrapper_free(grpc_wrapped_channel *channel) {
    grpc_wrapped_channel *ch = NULL;
    if (channel == NULL) {
       return;
    };
    ch = channel;

    if (ch->wrapped != NULL) {
      grpc_channel_destroy(ch->wrapped);
      grpc_completion_queue_shutdown_and_destroy(ch->queue);
    }
    gpr_free(channel);
}

/* Calls grpc_completion_queue_pluck, this called without holding the "GIL" as is done in ruby */
void *grpc_completion_queue_pluck_no_gil(next_call_stack* next_call) {
  gpr_timespec increment = gpr_time_from_millis(20, GPR_TIMESPAN);
  gpr_timespec deadline;
  do {
    deadline = gpr_time_add(gpr_now(GPR_CLOCK_REALTIME), increment);
    next_call->event = grpc_completion_queue_pluck(next_call->cq,
                                                   next_call->tag,
                                                   deadline, NULL);
    if (next_call->event.type != GRPC_QUEUE_TIMEOUT ||
        gpr_time_cmp(deadline, next_call->timeout) > 0) {
      break;
    }
  } while (!next_call->interrupted);
  return NULL;
}

/* Helper function to free a completion queue. */
void grpc_completion_queue_shutdown_and_destroy(grpc_completion_queue *cq) {
  /* Every function that adds an event to a queue also synchronously plucks
     that event from the queue, and holds a reference to the Ruby object that
     holds the queue, so we only get to this point if all of those functions
     have completed, and the queue is empty */
  grpc_completion_queue_shutdown(cq);
  grpc_completion_queue_destroy(cq);
}

// runs completion queue pluck in the same way that ruby does.
// before making the actual core call to pluck, release the GIL, obtain it again after the
// call is done
grpc_event run_completion_queue_pluck_mimick_ruby(grpc_completion_queue *queue, void *tag,
                                     gpr_timespec deadline, void *reserved) {
  next_call_stack next_call;
  memset(&next_call, 0, sizeof(next_call_stack));
  next_call.cq = queue;
  next_call.timeout = deadline;
  next_call.tag = tag;
  next_call.event.type = GRPC_QUEUE_TIMEOUT;
  (void)reserved;
  /* Loop until we finish a pluck without an interruption. The internal
     pluck function runs either until it is interrupted or it gets an
     event, or time runs out.

     The basic reason we need this relatively complicated construction is that
     we need to re-acquire the GVL when an interrupt comes in, so that the ruby
     interpreter can do what it needs to do with the interrupt. But we also need
     to get back to plucking when the interrupt has been handled. */
  do {
    next_call.interrupted = 0;
    // Release the GIL before making the call to "pluck_no_gil", similarly to how it's done in ruby
    gpr_mu_unlock(&fake_gil);
    grpc_completion_queue_pluck_no_gil(&next_call);
    gpr_mu_lock(&fake_gil);
    /* If an interrupt prevented pluck from returning useful information, then
       any plucks that did complete must have timed out */
  } while (next_call.interrupted &&
           next_call.event.type == GRPC_QUEUE_TIMEOUT);
  return next_call.event;
}

// converts a C string to a byte buffer
grpc_byte_buffer* grpc_s_to_byte_buffer(char *string, size_t length) {
  grpc_slice slice = grpc_slice_from_copied_buffer(string, length);
  grpc_byte_buffer *buffer = grpc_raw_byte_buffer_create(&slice, 1);
  grpc_slice_unref(slice);
  return buffer;
}

// Fill in the op based on the op type requested
// This is a modified version of how core ops are set up in ruby. It is tweaked
// for the case of this repro, for which we're only making client-side unary calls,
// and we're always sending the same hard-coded message
void grpc_run_batch_stack_fill_op(run_batch_stack *st, grpc_op_type this_op) {
  GPR_ASSERT(st->op_num == 0);
  st->ops[st->op_num].flags = 0; // not using the write flag to buffer sends
  switch (this_op) {
    case GRPC_OP_SEND_INITIAL_METADATA:
      st->ops[st->op_num].data.send_initial_metadata.count =
          st->send_metadata.count;
      st->ops[st->op_num].data.send_initial_metadata.metadata =
          st->send_metadata.metadata;
      break;
    case GRPC_OP_SEND_MESSAGE:
      // The message we send is hard coded to the raw proto SimpleRequest wire format
      st->ops[st->op_num].data.send_message = grpc_s_to_byte_buffer(
          (char*)RAW_PROTO_SIMPLE_REQUEST, RAW_PROTO_SIMPLE_REQUEST_LEN);
      GPR_ASSERT(st->write_flag == 0);
      st->ops[st->op_num].flags = st->write_flag;
      break;
    case GRPC_OP_SEND_CLOSE_FROM_CLIENT:
      break;
    case GRPC_OP_SEND_STATUS_FROM_SERVER:
      GPR_ASSERT(0); // Shouldn't be using this on the client
      break;
    case GRPC_OP_RECV_INITIAL_METADATA:
      st->ops[st->op_num].data.recv_initial_metadata = &st->recv_metadata;
      break;
    case GRPC_OP_RECV_MESSAGE:
      st->ops[st->op_num].data.recv_message = &st->recv_message;
      break;
    case GRPC_OP_RECV_STATUS_ON_CLIENT:
      st->ops[st->op_num].data.recv_status_on_client.trailing_metadata =
          &st->recv_trailing_metadata;
      st->ops[st->op_num].data.recv_status_on_client.status =
          &st->recv_status;
      st->ops[st->op_num].data.recv_status_on_client.status_details =
          &st->recv_status_details;
      st->ops[st->op_num].data.recv_status_on_client.status_details_capacity =
          &st->recv_status_details_capacity;
      break;
    case GRPC_OP_RECV_CLOSE_ON_SERVER:
      GPR_ASSERT(0); // Shouldn't be using this on the client
      break;
    default:
      GPR_ASSERT(0);
  };
  // for this repro, only doing one op at a time
  GPR_ASSERT(st->op_num == 0);
  st->ops[st->op_num].op = this_op;
  st->ops[st->op_num].reserved = NULL;
  st->op_num++;
}

// Run a start batch and pluck, mimicking the way it is done in ruby.
// The "GIL" gets released within the run_completion_queue_pluck call,
// and it gets taken again on the way out of that function
void grpc_run_batch(grpc_wrapped_call *wrapped_call, grpc_op_type this_op) {
  run_batch_stack st;
  grpc_call_error err;
  grpc_event ev;
  void *tag = (void*)&st;
  // no write flag
  // fake like we're in ruby with gil
  //

  grpc_run_batch_stack_init(&st, 0);
  grpc_run_batch_stack_fill_op(&st, this_op);

  err = grpc_call_start_batch(wrapped_call->wrapped, st.ops, st.op_num, tag, NULL);

  GPR_ASSERT(err == GRPC_CALL_OK);

  ev = run_completion_queue_pluck_mimick_ruby(wrapped_call->queue, tag,
                                 gpr_inf_future(GPR_CLOCK_REALTIME), NULL);

  /**** ERROR OCCURS HERE ****/
  // This always seems to fail on the SEND_CLOSE_FROM_CLIENT_OP (int value 2)
  if(!ev.success) {
    if(this_op == GRPC_OP_SEND_CLOSE_FROM_CLIENT) {
      fprintf(stderr, "completion queue pluck failed on the SEND_CLOSE_FROM_CLIENT op\n");
    }
    else {
      fprintf(stderr, "completion queue pluck failed on op type: %d\n", this_op);
    }
    exit(1);
  }

  if(this_op == GRPC_OP_RECV_STATUS_ON_CLIENT) {
    if(st.recv_status != GRPC_STATUS_OK) {
      fprintf(stderr, "got a bad status: %d\n", st.recv_status);
      exit(1);
    }
  }

  grpc_run_batch_stack_cleanup(&st);
}

// Run the core ops in the same order that ruby does for client-side unary calls
// It always seems to fail on the SEND_CLOSE_FROM_CLIENT op
void grpc_run_request_response_mimick_ruby(grpc_wrapped_call *wrapped_call) {
  grpc_run_batch(wrapped_call, GRPC_OP_SEND_INITIAL_METADATA);
  grpc_run_batch(wrapped_call, GRPC_OP_SEND_MESSAGE);
  grpc_run_batch(wrapped_call, GRPC_OP_SEND_CLOSE_FROM_CLIENT);
  grpc_run_batch(wrapped_call, GRPC_OP_RECV_INITIAL_METADATA);
  grpc_run_batch(wrapped_call, GRPC_OP_RECV_MESSAGE);
  grpc_run_batch(wrapped_call, GRPC_OP_RECV_STATUS_ON_CLIENT);
}

// runs in its own thread. runs an infinite loop that keeps making unary calls
// typically, only several calls are needed to trigger the error on pluck
// during the SEND_CLOSE_FROM_CLIENT op
void* make_calls_on_stream(void *param_channel) {
   grpc_wrapped_channel *channel = (grpc_wrapped_channel*)param_channel;
   grpc_wrapped_call* call;

   // mimick use of the GIL in ruby, this gets released in the pluck_no_gil call
   gpr_mu_lock(&fake_gil);
   fprintf(stderr, "begin making calls on stream\n");

   // keep making calls, it should crash shortly
   for(;;){
      // create new calls under lock
      fprintf(stderr, "stream %p about to create and do new call\n", &channel);
      // calls are hard coded to localhost:13000, using the benchmark unary call method
      call = grpc_channel_create_wrapped_call(channel, (char*)SERVICE_AND_METHOD
          , (char*)TARGET_HOST_PORT);

      grpc_run_request_response_mimick_ruby(call);

      // destroy old calls under lock
      destroy_call(call);
      fprintf(stderr, "stream %p just completed and destroyed call\n", &channel);
   }
   gpr_mu_unlock(&fake_gil);
   return NULL;
}

// Set up a certain number of channels and a certain number of streams per channel.
// then run separate threads for each stream, where each thread continously makes unary request
// and responses
void create_and_run_unary_calls() {
   int i, k;
   int num_streams;
   pthread_t *thread_ids;
   grpc_wrapped_channel **channels;
   num_streams = 2; // The error seems to occur frequently with just 2 channels and 2 streams

   fprintf(stderr, "using %d streams\n", num_streams);

   // mimick single-threaded GIL use as best as possible
   gpr_mu_lock(&fake_gil);

   thread_ids = gpr_malloc(sizeof(pthread_t) * num_streams);
   memset(thread_ids, 0, sizeof(pthread_t) * num_streams);

   channels = gpr_malloc(sizeof(grpc_wrapped_channel*));

   // setup channels
   channels[0] = grpc_channel_alloc_init();

   fprintf(stderr, "begin streams\n");

   // create a separate thread for each stream, that just repeatedly makes unary calls
   for(k = 0; k < num_streams; k++) {
     if(pthread_create(&thread_ids[k], NULL, make_calls_on_stream, (void*)channels[0])) {
       gpr_mu_unlock(&fake_gil);
       fprintf(stderr, "Error creating thread\n");
       exit(1);
     }
   }

   // release the "GIL" and let streams start making calls
   gpr_mu_unlock(&fake_gil);

   for(i = 0; i < num_streams; i++) {
     if(pthread_join(thread_ids[i], NULL)) {
       fprintf(stderr, "Error joining thread\n");
       exit(1);
     }
   }
   grpc_channel_wrapper_free(channels[0]);

   free(thread_ids);
}

int main() {
   grpc_init();
   gpr_mu_init(&fake_gil);
   create_and_run_unary_calls();
   gpr_mu_destroy(&fake_gil);

   return 0;
}
