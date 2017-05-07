/*
 *
 * Copyright 2015, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <ruby/ruby.h>
#include <ruby/thread.h>

#include "rb_grpc_imports.generated.h"
#include "rb_byte_buffer.h"
#include "rb_channel.h"

#include <grpc/grpc.h>
#include <grpc/grpc_security.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/time.h>
#include "rb_call.h"
#include "rb_channel_args.h"
#include "rb_channel_credentials.h"
#include "rb_completion_queue.h"
#include "rb_grpc.h"
#include "rb_server.h"

/* id_channel is the name of the hidden ivar that preserves a reference to the
 * channel on a call, so that calls are not GCed before their channel.  */
static ID id_channel;

/* id_target is the name of the hidden ivar that preserves a reference to the
 * target string used to create the call, preserved so that it does not get
 * GCed before the channel */
static ID id_target;

/* id_insecure_channel is used to indicate that a channel is insecure */
static VALUE id_insecure_channel;

/* grpc_rb_cChannel is the ruby class that proxies grpc_channel. */
static VALUE grpc_rb_cChannel = Qnil;

/* Used during the conversion of a hash to channel args during channel setup */
static VALUE grpc_rb_cChannelArgs;

/* grpc_rb_channel wraps a grpc_channel. */
typedef struct grpc_rb_channel {
  VALUE credentials;

  /* The actual channel */
  grpc_channel *wrapped;
} grpc_rb_channel;

typedef enum {
  CONTINUOUS_WATCH,
  WATCH_STATE_API
} watch_state_op_type;

typedef struct watch_state_op {
  watch_state_op_type op_type;
  // from event.success
  union {
    struct {
      int success;
      // has been called back due to a cq next call
      int called_back;
    } api_callback_args;
    struct {
      grpc_channel *wrapped_channel;
    } continuous_watch_callback_args;
  } op;
} watch_state_op;

typedef struct bg_watched_channel {
  grpc_channel *channel;
  struct bg_watched_channel *next;
  int destroyed_by_abort;
} bg_watched_channel;

bg_watched_channel *bg_watched_channel_list_head = NULL;

/* Forward declarations of functions involved in temporary fix to
 * https://github.com/grpc/grpc/issues/9941 */
static void grpc_rb_channel_try_register_connection_polling(
    grpc_channel *wrapper, int first_time_register);
static void *wait_until_channel_polling_thread_started_no_gil(void*);
static void wait_until_channel_polling_thread_started_unblocking_func(void*);

static grpc_completion_queue *channel_polling_cq;
static gpr_mu global_connection_polling_mu;
static gpr_cv global_connection_polling_cv;
static int abort_channel_polling = 0;
static int channel_polling_thread_started = 0;

static bg_watched_channel *bg_watched_channel_list_lookup_channel(grpc_channel *channel);
static void bg_watched_channel_list_add_channel(grpc_channel *channel);
static void bg_watched_channel_list_remove_channel(grpc_channel *channel);

static void grpc_rb_channel_watch_connection_state_op_complete(watch_state_op* op, int success) {
  gpr_mu_lock(&global_connection_polling_mu);
  GPR_ASSERT(!op->op.api_callback_args.called_back);
  op->op.api_callback_args.called_back = 1;
  op->op.api_callback_args.success = success;
  // there can be only one call blocked on this
  gpr_cv_broadcast(&global_connection_polling_cv);
  gpr_mu_unlock(&global_connection_polling_mu);
}

/* Avoids destroying a channel twice. */
static void grpc_rb_channel_safe_destroy(grpc_channel *channel) {
  bg_watched_channel *bg = NULL;

  gpr_mu_lock(&global_connection_polling_mu);
  bg = bg_watched_channel_list_lookup_channel(channel);
  if (bg != NULL) {
    if (!bg->destroyed_by_abort) {
      grpc_channel_destroy(channel);
    }
    bg_watched_channel_list_remove_channel(channel);
  }
  gpr_mu_unlock(&global_connection_polling_mu);
}

/* Destroys Channel instances. */
static void grpc_rb_channel_free(void *p) {
  grpc_rb_channel *ch = NULL;
  if (p == NULL) {
    return;
  };
  gpr_log(GPR_DEBUG, "channel GC function called!");
  ch = (grpc_rb_channel *)p;

  if (ch->wrapped != NULL) {
    grpc_rb_channel_safe_destroy(ch->wrapped);
    ch->wrapped = NULL;
  }

  xfree(p);
}

/* Protects the mark object from GC */
static void grpc_rb_channel_mark(void *p) {
  grpc_rb_channel *channel = NULL;
  if (p == NULL) {
    return;
  }
  channel = (grpc_rb_channel *)p;
  if (channel->credentials != Qnil) {
    rb_gc_mark(channel->credentials);
  }
}

static rb_data_type_t grpc_channel_data_type = {"grpc_channel",
                                                {grpc_rb_channel_mark,
                                                 grpc_rb_channel_free,
                                                 GRPC_RB_MEMSIZE_UNAVAILABLE,
                                                 {NULL, NULL}},
                                                NULL,
                                                NULL,
#ifdef RUBY_TYPED_FREE_IMMEDIATELY
                                                RUBY_TYPED_FREE_IMMEDIATELY
#endif
};

/* Allocates grpc_rb_channel instances. */
static VALUE grpc_rb_channel_alloc(VALUE cls) {
  grpc_rb_channel *wrapper = ALLOC(grpc_rb_channel);
  wrapper->wrapped = NULL;
  wrapper->credentials = Qnil;
  return TypedData_Wrap_Struct(cls, &grpc_channel_data_type, wrapper);
}

/*
  call-seq:
    insecure_channel = Channel:new("myhost:8080", {'arg1': 'value1'},
                                   :this_channel_is_insecure)
    creds = ...
    secure_channel = Channel:new("myhost:443", {'arg1': 'value1'}, creds)

  Creates channel instances. */
static VALUE grpc_rb_channel_init(int argc, VALUE *argv, VALUE self) {
  VALUE channel_args = Qnil;
  VALUE credentials = Qnil;
  VALUE target = Qnil;
  grpc_rb_channel *wrapper = NULL;
  grpc_channel *ch = NULL;
  grpc_channel_credentials *creds = NULL;
  char *target_chars = NULL;
  grpc_channel_args args;
  MEMZERO(&args, grpc_channel_args, 1);

  grpc_ruby_once_init();
  rb_thread_call_without_gvl(wait_until_channel_polling_thread_started_no_gil, NULL,
                             wait_until_channel_polling_thread_started_unblocking_func, NULL);

  /* "3" == 3 mandatory args */
  rb_scan_args(argc, argv, "3", &target, &channel_args, &credentials);

  TypedData_Get_Struct(self, grpc_rb_channel, &grpc_channel_data_type, wrapper);
  target_chars = StringValueCStr(target);
  grpc_rb_hash_convert_to_channel_args(channel_args, &args);
  if (TYPE(credentials) == T_SYMBOL) {
    if (id_insecure_channel != SYM2ID(credentials)) {
      rb_raise(rb_eTypeError,
               "bad creds symbol, want :this_channel_is_insecure");
      return Qnil;
    }
    ch = grpc_insecure_channel_create(target_chars, &args, NULL);
  } else {
    wrapper->credentials = credentials;
    creds = grpc_rb_get_wrapped_channel_credentials(credentials);
    ch = grpc_secure_channel_create(creds, target_chars, &args, NULL);
  }

  GPR_ASSERT(ch);

  wrapper->wrapped = ch;

  grpc_rb_channel_try_register_connection_polling(wrapper->wrapped, 1);

  if (args.args != NULL) {
    xfree(args.args); /* Allocated by grpc_rb_hash_convert_to_channel_args */
  }
  if (ch == NULL) {
    rb_raise(rb_eRuntimeError, "could not create an rpc channel to target:%s",
             target_chars);
    return Qnil;
  }
  rb_ivar_set(self, id_target, target);
  wrapper->wrapped = ch;
  return self;
}

/*
  call-seq:
    ch.connectivity_state       -> state
    ch.connectivity_state(true) -> state

  Indicates the current state of the channel, whose value is one of the
  constants defined in GRPC::Core::ConnectivityStates.

  It also tries to connect if the chennel is idle in the second form. */
static VALUE grpc_rb_channel_get_connectivity_state(int argc, VALUE *argv,
                                                    VALUE self) {
  VALUE try_to_connect_param = Qfalse;
  int grpc_try_to_connect = 0;
  grpc_rb_channel *wrapper = NULL;
  grpc_channel *ch = NULL;

  /* "01" == 0 mandatory args, 1 (try_to_connect) is optional */
  rb_scan_args(argc, argv, "01", &try_to_connect_param);
  grpc_try_to_connect = RTEST(try_to_connect_param) ? 1 : 0;

  TypedData_Get_Struct(self, grpc_rb_channel, &grpc_channel_data_type, wrapper);
  ch = wrapper->wrapped;
  if (ch == NULL) {
    rb_raise(rb_eRuntimeError, "closed!");
    return Qnil;
  }
  return LONG2NUM(grpc_channel_check_connectivity_state(wrapper->wrapped, grpc_try_to_connect));
}

typedef struct watch_state_stack {
  watch_state_op *op;
  gpr_timespec deadline;
} watch_state_stack;

static void *wait_for_watch_state_op_complete_without_gvl(void *arg) {
  watch_state_stack *stack = (watch_state_stack*)arg;
  gpr_timespec deadline = stack->deadline;
  watch_state_op *op = stack->op;
  void *success = (void*)0;

  gpr_mu_lock(&global_connection_polling_mu);
  while(!op->op.api_callback_args.called_back &&
        !abort_channel_polling &&
        gpr_time_cmp(deadline, gpr_now(GPR_CLOCK_REALTIME)) > 0) {
    gpr_cv_wait(&global_connection_polling_cv, &global_connection_polling_mu, deadline);
  }
  if (op->op.api_callback_args.called_back && op->op.api_callback_args.success) {
    success = (void*)1;
  }
  gpr_mu_unlock(&global_connection_polling_mu);

  return success;
}

static void watch_connectivity_state_unblocking_func() {
  gpr_mu_unlock(&global_connection_polling_mu);
  abort_channel_polling = 1;
  gpr_cv_broadcast(&global_connection_polling_cv);
  gpr_mu_lock(&global_connection_polling_mu);
}

/* Wait until the channel's connectivity state becomes different from
 * "last_state", or "deadline" expires.
 * Returns true if the the channel's connectivity state becomes
 * different from "last_state" within "deadline".
 * Returns false if "deadline" expires before the channel's connectivity
 * state changes from "last_state".
 * */
static VALUE grpc_rb_channel_watch_connectivity_state(VALUE self,
                                                      VALUE last_state,
                                                      VALUE deadline) {
  grpc_rb_channel *wrapper = NULL;
  watch_state_stack stack;
  watch_state_op *op = NULL;
  void* op_success = 0;

  TypedData_Get_Struct(self, grpc_rb_channel, &grpc_channel_data_type, wrapper);

  if (wrapper->wrapped == NULL) {
    rb_raise(rb_eRuntimeError, "closed!");
    return Qnil;
  }

  if (!FIXNUM_P(last_state)) {
    rb_raise(rb_eTypeError, "bad type for last_state. want a GRPC::Core::ChannelState constant");
    return Qnil;
  }

  gpr_mu_lock(&global_connection_polling_mu);
  // its unsafe to do a "watch" after "channel polling abort" because the cq has
  // been shut down.
  if (abort_channel_polling) {
    gpr_mu_unlock(&global_connection_polling_mu);
    return Qfalse;
  }
  op = gpr_zalloc(sizeof(watch_state_op));
  op->op_type = WATCH_STATE_API;
  grpc_channel_watch_connectivity_state(
      wrapper->wrapped, NUM2LONG(last_state), grpc_rb_time_timeval(deadline, 0), channel_polling_cq, op);
  gpr_mu_unlock(&global_connection_polling_mu);

  stack.op = op;
  stack.deadline = grpc_rb_time_timeval(deadline, 0);

  // Rely on the aborting or lack-of-starting of the main "channel watching"
  // thread background loop to cancel and finish early if needed.
  op_success = rb_thread_call_without_gvl(
      wait_for_watch_state_op_complete_without_gvl, &stack, watch_connectivity_state_unblocking_func, NULL);
  gpr_free(op);

  return op_success ? Qtrue : Qfalse;
}

/* Create a call given a grpc_channel, in order to call method. The request
   is not sent until grpc_call_invoke is called. */
static VALUE grpc_rb_channel_create_call(VALUE self, VALUE parent, VALUE mask,
                                         VALUE method, VALUE host,
                                         VALUE deadline) {
  VALUE res = Qnil;
  grpc_rb_channel *wrapper = NULL;
  grpc_call *call = NULL;
  grpc_call *parent_call = NULL;
  grpc_channel *ch = NULL;
  grpc_completion_queue *cq = NULL;
  int flags = GRPC_PROPAGATE_DEFAULTS;
  grpc_slice method_slice;
  grpc_slice host_slice;
  grpc_slice *host_slice_ptr = NULL;
  char *tmp_str = NULL;

  if (host != Qnil) {
    host_slice =
        grpc_slice_from_copied_buffer(RSTRING_PTR(host), RSTRING_LEN(host));
    host_slice_ptr = &host_slice;
  }
  if (mask != Qnil) {
    flags = NUM2UINT(mask);
  }
  if (parent != Qnil) {
    parent_call = grpc_rb_get_wrapped_call(parent);
  }

  cq = grpc_completion_queue_create(NULL);
  TypedData_Get_Struct(self, grpc_rb_channel, &grpc_channel_data_type, wrapper);
  ch = wrapper->wrapped;
  if (ch == NULL) {
    rb_raise(rb_eRuntimeError, "closed!");
    return Qnil;
  }

  method_slice =
      grpc_slice_from_copied_buffer(RSTRING_PTR(method), RSTRING_LEN(method));

  call = grpc_channel_create_call(ch, parent_call, flags, cq, method_slice,
                                  host_slice_ptr,
                                  grpc_rb_time_timeval(deadline,
                                                       /* absolute time */ 0),
                                  NULL);

  if (call == NULL) {
    tmp_str = grpc_slice_to_c_string(method_slice);
    rb_raise(rb_eRuntimeError, "cannot create call with method %s", tmp_str);
    return Qnil;
  }

  grpc_slice_unref(method_slice);
  if (host_slice_ptr != NULL) {
    grpc_slice_unref(host_slice);
  }

  res = grpc_rb_wrap_call(call, cq);

  /* Make this channel an instance attribute of the call so that it is not GCed
   * before the call. */
  rb_ivar_set(res, id_channel, self);
  return res;
}

/* Closes the channel, calling it's destroy method */
static VALUE grpc_rb_channel_destroy(VALUE self) {
  grpc_rb_channel *wrapper = NULL;
  grpc_channel *ch = NULL;

  TypedData_Get_Struct(self, grpc_rb_channel, &grpc_channel_data_type, wrapper);
  ch = wrapper->wrapped;
  if (ch != NULL) {
    grpc_rb_channel_safe_destroy(ch);
    wrapper->wrapped = NULL;
  }

  return Qnil;
}

/* Called to obtain the target that this channel accesses. */
static VALUE grpc_rb_channel_get_target(VALUE self) {
  grpc_rb_channel *wrapper = NULL;
  VALUE res = Qnil;
  char *target = NULL;

  TypedData_Get_Struct(self, grpc_rb_channel, &grpc_channel_data_type, wrapper);
  target = grpc_channel_get_target(wrapper->wrapped);
  res = rb_str_new2(target);
  gpr_free(target);

  return res;
}

/* Needs to be called under global_connection_polling_mu */
static bg_watched_channel *bg_watched_channel_list_lookup_channel(grpc_channel *channel) {
  bg_watched_channel *watched = bg_watched_channel_list_head;

  gpr_log(GPR_DEBUG, "check contains");
  while (watched != NULL) {
    if (watched->channel == channel) {
      return watched;
    }
    watched = watched->next;
  }

  return NULL;
}

/* Needs to be called under global_connection_polling_mu */
static void bg_watched_channel_list_add_channel(grpc_channel *channel) {
  bg_watched_channel *watched = gpr_zalloc(sizeof(bg_watched_channel));

  gpr_log(GPR_DEBUG, "add bg");
  GPR_ASSERT(!bg_watched_channel_list_lookup_channel(channel));
  watched->channel = channel;
  watched->next = bg_watched_channel_list_head;
  bg_watched_channel_list_head = watched;
}

/* Needs to be called under global_connection_polling_mu */
static void bg_watched_channel_list_remove_channel(grpc_channel *channel) {
  bg_watched_channel *bg = NULL;

  gpr_log(GPR_DEBUG, "remove bg");
  GPR_ASSERT(bg_watched_channel_list_lookup_channel(channel));
  if (bg_watched_channel_list_head->channel == channel) {
    bg = bg_watched_channel_list_head;
    bg_watched_channel_list_head = bg_watched_channel_list_head->next;
    gpr_free(bg);
    return;
  }
  bg = bg_watched_channel_list_head;
  while (bg != NULL && bg->next != NULL) {
    if (bg->next->channel == channel) {
      bg->next = bg->next->next;
      gpr_free(bg->next);
      return;
    }
    bg = bg->next;
  }
  GPR_ASSERT(0);
}

// Either start polling channel connection state or signal that it's free to
// destroy.
// Not safe to call while a channel's connection state is polled.
static void grpc_rb_channel_try_register_connection_polling(
  grpc_channel *wrapped_channel, int first_time_register) {
  grpc_connectivity_state conn_state;
  watch_state_op *op = NULL;

  gpr_mu_lock(&global_connection_polling_mu);
  GPR_ASSERT(channel_polling_thread_started || abort_channel_polling);
  conn_state = grpc_channel_check_connectivity_state(wrapped_channel, 0);
  // avoid posting work to the channel polling cq if it's been shutdown
  if (!abort_channel_polling && conn_state != GRPC_CHANNEL_SHUTDOWN) {
    if (first_time_register) {
      GPR_ASSERT(!bg_watched_channel_list_lookup_channel(wrapped_channel));
      bg_watched_channel_list_add_channel(wrapped_channel);
    } else {
      GPR_ASSERT(bg_watched_channel_list_lookup_channel(wrapped_channel));
    }
    op = gpr_zalloc(sizeof(watch_state_op));
    op->op_type = CONTINUOUS_WATCH;
    op->op.continuous_watch_callback_args.wrapped_channel = wrapped_channel;
    grpc_channel_watch_connectivity_state(
        wrapped_channel, conn_state, gpr_inf_future(GPR_CLOCK_REALTIME), channel_polling_cq, op);
  }
  gpr_mu_unlock(&global_connection_polling_mu);
}

// Note this loop breaks out with a single call of
// "run_poll_channels_loop_no_gil".
// This assumes that a ruby call the unblocking func
// indicates process shutdown.
// In the worst case, this stops polling channel connectivity
// early and falls back to current behavior.
static void *run_poll_channels_loop_no_gil(void *arg) {
  grpc_event event;
  watch_state_op *op = NULL;
  (void)arg;
  gpr_log(GPR_DEBUG, "GRPC_RUBY: run_poll_channels_loop_no_gil - begin");

  gpr_mu_lock(&global_connection_polling_mu);
  GPR_ASSERT(!channel_polling_thread_started);
  channel_polling_thread_started = 1;
  gpr_cv_broadcast(&global_connection_polling_cv);
  gpr_mu_unlock(&global_connection_polling_mu);

  for (;;) {
    event = grpc_completion_queue_next(
        channel_polling_cq, gpr_inf_future(GPR_CLOCK_REALTIME), NULL);
    if (event.type == GRPC_QUEUE_SHUTDOWN) {
      break;
    }
    if (event.type == GRPC_OP_COMPLETE) {
      op = (watch_state_op*)event.tag;
      if (op->op_type == CONTINUOUS_WATCH) {
        grpc_rb_channel_try_register_connection_polling((grpc_channel *)op->op.continuous_watch_callback_args.wrapped_channel, 0);
        gpr_free(op);
      } else if(op->op_type == WATCH_STATE_API) {
        grpc_rb_channel_watch_connection_state_op_complete((watch_state_op*)event.tag, event.success);
      } else {
        GPR_ASSERT(0);
      }
    }
  }
  grpc_completion_queue_destroy(channel_polling_cq);
  gpr_log(GPR_DEBUG, "GRPC_RUBY: run_poll_channels_loop_no_gil - exit connection polling loop");
  return NULL;
}

// Notify the channel polling loop to cleanup and shutdown.
static void run_poll_channels_loop_unblocking_func(void *arg) {
  bg_watched_channel *bg = NULL;
  (void)arg;

  gpr_mu_lock(&global_connection_polling_mu);
  gpr_log(GPR_DEBUG, "GRPC_RUBY: run_poll_channels_loop_unblocking_func - begin aborting connection polling");
  abort_channel_polling = 1;

  // force pending watches to end by switching to shutdown state
  bg = bg_watched_channel_list_head;
  while(bg != NULL) {
    grpc_channel_destroy(bg->channel);
    bg->destroyed_by_abort = 1;
    bg = bg->next;
  }

  grpc_completion_queue_shutdown(channel_polling_cq);
  gpr_cv_broadcast(&global_connection_polling_cv);
  gpr_mu_unlock(&global_connection_polling_mu);
  gpr_log(GPR_DEBUG, "GRPC_RUBY: run_poll_channels_loop_unblocking_func - begin aborting connection polling 22222");
}

// Poll channel connectivity states in background thread without the GIL.
static VALUE run_poll_channels_loop(VALUE arg) {
  (void)arg;
  gpr_log(GPR_DEBUG, "GRPC_RUBY: run_poll_channels_loop - create connection polling thread");
  rb_thread_call_without_gvl(run_poll_channels_loop_no_gil, NULL,
                             run_poll_channels_loop_unblocking_func, NULL);

  return Qnil;
}

static void *wait_until_channel_polling_thread_started_no_gil(void *arg) {
  (void)arg;
  gpr_log(GPR_DEBUG, "GRPC_RUBY: wait for channel polling thread to start");
  gpr_mu_lock(&global_connection_polling_mu);
  while (!channel_polling_thread_started && !abort_channel_polling) {
    gpr_cv_wait(&global_connection_polling_cv, &global_connection_polling_mu,
                gpr_inf_future(GPR_CLOCK_REALTIME));
  }
  gpr_mu_unlock(&global_connection_polling_mu);

  return NULL;
}

static void wait_until_channel_polling_thread_started_unblocking_func(void* arg) {
  (void)arg;
  gpr_mu_lock(&global_connection_polling_mu);
  gpr_log(GPR_DEBUG, "GRPC_RUBY: wait_until_channel_polling_thread_started_unblocking_func - begin aborting connection polling");
  abort_channel_polling = 1;
  gpr_cv_broadcast(&global_connection_polling_cv);
  gpr_mu_unlock(&global_connection_polling_mu);
}

/* Temporary fix for
 * https://github.com/GoogleCloudPlatform/google-cloud-ruby/issues/899.
 * Transports in idle channels can get destroyed. Normally c-core re-connects,
 * but in grpc-ruby core never gets a thread until an RPC is made, because ruby
 * only calls c-core's "completion_queu_pluck" API.
 * This uses a global background thread that calls
 * "completion_queue_next" on registered "watch_channel_connectivity_state"
 * calls - so that c-core can reconnect if needed, when there aren't any RPC's.
 * TODO(apolcyn) remove this when core handles new RPCs on dead connections.
 */
void grpc_rb_channel_polling_thread_start() {
  VALUE background_thread = Qnil;

  GPR_ASSERT(!abort_channel_polling);
  GPR_ASSERT(!channel_polling_thread_started);
  GPR_ASSERT(channel_polling_cq == NULL);

  gpr_mu_init(&global_connection_polling_mu);
  gpr_cv_init(&global_connection_polling_cv);

  channel_polling_cq = grpc_completion_queue_create(NULL);
  background_thread = rb_thread_create(run_poll_channels_loop, NULL);

  if (!RTEST(background_thread)) {
    gpr_log(GPR_DEBUG, "GRPC_RUBY: failed to spawn channel polling thread");
    gpr_mu_lock(&global_connection_polling_mu);
    abort_channel_polling = 1;
    gpr_cv_broadcast(&global_connection_polling_cv);
    gpr_mu_unlock(&global_connection_polling_mu);
  }
}

static void Init_grpc_propagate_masks() {
  /* Constants representing call propagation masks in grpc.h */
  VALUE grpc_rb_mPropagateMasks =
      rb_define_module_under(grpc_rb_mGrpcCore, "PropagateMasks");
  rb_define_const(grpc_rb_mPropagateMasks, "DEADLINE",
                  UINT2NUM(GRPC_PROPAGATE_DEADLINE));
  rb_define_const(grpc_rb_mPropagateMasks, "CENSUS_STATS_CONTEXT",
                  UINT2NUM(GRPC_PROPAGATE_CENSUS_STATS_CONTEXT));
  rb_define_const(grpc_rb_mPropagateMasks, "CENSUS_TRACING_CONTEXT",
                  UINT2NUM(GRPC_PROPAGATE_CENSUS_TRACING_CONTEXT));
  rb_define_const(grpc_rb_mPropagateMasks, "CANCELLATION",
                  UINT2NUM(GRPC_PROPAGATE_CANCELLATION));
  rb_define_const(grpc_rb_mPropagateMasks, "DEFAULTS",
                  UINT2NUM(GRPC_PROPAGATE_DEFAULTS));
}

static void Init_grpc_connectivity_states() {
  /* Constants representing call propagation masks in grpc.h */
  VALUE grpc_rb_mConnectivityStates =
      rb_define_module_under(grpc_rb_mGrpcCore, "ConnectivityStates");
  rb_define_const(grpc_rb_mConnectivityStates, "IDLE",
                  LONG2NUM(GRPC_CHANNEL_IDLE));
  rb_define_const(grpc_rb_mConnectivityStates, "CONNECTING",
                  LONG2NUM(GRPC_CHANNEL_CONNECTING));
  rb_define_const(grpc_rb_mConnectivityStates, "READY",
                  LONG2NUM(GRPC_CHANNEL_READY));
  rb_define_const(grpc_rb_mConnectivityStates, "TRANSIENT_FAILURE",
                  LONG2NUM(GRPC_CHANNEL_TRANSIENT_FAILURE));
  rb_define_const(grpc_rb_mConnectivityStates, "FATAL_FAILURE",
                  LONG2NUM(GRPC_CHANNEL_SHUTDOWN));
}

void Init_grpc_channel() {
  grpc_rb_cChannelArgs = rb_define_class("TmpChannelArgs", rb_cObject);
  grpc_rb_cChannel =
      rb_define_class_under(grpc_rb_mGrpcCore, "Channel", rb_cObject);

  /* Allocates an object managed by the ruby runtime */
  rb_define_alloc_func(grpc_rb_cChannel, grpc_rb_channel_alloc);

  /* Provides a ruby constructor and support for dup/clone. */
  rb_define_method(grpc_rb_cChannel, "initialize", grpc_rb_channel_init, -1);
  rb_define_method(grpc_rb_cChannel, "initialize_copy",
                   grpc_rb_cannot_init_copy, 1);

  /* Add ruby analogues of the Channel methods. */
  rb_define_method(grpc_rb_cChannel, "connectivity_state",
                   grpc_rb_channel_get_connectivity_state, -1);
  rb_define_method(grpc_rb_cChannel, "watch_connectivity_state",
                   grpc_rb_channel_watch_connectivity_state, 2);
  rb_define_method(grpc_rb_cChannel, "create_call", grpc_rb_channel_create_call,
                   5);
  rb_define_method(grpc_rb_cChannel, "target", grpc_rb_channel_get_target, 0);
  rb_define_method(grpc_rb_cChannel, "destroy", grpc_rb_channel_destroy, 0);
  rb_define_alias(grpc_rb_cChannel, "close", "destroy");

  id_channel = rb_intern("__channel");
  id_target = rb_intern("__target");
  rb_define_const(grpc_rb_cChannel, "SSL_TARGET",
                  ID2SYM(rb_intern(GRPC_SSL_TARGET_NAME_OVERRIDE_ARG)));
  rb_define_const(grpc_rb_cChannel, "ENABLE_CENSUS",
                  ID2SYM(rb_intern(GRPC_ARG_ENABLE_CENSUS)));
  rb_define_const(grpc_rb_cChannel, "MAX_CONCURRENT_STREAMS",
                  ID2SYM(rb_intern(GRPC_ARG_MAX_CONCURRENT_STREAMS)));
  rb_define_const(grpc_rb_cChannel, "MAX_MESSAGE_LENGTH",
                  ID2SYM(rb_intern(GRPC_ARG_MAX_RECEIVE_MESSAGE_LENGTH)));
  id_insecure_channel = rb_intern("this_channel_is_insecure");
  Init_grpc_propagate_masks();
  Init_grpc_connectivity_states();
}

/* Gets the wrapped channel from the ruby wrapper */
grpc_channel *grpc_rb_get_wrapped_channel(VALUE v) {
  grpc_rb_channel *wrapper = NULL;
  TypedData_Get_Struct(v, grpc_rb_channel, &grpc_channel_data_type, wrapper);
  return wrapper->wrapped;
}
