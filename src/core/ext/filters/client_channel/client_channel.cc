//
// Copyright 2015 gRPC authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include <grpc/support/port_platform.h>

#include "src/core/ext/filters/client_channel/client_channel.h"

#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <set>

#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"

#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>
#include <grpc/support/sync.h>

#include "absl/container/inlined_vector.h"
#include "absl/types/optional.h"

#include "src/core/ext/filters/client_channel/backend_metric.h"
#include "src/core/ext/filters/client_channel/backup_poller.h"
#include "src/core/ext/filters/client_channel/config_selector.h"
#include "src/core/ext/filters/client_channel/dynamic_filters.h"
#include "src/core/ext/filters/client_channel/global_subchannel_pool.h"
#include "src/core/ext/filters/client_channel/http_connect_handshaker.h"
#include "src/core/ext/filters/client_channel/lb_policy/child_policy_handler.h"
#include "src/core/ext/filters/client_channel/lb_policy_registry.h"
#include "src/core/ext/filters/client_channel/local_subchannel_pool.h"
#include "src/core/ext/filters/client_channel/proxy_mapper_registry.h"
#include "src/core/ext/filters/client_channel/resolver_registry.h"
#include "src/core/ext/filters/client_channel/resolver_result_parsing.h"
#include "src/core/ext/filters/client_channel/retry_throttle.h"
#include "src/core/ext/filters/client_channel/service_config.h"
#include "src/core/ext/filters/client_channel/service_config_call_data.h"
#include "src/core/ext/filters/client_channel/subchannel.h"
#include "src/core/ext/filters/deadline/deadline_filter.h"
#include "src/core/lib/backoff/backoff.h"
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/channel/connected_channel.h"
#include "src/core/lib/channel/status_util.h"
#include "src/core/lib/gpr/string.h"
#include "src/core/lib/gprpp/manual_constructor.h"
#include "src/core/lib/gprpp/sync.h"
#include "src/core/lib/iomgr/iomgr.h"
#include "src/core/lib/iomgr/polling_entity.h"
#include "src/core/lib/iomgr/work_serializer.h"
#include "src/core/lib/profiling/timers.h"
#include "src/core/lib/slice/slice_internal.h"
#include "src/core/lib/slice/slice_string_helpers.h"
#include "src/core/lib/surface/channel.h"
#include "src/core/lib/transport/connectivity_state.h"
#include "src/core/lib/transport/error_utils.h"
#include "src/core/lib/transport/metadata.h"
#include "src/core/lib/transport/metadata_batch.h"
#include "src/core/lib/transport/static_metadata.h"
#include "src/core/lib/transport/status_metadata.h"

//
// Client channel filter
//

// By default, we buffer 256 KiB per RPC for retries.
// TODO(roth): Do we have any data to suggest a better value?
#define DEFAULT_PER_RPC_RETRY_BUFFER_SIZE (256 << 10)

// This value was picked arbitrarily.  It can be changed if there is
// any even moderately compelling reason to do so.
#define RETRY_BACKOFF_JITTER 0.2

// Max number of batches that can be pending on a call at any given
// time.  This includes one batch for each of the following ops:
//   recv_initial_metadata
//   send_initial_metadata
//   recv_message
//   send_message
//   recv_trailing_metadata
//   send_trailing_metadata
#define MAX_PENDING_BATCHES 6

// Channel arg containing a pointer to the ChannelData object.
#define GRPC_ARG_CLIENT_CHANNEL_DATA "grpc.internal.client_channel_data"

// Channel arg containing a pointer to the RetryThrottleData object.
#define GRPC_ARG_RETRY_THROTTLE_DATA "grpc.internal.retry_throttle_data"

namespace grpc_core {

using internal::ClientChannelGlobalParsedConfig;
using internal::ClientChannelMethodParsedConfig;
using internal::ClientChannelServiceConfigParser;
using internal::ServerRetryThrottleData;

TraceFlag grpc_client_channel_call_trace(false, "client_channel_call");
TraceFlag grpc_client_channel_routing_trace(false, "client_channel_routing");

namespace {

//
// ChannelData definition
//

class LoadBalancedCall;

class ChannelData {
 public:
  struct ResolverQueuedCall {
    grpc_call_element* elem;
    ResolverQueuedCall* next = nullptr;
  };
  struct LbQueuedCall {
    LoadBalancedCall* lb_call;
    LbQueuedCall* next = nullptr;
  };

  static grpc_error* Init(grpc_channel_element* elem,
                          grpc_channel_element_args* args);
  static void Destroy(grpc_channel_element* elem);
  static void StartTransportOp(grpc_channel_element* elem,
                               grpc_transport_op* op);
  static void GetChannelInfo(grpc_channel_element* elem,
                             const grpc_channel_info* info);

  bool deadline_checking_enabled() const { return deadline_checking_enabled_; }
  bool enable_retries() const { return enable_retries_; }
  size_t per_rpc_retry_buffer_size() const {
    return per_rpc_retry_buffer_size_;
  }
  grpc_channel_stack* owning_stack() const { return owning_stack_; }

  // Note: Does NOT return a new ref.
  grpc_error* disconnect_error() const {
    return disconnect_error_.Load(MemoryOrder::ACQUIRE);
  }

  Mutex* resolution_mu() const { return &resolution_mu_; }
  // These methods all require holding resolution_mu_.
  void AddResolverQueuedCall(ResolverQueuedCall* call,
                             grpc_polling_entity* pollent);
  void RemoveResolverQueuedCall(ResolverQueuedCall* to_remove,
                                grpc_polling_entity* pollent);
  bool received_service_config_data() const {
    return received_service_config_data_;
  }
  grpc_error* resolver_transient_failure_error() const {
    return resolver_transient_failure_error_;
  }
  RefCountedPtr<ServiceConfig> service_config() const {
    return service_config_;
  }
  ConfigSelector* config_selector() const { return config_selector_.get(); }
  RefCountedPtr<DynamicFilters> dynamic_filters() const {
    return dynamic_filters_;
  }

  Mutex* data_plane_mu() const { return &data_plane_mu_; }
  // These methods all require holding data_plane_mu_.
  LoadBalancingPolicy::SubchannelPicker* picker() const {
    return picker_.get();
  }
  void AddLbQueuedCall(LbQueuedCall* call, grpc_polling_entity* pollent);
  void RemoveLbQueuedCall(LbQueuedCall* to_remove,
                          grpc_polling_entity* pollent);
  RefCountedPtr<ConnectedSubchannel> GetConnectedSubchannelInDataPlane(
      SubchannelInterface* subchannel) const;

  WorkSerializer* work_serializer() const { return work_serializer_.get(); }

  grpc_connectivity_state CheckConnectivityState(bool try_to_connect);

  void AddExternalConnectivityWatcher(grpc_polling_entity pollent,
                                      grpc_connectivity_state* state,
                                      grpc_closure* on_complete,
                                      grpc_closure* watcher_timer_init) {
    new ExternalConnectivityWatcher(this, pollent, state, on_complete,
                                    watcher_timer_init);
  }

  void RemoveExternalConnectivityWatcher(grpc_closure* on_complete,
                                         bool cancel) {
    ExternalConnectivityWatcher::RemoveWatcherFromExternalWatchersMap(
        this, on_complete, cancel);
  }

  int NumExternalConnectivityWatchers() const {
    MutexLock lock(&external_watchers_mu_);
    return static_cast<int>(external_watchers_.size());
  }

  void AddConnectivityWatcher(
      grpc_connectivity_state initial_state,
      OrphanablePtr<AsyncConnectivityStateWatcherInterface> watcher);
  void RemoveConnectivityWatcher(
      AsyncConnectivityStateWatcherInterface* watcher);

 private:
  class SubchannelWrapper;
  class ClientChannelControlHelper;
  class ConnectivityWatcherAdder;
  class ConnectivityWatcherRemover;

  // Represents a pending connectivity callback from an external caller
  // via grpc_client_channel_watch_connectivity_state().
  class ExternalConnectivityWatcher : public ConnectivityStateWatcherInterface {
   public:
    ExternalConnectivityWatcher(ChannelData* chand, grpc_polling_entity pollent,
                                grpc_connectivity_state* state,
                                grpc_closure* on_complete,
                                grpc_closure* watcher_timer_init);

    ~ExternalConnectivityWatcher() override;

    // Removes the watcher from the external_watchers_ map.
    static void RemoveWatcherFromExternalWatchersMap(ChannelData* chand,
                                                     grpc_closure* on_complete,
                                                     bool cancel);

    void Notify(grpc_connectivity_state state,
                const absl::Status& /* status */) override;

    void Cancel();

   private:
    // Adds the watcher to state_tracker_. Consumes the ref that is passed to it
    // from Start().
    void AddWatcherLocked();
    void RemoveWatcherLocked();

    ChannelData* chand_;
    grpc_polling_entity pollent_;
    grpc_connectivity_state initial_state_;
    grpc_connectivity_state* state_;
    grpc_closure* on_complete_;
    grpc_closure* watcher_timer_init_;
    Atomic<bool> done_{false};
  };

  class ResolverResultHandler : public Resolver::ResultHandler {
   public:
    explicit ResolverResultHandler(ChannelData* chand) : chand_(chand) {
      GRPC_CHANNEL_STACK_REF(chand_->owning_stack_, "ResolverResultHandler");
    }

    ~ResolverResultHandler() override {
      if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
        gpr_log(GPR_INFO, "chand=%p: resolver shutdown complete", chand_);
      }
      GRPC_CHANNEL_STACK_UNREF(chand_->owning_stack_, "ResolverResultHandler");
    }

    void ReturnResult(Resolver::Result result) override {
      chand_->OnResolverResultChangedLocked(std::move(result));
    }

    void ReturnError(grpc_error* error) override {
      chand_->OnResolverErrorLocked(error);
    }

   private:
    ChannelData* chand_;
  };

  ChannelData(grpc_channel_element_args* args, grpc_error** error);
  ~ChannelData();

  // Note: All methods with "Locked" suffix must be invoked from within
  // work_serializer_.

  void OnResolverResultChangedLocked(Resolver::Result result);
  void OnResolverErrorLocked(grpc_error* error);

  void CreateOrUpdateLbPolicyLocked(
      RefCountedPtr<LoadBalancingPolicy::Config> lb_policy_config,
      Resolver::Result result);
  OrphanablePtr<LoadBalancingPolicy> CreateLbPolicyLocked(
      const grpc_channel_args& args);

  void UpdateStateAndPickerLocked(
      grpc_connectivity_state state, const absl::Status& status,
      const char* reason,
      std::unique_ptr<LoadBalancingPolicy::SubchannelPicker> picker);

  void UpdateServiceConfigInControlPlaneLocked(
      RefCountedPtr<ServiceConfig> service_config,
      RefCountedPtr<ConfigSelector> config_selector,
      const internal::ClientChannelGlobalParsedConfig* parsed_service_config,
      const char* lb_policy_name);

  void UpdateServiceConfigInDataPlaneLocked();

  void CreateResolverLocked();
  void DestroyResolverAndLbPolicyLocked();

  grpc_error* DoPingLocked(grpc_transport_op* op);

  void StartTransportOpLocked(grpc_transport_op* op);

  void TryToConnectLocked();

  //
  // Fields set at construction and never modified.
  //
  const bool deadline_checking_enabled_;
  const bool enable_retries_;
  const size_t per_rpc_retry_buffer_size_;
  grpc_channel_stack* owning_stack_;
  ClientChannelFactory* client_channel_factory_;
  const grpc_channel_args* channel_args_;
  RefCountedPtr<ServiceConfig> default_service_config_;
  std::string server_name_;
  UniquePtr<char> target_uri_;
  channelz::ChannelNode* channelz_node_;

  //
  // Fields related to name resolution.  Guarded by resolution_mu_.
  //
  mutable Mutex resolution_mu_;
  // Linked list of calls queued waiting for resolver result.
  ResolverQueuedCall* resolver_queued_calls_ = nullptr;
  // Data from service config.
  grpc_error* resolver_transient_failure_error_ = GRPC_ERROR_NONE;
  bool received_service_config_data_ = false;
  RefCountedPtr<ServiceConfig> service_config_;
  RefCountedPtr<ConfigSelector> config_selector_;
  RefCountedPtr<DynamicFilters> dynamic_filters_;

  //
  // Fields used in the data plane.  Guarded by data_plane_mu_.
  //
  mutable Mutex data_plane_mu_;
  std::unique_ptr<LoadBalancingPolicy::SubchannelPicker> picker_;
  // Linked list of calls queued waiting for LB pick.
  LbQueuedCall* lb_queued_calls_ = nullptr;

  //
  // Fields used in the control plane.  Guarded by work_serializer.
  //
  std::shared_ptr<WorkSerializer> work_serializer_;
  grpc_pollset_set* interested_parties_;
  ConnectivityStateTracker state_tracker_;
  OrphanablePtr<Resolver> resolver_;
  bool previous_resolution_contained_addresses_ = false;
  RefCountedPtr<ServiceConfig> saved_service_config_;
  RefCountedPtr<ConfigSelector> saved_config_selector_;
  absl::optional<std::string> health_check_service_name_;
  OrphanablePtr<LoadBalancingPolicy> lb_policy_;
  RefCountedPtr<SubchannelPoolInterface> subchannel_pool_;
  // The number of SubchannelWrapper instances referencing a given Subchannel.
  std::map<Subchannel*, int> subchannel_refcount_map_;
  // The set of SubchannelWrappers that currently exist.
  // No need to hold a ref, since the map is updated in the control-plane
  // work_serializer when the SubchannelWrappers are created and destroyed.
  std::set<SubchannelWrapper*> subchannel_wrappers_;
  // Pending ConnectedSubchannel updates for each SubchannelWrapper.
  // Updates are queued here in the control plane work_serializer and then
  // applied in the data plane mutex when the picker is updated.
  std::map<RefCountedPtr<SubchannelWrapper>, RefCountedPtr<ConnectedSubchannel>>
      pending_subchannel_updates_;
  int keepalive_time_ = -1;

  //
  // Fields accessed from both data plane mutex and control plane
  // work_serializer.
  //
  Atomic<grpc_error*> disconnect_error_;

  //
  // Fields guarded by a mutex, since they need to be accessed
  // synchronously via get_channel_info().
  //
  Mutex info_mu_;
  UniquePtr<char> info_lb_policy_name_;
  UniquePtr<char> info_service_config_json_;

  //
  // Fields guarded by a mutex, since they need to be accessed
  // synchronously via grpc_channel_num_external_connectivity_watchers().
  //
  mutable Mutex external_watchers_mu_;
  std::map<grpc_closure*, RefCountedPtr<ExternalConnectivityWatcher>>
      external_watchers_;
};

//
// CallData definition
//

class CallData {
 public:
  static grpc_error* Init(grpc_call_element* elem,
                          const grpc_call_element_args* args);
  static void Destroy(grpc_call_element* elem,
                      const grpc_call_final_info* final_info,
                      grpc_closure* then_schedule_closure);
  static void StartTransportStreamOpBatch(
      grpc_call_element* elem, grpc_transport_stream_op_batch* batch);
  static void SetPollent(grpc_call_element* elem, grpc_polling_entity* pollent);

  // Invoked by channel for queued calls when name resolution is completed.
  static void CheckResolution(void* arg, grpc_error* error);
  // Helper function for applying the service config to a call while
  // holding ChannelData::resolution_mu_.
  // Returns true if the service config has been applied to the call, in which
  // case the caller must invoke ResolutionDone() or AsyncResolutionDone()
  // with the returned error.
  bool CheckResolutionLocked(grpc_call_element* elem, grpc_error** error);
  // Schedules a callback to continue processing the call once
  // resolution is complete.  The callback will not run until after this
  // method returns.
  void AsyncResolutionDone(grpc_call_element* elem, grpc_error* error);

 private:
  class ResolverQueuedCallCanceller;

  CallData(grpc_call_element* elem, const ChannelData& chand,
           const grpc_call_element_args& args);
  ~CallData();

  // Returns the index into pending_batches_ to be used for batch.
  static size_t GetBatchIndex(grpc_transport_stream_op_batch* batch);
  void PendingBatchesAdd(grpc_call_element* elem,
                         grpc_transport_stream_op_batch* batch);
  static void FailPendingBatchInCallCombiner(void* arg, grpc_error* error);
  // A predicate type and some useful implementations for PendingBatchesFail().
  typedef bool (*YieldCallCombinerPredicate)(
      const CallCombinerClosureList& closures);
  static bool YieldCallCombiner(const CallCombinerClosureList& /*closures*/) {
    return true;
  }
  static bool NoYieldCallCombiner(const CallCombinerClosureList& /*closures*/) {
    return false;
  }
  static bool YieldCallCombinerIfPendingBatchesFound(
      const CallCombinerClosureList& closures) {
    return closures.size() > 0;
  }
  // Fails all pending batches.
  // If yield_call_combiner_predicate returns true, assumes responsibility for
  // yielding the call combiner.
  void PendingBatchesFail(
      grpc_call_element* elem, grpc_error* error,
      YieldCallCombinerPredicate yield_call_combiner_predicate);
  static void ResumePendingBatchInCallCombiner(void* arg, grpc_error* ignored);
  // Resumes all pending batches on lb_call_.
  void PendingBatchesResume(grpc_call_element* elem);

  // Applies service config to the call.  Must be invoked once we know
  // that the resolver has returned results to the channel.
  // If an error is returned, the error indicates the status with which
  // the call should be failed.
  grpc_error* ApplyServiceConfigToCallLocked(
      grpc_call_element* elem, grpc_metadata_batch* initial_metadata);
  // Invoked when the resolver result is applied to the caller, on both
  // success or failure.
  static void ResolutionDone(void* arg, grpc_error* error);
  // Removes the call (if present) from the channel's list of calls queued
  // for name resolution.
  void MaybeRemoveCallFromResolverQueuedCallsLocked(grpc_call_element* elem);
  // Adds the call (if not already present) to the channel's list of
  // calls queued for name resolution.
  void MaybeAddCallToResolverQueuedCallsLocked(grpc_call_element* elem);

  static void RecvInitialMetadataReadyForConfigSelectorCommitCallback(
      void* arg, grpc_error* error);
  void InjectRecvInitialMetadataReadyForConfigSelectorCommitCallback(
      grpc_transport_stream_op_batch* batch);

  void CreateDynamicCall(grpc_call_element* elem);

  // State for handling deadlines.
  // The code in deadline_filter.c requires this to be the first field.
  // TODO(roth): This is slightly sub-optimal in that grpc_deadline_state
  // and this struct both independently store pointers to the call stack
  // and call combiner.  If/when we have time, find a way to avoid this
  // without breaking the grpc_deadline_state abstraction.
  grpc_deadline_state deadline_state_;

  grpc_slice path_;  // Request path.
  gpr_cycle_counter call_start_time_;
  grpc_millis deadline_;
  Arena* arena_;
  grpc_call_stack* owning_call_;
  CallCombiner* call_combiner_;
  grpc_call_context_element* call_context_;

  grpc_polling_entity* pollent_ = nullptr;

  grpc_closure pick_closure_;

  // Accessed while holding ChannelData::resolution_mu_.
  bool service_config_applied_ = false;
  bool queued_pending_resolver_result_ = false;
  ChannelData::ResolverQueuedCall resolver_queued_call_;
  ResolverQueuedCallCanceller* resolver_call_canceller_ = nullptr;

  std::function<void()> on_call_committed_;

  grpc_closure* original_recv_initial_metadata_ready_ = nullptr;
  grpc_closure recv_initial_metadata_ready_;

  RefCountedPtr<DynamicFilters> dynamic_filters_;
  RefCountedPtr<DynamicFilters::Call> dynamic_call_;

  // Batches are added to this list when received from above.
  // They are removed when we are done handling the batch (i.e., when
  // either we have invoked all of the batch's callbacks or we have
  // passed the batch down to the LB call and are not intercepting any of
  // its callbacks).
  grpc_transport_stream_op_batch* pending_batches_[MAX_PENDING_BATCHES] = {};

  // Set when we get a cancel_stream op.
  grpc_error* cancel_error_ = GRPC_ERROR_NONE;
};

//
// RetryingCall definition
//

class RetryingCall {
 public:
  RetryingCall(
      ChannelData* chand, const grpc_call_element_args& args,
      grpc_polling_entity* pollent,
      RefCountedPtr<ServerRetryThrottleData> retry_throttle_data,
      const ClientChannelMethodParsedConfig::RetryPolicy* retry_policy);
  ~RetryingCall();

  void StartTransportStreamOpBatch(grpc_transport_stream_op_batch* batch);

  RefCountedPtr<SubchannelCall> subchannel_call() const;

 private:
  // State used for starting a retryable batch on a subchannel call.
  // This provides its own grpc_transport_stream_op_batch and other data
  // structures needed to populate the ops in the batch.
  // We allocate one struct on the arena for each attempt at starting a
  // batch on a given subchannel call.
  struct SubchannelCallBatchData {
    // Creates a SubchannelCallBatchData object on the call's arena with the
    // specified refcount.  If set_on_complete is true, the batch's
    // on_complete callback will be set to point to on_complete();
    // otherwise, the batch's on_complete callback will be null.
    static SubchannelCallBatchData* Create(RetryingCall* call, int refcount,
                                           bool set_on_complete);

    void Unref() {
      if (gpr_unref(&refs)) Destroy();
    }

    SubchannelCallBatchData(RetryingCall* call, int refcount,
                            bool set_on_complete);
    // All dtor code must be added in `Destroy()`. This is because we may
    // call closures in `SubchannelCallBatchData` after they are unrefed by
    // `Unref()`, and msan would complain about accessing this class
    // after calling dtor. As a result we cannot call the `dtor` in `Unref()`.
    // TODO(soheil): We should try to call the dtor in `Unref()`.
    ~SubchannelCallBatchData() { Destroy(); }
    void Destroy();

    gpr_refcount refs;
    grpc_call_element* elem;
    RetryingCall* call;
    RefCountedPtr<LoadBalancedCall> lb_call;
    // The batch to use in the subchannel call.
    // Its payload field points to SubchannelCallRetryState::batch_payload.
    grpc_transport_stream_op_batch batch;
    // For intercepting on_complete.
    grpc_closure on_complete;
  };

  // Retry state associated with a subchannel call.
  // Stored in the parent_data of the subchannel call object.
  struct SubchannelCallRetryState {
    explicit SubchannelCallRetryState(grpc_call_context_element* context)
        : batch_payload(context),
          started_send_initial_metadata(false),
          completed_send_initial_metadata(false),
          started_send_trailing_metadata(false),
          completed_send_trailing_metadata(false),
          started_recv_initial_metadata(false),
          completed_recv_initial_metadata(false),
          started_recv_trailing_metadata(false),
          completed_recv_trailing_metadata(false),
          retry_dispatched(false) {}

    // SubchannelCallBatchData.batch.payload points to this.
    grpc_transport_stream_op_batch_payload batch_payload;
    // For send_initial_metadata.
    // Note that we need to make a copy of the initial metadata for each
    // subchannel call instead of just referring to the copy in call_data,
    // because filters in the subchannel stack will probably add entries,
    // so we need to start in a pristine state for each attempt of the call.
    grpc_linked_mdelem* send_initial_metadata_storage;
    grpc_metadata_batch send_initial_metadata;
    // For send_message.
    // TODO(roth): Restructure this to eliminate use of ManualConstructor.
    ManualConstructor<ByteStreamCache::CachingByteStream> send_message;
    // For send_trailing_metadata.
    grpc_linked_mdelem* send_trailing_metadata_storage;
    grpc_metadata_batch send_trailing_metadata;
    // For intercepting recv_initial_metadata.
    grpc_metadata_batch recv_initial_metadata;
    grpc_closure recv_initial_metadata_ready;
    bool trailing_metadata_available = false;
    // For intercepting recv_message.
    grpc_closure recv_message_ready;
    OrphanablePtr<ByteStream> recv_message;
    // For intercepting recv_trailing_metadata.
    grpc_metadata_batch recv_trailing_metadata;
    grpc_transport_stream_stats collect_stats;
    grpc_closure recv_trailing_metadata_ready;
    // These fields indicate which ops have been started and completed on
    // this subchannel call.
    size_t started_send_message_count = 0;
    size_t completed_send_message_count = 0;
    size_t started_recv_message_count = 0;
    size_t completed_recv_message_count = 0;
    bool started_send_initial_metadata : 1;
    bool completed_send_initial_metadata : 1;
    bool started_send_trailing_metadata : 1;
    bool completed_send_trailing_metadata : 1;
    bool started_recv_initial_metadata : 1;
    bool completed_recv_initial_metadata : 1;
    bool started_recv_trailing_metadata : 1;
    bool completed_recv_trailing_metadata : 1;
    // State for callback processing.
    SubchannelCallBatchData* recv_initial_metadata_ready_deferred_batch =
        nullptr;
    grpc_error* recv_initial_metadata_error = GRPC_ERROR_NONE;
    SubchannelCallBatchData* recv_message_ready_deferred_batch = nullptr;
    grpc_error* recv_message_error = GRPC_ERROR_NONE;
    SubchannelCallBatchData* recv_trailing_metadata_internal_batch = nullptr;
    // NOTE: Do not move this next to the metadata bitfields above. That would
    //       save space but will also result in a data race because compiler
    //       will generate a 2 byte store which overwrites the meta-data
    //       fields upon setting this field.
    bool retry_dispatched : 1;
  };

  // Pending batches stored in call data.
  struct PendingBatch {
    // The pending batch.  If nullptr, this slot is empty.
    grpc_transport_stream_op_batch* batch = nullptr;
    // Indicates whether payload for send ops has been cached in CallData.
    bool send_ops_cached = false;
  };

  // Caches data for send ops so that it can be retried later, if not
  // already cached.
  void MaybeCacheSendOpsForBatch(PendingBatch* pending);
  void FreeCachedSendInitialMetadata();
  // Frees cached send_message at index idx.
  void FreeCachedSendMessage(size_t idx);
  void FreeCachedSendTrailingMetadata();
  // Frees cached send ops that have already been completed after
  // committing the call.
  void FreeCachedSendOpDataAfterCommit(SubchannelCallRetryState* retry_state);
  // Frees cached send ops that were completed by the completed batch in
  // batch_data.  Used when batches are completed after the call is committed.
  void FreeCachedSendOpDataForCompletedBatch(
      SubchannelCallBatchData* batch_data,
      SubchannelCallRetryState* retry_state);

  // Returns the index into pending_batches_ to be used for batch.
  static size_t GetBatchIndex(grpc_transport_stream_op_batch* batch);
  void PendingBatchesAdd(grpc_transport_stream_op_batch* batch);
  void PendingBatchClear(PendingBatch* pending);
  void MaybeClearPendingBatch(PendingBatch* pending);
  static void FailPendingBatchInCallCombiner(void* arg, grpc_error* error);
  // A predicate type and some useful implementations for PendingBatchesFail().
  typedef bool (*YieldCallCombinerPredicate)(
      const CallCombinerClosureList& closures);
  static bool YieldCallCombiner(const CallCombinerClosureList& /*closures*/) {
    return true;
  }
  static bool NoYieldCallCombiner(const CallCombinerClosureList& /*closures*/) {
    return false;
  }
  static bool YieldCallCombinerIfPendingBatchesFound(
      const CallCombinerClosureList& closures) {
    return closures.size() > 0;
  }
  // Fails all pending batches.
  // If yield_call_combiner_predicate returns true, assumes responsibility for
  // yielding the call combiner.
  void PendingBatchesFail(
      grpc_error* error,
      YieldCallCombinerPredicate yield_call_combiner_predicate);
  static void ResumePendingBatchInCallCombiner(void* arg, grpc_error* ignored);
  // Resumes all pending batches on lb_call_.
  void PendingBatchesResume();
  // Returns a pointer to the first pending batch for which predicate(batch)
  // returns true, or null if not found.
  template <typename Predicate>
  PendingBatch* PendingBatchFind(const char* log_message, Predicate predicate);

  // Commits the call so that no further retry attempts will be performed.
  void RetryCommit(SubchannelCallRetryState* retry_state);
  // Starts a retry after appropriate back-off.
  void DoRetry(SubchannelCallRetryState* retry_state,
               grpc_millis server_pushback_ms);
  // Returns true if the call is being retried.
  bool MaybeRetry(SubchannelCallBatchData* batch_data, grpc_status_code status,
                  grpc_mdelem* server_pushback_md);

  // Invokes recv_initial_metadata_ready for a subchannel batch.
  static void InvokeRecvInitialMetadataCallback(void* arg, grpc_error* error);
  // Intercepts recv_initial_metadata_ready callback for retries.
  // Commits the call and returns the initial metadata up the stack.
  static void RecvInitialMetadataReady(void* arg, grpc_error* error);

  // Invokes recv_message_ready for a subchannel batch.
  static void InvokeRecvMessageCallback(void* arg, grpc_error* error);
  // Intercepts recv_message_ready callback for retries.
  // Commits the call and returns the message up the stack.
  static void RecvMessageReady(void* arg, grpc_error* error);

  // Sets *status and *server_pushback_md based on md_batch and error.
  // Only sets *server_pushback_md if server_pushback_md != nullptr.
  void GetCallStatus(grpc_metadata_batch* md_batch, grpc_error* error,
                     grpc_status_code* status,
                     grpc_mdelem** server_pushback_md);
  // Adds recv_trailing_metadata_ready closure to closures.
  void AddClosureForRecvTrailingMetadataReady(
      SubchannelCallBatchData* batch_data, grpc_error* error,
      CallCombinerClosureList* closures);
  // Adds any necessary closures for deferred recv_initial_metadata and
  // recv_message callbacks to closures.
  static void AddClosuresForDeferredRecvCallbacks(
      SubchannelCallBatchData* batch_data,
      SubchannelCallRetryState* retry_state, CallCombinerClosureList* closures);
  // Returns true if any op in the batch was not yet started.
  // Only looks at send ops, since recv ops are always started immediately.
  bool PendingBatchIsUnstarted(PendingBatch* pending,
                               SubchannelCallRetryState* retry_state);
  // For any pending batch containing an op that has not yet been started,
  // adds the pending batch's completion closures to closures.
  void AddClosuresToFailUnstartedPendingBatches(
      SubchannelCallRetryState* retry_state, grpc_error* error,
      CallCombinerClosureList* closures);
  // Runs necessary closures upon completion of a call attempt.
  void RunClosuresForCompletedCall(SubchannelCallBatchData* batch_data,
                                   grpc_error* error);
  // Intercepts recv_trailing_metadata_ready callback for retries.
  // Commits the call and returns the trailing metadata up the stack.
  static void RecvTrailingMetadataReady(void* arg, grpc_error* error);

  // Adds the on_complete closure for the pending batch completed in
  // batch_data to closures.
  void AddClosuresForCompletedPendingBatch(SubchannelCallBatchData* batch_data,
                                           grpc_error* error,
                                           CallCombinerClosureList* closures);

  // If there are any cached ops to replay or pending ops to start on the
  // subchannel call, adds a closure to closures to invoke
  // StartRetriableSubchannelBatches().
  void AddClosuresForReplayOrPendingSendOps(
      SubchannelCallBatchData* batch_data,
      SubchannelCallRetryState* retry_state, CallCombinerClosureList* closures);

  // Callback used to intercept on_complete from subchannel calls.
  // Called only when retries are enabled.
  static void OnComplete(void* arg, grpc_error* error);

  static void StartBatchInCallCombiner(void* arg, grpc_error* ignored);
  // Adds a closure to closures that will execute batch in the call combiner.
  void AddClosureForSubchannelBatch(grpc_transport_stream_op_batch* batch,
                                    CallCombinerClosureList* closures);
  // Adds retriable send_initial_metadata op to batch_data.
  void AddRetriableSendInitialMetadataOp(SubchannelCallRetryState* retry_state,
                                         SubchannelCallBatchData* batch_data);
  // Adds retriable send_message op to batch_data.
  void AddRetriableSendMessageOp(SubchannelCallRetryState* retry_state,
                                 SubchannelCallBatchData* batch_data);
  // Adds retriable send_trailing_metadata op to batch_data.
  void AddRetriableSendTrailingMetadataOp(SubchannelCallRetryState* retry_state,
                                          SubchannelCallBatchData* batch_data);
  // Adds retriable recv_initial_metadata op to batch_data.
  void AddRetriableRecvInitialMetadataOp(SubchannelCallRetryState* retry_state,
                                         SubchannelCallBatchData* batch_data);
  // Adds retriable recv_message op to batch_data.
  void AddRetriableRecvMessageOp(SubchannelCallRetryState* retry_state,
                                 SubchannelCallBatchData* batch_data);
  // Adds retriable recv_trailing_metadata op to batch_data.
  void AddRetriableRecvTrailingMetadataOp(SubchannelCallRetryState* retry_state,
                                          SubchannelCallBatchData* batch_data);
  // Helper function used to start a recv_trailing_metadata batch.  This
  // is used in the case where a recv_initial_metadata or recv_message
  // op fails in a way that we know the call is over but when the application
  // has not yet started its own recv_trailing_metadata op.
  void StartInternalRecvTrailingMetadata();
  // If there are any cached send ops that need to be replayed on the
  // current subchannel call, creates and returns a new subchannel batch
  // to replay those ops.  Otherwise, returns nullptr.
  SubchannelCallBatchData* MaybeCreateSubchannelBatchForReplay(
      SubchannelCallRetryState* retry_state);
  // Adds subchannel batches for pending batches to closures.
  void AddSubchannelBatchesForPendingBatches(
      SubchannelCallRetryState* retry_state, CallCombinerClosureList* closures);
  // Constructs and starts whatever subchannel batches are needed on the
  // subchannel call.
  static void StartRetriableSubchannelBatches(void* arg, grpc_error* ignored);

  static void CreateLbCall(void* arg, grpc_error* error);

  ChannelData* chand_;
  grpc_polling_entity* pollent_;
  RefCountedPtr<ServerRetryThrottleData> retry_throttle_data_;
  const ClientChannelMethodParsedConfig::RetryPolicy* retry_policy_ = nullptr;
  BackOff retry_backoff_;

  grpc_slice path_;  // Request path.
  gpr_cycle_counter call_start_time_;
  grpc_millis deadline_;
  Arena* arena_;
  grpc_call_stack* owning_call_;
  CallCombiner* call_combiner_;
  grpc_call_context_element* call_context_;

  grpc_closure retry_closure_;

  RefCountedPtr<LoadBalancedCall> lb_call_;

  // Batches are added to this list when received from above.
  // They are removed when we are done handling the batch (i.e., when
  // either we have invoked all of the batch's callbacks or we have
  // passed the batch down to the LB call and are not intercepting any of
  // its callbacks).
  // TODO(roth): Now that the retry code is split out into its own call
  // object, revamp this to work in a cleaner way, since we no longer need
  // for batches to ever wait for name resolution or LB picks.
  PendingBatch pending_batches_[MAX_PENDING_BATCHES];
  bool pending_send_initial_metadata_ : 1;
  bool pending_send_message_ : 1;
  bool pending_send_trailing_metadata_ : 1;

  // Set when we get a cancel_stream op.
  grpc_error* cancel_error_ = GRPC_ERROR_NONE;

  // Retry state.
  bool enable_retries_ : 1;
  bool retry_committed_ : 1;
  bool last_attempt_got_server_pushback_ : 1;
  int num_attempts_completed_ = 0;
  size_t bytes_buffered_for_retry_ = 0;
  grpc_timer retry_timer_;

  // The number of pending retriable subchannel batches containing send ops.
  // We hold a ref to the call stack while this is non-zero, since replay
  // batches may not complete until after all callbacks have been returned
  // to the surface, and we need to make sure that the call is not destroyed
  // until all of these batches have completed.
  // Note that we actually only need to track replay batches, but it's
  // easier to track all batches with send ops.
  int num_pending_retriable_subchannel_send_batches_ = 0;

  // Cached data for retrying send ops.
  // send_initial_metadata
  bool seen_send_initial_metadata_ = false;
  grpc_linked_mdelem* send_initial_metadata_storage_ = nullptr;
  grpc_metadata_batch send_initial_metadata_;
  uint32_t send_initial_metadata_flags_;
  gpr_atm* peer_string_;
  // send_message
  // When we get a send_message op, we replace the original byte stream
  // with a CachingByteStream that caches the slices to a local buffer for
  // use in retries.
  // Note: We inline the cache for the first 3 send_message ops and use
  // dynamic allocation after that.  This number was essentially picked
  // at random; it could be changed in the future to tune performance.
  absl::InlinedVector<ByteStreamCache*, 3> send_messages_;
  // send_trailing_metadata
  bool seen_send_trailing_metadata_ = false;
  grpc_linked_mdelem* send_trailing_metadata_storage_ = nullptr;
  grpc_metadata_batch send_trailing_metadata_;
};

//
// LoadBalancedCall definition
//

// This object is ref-counted, but it cannot inherit from RefCounted<>,
// because it is allocated on the arena and can't free its memory when
// its refcount goes to zero.  So instead, it manually implements the
// same API as RefCounted<>, so that it can be used with RefCountedPtr<>.
class LoadBalancedCall {
 public:
  static RefCountedPtr<LoadBalancedCall> Create(
      ChannelData* chand, const grpc_call_element_args& args,
      grpc_polling_entity* pollent, size_t parent_data_size);

  LoadBalancedCall(ChannelData* chand, const grpc_call_element_args& args,
                   grpc_polling_entity* pollent);
  ~LoadBalancedCall();

  // Interface of RefCounted<>.
  RefCountedPtr<LoadBalancedCall> Ref() GRPC_MUST_USE_RESULT;
  RefCountedPtr<LoadBalancedCall> Ref(const DebugLocation& location,
                                      const char* reason) GRPC_MUST_USE_RESULT;
  // When refcount drops to 0, destroys itself and the associated call stack,
  // but does NOT free the memory because it's in the call arena.
  void Unref();
  void Unref(const DebugLocation& location, const char* reason);

  void* GetParentData();

  void StartTransportStreamOpBatch(grpc_transport_stream_op_batch* batch);

  // Invoked by channel for queued LB picks when the picker is updated.
  static void PickSubchannel(void* arg, grpc_error* error);
  // Helper function for performing an LB pick while holding the data plane
  // mutex.  Returns true if the pick is complete, in which case the caller
  // must invoke PickDone() or AsyncPickDone() with the returned error.
  bool PickSubchannelLocked(grpc_error** error);
  // Schedules a callback to process the completed pick.  The callback
  // will not run until after this method returns.
  void AsyncPickDone(grpc_error* error);

  RefCountedPtr<SubchannelCall> subchannel_call() const {
    return subchannel_call_;
  }

 private:
  // Allow RefCountedPtr<> to access IncrementRefCount().
  template <typename T>
  friend class ::grpc_core::RefCountedPtr;

  class LbQueuedCallCanceller;
  class Metadata;
  class LbCallState;

  // Interface of RefCounted<>.
  void IncrementRefCount();
  void IncrementRefCount(const DebugLocation& location, const char* reason);

  // Returns the index into pending_batches_ to be used for batch.
  static size_t GetBatchIndex(grpc_transport_stream_op_batch* batch);
  void PendingBatchesAdd(grpc_transport_stream_op_batch* batch);
  static void FailPendingBatchInCallCombiner(void* arg, grpc_error* error);
  // A predicate type and some useful implementations for PendingBatchesFail().
  typedef bool (*YieldCallCombinerPredicate)(
      const CallCombinerClosureList& closures);
  static bool YieldCallCombiner(const CallCombinerClosureList& /*closures*/) {
    return true;
  }
  static bool NoYieldCallCombiner(const CallCombinerClosureList& /*closures*/) {
    return false;
  }
  static bool YieldCallCombinerIfPendingBatchesFound(
      const CallCombinerClosureList& closures) {
    return closures.size() > 0;
  }
  // Fails all pending batches.
  // If yield_call_combiner_predicate returns true, assumes responsibility for
  // yielding the call combiner.
  void PendingBatchesFail(
      grpc_error* error,
      YieldCallCombinerPredicate yield_call_combiner_predicate);
  static void ResumePendingBatchInCallCombiner(void* arg, grpc_error* ignored);
  // Resumes all pending batches on subchannel_call_.
  void PendingBatchesResume();

  static void RecvTrailingMetadataReadyForLoadBalancingPolicy(
      void* arg, grpc_error* error);
  void InjectRecvTrailingMetadataReadyForLoadBalancingPolicy(
      grpc_transport_stream_op_batch* batch);

  void CreateSubchannelCall();
  // Invoked when a pick is completed, on both success or failure.
  static void PickDone(void* arg, grpc_error* error);
  // Removes the call from the channel's list of queued picks if present.
  void MaybeRemoveCallFromLbQueuedCallsLocked();
  // Adds the call to the channel's list of queued picks if not already present.
  void MaybeAddCallToLbQueuedCallsLocked();

  RefCount refs_;

  ChannelData* chand_;

  // TODO(roth): Instead of duplicating these fields in every filter
  // that uses any one of them, we should store them in the call
  // context.  This will save per-call memory overhead.
  grpc_slice path_;  // Request path.
  gpr_cycle_counter call_start_time_;
  grpc_millis deadline_;
  Arena* arena_;
  grpc_call_stack* owning_call_;
  CallCombiner* call_combiner_;
  grpc_call_context_element* call_context_;

  // Set when we get a cancel_stream op.
  grpc_error* cancel_error_ = GRPC_ERROR_NONE;

  grpc_polling_entity* pollent_ = nullptr;

  grpc_closure pick_closure_;

  // Accessed while holding ChannelData::data_plane_mu_.
  ChannelData::LbQueuedCall queued_call_;
  bool queued_pending_lb_pick_ = false;
  const LoadBalancingPolicy::BackendMetricData* backend_metric_data_ = nullptr;
  RefCountedPtr<ConnectedSubchannel> connected_subchannel_;
  std::function<void(grpc_error*, LoadBalancingPolicy::MetadataInterface*,
                     LoadBalancingPolicy::CallState*)>
      lb_recv_trailing_metadata_ready_;
  LbQueuedCallCanceller* lb_call_canceller_ = nullptr;

  RefCountedPtr<SubchannelCall> subchannel_call_;

  // For intercepting recv_trailing_metadata_ready for the LB policy.
  grpc_metadata_batch* recv_trailing_metadata_ = nullptr;
  grpc_closure recv_trailing_metadata_ready_;
  grpc_closure* original_recv_trailing_metadata_ready_ = nullptr;

  // Batches are added to this list when received from above.
  // They are removed when we are done handling the batch (i.e., when
  // either we have invoked all of the batch's callbacks or we have
  // passed the batch down to the subchannel call and are not
  // intercepting any of its callbacks).
  grpc_transport_stream_op_batch* pending_batches_[MAX_PENDING_BATCHES] = {};
};

//
// dynamic termination filter
//

// Channel arg pointer vtable for GRPC_ARG_CLIENT_CHANNEL_DATA.
void* ChannelDataArgCopy(void* p) { return p; }
void ChannelDataArgDestroy(void* /*p*/) {}
int ChannelDataArgCmp(void* p, void* q) { return GPR_ICMP(p, q); }
const grpc_arg_pointer_vtable kChannelDataArgPointerVtable = {
    ChannelDataArgCopy, ChannelDataArgDestroy, ChannelDataArgCmp};

// Channel arg pointer vtable for GRPC_ARG_RETRY_THROTTLE_DATA.
void* RetryThrottleDataArgCopy(void* p) {
  auto* retry_throttle_data = static_cast<ServerRetryThrottleData*>(p);
  retry_throttle_data->Ref().release();
  return p;
}
void RetryThrottleDataArgDestroy(void* p) {
  auto* retry_throttle_data = static_cast<ServerRetryThrottleData*>(p);
  retry_throttle_data->Unref();
}
int RetryThrottleDataArgCmp(void* p, void* q) { return GPR_ICMP(p, q); }
const grpc_arg_pointer_vtable kRetryThrottleDataArgPointerVtable = {
    RetryThrottleDataArgCopy, RetryThrottleDataArgDestroy,
    RetryThrottleDataArgCmp};

class DynamicTerminationFilterChannelData {
 public:
  static grpc_error* Init(grpc_channel_element* elem,
                          grpc_channel_element_args* args);

  static void Destroy(grpc_channel_element* elem) {
    auto* chand =
        static_cast<DynamicTerminationFilterChannelData*>(elem->channel_data);
    chand->~DynamicTerminationFilterChannelData();
  }

  // Will never be called.
  static void StartTransportOp(grpc_channel_element* /*elem*/,
                               grpc_transport_op* /*op*/) {}
  static void GetChannelInfo(grpc_channel_element* /*elem*/,
                             const grpc_channel_info* /*info*/) {}

  ChannelData* chand() const { return chand_; }
  RefCountedPtr<ServerRetryThrottleData> retry_throttle_data() const {
    return retry_throttle_data_;
  }

 private:
  static RefCountedPtr<ServerRetryThrottleData> GetRetryThrottleDataFromArgs(
      const grpc_channel_args* args) {
    auto* retry_throttle_data =
        grpc_channel_args_find_pointer<ServerRetryThrottleData>(
            args, GRPC_ARG_RETRY_THROTTLE_DATA);
    if (retry_throttle_data == nullptr) return nullptr;
    return retry_throttle_data->Ref();
  }

  explicit DynamicTerminationFilterChannelData(const grpc_channel_args* args)
      : chand_(grpc_channel_args_find_pointer<ChannelData>(
            args, GRPC_ARG_CLIENT_CHANNEL_DATA)),
        retry_throttle_data_(GetRetryThrottleDataFromArgs(args)) {}

  ChannelData* chand_;
  RefCountedPtr<ServerRetryThrottleData> retry_throttle_data_;
};

class DynamicTerminationFilterCallData {
 public:
  static grpc_error* Init(grpc_call_element* elem,
                          const grpc_call_element_args* args) {
    new (elem->call_data) DynamicTerminationFilterCallData(*args);
    return GRPC_ERROR_NONE;
  }

  static void Destroy(grpc_call_element* elem,
                      const grpc_call_final_info* /*final_info*/,
                      grpc_closure* then_schedule_closure) {
    auto* calld =
        static_cast<DynamicTerminationFilterCallData*>(elem->call_data);
    auto* chand =
        static_cast<DynamicTerminationFilterChannelData*>(elem->channel_data);
    RefCountedPtr<SubchannelCall> subchannel_call;
    if (chand->chand()->enable_retries()) {
      if (GPR_LIKELY(calld->retrying_call_ != nullptr)) {
        subchannel_call = calld->retrying_call_->subchannel_call();
        calld->retrying_call_->~RetryingCall();
      }
    } else {
      if (GPR_LIKELY(calld->lb_call_ != nullptr)) {
        subchannel_call = calld->lb_call_->subchannel_call();
      }
    }
    calld->~DynamicTerminationFilterCallData();
    if (GPR_LIKELY(subchannel_call != nullptr)) {
      subchannel_call->SetAfterCallStackDestroy(then_schedule_closure);
    } else {
      // TODO(yashkt) : This can potentially be a Closure::Run
      ExecCtx::Run(DEBUG_LOCATION, then_schedule_closure, GRPC_ERROR_NONE);
    }
  }

  static void StartTransportStreamOpBatch(
      grpc_call_element* elem, grpc_transport_stream_op_batch* batch) {
    auto* calld =
        static_cast<DynamicTerminationFilterCallData*>(elem->call_data);
    auto* chand =
        static_cast<DynamicTerminationFilterChannelData*>(elem->channel_data);
    if (chand->chand()->enable_retries()) {
      calld->retrying_call_->StartTransportStreamOpBatch(batch);
    } else {
      calld->lb_call_->StartTransportStreamOpBatch(batch);
    }
  }

  static void SetPollent(grpc_call_element* elem,
                         grpc_polling_entity* pollent) {
    auto* calld =
        static_cast<DynamicTerminationFilterCallData*>(elem->call_data);
    auto* chand =
        static_cast<DynamicTerminationFilterChannelData*>(elem->channel_data);
    ChannelData* client_channel = chand->chand();
    grpc_call_element_args args = {
        calld->owning_call_,     nullptr,
        calld->call_context_,    calld->path_,
        calld->call_start_time_, calld->deadline_,
        calld->arena_,           calld->call_combiner_};
    if (client_channel->enable_retries()) {
      // Get retry settings from service config.
      auto* svc_cfg_call_data = static_cast<ServiceConfigCallData*>(
          calld->call_context_[GRPC_CONTEXT_SERVICE_CONFIG_CALL_DATA].value);
      GPR_ASSERT(svc_cfg_call_data != nullptr);
      auto* method_config = static_cast<const ClientChannelMethodParsedConfig*>(
          svc_cfg_call_data->GetMethodParsedConfig(
              ClientChannelServiceConfigParser::ParserIndex()));
      // Create retrying call.
      calld->retrying_call_ = calld->arena_->New<RetryingCall>(
          client_channel, args, pollent, chand->retry_throttle_data(),
          method_config == nullptr ? nullptr : method_config->retry_policy());
      if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
        gpr_log(
            GPR_INFO,
            "chand=%p dymamic_termination_calld=%p: create retrying_call=%p",
            client_channel, calld, calld->retrying_call_);
      }
    } else {
      calld->lb_call_ =
          LoadBalancedCall::Create(client_channel, args, pollent, 0);
      if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
        gpr_log(GPR_INFO,
                "chand=%p dynamic_termination_calld=%p: create lb_call=%p",
                chand, client_channel, calld->lb_call_.get());
      }
    }
  }

 private:
  explicit DynamicTerminationFilterCallData(const grpc_call_element_args& args)
      : path_(grpc_slice_ref_internal(args.path)),
        call_start_time_(args.start_time),
        deadline_(args.deadline),
        arena_(args.arena),
        owning_call_(args.call_stack),
        call_combiner_(args.call_combiner),
        call_context_(args.context) {}

  ~DynamicTerminationFilterCallData() { grpc_slice_unref_internal(path_); }

  grpc_slice path_;  // Request path.
  gpr_cycle_counter call_start_time_;
  grpc_millis deadline_;
  Arena* arena_;
  grpc_call_stack* owning_call_;
  CallCombiner* call_combiner_;
  grpc_call_context_element* call_context_;

  RetryingCall* retrying_call_ = nullptr;
  RefCountedPtr<LoadBalancedCall> lb_call_;
};

const grpc_channel_filter kDynamicTerminationFilterVtable = {
    DynamicTerminationFilterCallData::StartTransportStreamOpBatch,
    DynamicTerminationFilterChannelData::StartTransportOp,
    sizeof(DynamicTerminationFilterCallData),
    DynamicTerminationFilterCallData::Init,
    DynamicTerminationFilterCallData::SetPollent,
    DynamicTerminationFilterCallData::Destroy,
    sizeof(DynamicTerminationFilterChannelData),
    DynamicTerminationFilterChannelData::Init,
    DynamicTerminationFilterChannelData::Destroy,
    DynamicTerminationFilterChannelData::GetChannelInfo,
    "dynamic_filter_termination",
};

grpc_error* DynamicTerminationFilterChannelData::Init(
    grpc_channel_element* elem, grpc_channel_element_args* args) {
  GPR_ASSERT(args->is_last);
  GPR_ASSERT(elem->filter == &kDynamicTerminationFilterVtable);
  new (elem->channel_data)
      DynamicTerminationFilterChannelData(args->channel_args);
  return GRPC_ERROR_NONE;
}

//
// ChannelData::SubchannelWrapper
//

// This class is a wrapper for Subchannel that hides details of the
// channel's implementation (such as the health check service name and
// connected subchannel) from the LB policy API.
//
// Note that no synchronization is needed here, because even if the
// underlying subchannel is shared between channels, this wrapper will only
// be used within one channel, so it will always be synchronized by the
// control plane work_serializer.
class ChannelData::SubchannelWrapper : public SubchannelInterface {
 public:
  SubchannelWrapper(ChannelData* chand, RefCountedPtr<Subchannel> subchannel,
                    absl::optional<std::string> health_check_service_name)
      : SubchannelInterface(
            GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)
                ? "SubchannelWrapper"
                : nullptr),
        chand_(chand),
        subchannel_(std::move(subchannel)),
        health_check_service_name_(std::move(health_check_service_name)) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
      gpr_log(GPR_INFO,
              "chand=%p: creating subchannel wrapper %p for subchannel %p",
              chand, this, subchannel_);
    }
    GRPC_CHANNEL_STACK_REF(chand_->owning_stack_, "SubchannelWrapper");
    auto* subchannel_node = subchannel_->channelz_node();
    if (subchannel_node != nullptr) {
      auto it = chand_->subchannel_refcount_map_.find(subchannel_);
      if (it == chand_->subchannel_refcount_map_.end()) {
        chand_->channelz_node_->AddChildSubchannel(subchannel_node->uuid());
        it = chand_->subchannel_refcount_map_.emplace(subchannel_, 0).first;
      }
      ++it->second;
    }
    chand_->subchannel_wrappers_.insert(this);
  }

  ~SubchannelWrapper() override {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
      gpr_log(GPR_INFO,
              "chand=%p: destroying subchannel wrapper %p for subchannel %p",
              chand_, this, subchannel_);
    }
    chand_->subchannel_wrappers_.erase(this);
    auto* subchannel_node = subchannel_->channelz_node();
    if (subchannel_node != nullptr) {
      auto it = chand_->subchannel_refcount_map_.find(subchannel_);
      GPR_ASSERT(it != chand_->subchannel_refcount_map_.end());
      --it->second;
      if (it->second == 0) {
        chand_->channelz_node_->RemoveChildSubchannel(subchannel_node->uuid());
        chand_->subchannel_refcount_map_.erase(it);
      }
    }
    GRPC_CHANNEL_STACK_UNREF(chand_->owning_stack_, "SubchannelWrapper");
  }

  grpc_connectivity_state CheckConnectivityState() override {
    RefCountedPtr<ConnectedSubchannel> connected_subchannel;
    grpc_connectivity_state connectivity_state =
        subchannel_->CheckConnectivityState(health_check_service_name_,
                                            &connected_subchannel);
    MaybeUpdateConnectedSubchannel(std::move(connected_subchannel));
    return connectivity_state;
  }

  void WatchConnectivityState(
      grpc_connectivity_state initial_state,
      std::unique_ptr<ConnectivityStateWatcherInterface> watcher) override {
    auto& watcher_wrapper = watcher_map_[watcher.get()];
    GPR_ASSERT(watcher_wrapper == nullptr);
    watcher_wrapper = new WatcherWrapper(std::move(watcher),
                                         Ref(DEBUG_LOCATION, "WatcherWrapper"),
                                         initial_state);
    subchannel_->WatchConnectivityState(
        initial_state, health_check_service_name_,
        RefCountedPtr<Subchannel::ConnectivityStateWatcherInterface>(
            watcher_wrapper));
  }

  void CancelConnectivityStateWatch(
      ConnectivityStateWatcherInterface* watcher) override {
    auto it = watcher_map_.find(watcher);
    GPR_ASSERT(it != watcher_map_.end());
    subchannel_->CancelConnectivityStateWatch(health_check_service_name_,
                                              it->second);
    watcher_map_.erase(it);
  }

  void AttemptToConnect() override { subchannel_->AttemptToConnect(); }

  void ResetBackoff() override { subchannel_->ResetBackoff(); }

  const grpc_channel_args* channel_args() override {
    return subchannel_->channel_args();
  }

  void ThrottleKeepaliveTime(int new_keepalive_time) {
    subchannel_->ThrottleKeepaliveTime(new_keepalive_time);
  }

  void UpdateHealthCheckServiceName(
      absl::optional<std::string> health_check_service_name) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
      gpr_log(GPR_INFO,
              "chand=%p: subchannel wrapper %p: updating health check service "
              "name from \"%s\" to \"%s\"",
              chand_, this, health_check_service_name_->c_str(),
              health_check_service_name->c_str());
    }
    for (auto& p : watcher_map_) {
      WatcherWrapper*& watcher_wrapper = p.second;
      // Cancel the current watcher and create a new one using the new
      // health check service name.
      // TODO(roth): If there is not already an existing health watch
      // call for the new name, then the watcher will initially report
      // state CONNECTING.  If the LB policy is currently reporting
      // state READY, this may cause it to switch to CONNECTING before
      // switching back to READY.  This could cause a small delay for
      // RPCs being started on the channel.  If/when this becomes a
      // problem, we may be able to handle it by waiting for the new
      // watcher to report READY before we use it to replace the old one.
      WatcherWrapper* replacement = watcher_wrapper->MakeReplacement();
      subchannel_->CancelConnectivityStateWatch(health_check_service_name_,
                                                watcher_wrapper);
      watcher_wrapper = replacement;
      subchannel_->WatchConnectivityState(
          replacement->last_seen_state(), health_check_service_name,
          RefCountedPtr<Subchannel::ConnectivityStateWatcherInterface>(
              replacement));
    }
    // Save the new health check service name.
    health_check_service_name_ = std::move(health_check_service_name);
  }

  // Caller must be holding the control-plane work_serializer.
  ConnectedSubchannel* connected_subchannel() const {
    return connected_subchannel_.get();
  }

  // Caller must be holding the data-plane mutex.
  ConnectedSubchannel* connected_subchannel_in_data_plane() const {
    return connected_subchannel_in_data_plane_.get();
  }
  void set_connected_subchannel_in_data_plane(
      RefCountedPtr<ConnectedSubchannel> connected_subchannel) {
    connected_subchannel_in_data_plane_ = std::move(connected_subchannel);
  }

 private:
  // Subchannel and SubchannelInterface have different interfaces for
  // their respective ConnectivityStateWatcherInterface classes.
  // The one in Subchannel updates the ConnectedSubchannel along with
  // the state, whereas the one in SubchannelInterface does not expose
  // the ConnectedSubchannel.
  //
  // This wrapper provides a bridge between the two.  It implements
  // Subchannel::ConnectivityStateWatcherInterface and wraps
  // the instance of SubchannelInterface::ConnectivityStateWatcherInterface
  // that was passed in by the LB policy.  We pass an instance of this
  // class to the underlying Subchannel, and when we get updates from
  // the subchannel, we pass those on to the wrapped watcher to return
  // the update to the LB policy.  This allows us to set the connected
  // subchannel before passing the result back to the LB policy.
  class WatcherWrapper : public Subchannel::ConnectivityStateWatcherInterface {
   public:
    WatcherWrapper(
        std::unique_ptr<SubchannelInterface::ConnectivityStateWatcherInterface>
            watcher,
        RefCountedPtr<SubchannelWrapper> parent,
        grpc_connectivity_state initial_state)
        : watcher_(std::move(watcher)),
          parent_(std::move(parent)),
          last_seen_state_(initial_state) {}

    ~WatcherWrapper() override {
      auto* parent = parent_.release();  // ref owned by lambda
      parent->chand_->work_serializer_->Run(
          [parent]() { parent->Unref(DEBUG_LOCATION, "WatcherWrapper"); },
          DEBUG_LOCATION);
    }

    void OnConnectivityStateChange() override {
      if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
        gpr_log(GPR_INFO,
                "chand=%p: connectivity change for subchannel wrapper %p "
                "subchannel %p; hopping into work_serializer",
                parent_->chand_, parent_.get(), parent_->subchannel_);
      }
      Ref().release();  // ref owned by lambda
      parent_->chand_->work_serializer_->Run(
          [this]() {
            ApplyUpdateInControlPlaneWorkSerializer();
            Unref();
          },
          DEBUG_LOCATION);
    }

    grpc_pollset_set* interested_parties() override {
      SubchannelInterface::ConnectivityStateWatcherInterface* watcher =
          watcher_.get();
      if (watcher_ == nullptr) watcher = replacement_->watcher_.get();
      return watcher->interested_parties();
    }

    WatcherWrapper* MakeReplacement() {
      auto* replacement =
          new WatcherWrapper(std::move(watcher_), parent_, last_seen_state_);
      replacement_ = replacement;
      return replacement;
    }

    grpc_connectivity_state last_seen_state() const { return last_seen_state_; }

   private:
    void ApplyUpdateInControlPlaneWorkSerializer() {
      if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
        gpr_log(GPR_INFO,
                "chand=%p: processing connectivity change in work serializer "
                "for subchannel wrapper %p subchannel %p "
                "watcher=%p",
                parent_->chand_, parent_.get(), parent_->subchannel_,
                watcher_.get());
      }
      ConnectivityStateChange state_change = PopConnectivityStateChange();
      absl::optional<absl::Cord> keepalive_throttling =
          state_change.status.GetPayload(kKeepaliveThrottlingKey);
      if (keepalive_throttling.has_value()) {
        int new_keepalive_time = -1;
        if (absl::SimpleAtoi(std::string(keepalive_throttling.value()),
                             &new_keepalive_time)) {
          if (new_keepalive_time > parent_->chand_->keepalive_time_) {
            parent_->chand_->keepalive_time_ = new_keepalive_time;
            if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
              gpr_log(GPR_INFO, "chand=%p: throttling keepalive time to %d",
                      parent_->chand_, parent_->chand_->keepalive_time_);
            }
            // Propagate the new keepalive time to all subchannels. This is so
            // that new transports created by any subchannel (and not just the
            // subchannel that received the GOAWAY), use the new keepalive time.
            for (auto* subchannel_wrapper :
                 parent_->chand_->subchannel_wrappers_) {
              subchannel_wrapper->ThrottleKeepaliveTime(new_keepalive_time);
            }
          }
        } else {
          gpr_log(GPR_ERROR, "chand=%p: Illegal keepalive throttling value %s",
                  parent_->chand_,
                  std::string(keepalive_throttling.value()).c_str());
        }
      }
      // Ignore update if the parent WatcherWrapper has been replaced
      // since this callback was scheduled.
      if (watcher_ != nullptr) {
        last_seen_state_ = state_change.state;
        parent_->MaybeUpdateConnectedSubchannel(
            std::move(state_change.connected_subchannel));
        watcher_->OnConnectivityStateChange(state_change.state);
      }
    }

    std::unique_ptr<SubchannelInterface::ConnectivityStateWatcherInterface>
        watcher_;
    RefCountedPtr<SubchannelWrapper> parent_;
    grpc_connectivity_state last_seen_state_;
    WatcherWrapper* replacement_ = nullptr;
  };

  void MaybeUpdateConnectedSubchannel(
      RefCountedPtr<ConnectedSubchannel> connected_subchannel) {
    // Update the connected subchannel only if the channel is not shutting
    // down.  This is because once the channel is shutting down, we
    // ignore picker updates from the LB policy, which means that
    // UpdateStateAndPickerLocked() will never process the entries
    // in chand_->pending_subchannel_updates_.  So we don't want to add
    // entries there that will never be processed, since that would
    // leave dangling refs to the channel and prevent its destruction.
    grpc_error* disconnect_error = chand_->disconnect_error();
    if (disconnect_error != GRPC_ERROR_NONE) return;
    // Not shutting down, so do the update.
    if (connected_subchannel_ != connected_subchannel) {
      connected_subchannel_ = std::move(connected_subchannel);
      // Record the new connected subchannel so that it can be updated
      // in the data plane mutex the next time the picker is updated.
      chand_->pending_subchannel_updates_[Ref(
          DEBUG_LOCATION, "ConnectedSubchannelUpdate")] = connected_subchannel_;
    }
  }

  ChannelData* chand_;
  RefCountedPtr<Subchannel> subchannel_;
  absl::optional<std::string> health_check_service_name_;
  // Maps from the address of the watcher passed to us by the LB policy
  // to the address of the WrapperWatcher that we passed to the underlying
  // subchannel.  This is needed so that when the LB policy calls
  // CancelConnectivityStateWatch() with its watcher, we know the
  // corresponding WrapperWatcher to cancel on the underlying subchannel.
  std::map<ConnectivityStateWatcherInterface*, WatcherWrapper*> watcher_map_;
  // To be accessed only in the control plane work_serializer.
  RefCountedPtr<ConnectedSubchannel> connected_subchannel_;
  // To be accessed only in the data plane mutex.
  RefCountedPtr<ConnectedSubchannel> connected_subchannel_in_data_plane_;
};

//
// ChannelData::ExternalConnectivityWatcher
//

ChannelData::ExternalConnectivityWatcher::ExternalConnectivityWatcher(
    ChannelData* chand, grpc_polling_entity pollent,
    grpc_connectivity_state* state, grpc_closure* on_complete,
    grpc_closure* watcher_timer_init)
    : chand_(chand),
      pollent_(pollent),
      initial_state_(*state),
      state_(state),
      on_complete_(on_complete),
      watcher_timer_init_(watcher_timer_init) {
  grpc_polling_entity_add_to_pollset_set(&pollent_,
                                         chand_->interested_parties_);
  GRPC_CHANNEL_STACK_REF(chand_->owning_stack_, "ExternalConnectivityWatcher");
  {
    MutexLock lock(&chand_->external_watchers_mu_);
    // Will be deleted when the watch is complete.
    GPR_ASSERT(chand->external_watchers_[on_complete] == nullptr);
    // Store a ref to the watcher in the external_watchers_ map.
    chand->external_watchers_[on_complete] =
        Ref(DEBUG_LOCATION, "AddWatcherToExternalWatchersMapLocked");
  }
  // Pass the ref from creating the object to Start().
  chand_->work_serializer_->Run(
      [this]() {
        // The ref is passed to AddWatcherLocked().
        AddWatcherLocked();
      },
      DEBUG_LOCATION);
}

ChannelData::ExternalConnectivityWatcher::~ExternalConnectivityWatcher() {
  grpc_polling_entity_del_from_pollset_set(&pollent_,
                                           chand_->interested_parties_);
  GRPC_CHANNEL_STACK_UNREF(chand_->owning_stack_,
                           "ExternalConnectivityWatcher");
}

void ChannelData::ExternalConnectivityWatcher::
    RemoveWatcherFromExternalWatchersMap(ChannelData* chand,
                                         grpc_closure* on_complete,
                                         bool cancel) {
  RefCountedPtr<ExternalConnectivityWatcher> watcher;
  {
    MutexLock lock(&chand->external_watchers_mu_);
    auto it = chand->external_watchers_.find(on_complete);
    if (it != chand->external_watchers_.end()) {
      watcher = std::move(it->second);
      chand->external_watchers_.erase(it);
    }
  }
  // watcher->Cancel() will hop into the WorkSerializer, so we have to unlock
  // the mutex before calling it.
  if (watcher != nullptr && cancel) watcher->Cancel();
}

void ChannelData::ExternalConnectivityWatcher::Notify(
    grpc_connectivity_state state, const absl::Status& /* status */) {
  bool done = false;
  if (!done_.CompareExchangeStrong(&done, true, MemoryOrder::RELAXED,
                                   MemoryOrder::RELAXED)) {
    return;  // Already done.
  }
  // Remove external watcher.
  chand_->RemoveExternalConnectivityWatcher(on_complete_, /*cancel=*/false);
  // Report new state to the user.
  *state_ = state;
  ExecCtx::Run(DEBUG_LOCATION, on_complete_, GRPC_ERROR_NONE);
  // Hop back into the work_serializer to clean up.
  // Not needed in state SHUTDOWN, because the tracker will
  // automatically remove all watchers in that case.
  if (state != GRPC_CHANNEL_SHUTDOWN) {
    chand_->work_serializer_->Run([this]() { RemoveWatcherLocked(); },
                                  DEBUG_LOCATION);
  }
}

void ChannelData::ExternalConnectivityWatcher::Cancel() {
  bool done = false;
  if (!done_.CompareExchangeStrong(&done, true, MemoryOrder::RELAXED,
                                   MemoryOrder::RELAXED)) {
    return;  // Already done.
  }
  ExecCtx::Run(DEBUG_LOCATION, on_complete_, GRPC_ERROR_CANCELLED);
  // Hop back into the work_serializer to clean up.
  chand_->work_serializer_->Run([this]() { RemoveWatcherLocked(); },
                                DEBUG_LOCATION);
}

void ChannelData::ExternalConnectivityWatcher::AddWatcherLocked() {
  Closure::Run(DEBUG_LOCATION, watcher_timer_init_, GRPC_ERROR_NONE);
  // Add new watcher. Pass the ref of the object from creation to OrphanablePtr.
  chand_->state_tracker_.AddWatcher(
      initial_state_, OrphanablePtr<ConnectivityStateWatcherInterface>(this));
}

void ChannelData::ExternalConnectivityWatcher::RemoveWatcherLocked() {
  chand_->state_tracker_.RemoveWatcher(this);
}

//
// ChannelData::ConnectivityWatcherAdder
//

class ChannelData::ConnectivityWatcherAdder {
 public:
  ConnectivityWatcherAdder(
      ChannelData* chand, grpc_connectivity_state initial_state,
      OrphanablePtr<AsyncConnectivityStateWatcherInterface> watcher)
      : chand_(chand),
        initial_state_(initial_state),
        watcher_(std::move(watcher)) {
    GRPC_CHANNEL_STACK_REF(chand_->owning_stack_, "ConnectivityWatcherAdder");
    chand_->work_serializer_->Run([this]() { AddWatcherLocked(); },
                                  DEBUG_LOCATION);
  }

 private:
  void AddWatcherLocked() {
    chand_->state_tracker_.AddWatcher(initial_state_, std::move(watcher_));
    GRPC_CHANNEL_STACK_UNREF(chand_->owning_stack_, "ConnectivityWatcherAdder");
    delete this;
  }

  ChannelData* chand_;
  grpc_connectivity_state initial_state_;
  OrphanablePtr<AsyncConnectivityStateWatcherInterface> watcher_;
};

//
// ChannelData::ConnectivityWatcherRemover
//

class ChannelData::ConnectivityWatcherRemover {
 public:
  ConnectivityWatcherRemover(ChannelData* chand,
                             AsyncConnectivityStateWatcherInterface* watcher)
      : chand_(chand), watcher_(watcher) {
    GRPC_CHANNEL_STACK_REF(chand_->owning_stack_, "ConnectivityWatcherRemover");
    chand_->work_serializer_->Run([this]() { RemoveWatcherLocked(); },
                                  DEBUG_LOCATION);
  }

 private:
  void RemoveWatcherLocked() {
    chand_->state_tracker_.RemoveWatcher(watcher_);
    GRPC_CHANNEL_STACK_UNREF(chand_->owning_stack_,
                             "ConnectivityWatcherRemover");
    delete this;
  }

  ChannelData* chand_;
  AsyncConnectivityStateWatcherInterface* watcher_;
};

//
// ChannelData::ClientChannelControlHelper
//

class ChannelData::ClientChannelControlHelper
    : public LoadBalancingPolicy::ChannelControlHelper {
 public:
  explicit ClientChannelControlHelper(ChannelData* chand) : chand_(chand) {
    GRPC_CHANNEL_STACK_REF(chand_->owning_stack_, "ClientChannelControlHelper");
  }

  ~ClientChannelControlHelper() override {
    GRPC_CHANNEL_STACK_UNREF(chand_->owning_stack_,
                             "ClientChannelControlHelper");
  }

  RefCountedPtr<SubchannelInterface> CreateSubchannel(
      ServerAddress address, const grpc_channel_args& args) override {
    if (chand_->resolver_ == nullptr) return nullptr;  // Shutting down.
    // Determine health check service name.
    bool inhibit_health_checking = grpc_channel_arg_get_bool(
        grpc_channel_args_find(&args, GRPC_ARG_INHIBIT_HEALTH_CHECKING), false);
    absl::optional<std::string> health_check_service_name;
    if (!inhibit_health_checking) {
      health_check_service_name = chand_->health_check_service_name_;
    }
    // Remove channel args that should not affect subchannel uniqueness.
    static const char* args_to_remove[] = {
        GRPC_ARG_INHIBIT_HEALTH_CHECKING,
        GRPC_ARG_CHANNELZ_CHANNEL_NODE,
    };
    // Add channel args needed for the subchannel.
    absl::InlinedVector<grpc_arg, 3> args_to_add = {
        Subchannel::CreateSubchannelAddressArg(&address.address()),
        SubchannelPoolInterface::CreateChannelArg(
            chand_->subchannel_pool_.get()),
    };
    if (address.args() != nullptr) {
      for (size_t j = 0; j < address.args()->num_args; ++j) {
        args_to_add.emplace_back(address.args()->args[j]);
      }
    }
    grpc_channel_args* new_args = grpc_channel_args_copy_and_add_and_remove(
        &args, args_to_remove, GPR_ARRAY_SIZE(args_to_remove),
        args_to_add.data(), args_to_add.size());
    gpr_free(args_to_add[0].value.string);
    // Create subchannel.
    RefCountedPtr<Subchannel> subchannel =
        chand_->client_channel_factory_->CreateSubchannel(new_args);
    grpc_channel_args_destroy(new_args);
    if (subchannel == nullptr) return nullptr;
    // Make sure the subchannel has updated keepalive time.
    subchannel->ThrottleKeepaliveTime(chand_->keepalive_time_);
    // Create and return wrapper for the subchannel.
    return MakeRefCounted<SubchannelWrapper>(
        chand_, std::move(subchannel), std::move(health_check_service_name));
  }

  void UpdateState(
      grpc_connectivity_state state, const absl::Status& status,
      std::unique_ptr<LoadBalancingPolicy::SubchannelPicker> picker) override {
    if (chand_->resolver_ == nullptr) return;  // Shutting down.
    grpc_error* disconnect_error = chand_->disconnect_error();
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
      const char* extra = disconnect_error == GRPC_ERROR_NONE
                              ? ""
                              : " (ignoring -- channel shutting down)";
      gpr_log(GPR_INFO, "chand=%p: update: state=%s status=(%s) picker=%p%s",
              chand_, ConnectivityStateName(state), status.ToString().c_str(),
              picker.get(), extra);
    }
    // Do update only if not shutting down.
    if (disconnect_error == GRPC_ERROR_NONE) {
      chand_->UpdateStateAndPickerLocked(state, status, "helper",
                                         std::move(picker));
    }
  }

  void RequestReresolution() override {
    if (chand_->resolver_ == nullptr) return;  // Shutting down.
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
      gpr_log(GPR_INFO, "chand=%p: started name re-resolving", chand_);
    }
    chand_->resolver_->RequestReresolutionLocked();
  }

  void AddTraceEvent(TraceSeverity severity,
                     absl::string_view message) override {
    if (chand_->resolver_ == nullptr) return;  // Shutting down.
    if (chand_->channelz_node_ != nullptr) {
      chand_->channelz_node_->AddTraceEvent(
          ConvertSeverityEnum(severity),
          grpc_slice_from_copied_buffer(message.data(), message.size()));
    }
  }

 private:
  static channelz::ChannelTrace::Severity ConvertSeverityEnum(
      TraceSeverity severity) {
    if (severity == TRACE_INFO) return channelz::ChannelTrace::Info;
    if (severity == TRACE_WARNING) return channelz::ChannelTrace::Warning;
    return channelz::ChannelTrace::Error;
  }

  ChannelData* chand_;
};

//
// ChannelData implementation
//

grpc_error* ChannelData::Init(grpc_channel_element* elem,
                              grpc_channel_element_args* args) {
  GPR_ASSERT(args->is_last);
  GPR_ASSERT(elem->filter == &grpc_client_channel_filter);
  grpc_error* error = GRPC_ERROR_NONE;
  new (elem->channel_data) ChannelData(args, &error);
  return error;
}

void ChannelData::Destroy(grpc_channel_element* elem) {
  ChannelData* chand = static_cast<ChannelData*>(elem->channel_data);
  chand->~ChannelData();
}

bool GetEnableRetries(const grpc_channel_args* args) {
  return grpc_channel_arg_get_bool(
      grpc_channel_args_find(args, GRPC_ARG_ENABLE_RETRIES), true);
}

size_t GetMaxPerRpcRetryBufferSize(const grpc_channel_args* args) {
  return static_cast<size_t>(grpc_channel_arg_get_integer(
      grpc_channel_args_find(args, GRPC_ARG_PER_RPC_RETRY_BUFFER_SIZE),
      {DEFAULT_PER_RPC_RETRY_BUFFER_SIZE, 0, INT_MAX}));
}

RefCountedPtr<SubchannelPoolInterface> GetSubchannelPool(
    const grpc_channel_args* args) {
  const bool use_local_subchannel_pool = grpc_channel_arg_get_bool(
      grpc_channel_args_find(args, GRPC_ARG_USE_LOCAL_SUBCHANNEL_POOL), false);
  if (use_local_subchannel_pool) {
    return MakeRefCounted<LocalSubchannelPool>();
  }
  return GlobalSubchannelPool::instance();
}

channelz::ChannelNode* GetChannelzNode(const grpc_channel_args* args) {
  const grpc_arg* arg =
      grpc_channel_args_find(args, GRPC_ARG_CHANNELZ_CHANNEL_NODE);
  if (arg != nullptr && arg->type == GRPC_ARG_POINTER) {
    return static_cast<channelz::ChannelNode*>(arg->value.pointer.p);
  }
  return nullptr;
}

ChannelData::ChannelData(grpc_channel_element_args* args, grpc_error** error)
    : deadline_checking_enabled_(
          grpc_deadline_checking_enabled(args->channel_args)),
      enable_retries_(GetEnableRetries(args->channel_args)),
      per_rpc_retry_buffer_size_(
          GetMaxPerRpcRetryBufferSize(args->channel_args)),
      owning_stack_(args->channel_stack),
      client_channel_factory_(
          ClientChannelFactory::GetFromChannelArgs(args->channel_args)),
      channelz_node_(GetChannelzNode(args->channel_args)),
      work_serializer_(std::make_shared<WorkSerializer>()),
      interested_parties_(grpc_pollset_set_create()),
      state_tracker_("client_channel", GRPC_CHANNEL_IDLE),
      subchannel_pool_(GetSubchannelPool(args->channel_args)),
      disconnect_error_(GRPC_ERROR_NONE) {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
    gpr_log(GPR_INFO, "chand=%p: creating client_channel for channel stack %p",
            this, owning_stack_);
  }
  // Start backup polling.
  grpc_client_channel_start_backup_polling(interested_parties_);
  // Check client channel factory.
  if (client_channel_factory_ == nullptr) {
    *error = GRPC_ERROR_CREATE_FROM_STATIC_STRING(
        "Missing client channel factory in args for client channel filter");
    return;
  }
  // Get server name to resolve, using proxy mapper if needed.
  const char* server_uri = grpc_channel_arg_get_string(
      grpc_channel_args_find(args->channel_args, GRPC_ARG_SERVER_URI));
  if (server_uri == nullptr) {
    *error = GRPC_ERROR_CREATE_FROM_STATIC_STRING(
        "server URI channel arg missing or wrong type in client channel "
        "filter");
    return;
  }
  // Get default service config.  If none is specified via the client API,
  // we use an empty config.
  const char* service_config_json = grpc_channel_arg_get_string(
      grpc_channel_args_find(args->channel_args, GRPC_ARG_SERVICE_CONFIG));
  if (service_config_json == nullptr) service_config_json = "{}";
  *error = GRPC_ERROR_NONE;
  default_service_config_ =
      ServiceConfig::Create(args->channel_args, service_config_json, error);
  if (*error != GRPC_ERROR_NONE) {
    default_service_config_.reset();
    return;
  }
  absl::StatusOr<URI> uri = URI::Parse(server_uri);
  if (uri.ok() && !uri->path().empty()) {
    server_name_ = std::string(absl::StripPrefix(uri->path(), "/"));
  }
  char* proxy_name = nullptr;
  grpc_channel_args* new_args = nullptr;
  ProxyMapperRegistry::MapName(server_uri, args->channel_args, &proxy_name,
                               &new_args);
  target_uri_.reset(proxy_name != nullptr ? proxy_name
                                          : gpr_strdup(server_uri));
  // Strip out service config channel arg, so that it doesn't affect
  // subchannel uniqueness when the args flow down to that layer.
  const char* arg_to_remove = GRPC_ARG_SERVICE_CONFIG;
  channel_args_ = grpc_channel_args_copy_and_remove(
      new_args != nullptr ? new_args : args->channel_args, &arg_to_remove, 1);
  grpc_channel_args_destroy(new_args);
  keepalive_time_ = grpc_channel_args_find_integer(
      channel_args_, GRPC_ARG_KEEPALIVE_TIME_MS,
      {-1 /* default value, unset */, 1, INT_MAX});
  if (!ResolverRegistry::IsValidTarget(target_uri_.get())) {
    std::string error_message =
        absl::StrCat("the target uri is not valid: ", target_uri_.get());
    *error = GRPC_ERROR_CREATE_FROM_COPIED_STRING(error_message.c_str());
    return;
  }
  *error = GRPC_ERROR_NONE;
}

ChannelData::~ChannelData() {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
    gpr_log(GPR_INFO, "chand=%p: destroying channel", this);
  }
  DestroyResolverAndLbPolicyLocked();
  grpc_channel_args_destroy(channel_args_);
  GRPC_ERROR_UNREF(resolver_transient_failure_error_);
  // Stop backup polling.
  grpc_client_channel_stop_backup_polling(interested_parties_);
  grpc_pollset_set_destroy(interested_parties_);
  GRPC_ERROR_UNREF(disconnect_error_.Load(MemoryOrder::RELAXED));
}

RefCountedPtr<LoadBalancingPolicy::Config> ChooseLbPolicy(
    const Resolver::Result& resolver_result,
    const internal::ClientChannelGlobalParsedConfig* parsed_service_config) {
  // Prefer the LB policy config found in the service config.
  if (parsed_service_config->parsed_lb_config() != nullptr) {
    return parsed_service_config->parsed_lb_config();
  }
  // Try the deprecated LB policy name from the service config.
  // If not, try the setting from channel args.
  const char* policy_name = nullptr;
  if (!parsed_service_config->parsed_deprecated_lb_policy().empty()) {
    policy_name = parsed_service_config->parsed_deprecated_lb_policy().c_str();
  } else {
    const grpc_arg* channel_arg =
        grpc_channel_args_find(resolver_result.args, GRPC_ARG_LB_POLICY_NAME);
    policy_name = grpc_channel_arg_get_string(channel_arg);
  }
  // Use pick_first if nothing was specified and we didn't select grpclb
  // above.
  if (policy_name == nullptr) policy_name = "pick_first";
  // Now that we have the policy name, construct an empty config for it.
  Json config_json = Json::Array{Json::Object{
      {policy_name, Json::Object{}},
  }};
  grpc_error* parse_error = GRPC_ERROR_NONE;
  auto lb_policy_config = LoadBalancingPolicyRegistry::ParseLoadBalancingConfig(
      config_json, &parse_error);
  // The policy name came from one of three places:
  // - The deprecated loadBalancingPolicy field in the service config,
  //   in which case the code in ClientChannelServiceConfigParser
  //   already verified that the policy does not require a config.
  // - One of the hard-coded values here, all of which are known to not
  //   require a config.
  // - A channel arg, in which case the application did something that
  //   is a misuse of our API.
  // In the first two cases, these assertions will always be true.  In
  // the last case, this is probably fine for now.
  // TODO(roth): If the last case becomes a problem, add better error
  // handling here.
  GPR_ASSERT(lb_policy_config != nullptr);
  GPR_ASSERT(parse_error == GRPC_ERROR_NONE);
  return lb_policy_config;
}

void ChannelData::OnResolverResultChangedLocked(Resolver::Result result) {
  // Handle race conditions.
  if (resolver_ == nullptr) return;
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
    gpr_log(GPR_INFO, "chand=%p: got resolver result", this);
  }
  // We only want to trace the address resolution in the follow cases:
  // (a) Address resolution resulted in service config change.
  // (b) Address resolution that causes number of backends to go from
  //     zero to non-zero.
  // (c) Address resolution that causes number of backends to go from
  //     non-zero to zero.
  // (d) Address resolution that causes a new LB policy to be created.
  //
  // We track a list of strings to eventually be concatenated and traced.
  absl::InlinedVector<const char*, 3> trace_strings;
  if (result.addresses.empty() && previous_resolution_contained_addresses_) {
    trace_strings.push_back("Address list became empty");
  } else if (!result.addresses.empty() &&
             !previous_resolution_contained_addresses_) {
    trace_strings.push_back("Address list became non-empty");
  }
  previous_resolution_contained_addresses_ = !result.addresses.empty();
  // The result of grpc_error_string() is owned by the error itself.
  // We're storing that string in trace_strings, so we need to make sure
  // that the error lives until we're done with the string.
  grpc_error* service_config_error =
      GRPC_ERROR_REF(result.service_config_error);
  if (service_config_error != GRPC_ERROR_NONE) {
    trace_strings.push_back(grpc_error_string(service_config_error));
  }
  // Choose the service config.
  RefCountedPtr<ServiceConfig> service_config;
  RefCountedPtr<ConfigSelector> config_selector;
  if (service_config_error != GRPC_ERROR_NONE) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
      gpr_log(GPR_INFO, "chand=%p: resolver returned service config error: %s",
              this, grpc_error_string(service_config_error));
    }
    // If the service config was invalid, then fallback to the
    // previously returned service config.
    if (saved_service_config_ != nullptr) {
      if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
        gpr_log(GPR_INFO,
                "chand=%p: resolver returned invalid service config. "
                "Continuing to use previous service config.",
                this);
      }
      service_config = saved_service_config_;
      config_selector = saved_config_selector_;
    } else {
      // We received an invalid service config and we don't have a
      // previous service config to fall back to.  Put the channel into
      // TRANSIENT_FAILURE.
      OnResolverErrorLocked(GRPC_ERROR_REF(service_config_error));
      trace_strings.push_back("no valid service config");
    }
  } else if (result.service_config == nullptr) {
    // Resolver did not return any service config.
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
      gpr_log(GPR_INFO,
              "chand=%p: resolver returned no service config. Using default "
              "service config for channel.",
              this);
    }
    service_config = default_service_config_;
  } else {
    // Use ServiceConfig and ConfigSelector returned by resolver.
    service_config = result.service_config;
    config_selector = ConfigSelector::GetFromChannelArgs(*result.args);
  }
  if (service_config != nullptr) {
    // Extract global config for client channel.
    const internal::ClientChannelGlobalParsedConfig* parsed_service_config =
        static_cast<const internal::ClientChannelGlobalParsedConfig*>(
            service_config->GetGlobalParsedConfig(
                internal::ClientChannelServiceConfigParser::ParserIndex()));
    // Choose LB policy config.
    RefCountedPtr<LoadBalancingPolicy::Config> lb_policy_config =
        ChooseLbPolicy(result, parsed_service_config);
    // Check if the ServiceConfig has changed.
    const bool service_config_changed =
        saved_service_config_ == nullptr ||
        service_config->json_string() != saved_service_config_->json_string();
    // Check if the ConfigSelector has changed.
    const bool config_selector_changed = !ConfigSelector::Equals(
        saved_config_selector_.get(), config_selector.get());
    // If either has changed, apply the global parameters now.
    if (service_config_changed || config_selector_changed) {
      // Update service config in control plane.
      UpdateServiceConfigInControlPlaneLocked(
          std::move(service_config), std::move(config_selector),
          parsed_service_config, lb_policy_config->name());
    } else if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
      gpr_log(GPR_INFO, "chand=%p: service config not changed", this);
    }
    // Create or update LB policy, as needed.
    CreateOrUpdateLbPolicyLocked(std::move(lb_policy_config),
                                 std::move(result));
    if (service_config_changed || config_selector_changed) {
      // Start using new service config for calls.
      // This needs to happen after the LB policy has been updated, since
      // the ConfigSelector may need the LB policy to know about new
      // destinations before it can send RPCs to those destinations.
      UpdateServiceConfigInDataPlaneLocked();
      // TODO(ncteisen): might be worth somehow including a snippet of the
      // config in the trace, at the risk of bloating the trace logs.
      trace_strings.push_back("Service config changed");
    }
  }
  // Add channel trace event.
  if (!trace_strings.empty()) {
    std::string message =
        absl::StrCat("Resolution event: ", absl::StrJoin(trace_strings, ", "));
    if (channelz_node_ != nullptr) {
      channelz_node_->AddTraceEvent(channelz::ChannelTrace::Severity::Info,
                                    grpc_slice_from_cpp_string(message));
    }
  }
  GRPC_ERROR_UNREF(service_config_error);
}

void ChannelData::OnResolverErrorLocked(grpc_error* error) {
  if (resolver_ == nullptr) {
    GRPC_ERROR_UNREF(error);
    return;
  }
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
    gpr_log(GPR_INFO, "chand=%p: resolver transient failure: %s", this,
            grpc_error_string(error));
  }
  // If we already have an LB policy from a previous resolution
  // result, then we continue to let it set the connectivity state.
  // Otherwise, we go into TRANSIENT_FAILURE.
  if (lb_policy_ == nullptr) {
    grpc_error* state_error = GRPC_ERROR_CREATE_REFERENCING_FROM_STATIC_STRING(
        "Resolver transient failure", &error, 1);
    {
      MutexLock lock(&resolution_mu_);
      // Update resolver transient failure.
      GRPC_ERROR_UNREF(resolver_transient_failure_error_);
      resolver_transient_failure_error_ = GRPC_ERROR_REF(state_error);
      // Process calls that were queued waiting for the resolver result.
      for (ResolverQueuedCall* call = resolver_queued_calls_; call != nullptr;
           call = call->next) {
        grpc_call_element* elem = call->elem;
        CallData* calld = static_cast<CallData*>(elem->call_data);
        grpc_error* error = GRPC_ERROR_NONE;
        if (calld->CheckResolutionLocked(elem, &error)) {
          calld->AsyncResolutionDone(elem, error);
        }
      }
    }
    // Update connectivity state.
    UpdateStateAndPickerLocked(
        GRPC_CHANNEL_TRANSIENT_FAILURE, grpc_error_to_absl_status(state_error),
        "resolver failure",
        absl::make_unique<LoadBalancingPolicy::TransientFailurePicker>(
            state_error));
  }
  GRPC_ERROR_UNREF(error);
}

void ChannelData::CreateOrUpdateLbPolicyLocked(
    RefCountedPtr<LoadBalancingPolicy::Config> lb_policy_config,
    Resolver::Result result) {
  // Construct update.
  LoadBalancingPolicy::UpdateArgs update_args;
  update_args.addresses = std::move(result.addresses);
  update_args.config = std::move(lb_policy_config);
  // Remove the config selector from channel args so that we're not holding
  // unnecessary refs that cause it to be destroyed somewhere other than in the
  // WorkSerializer.
  const char* arg_name = GRPC_ARG_CONFIG_SELECTOR;
  update_args.args =
      grpc_channel_args_copy_and_remove(result.args, &arg_name, 1);
  // Create policy if needed.
  if (lb_policy_ == nullptr) {
    lb_policy_ = CreateLbPolicyLocked(*update_args.args);
  }
  // Update the policy.
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
    gpr_log(GPR_INFO, "chand=%p: Updating child policy %p", this,
            lb_policy_.get());
  }
  lb_policy_->UpdateLocked(std::move(update_args));
}

// Creates a new LB policy.
OrphanablePtr<LoadBalancingPolicy> ChannelData::CreateLbPolicyLocked(
    const grpc_channel_args& args) {
  LoadBalancingPolicy::Args lb_policy_args;
  lb_policy_args.work_serializer = work_serializer_;
  lb_policy_args.channel_control_helper =
      absl::make_unique<ClientChannelControlHelper>(this);
  lb_policy_args.args = &args;
  OrphanablePtr<LoadBalancingPolicy> lb_policy =
      MakeOrphanable<ChildPolicyHandler>(std::move(lb_policy_args),
                                         &grpc_client_channel_routing_trace);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
    gpr_log(GPR_INFO, "chand=%p: created new LB policy %p", this,
            lb_policy.get());
  }
  grpc_pollset_set_add_pollset_set(lb_policy->interested_parties(),
                                   interested_parties_);
  return lb_policy;
}

void ChannelData::AddResolverQueuedCall(ResolverQueuedCall* call,
                                        grpc_polling_entity* pollent) {
  // Add call to queued calls list.
  call->next = resolver_queued_calls_;
  resolver_queued_calls_ = call;
  // Add call's pollent to channel's interested_parties, so that I/O
  // can be done under the call's CQ.
  grpc_polling_entity_add_to_pollset_set(pollent, interested_parties_);
}

void ChannelData::RemoveResolverQueuedCall(ResolverQueuedCall* to_remove,
                                           grpc_polling_entity* pollent) {
  // Remove call's pollent from channel's interested_parties.
  grpc_polling_entity_del_from_pollset_set(pollent, interested_parties_);
  // Remove from queued calls list.
  for (ResolverQueuedCall** call = &resolver_queued_calls_; *call != nullptr;
       call = &(*call)->next) {
    if (*call == to_remove) {
      *call = to_remove->next;
      return;
    }
  }
}

void ChannelData::UpdateServiceConfigInControlPlaneLocked(
    RefCountedPtr<ServiceConfig> service_config,
    RefCountedPtr<ConfigSelector> config_selector,
    const internal::ClientChannelGlobalParsedConfig* parsed_service_config,
    const char* lb_policy_name) {
  UniquePtr<char> service_config_json(
      gpr_strdup(service_config->json_string().c_str()));
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
    gpr_log(GPR_INFO,
            "chand=%p: resolver returned updated service config: \"%s\"", this,
            service_config_json.get());
  }
  // Save service config.
  saved_service_config_ = std::move(service_config);
  // Update health check service name if needed.
  if (health_check_service_name_ !=
      parsed_service_config->health_check_service_name()) {
    health_check_service_name_ =
        parsed_service_config->health_check_service_name();
    // Update health check service name used by existing subchannel wrappers.
    for (auto* subchannel_wrapper : subchannel_wrappers_) {
      subchannel_wrapper->UpdateHealthCheckServiceName(
          health_check_service_name_);
    }
  }
  // Swap out the data used by GetChannelInfo().
  UniquePtr<char> lb_policy_name_owned(gpr_strdup(lb_policy_name));
  {
    MutexLock lock(&info_mu_);
    info_lb_policy_name_ = std::move(lb_policy_name_owned);
    info_service_config_json_ = std::move(service_config_json);
  }
  // Save config selector.
  saved_config_selector_ = std::move(config_selector);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
    gpr_log(GPR_INFO, "chand=%p: using ConfigSelector %p", this,
            saved_config_selector_.get());
  }
}

void ChannelData::UpdateServiceConfigInDataPlaneLocked() {
  // Grab ref to service config.
  RefCountedPtr<ServiceConfig> service_config = saved_service_config_;
  // Grab ref to config selector.  Use default if resolver didn't supply one.
  RefCountedPtr<ConfigSelector> config_selector = saved_config_selector_;
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
    gpr_log(GPR_INFO, "chand=%p: switching to ConfigSelector %p", this,
            saved_config_selector_.get());
  }
  if (config_selector == nullptr) {
    config_selector =
        MakeRefCounted<DefaultConfigSelector>(saved_service_config_);
  }
  // Get retry throttle data from service config.
  const internal::ClientChannelGlobalParsedConfig* parsed_service_config =
      static_cast<const internal::ClientChannelGlobalParsedConfig*>(
          saved_service_config_->GetGlobalParsedConfig(
              internal::ClientChannelServiceConfigParser::ParserIndex()));
  absl::optional<internal::ClientChannelGlobalParsedConfig::RetryThrottling>
      retry_throttle_config = parsed_service_config->retry_throttling();
  RefCountedPtr<ServerRetryThrottleData> retry_throttle_data;
  if (retry_throttle_config.has_value()) {
    retry_throttle_data = internal::ServerRetryThrottleMap::GetDataForServer(
        server_name_, retry_throttle_config.value().max_milli_tokens,
        retry_throttle_config.value().milli_token_ratio);
  }
  // Construct per-LB filter stack.
  std::vector<const grpc_channel_filter*> filters =
      config_selector->GetFilters();
  filters.push_back(&kDynamicTerminationFilterVtable);
  absl::InlinedVector<grpc_arg, 2> args_to_add;
  args_to_add.push_back(grpc_channel_arg_pointer_create(
      const_cast<char*>(GRPC_ARG_CLIENT_CHANNEL_DATA), this,
      &kChannelDataArgPointerVtable));
  if (retry_throttle_data != nullptr) {
    args_to_add.push_back(grpc_channel_arg_pointer_create(
        const_cast<char*>(GRPC_ARG_RETRY_THROTTLE_DATA),
        retry_throttle_data.get(), &kRetryThrottleDataArgPointerVtable));
  }
  grpc_channel_args* new_args = grpc_channel_args_copy_and_add(
      channel_args_, args_to_add.data(), args_to_add.size());
  RefCountedPtr<DynamicFilters> dynamic_filters =
      DynamicFilters::Create(new_args, std::move(filters));
  GPR_ASSERT(dynamic_filters != nullptr);
  grpc_channel_args_destroy(new_args);
  // Grab data plane lock to update service config.
  //
  // We defer unreffing the old values (and deallocating memory) until
  // after releasing the lock to keep the critical section small.
  std::set<grpc_call_element*> calls_pending_resolver_result;
  {
    MutexLock lock(&resolution_mu_);
    GRPC_ERROR_UNREF(resolver_transient_failure_error_);
    resolver_transient_failure_error_ = GRPC_ERROR_NONE;
    // Update service config.
    received_service_config_data_ = true;
    // Old values will be unreffed after lock is released.
    service_config_.swap(service_config);
    config_selector_.swap(config_selector);
    dynamic_filters_.swap(dynamic_filters);
    // Process calls that were queued waiting for the resolver result.
    for (ResolverQueuedCall* call = resolver_queued_calls_; call != nullptr;
         call = call->next) {
      grpc_call_element* elem = call->elem;
      CallData* calld = static_cast<CallData*>(elem->call_data);
      grpc_error* error = GRPC_ERROR_NONE;
      if (calld->CheckResolutionLocked(elem, &error)) {
        calld->AsyncResolutionDone(elem, error);
      }
    }
  }
  // Old values will be unreffed after lock is released when they go out
  // of scope.
}

void ChannelData::CreateResolverLocked() {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
    gpr_log(GPR_INFO, "chand=%p: starting name resolution", this);
  }
  resolver_ = ResolverRegistry::CreateResolver(
      target_uri_.get(), channel_args_, interested_parties_, work_serializer_,
      absl::make_unique<ResolverResultHandler>(this));
  // Since the validity of the args was checked when the channel was created,
  // CreateResolver() must return a non-null result.
  GPR_ASSERT(resolver_ != nullptr);
  UpdateStateAndPickerLocked(
      GRPC_CHANNEL_CONNECTING, absl::Status(), "started resolving",
      absl::make_unique<LoadBalancingPolicy::QueuePicker>(nullptr));
  resolver_->StartLocked();
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
    gpr_log(GPR_INFO, "chand=%p: created resolver=%p", this, resolver_.get());
  }
}

void ChannelData::DestroyResolverAndLbPolicyLocked() {
  if (resolver_ != nullptr) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
      gpr_log(GPR_INFO, "chand=%p: shutting down resolver=%p", this,
              resolver_.get());
    }
    resolver_.reset();
    if (lb_policy_ != nullptr) {
      if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
        gpr_log(GPR_INFO, "chand=%p: shutting down lb_policy=%p", this,
                lb_policy_.get());
      }
      grpc_pollset_set_del_pollset_set(lb_policy_->interested_parties(),
                                       interested_parties_);
      lb_policy_.reset();
    }
  }
}

void ChannelData::UpdateStateAndPickerLocked(
    grpc_connectivity_state state, const absl::Status& status,
    const char* reason,
    std::unique_ptr<LoadBalancingPolicy::SubchannelPicker> picker) {
  // Special case for IDLE and SHUTDOWN states.
  if (picker == nullptr || state == GRPC_CHANNEL_SHUTDOWN) {
    saved_service_config_.reset();
    saved_config_selector_.reset();
    // Acquire resolution lock to update config selector and associated state.
    // To minimize lock contention, we wait to unref these objects until
    // after we release the lock.
    RefCountedPtr<ServiceConfig> service_config_to_unref;
    RefCountedPtr<ConfigSelector> config_selector_to_unref;
    RefCountedPtr<DynamicFilters> dynamic_filters_to_unref;
    {
      MutexLock lock(&resolution_mu_);
      received_service_config_data_ = false;
      service_config_to_unref = std::move(service_config_);
      config_selector_to_unref = std::move(config_selector_);
      dynamic_filters_to_unref = std::move(dynamic_filters_);
    }
  }
  // Update connectivity state.
  state_tracker_.SetState(state, status, reason);
  if (channelz_node_ != nullptr) {
    channelz_node_->SetConnectivityState(state);
    channelz_node_->AddTraceEvent(
        channelz::ChannelTrace::Severity::Info,
        grpc_slice_from_static_string(
            channelz::ChannelNode::GetChannelConnectivityStateChangeString(
                state)));
  }
  // Grab data plane lock to do subchannel updates and update the picker.
  //
  // Note that we want to minimize the work done while holding the data
  // plane lock, to keep the critical section small.  So, for all of the
  // objects that we might wind up unreffing here, we actually hold onto
  // the refs until after we release the lock, and then unref them at
  // that point.  This includes the following:
  // - refs to subchannel wrappers in the keys of pending_subchannel_updates_
  // - ownership of the existing picker in picker_
  {
    MutexLock lock(&data_plane_mu_);
    // Handle subchannel updates.
    for (auto& p : pending_subchannel_updates_) {
      if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
        gpr_log(GPR_INFO,
                "chand=%p: updating subchannel wrapper %p data plane "
                "connected_subchannel to %p",
                this, p.first.get(), p.second.get());
      }
      // Note: We do not remove the entry from pending_subchannel_updates_
      // here, since this would unref the subchannel wrapper; instead,
      // we wait until we've released the lock to clear the map.
      p.first->set_connected_subchannel_in_data_plane(std::move(p.second));
    }
    // Swap out the picker.
    // Note: Original value will be destroyed after the lock is released.
    picker_.swap(picker);
    // Re-process queued picks.
    for (LbQueuedCall* call = lb_queued_calls_; call != nullptr;
         call = call->next) {
      grpc_error* error = GRPC_ERROR_NONE;
      if (call->lb_call->PickSubchannelLocked(&error)) {
        call->lb_call->AsyncPickDone(error);
      }
    }
  }
  // Clear the pending update map after releasing the lock, to keep the
  // critical section small.
  pending_subchannel_updates_.clear();
}

grpc_error* ChannelData::DoPingLocked(grpc_transport_op* op) {
  if (state_tracker_.state() != GRPC_CHANNEL_READY) {
    return GRPC_ERROR_CREATE_FROM_STATIC_STRING("channel not connected");
  }
  LoadBalancingPolicy::PickResult result =
      picker_->Pick(LoadBalancingPolicy::PickArgs());
  ConnectedSubchannel* connected_subchannel = nullptr;
  if (result.subchannel != nullptr) {
    SubchannelWrapper* subchannel =
        static_cast<SubchannelWrapper*>(result.subchannel.get());
    connected_subchannel = subchannel->connected_subchannel();
  }
  if (connected_subchannel != nullptr) {
    connected_subchannel->Ping(op->send_ping.on_initiate, op->send_ping.on_ack);
  } else {
    if (result.error == GRPC_ERROR_NONE) {
      result.error = GRPC_ERROR_CREATE_FROM_STATIC_STRING(
          "LB policy dropped call on ping");
    }
  }
  return result.error;
}

void ChannelData::StartTransportOpLocked(grpc_transport_op* op) {
  // Connectivity watch.
  if (op->start_connectivity_watch != nullptr) {
    state_tracker_.AddWatcher(op->start_connectivity_watch_state,
                              std::move(op->start_connectivity_watch));
  }
  if (op->stop_connectivity_watch != nullptr) {
    state_tracker_.RemoveWatcher(op->stop_connectivity_watch);
  }
  // Ping.
  if (op->send_ping.on_initiate != nullptr || op->send_ping.on_ack != nullptr) {
    grpc_error* error = DoPingLocked(op);
    if (error != GRPC_ERROR_NONE) {
      ExecCtx::Run(DEBUG_LOCATION, op->send_ping.on_initiate,
                   GRPC_ERROR_REF(error));
      ExecCtx::Run(DEBUG_LOCATION, op->send_ping.on_ack, error);
    }
    op->bind_pollset = nullptr;
    op->send_ping.on_initiate = nullptr;
    op->send_ping.on_ack = nullptr;
  }
  // Reset backoff.
  if (op->reset_connect_backoff) {
    if (lb_policy_ != nullptr) {
      lb_policy_->ResetBackoffLocked();
    }
  }
  // Disconnect or enter IDLE.
  if (op->disconnect_with_error != GRPC_ERROR_NONE) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
      gpr_log(GPR_INFO, "chand=%p: disconnect_with_error: %s", this,
              grpc_error_string(op->disconnect_with_error));
    }
    DestroyResolverAndLbPolicyLocked();
    intptr_t value;
    if (grpc_error_get_int(op->disconnect_with_error,
                           GRPC_ERROR_INT_CHANNEL_CONNECTIVITY_STATE, &value) &&
        static_cast<grpc_connectivity_state>(value) == GRPC_CHANNEL_IDLE) {
      if (disconnect_error() == GRPC_ERROR_NONE) {
        // Enter IDLE state.
        UpdateStateAndPickerLocked(GRPC_CHANNEL_IDLE, absl::Status(),
                                   "channel entering IDLE", nullptr);
      }
      GRPC_ERROR_UNREF(op->disconnect_with_error);
    } else {
      // Disconnect.
      GPR_ASSERT(disconnect_error_.Load(MemoryOrder::RELAXED) ==
                 GRPC_ERROR_NONE);
      disconnect_error_.Store(op->disconnect_with_error, MemoryOrder::RELEASE);
      UpdateStateAndPickerLocked(
          GRPC_CHANNEL_SHUTDOWN, absl::Status(), "shutdown from API",
          absl::make_unique<LoadBalancingPolicy::TransientFailurePicker>(
              GRPC_ERROR_REF(op->disconnect_with_error)));
    }
  }
  GRPC_CHANNEL_STACK_UNREF(owning_stack_, "start_transport_op");
  ExecCtx::Run(DEBUG_LOCATION, op->on_consumed, GRPC_ERROR_NONE);
}

void ChannelData::StartTransportOp(grpc_channel_element* elem,
                                   grpc_transport_op* op) {
  ChannelData* chand = static_cast<ChannelData*>(elem->channel_data);
  GPR_ASSERT(op->set_accept_stream == false);
  // Handle bind_pollset.
  if (op->bind_pollset != nullptr) {
    grpc_pollset_set_add_pollset(chand->interested_parties_, op->bind_pollset);
  }
  // Pop into control plane work_serializer for remaining ops.
  GRPC_CHANNEL_STACK_REF(chand->owning_stack_, "start_transport_op");
  chand->work_serializer_->Run(
      [chand, op]() { chand->StartTransportOpLocked(op); }, DEBUG_LOCATION);
}

void ChannelData::GetChannelInfo(grpc_channel_element* elem,
                                 const grpc_channel_info* info) {
  ChannelData* chand = static_cast<ChannelData*>(elem->channel_data);
  MutexLock lock(&chand->info_mu_);
  if (info->lb_policy_name != nullptr) {
    *info->lb_policy_name = gpr_strdup(chand->info_lb_policy_name_.get());
  }
  if (info->service_config_json != nullptr) {
    *info->service_config_json =
        gpr_strdup(chand->info_service_config_json_.get());
  }
}

void ChannelData::AddLbQueuedCall(LbQueuedCall* call,
                                  grpc_polling_entity* pollent) {
  // Add call to queued picks list.
  call->next = lb_queued_calls_;
  lb_queued_calls_ = call;
  // Add call's pollent to channel's interested_parties, so that I/O
  // can be done under the call's CQ.
  grpc_polling_entity_add_to_pollset_set(pollent, interested_parties_);
}

void ChannelData::RemoveLbQueuedCall(LbQueuedCall* to_remove,
                                     grpc_polling_entity* pollent) {
  // Remove call's pollent from channel's interested_parties.
  grpc_polling_entity_del_from_pollset_set(pollent, interested_parties_);
  // Remove from queued picks list.
  for (LbQueuedCall** call = &lb_queued_calls_; *call != nullptr;
       call = &(*call)->next) {
    if (*call == to_remove) {
      *call = to_remove->next;
      return;
    }
  }
}

RefCountedPtr<ConnectedSubchannel>
ChannelData::GetConnectedSubchannelInDataPlane(
    SubchannelInterface* subchannel) const {
  SubchannelWrapper* subchannel_wrapper =
      static_cast<SubchannelWrapper*>(subchannel);
  ConnectedSubchannel* connected_subchannel =
      subchannel_wrapper->connected_subchannel_in_data_plane();
  if (connected_subchannel == nullptr) return nullptr;
  return connected_subchannel->Ref();
}

void ChannelData::TryToConnectLocked() {
  if (lb_policy_ != nullptr) {
    lb_policy_->ExitIdleLocked();
  } else if (resolver_ == nullptr) {
    CreateResolverLocked();
  }
  GRPC_CHANNEL_STACK_UNREF(owning_stack_, "TryToConnect");
}

grpc_connectivity_state ChannelData::CheckConnectivityState(
    bool try_to_connect) {
  grpc_connectivity_state out = state_tracker_.state();
  if (out == GRPC_CHANNEL_IDLE && try_to_connect) {
    GRPC_CHANNEL_STACK_REF(owning_stack_, "TryToConnect");
    work_serializer_->Run([this]() { TryToConnectLocked(); }, DEBUG_LOCATION);
  }
  return out;
}

void ChannelData::AddConnectivityWatcher(
    grpc_connectivity_state initial_state,
    OrphanablePtr<AsyncConnectivityStateWatcherInterface> watcher) {
  new ConnectivityWatcherAdder(this, initial_state, std::move(watcher));
}

void ChannelData::RemoveConnectivityWatcher(
    AsyncConnectivityStateWatcherInterface* watcher) {
  new ConnectivityWatcherRemover(this, watcher);
}

//
// CallData implementation
//

CallData::CallData(grpc_call_element* elem, const ChannelData& chand,
                   const grpc_call_element_args& args)
    : deadline_state_(elem, args,
                      GPR_LIKELY(chand.deadline_checking_enabled())
                          ? args.deadline
                          : GRPC_MILLIS_INF_FUTURE),
      path_(grpc_slice_ref_internal(args.path)),
      call_start_time_(args.start_time),
      deadline_(args.deadline),
      arena_(args.arena),
      owning_call_(args.call_stack),
      call_combiner_(args.call_combiner),
      call_context_(args.context) {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
    gpr_log(GPR_INFO, "chand=%p calld=%p: created call", &chand, this);
  }
}

CallData::~CallData() {
  grpc_slice_unref_internal(path_);
  GRPC_ERROR_UNREF(cancel_error_);
  // Make sure there are no remaining pending batches.
  for (size_t i = 0; i < GPR_ARRAY_SIZE(pending_batches_); ++i) {
    GPR_ASSERT(pending_batches_[i] == nullptr);
  }
}

grpc_error* CallData::Init(grpc_call_element* elem,
                           const grpc_call_element_args* args) {
  ChannelData* chand = static_cast<ChannelData*>(elem->channel_data);
  new (elem->call_data) CallData(elem, *chand, *args);
  return GRPC_ERROR_NONE;
}

void CallData::Destroy(grpc_call_element* elem,
                       const grpc_call_final_info* /*final_info*/,
                       grpc_closure* then_schedule_closure) {
  CallData* calld = static_cast<CallData*>(elem->call_data);
  RefCountedPtr<DynamicFilters::Call> dynamic_call =
      std::move(calld->dynamic_call_);
  calld->~CallData();
  if (GPR_LIKELY(dynamic_call != nullptr)) {
    dynamic_call->SetAfterCallStackDestroy(then_schedule_closure);
  } else {
    // TODO(yashkt) : This can potentially be a Closure::Run
    ExecCtx::Run(DEBUG_LOCATION, then_schedule_closure, GRPC_ERROR_NONE);
  }
}

void CallData::StartTransportStreamOpBatch(
    grpc_call_element* elem, grpc_transport_stream_op_batch* batch) {
  GPR_TIMER_SCOPE("cc_start_transport_stream_op_batch", 0);
  CallData* calld = static_cast<CallData*>(elem->call_data);
  ChannelData* chand = static_cast<ChannelData*>(elem->channel_data);
  if (GPR_LIKELY(chand->deadline_checking_enabled())) {
    grpc_deadline_state_client_start_transport_stream_op_batch(elem, batch);
  }
  // Intercept recv_initial_metadata for config selector on-committed callback.
  if (batch->recv_initial_metadata) {
    calld->InjectRecvInitialMetadataReadyForConfigSelectorCommitCallback(batch);
  }
  // If we've previously been cancelled, immediately fail any new batches.
  if (GPR_UNLIKELY(calld->cancel_error_ != GRPC_ERROR_NONE)) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
      gpr_log(GPR_INFO, "chand=%p calld=%p: failing batch with error: %s",
              chand, calld, grpc_error_string(calld->cancel_error_));
    }
    // Note: This will release the call combiner.
    grpc_transport_stream_op_batch_finish_with_failure(
        batch, GRPC_ERROR_REF(calld->cancel_error_), calld->call_combiner_);
    return;
  }
  // Handle cancellation.
  if (GPR_UNLIKELY(batch->cancel_stream)) {
    // Stash a copy of cancel_error in our call data, so that we can use
    // it for subsequent operations.  This ensures that if the call is
    // cancelled before any batches are passed down (e.g., if the deadline
    // is in the past when the call starts), we can return the right
    // error to the caller when the first batch does get passed down.
    GRPC_ERROR_UNREF(calld->cancel_error_);
    calld->cancel_error_ =
        GRPC_ERROR_REF(batch->payload->cancel_stream.cancel_error);
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
      gpr_log(GPR_INFO, "chand=%p calld=%p: recording cancel_error=%s", chand,
              calld, grpc_error_string(calld->cancel_error_));
    }
    // If we do not have a dynamic call (i.e., name resolution has not
    // yet completed), fail all pending batches.  Otherwise, send the
    // cancellation down to the dynamic call.
    if (calld->dynamic_call_ == nullptr) {
      calld->PendingBatchesFail(elem, GRPC_ERROR_REF(calld->cancel_error_),
                                NoYieldCallCombiner);
      // Note: This will release the call combiner.
      grpc_transport_stream_op_batch_finish_with_failure(
          batch, GRPC_ERROR_REF(calld->cancel_error_), calld->call_combiner_);
    } else {
      // Note: This will release the call combiner.
      calld->dynamic_call_->StartTransportStreamOpBatch(batch);
    }
    return;
  }
  // Add the batch to the pending list.
  calld->PendingBatchesAdd(elem, batch);
  // Check if we've already created a dynamic call.
  // Note that once we have done so, we do not need to acquire the channel's
  // resolution mutex, which is more efficient (especially for streaming calls).
  if (calld->dynamic_call_ != nullptr) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
      gpr_log(GPR_INFO, "chand=%p calld=%p: starting batch on dynamic_call=%p",
              chand, calld, calld->dynamic_call_.get());
    }
    calld->PendingBatchesResume(elem);
    return;
  }
  // We do not yet have a dynamic call.
  // For batches containing a send_initial_metadata op, acquire the
  // channel's resolution mutex to apply the service config to the call,
  // after which we will create a dynamic call.
  if (GPR_LIKELY(batch->send_initial_metadata)) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
      gpr_log(GPR_INFO,
              "chand=%p calld=%p: grabbing resolution mutex to apply service "
              "config",
              chand, calld);
    }
    CheckResolution(elem, GRPC_ERROR_NONE);
  } else {
    // For all other batches, release the call combiner.
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
      gpr_log(GPR_INFO,
              "chand=%p calld=%p: saved batch, yielding call combiner", chand,
              calld);
    }
    GRPC_CALL_COMBINER_STOP(calld->call_combiner_,
                            "batch does not include send_initial_metadata");
  }
}

void CallData::SetPollent(grpc_call_element* elem,
                          grpc_polling_entity* pollent) {
  CallData* calld = static_cast<CallData*>(elem->call_data);
  calld->pollent_ = pollent;
}

//
// pending_batches management
//

size_t CallData::GetBatchIndex(grpc_transport_stream_op_batch* batch) {
  // Note: It is important the send_initial_metadata be the first entry
  // here, since the code in pick_subchannel_locked() assumes it will be.
  if (batch->send_initial_metadata) return 0;
  if (batch->send_message) return 1;
  if (batch->send_trailing_metadata) return 2;
  if (batch->recv_initial_metadata) return 3;
  if (batch->recv_message) return 4;
  if (batch->recv_trailing_metadata) return 5;
  GPR_UNREACHABLE_CODE(return (size_t)-1);
}

// This is called via the call combiner, so access to calld is synchronized.
void CallData::PendingBatchesAdd(grpc_call_element* elem,
                                 grpc_transport_stream_op_batch* batch) {
  ChannelData* chand = static_cast<ChannelData*>(elem->channel_data);
  const size_t idx = GetBatchIndex(batch);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
    gpr_log(GPR_INFO,
            "chand=%p calld=%p: adding pending batch at index %" PRIuPTR, chand,
            this, idx);
  }
  grpc_transport_stream_op_batch*& pending = pending_batches_[idx];
  GPR_ASSERT(pending == nullptr);
  pending = batch;
}

// This is called via the call combiner, so access to calld is synchronized.
void CallData::FailPendingBatchInCallCombiner(void* arg, grpc_error* error) {
  grpc_transport_stream_op_batch* batch =
      static_cast<grpc_transport_stream_op_batch*>(arg);
  CallData* calld = static_cast<CallData*>(batch->handler_private.extra_arg);
  // Note: This will release the call combiner.
  grpc_transport_stream_op_batch_finish_with_failure(
      batch, GRPC_ERROR_REF(error), calld->call_combiner_);
}

// This is called via the call combiner, so access to calld is synchronized.
void CallData::PendingBatchesFail(
    grpc_call_element* elem, grpc_error* error,
    YieldCallCombinerPredicate yield_call_combiner_predicate) {
  GPR_ASSERT(error != GRPC_ERROR_NONE);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
    size_t num_batches = 0;
    for (size_t i = 0; i < GPR_ARRAY_SIZE(pending_batches_); ++i) {
      if (pending_batches_[i] != nullptr) ++num_batches;
    }
    gpr_log(GPR_INFO,
            "chand=%p calld=%p: failing %" PRIuPTR " pending batches: %s",
            elem->channel_data, this, num_batches, grpc_error_string(error));
  }
  CallCombinerClosureList closures;
  for (size_t i = 0; i < GPR_ARRAY_SIZE(pending_batches_); ++i) {
    grpc_transport_stream_op_batch*& batch = pending_batches_[i];
    if (batch != nullptr) {
      batch->handler_private.extra_arg = this;
      GRPC_CLOSURE_INIT(&batch->handler_private.closure,
                        FailPendingBatchInCallCombiner, batch,
                        grpc_schedule_on_exec_ctx);
      closures.Add(&batch->handler_private.closure, GRPC_ERROR_REF(error),
                   "PendingBatchesFail");
      batch = nullptr;
    }
  }
  if (yield_call_combiner_predicate(closures)) {
    closures.RunClosures(call_combiner_);
  } else {
    closures.RunClosuresWithoutYielding(call_combiner_);
  }
  GRPC_ERROR_UNREF(error);
}

// This is called via the call combiner, so access to calld is synchronized.
void CallData::ResumePendingBatchInCallCombiner(void* arg,
                                                grpc_error* /*ignored*/) {
  grpc_transport_stream_op_batch* batch =
      static_cast<grpc_transport_stream_op_batch*>(arg);
  auto* elem =
      static_cast<grpc_call_element*>(batch->handler_private.extra_arg);
  auto* calld = static_cast<CallData*>(elem->call_data);
  // Note: This will release the call combiner.
  calld->dynamic_call_->StartTransportStreamOpBatch(batch);
}

// This is called via the call combiner, so access to calld is synchronized.
void CallData::PendingBatchesResume(grpc_call_element* elem) {
  ChannelData* chand = static_cast<ChannelData*>(elem->channel_data);
  // Retries not enabled; send down batches as-is.
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
    size_t num_batches = 0;
    for (size_t i = 0; i < GPR_ARRAY_SIZE(pending_batches_); ++i) {
      if (pending_batches_[i] != nullptr) ++num_batches;
    }
    gpr_log(GPR_INFO,
            "chand=%p calld=%p: starting %" PRIuPTR
            " pending batches on dynamic_call=%p",
            chand, this, num_batches, dynamic_call_.get());
  }
  CallCombinerClosureList closures;
  for (size_t i = 0; i < GPR_ARRAY_SIZE(pending_batches_); ++i) {
    grpc_transport_stream_op_batch*& batch = pending_batches_[i];
    if (batch != nullptr) {
      batch->handler_private.extra_arg = elem;
      GRPC_CLOSURE_INIT(&batch->handler_private.closure,
                        ResumePendingBatchInCallCombiner, batch, nullptr);
      closures.Add(&batch->handler_private.closure, GRPC_ERROR_NONE,
                   "PendingBatchesResume");
      batch = nullptr;
    }
  }
  // Note: This will release the call combiner.
  closures.RunClosures(call_combiner_);
}

//
// name resolution
//

// A class to handle the call combiner cancellation callback for a
// queued pick.
class CallData::ResolverQueuedCallCanceller {
 public:
  explicit ResolverQueuedCallCanceller(grpc_call_element* elem) : elem_(elem) {
    auto* calld = static_cast<CallData*>(elem->call_data);
    GRPC_CALL_STACK_REF(calld->owning_call_, "ResolverQueuedCallCanceller");
    GRPC_CLOSURE_INIT(&closure_, &CancelLocked, this,
                      grpc_schedule_on_exec_ctx);
    calld->call_combiner_->SetNotifyOnCancel(&closure_);
  }

 private:
  static void CancelLocked(void* arg, grpc_error* error) {
    auto* self = static_cast<ResolverQueuedCallCanceller*>(arg);
    auto* chand = static_cast<ChannelData*>(self->elem_->channel_data);
    auto* calld = static_cast<CallData*>(self->elem_->call_data);
    {
      MutexLock lock(chand->resolution_mu());
      if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
        gpr_log(GPR_INFO,
                "chand=%p calld=%p: cancelling resolver queued pick: "
                "error=%s self=%p calld->resolver_pick_canceller=%p",
                chand, calld, grpc_error_string(error), self,
                calld->resolver_call_canceller_);
      }
      if (calld->resolver_call_canceller_ == self && error != GRPC_ERROR_NONE) {
        // Remove pick from list of queued picks.
        calld->MaybeRemoveCallFromResolverQueuedCallsLocked(self->elem_);
        // Fail pending batches on the call.
        calld->PendingBatchesFail(self->elem_, GRPC_ERROR_REF(error),
                                  YieldCallCombinerIfPendingBatchesFound);
      }
    }
    GRPC_CALL_STACK_UNREF(calld->owning_call_, "ResolvingQueuedCallCanceller");
    delete self;
  }

  grpc_call_element* elem_;
  grpc_closure closure_;
};

void CallData::MaybeRemoveCallFromResolverQueuedCallsLocked(
    grpc_call_element* elem) {
  if (!queued_pending_resolver_result_) return;
  auto* chand = static_cast<ChannelData*>(elem->channel_data);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
    gpr_log(GPR_INFO,
            "chand=%p calld=%p: removing from resolver queued picks list",
            chand, this);
  }
  chand->RemoveResolverQueuedCall(&resolver_queued_call_, pollent_);
  queued_pending_resolver_result_ = false;
  // Lame the call combiner canceller.
  resolver_call_canceller_ = nullptr;
}

void CallData::MaybeAddCallToResolverQueuedCallsLocked(
    grpc_call_element* elem) {
  if (queued_pending_resolver_result_) return;
  auto* chand = static_cast<ChannelData*>(elem->channel_data);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
    gpr_log(GPR_INFO, "chand=%p calld=%p: adding to resolver queued picks list",
            chand, this);
  }
  queued_pending_resolver_result_ = true;
  resolver_queued_call_.elem = elem;
  chand->AddResolverQueuedCall(&resolver_queued_call_, pollent_);
  // Register call combiner cancellation callback.
  resolver_call_canceller_ = new ResolverQueuedCallCanceller(elem);
}

grpc_error* CallData::ApplyServiceConfigToCallLocked(
    grpc_call_element* elem, grpc_metadata_batch* initial_metadata) {
  ChannelData* chand = static_cast<ChannelData*>(elem->channel_data);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
    gpr_log(GPR_INFO, "chand=%p calld=%p: applying service config to call",
            chand, this);
  }
  ConfigSelector* config_selector = chand->config_selector();
  if (config_selector != nullptr) {
    // Use the ConfigSelector to determine the config for the call.
    ConfigSelector::CallConfig call_config =
        config_selector->GetCallConfig({&path_, initial_metadata, arena_});
    if (call_config.error != GRPC_ERROR_NONE) return call_config.error;
    on_call_committed_ = std::move(call_config.on_call_committed);
    // Create a ServiceConfigCallData for the call.  This stores a ref to the
    // ServiceConfig and caches the right set of parsed configs to use for
    // the call.  The MethodConfig will store itself in the call context,
    // so that it can be accessed by filters in the subchannel, and it
    // will be cleaned up when the call ends.
    auto* service_config_call_data = arena_->New<ServiceConfigCallData>(
        std::move(call_config.service_config), call_config.method_configs,
        std::move(call_config.call_attributes), call_context_);
    // Apply our own method params to the call.
    auto* method_params = static_cast<ClientChannelMethodParsedConfig*>(
        service_config_call_data->GetMethodParsedConfig(
            internal::ClientChannelServiceConfigParser::ParserIndex()));
    if (method_params != nullptr) {
      // If the deadline from the service config is shorter than the one
      // from the client API, reset the deadline timer.
      if (chand->deadline_checking_enabled() && method_params->timeout() != 0) {
        const grpc_millis per_method_deadline =
            grpc_cycle_counter_to_millis_round_up(call_start_time_) +
            method_params->timeout();
        if (per_method_deadline < deadline_) {
          deadline_ = per_method_deadline;
          grpc_deadline_state_reset(elem, deadline_);
        }
      }
      // If the service config set wait_for_ready and the application
      // did not explicitly set it, use the value from the service config.
      uint32_t* send_initial_metadata_flags =
          &pending_batches_[0]
               ->payload->send_initial_metadata.send_initial_metadata_flags;
      if (method_params->wait_for_ready().has_value() &&
          !(*send_initial_metadata_flags &
            GRPC_INITIAL_METADATA_WAIT_FOR_READY_EXPLICITLY_SET)) {
        if (method_params->wait_for_ready().value()) {
          *send_initial_metadata_flags |= GRPC_INITIAL_METADATA_WAIT_FOR_READY;
        } else {
          *send_initial_metadata_flags &= ~GRPC_INITIAL_METADATA_WAIT_FOR_READY;
        }
      }
    }
    // Set the dynamic filter stack.
    dynamic_filters_ = chand->dynamic_filters();
  }
  return GRPC_ERROR_NONE;
}

void CallData::RecvInitialMetadataReadyForConfigSelectorCommitCallback(
    void* arg, grpc_error* error) {
  auto* self = static_cast<CallData*>(arg);
  if (self->on_call_committed_ != nullptr) {
    self->on_call_committed_();
    self->on_call_committed_ = nullptr;
  }
  // Chain to original callback.
  Closure::Run(DEBUG_LOCATION, self->original_recv_initial_metadata_ready_,
               GRPC_ERROR_REF(error));
}

// TODO(roth): Consider not intercepting this callback unless we
// actually need to, if this causes a performance problem.
void CallData::InjectRecvInitialMetadataReadyForConfigSelectorCommitCallback(
    grpc_transport_stream_op_batch* batch) {
  original_recv_initial_metadata_ready_ =
      batch->payload->recv_initial_metadata.recv_initial_metadata_ready;
  GRPC_CLOSURE_INIT(&recv_initial_metadata_ready_,
                    RecvInitialMetadataReadyForConfigSelectorCommitCallback,
                    this, nullptr);
  batch->payload->recv_initial_metadata.recv_initial_metadata_ready =
      &recv_initial_metadata_ready_;
}

void CallData::AsyncResolutionDone(grpc_call_element* elem, grpc_error* error) {
  GRPC_CLOSURE_INIT(&pick_closure_, ResolutionDone, elem, nullptr);
  ExecCtx::Run(DEBUG_LOCATION, &pick_closure_, error);
}

void CallData::ResolutionDone(void* arg, grpc_error* error) {
  grpc_call_element* elem = static_cast<grpc_call_element*>(arg);
  ChannelData* chand = static_cast<ChannelData*>(elem->channel_data);
  CallData* calld = static_cast<CallData*>(elem->call_data);
  if (error != GRPC_ERROR_NONE) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
      gpr_log(GPR_INFO,
              "chand=%p calld=%p: error applying config to call: error=%s",
              chand, calld, grpc_error_string(error));
    }
    calld->PendingBatchesFail(elem, GRPC_ERROR_REF(error), YieldCallCombiner);
    return;
  }
  calld->CreateDynamicCall(elem);
}

void CallData::CheckResolution(void* arg, grpc_error* error) {
  grpc_call_element* elem = static_cast<grpc_call_element*>(arg);
  CallData* calld = static_cast<CallData*>(elem->call_data);
  ChannelData* chand = static_cast<ChannelData*>(elem->channel_data);
  bool resolution_complete;
  {
    MutexLock lock(chand->resolution_mu());
    resolution_complete = calld->CheckResolutionLocked(elem, &error);
  }
  if (resolution_complete) {
    ResolutionDone(elem, error);
    GRPC_ERROR_UNREF(error);
  }
}

bool CallData::CheckResolutionLocked(grpc_call_element* elem,
                                     grpc_error** error) {
  ChannelData* chand = static_cast<ChannelData*>(elem->channel_data);
  // If we're still in IDLE, we need to start resolving.
  if (GPR_UNLIKELY(chand->CheckConnectivityState(false) == GRPC_CHANNEL_IDLE)) {
    // Bounce into the control plane work serializer to start resolving,
    // in case we are still in IDLE state.  Since we are holding on to the
    // resolution mutex here, we offload it on the ExecCtx so that we don't
    // deadlock with ourselves.
    GRPC_CHANNEL_STACK_REF(chand->owning_stack(), "CheckResolutionLocked");
    ExecCtx::Run(
        DEBUG_LOCATION,
        GRPC_CLOSURE_CREATE(
            [](void* arg, grpc_error* /*error*/) {
              auto* chand = static_cast<ChannelData*>(arg);
              chand->work_serializer()->Run(
                  [chand]() {
                    chand->CheckConnectivityState(/*try_to_connect=*/true);
                    GRPC_CHANNEL_STACK_UNREF(chand->owning_stack(),
                                             "CheckResolutionLocked");
                  },
                  DEBUG_LOCATION);
            },
            chand, nullptr),
        GRPC_ERROR_NONE);
  }
  // Get send_initial_metadata batch and flags.
  auto& send_initial_metadata =
      pending_batches_[0]->payload->send_initial_metadata;
  grpc_metadata_batch* initial_metadata_batch =
      send_initial_metadata.send_initial_metadata;
  const uint32_t send_initial_metadata_flags =
      send_initial_metadata.send_initial_metadata_flags;
  // If we don't yet have a resolver result, we need to queue the call
  // until we get one.
  if (GPR_UNLIKELY(!chand->received_service_config_data())) {
    // If the resolver returned transient failure before returning the
    // first service config, fail any non-wait_for_ready calls.
    grpc_error* resolver_error = chand->resolver_transient_failure_error();
    if (resolver_error != GRPC_ERROR_NONE &&
        (send_initial_metadata_flags & GRPC_INITIAL_METADATA_WAIT_FOR_READY) ==
            0) {
      MaybeRemoveCallFromResolverQueuedCallsLocked(elem);
      *error = GRPC_ERROR_REF(resolver_error);
      return true;
    }
    // Either the resolver has not yet returned a result, or it has
    // returned transient failure but the call is wait_for_ready.  In
    // either case, queue the call.
    MaybeAddCallToResolverQueuedCallsLocked(elem);
    return false;
  }
  // Apply service config to call if not yet applied.
  if (GPR_LIKELY(!service_config_applied_)) {
    service_config_applied_ = true;
    *error = ApplyServiceConfigToCallLocked(elem, initial_metadata_batch);
  }
  MaybeRemoveCallFromResolverQueuedCallsLocked(elem);
  return true;
}

void CallData::CreateDynamicCall(grpc_call_element* elem) {
  auto* chand = static_cast<ChannelData*>(elem->channel_data);
  DynamicFilters::Call::Args args = {std::move(dynamic_filters_),
                                     pollent_,
                                     path_,
                                     call_start_time_,
                                     deadline_,
                                     arena_,
                                     call_context_,
                                     call_combiner_};
  grpc_error* error = GRPC_ERROR_NONE;
  DynamicFilters* channel_stack = args.channel_stack.get();
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
    gpr_log(
        GPR_INFO,
        "chand=%p calld=%p: creating dynamic call stack on channel_stack=%p",
        chand, this, channel_stack);
  }
  dynamic_call_ = channel_stack->CreateCall(std::move(args), &error);
  if (error != GRPC_ERROR_NONE) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
      gpr_log(GPR_INFO,
              "chand=%p calld=%p: failed to create dynamic call: error=%s",
              chand, this, grpc_error_string(error));
    }
    PendingBatchesFail(elem, error, YieldCallCombiner);
    return;
  }
  PendingBatchesResume(elem);
}

//
// RetryingCall implementation
//

// Retry support:
//
// In order to support retries, we act as a proxy for stream op batches.
// When we get a batch from the surface, we add it to our list of pending
// batches, and we then use those batches to construct separate "child"
// batches to be started on the subchannel call.  When the child batches
// return, we then decide which pending batches have been completed and
// schedule their callbacks accordingly.  If a subchannel call fails and
// we want to retry it, we do a new pick and start again, constructing
// new "child" batches for the new subchannel call.
//
// Note that retries are committed when receiving data from the server
// (except for Trailers-Only responses).  However, there may be many
// send ops started before receiving any data, so we may have already
// completed some number of send ops (and returned the completions up to
// the surface) by the time we realize that we need to retry.  To deal
// with this, we cache data for send ops, so that we can replay them on a
// different subchannel call even after we have completed the original
// batches.
//
// There are two sets of data to maintain:
// - In call_data (in the parent channel), we maintain a list of pending
//   ops and cached data for send ops.
// - In the subchannel call, we maintain state to indicate what ops have
//   already been sent down to that call.
//
// When constructing the "child" batches, we compare those two sets of
// data to see which batches need to be sent to the subchannel call.

// TODO(roth): In subsequent PRs:
// - add support for transparent retries (including initial metadata)
// - figure out how to record stats in census for retries
//   (census filter is on top of this one)
// - add census stats for retries

RetryingCall::RetryingCall(
    ChannelData* chand, const grpc_call_element_args& args,
    grpc_polling_entity* pollent,
    RefCountedPtr<ServerRetryThrottleData> retry_throttle_data,
    const ClientChannelMethodParsedConfig::RetryPolicy* retry_policy)
    : chand_(chand),
      pollent_(pollent),
      retry_throttle_data_(std::move(retry_throttle_data)),
      retry_policy_(retry_policy),
      retry_backoff_(
          BackOff::Options()
              .set_initial_backoff(
                  retry_policy_ == nullptr ? 0 : retry_policy_->initial_backoff)
              .set_multiplier(retry_policy_ == nullptr
                                  ? 0
                                  : retry_policy_->backoff_multiplier)
              .set_jitter(RETRY_BACKOFF_JITTER)
              .set_max_backoff(
                  retry_policy_ == nullptr ? 0 : retry_policy_->max_backoff)),
      path_(grpc_slice_ref_internal(args.path)),
      call_start_time_(args.start_time),
      deadline_(args.deadline),
      arena_(args.arena),
      owning_call_(args.call_stack),
      call_combiner_(args.call_combiner),
      call_context_(args.context),
      pending_send_initial_metadata_(false),
      pending_send_message_(false),
      pending_send_trailing_metadata_(false),
      enable_retries_(true),
      retry_committed_(false),
      last_attempt_got_server_pushback_(false) {}

RetryingCall::~RetryingCall() {
  grpc_slice_unref_internal(path_);
  GRPC_ERROR_UNREF(cancel_error_);
  // Make sure there are no remaining pending batches.
  for (size_t i = 0; i < GPR_ARRAY_SIZE(pending_batches_); ++i) {
    GPR_ASSERT(pending_batches_[i].batch == nullptr);
  }
}

void RetryingCall::StartTransportStreamOpBatch(
    grpc_transport_stream_op_batch* batch) {
  // If we've previously been cancelled, immediately fail any new batches.
  if (GPR_UNLIKELY(cancel_error_ != GRPC_ERROR_NONE)) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
      gpr_log(GPR_INFO,
              "chand=%p retrying_call=%p: failing batch with error: %s", chand_,
              this, grpc_error_string(cancel_error_));
    }
    // Note: This will release the call combiner.
    grpc_transport_stream_op_batch_finish_with_failure(
        batch, GRPC_ERROR_REF(cancel_error_), call_combiner_);
    return;
  }
  // Handle cancellation.
  if (GPR_UNLIKELY(batch->cancel_stream)) {
    // Stash a copy of cancel_error in our call data, so that we can use
    // it for subsequent operations.  This ensures that if the call is
    // cancelled before any batches are passed down (e.g., if the deadline
    // is in the past when the call starts), we can return the right
    // error to the caller when the first batch does get passed down.
    GRPC_ERROR_UNREF(cancel_error_);
    cancel_error_ = GRPC_ERROR_REF(batch->payload->cancel_stream.cancel_error);
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
      gpr_log(GPR_INFO, "chand=%p retrying_call=%p: recording cancel_error=%s",
              chand_, this, grpc_error_string(cancel_error_));
    }
    // If we do not have an LB call (i.e., a pick has not yet been started),
    // fail all pending batches.  Otherwise, send the cancellation down to the
    // LB call.
    if (lb_call_ == nullptr) {
      // TODO(roth): If there is a pending retry callback, do we need to
      // cancel it here?
      PendingBatchesFail(GRPC_ERROR_REF(cancel_error_), NoYieldCallCombiner);
      // Note: This will release the call combiner.
      grpc_transport_stream_op_batch_finish_with_failure(
          batch, GRPC_ERROR_REF(cancel_error_), call_combiner_);
    } else {
      // Note: This will release the call combiner.
      lb_call_->StartTransportStreamOpBatch(batch);
    }
    return;
  }
  // Add the batch to the pending list.
  PendingBatchesAdd(batch);
  // Create LB call if needed.
  // TODO(roth): If we get a new batch from the surface after the
  // initial retry attempt has failed, while the retry timer is pending,
  // we should queue the batch and not try to send it immediately.
  if (lb_call_ == nullptr) {
    // We do not yet have an LB call, so create one.
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
      gpr_log(GPR_INFO, "chand=%p retrying_call=%p: creating LB call", chand_,
              this);
    }
    CreateLbCall(this, GRPC_ERROR_NONE);
    return;
  }
  // Send batches to LB call.
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
    gpr_log(GPR_INFO, "chand=%p retrying_call=%p: starting batch on lb_call=%p",
            chand_, this, lb_call_.get());
  }
  PendingBatchesResume();
}

RefCountedPtr<SubchannelCall> RetryingCall::subchannel_call() const {
  if (lb_call_ == nullptr) return nullptr;
  return lb_call_->subchannel_call();
}

//
// send op data caching
//

void RetryingCall::MaybeCacheSendOpsForBatch(PendingBatch* pending) {
  if (pending->send_ops_cached) return;
  pending->send_ops_cached = true;
  grpc_transport_stream_op_batch* batch = pending->batch;
  // Save a copy of metadata for send_initial_metadata ops.
  if (batch->send_initial_metadata) {
    seen_send_initial_metadata_ = true;
    GPR_ASSERT(send_initial_metadata_storage_ == nullptr);
    grpc_metadata_batch* send_initial_metadata =
        batch->payload->send_initial_metadata.send_initial_metadata;
    send_initial_metadata_storage_ =
        static_cast<grpc_linked_mdelem*>(arena_->Alloc(
            sizeof(grpc_linked_mdelem) * send_initial_metadata->list.count));
    grpc_metadata_batch_copy(send_initial_metadata, &send_initial_metadata_,
                             send_initial_metadata_storage_);
    send_initial_metadata_flags_ =
        batch->payload->send_initial_metadata.send_initial_metadata_flags;
    peer_string_ = batch->payload->send_initial_metadata.peer_string;
  }
  // Set up cache for send_message ops.
  if (batch->send_message) {
    ByteStreamCache* cache = arena_->New<ByteStreamCache>(
        std::move(batch->payload->send_message.send_message));
    send_messages_.push_back(cache);
  }
  // Save metadata batch for send_trailing_metadata ops.
  if (batch->send_trailing_metadata) {
    seen_send_trailing_metadata_ = true;
    GPR_ASSERT(send_trailing_metadata_storage_ == nullptr);
    grpc_metadata_batch* send_trailing_metadata =
        batch->payload->send_trailing_metadata.send_trailing_metadata;
    send_trailing_metadata_storage_ =
        static_cast<grpc_linked_mdelem*>(arena_->Alloc(
            sizeof(grpc_linked_mdelem) * send_trailing_metadata->list.count));
    grpc_metadata_batch_copy(send_trailing_metadata, &send_trailing_metadata_,
                             send_trailing_metadata_storage_);
  }
}

void RetryingCall::FreeCachedSendInitialMetadata() {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
    gpr_log(GPR_INFO,
            "chand=%p retrying_call=%p: destroying send_initial_metadata",
            chand_, this);
  }
  grpc_metadata_batch_destroy(&send_initial_metadata_);
}

void RetryingCall::FreeCachedSendMessage(size_t idx) {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
    gpr_log(GPR_INFO,
            "chand=%p retrying_call=%p: destroying send_messages[%" PRIuPTR "]",
            chand_, this, idx);
  }
  send_messages_[idx]->Destroy();
}

void RetryingCall::FreeCachedSendTrailingMetadata() {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
    gpr_log(GPR_INFO,
            "chand_=%p retrying_call=%p: destroying send_trailing_metadata",
            chand_, this);
  }
  grpc_metadata_batch_destroy(&send_trailing_metadata_);
}

void RetryingCall::FreeCachedSendOpDataAfterCommit(
    SubchannelCallRetryState* retry_state) {
  if (retry_state->completed_send_initial_metadata) {
    FreeCachedSendInitialMetadata();
  }
  for (size_t i = 0; i < retry_state->completed_send_message_count; ++i) {
    FreeCachedSendMessage(i);
  }
  if (retry_state->completed_send_trailing_metadata) {
    FreeCachedSendTrailingMetadata();
  }
}

void RetryingCall::FreeCachedSendOpDataForCompletedBatch(
    SubchannelCallBatchData* batch_data,
    SubchannelCallRetryState* retry_state) {
  if (batch_data->batch.send_initial_metadata) {
    FreeCachedSendInitialMetadata();
  }
  if (batch_data->batch.send_message) {
    FreeCachedSendMessage(retry_state->completed_send_message_count - 1);
  }
  if (batch_data->batch.send_trailing_metadata) {
    FreeCachedSendTrailingMetadata();
  }
}

//
// pending_batches management
//

size_t RetryingCall::GetBatchIndex(grpc_transport_stream_op_batch* batch) {
  // Note: It is important the send_initial_metadata be the first entry
  // here, since the code in pick_subchannel_locked() assumes it will be.
  if (batch->send_initial_metadata) return 0;
  if (batch->send_message) return 1;
  if (batch->send_trailing_metadata) return 2;
  if (batch->recv_initial_metadata) return 3;
  if (batch->recv_message) return 4;
  if (batch->recv_trailing_metadata) return 5;
  GPR_UNREACHABLE_CODE(return (size_t)-1);
}

// This is called via the call combiner, so access to calld is synchronized.
void RetryingCall::PendingBatchesAdd(grpc_transport_stream_op_batch* batch) {
  const size_t idx = GetBatchIndex(batch);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
    gpr_log(
        GPR_INFO,
        "chand_=%p retrying_call=%p: adding pending batch at index %" PRIuPTR,
        chand_, this, idx);
  }
  PendingBatch* pending = &pending_batches_[idx];
  GPR_ASSERT(pending->batch == nullptr);
  pending->batch = batch;
  pending->send_ops_cached = false;
  if (enable_retries_) {
    // Update state in calld about pending batches.
    // Also check if the batch takes us over the retry buffer limit.
    // Note: We don't check the size of trailing metadata here, because
    // gRPC clients do not send trailing metadata.
    if (batch->send_initial_metadata) {
      pending_send_initial_metadata_ = true;
      bytes_buffered_for_retry_ += grpc_metadata_batch_size(
          batch->payload->send_initial_metadata.send_initial_metadata);
    }
    if (batch->send_message) {
      pending_send_message_ = true;
      bytes_buffered_for_retry_ +=
          batch->payload->send_message.send_message->length();
    }
    if (batch->send_trailing_metadata) {
      pending_send_trailing_metadata_ = true;
    }
    if (GPR_UNLIKELY(bytes_buffered_for_retry_ >
                     chand_->per_rpc_retry_buffer_size())) {
      if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
        gpr_log(GPR_INFO,
                "chand=%p retrying_call=%p: exceeded retry buffer size, "
                "committing",
                chand_, this);
      }
      SubchannelCallRetryState* retry_state =
          lb_call_ == nullptr ? nullptr
                              : static_cast<SubchannelCallRetryState*>(
                                    lb_call_->GetParentData());
      RetryCommit(retry_state);
      // If we are not going to retry and have not yet started, pretend
      // retries are disabled so that we don't bother with retry overhead.
      if (num_attempts_completed_ == 0) {
        if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
          gpr_log(GPR_INFO,
                  "chand=%p retrying_call=%p: disabling retries before first "
                  "attempt",
                  chand_, this);
        }
        // TODO(roth): Treat this as a commit?
        enable_retries_ = false;
      }
    }
  }
}

void RetryingCall::PendingBatchClear(PendingBatch* pending) {
  if (enable_retries_) {
    if (pending->batch->send_initial_metadata) {
      pending_send_initial_metadata_ = false;
    }
    if (pending->batch->send_message) {
      pending_send_message_ = false;
    }
    if (pending->batch->send_trailing_metadata) {
      pending_send_trailing_metadata_ = false;
    }
  }
  pending->batch = nullptr;
}

void RetryingCall::MaybeClearPendingBatch(PendingBatch* pending) {
  grpc_transport_stream_op_batch* batch = pending->batch;
  // We clear the pending batch if all of its callbacks have been
  // scheduled and reset to nullptr.
  if (batch->on_complete == nullptr &&
      (!batch->recv_initial_metadata ||
       batch->payload->recv_initial_metadata.recv_initial_metadata_ready ==
           nullptr) &&
      (!batch->recv_message ||
       batch->payload->recv_message.recv_message_ready == nullptr) &&
      (!batch->recv_trailing_metadata ||
       batch->payload->recv_trailing_metadata.recv_trailing_metadata_ready ==
           nullptr)) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
      gpr_log(GPR_INFO, "chand=%p retrying_call=%p: clearing pending batch",
              chand_, this);
    }
    PendingBatchClear(pending);
  }
}

// This is called via the call combiner, so access to calld is synchronized.
void RetryingCall::FailPendingBatchInCallCombiner(void* arg,
                                                  grpc_error* error) {
  grpc_transport_stream_op_batch* batch =
      static_cast<grpc_transport_stream_op_batch*>(arg);
  RetryingCall* call =
      static_cast<RetryingCall*>(batch->handler_private.extra_arg);
  // Note: This will release the call combiner.
  grpc_transport_stream_op_batch_finish_with_failure(
      batch, GRPC_ERROR_REF(error), call->call_combiner_);
}

// This is called via the call combiner, so access to calld is synchronized.
void RetryingCall::PendingBatchesFail(
    grpc_error* error,
    YieldCallCombinerPredicate yield_call_combiner_predicate) {
  GPR_ASSERT(error != GRPC_ERROR_NONE);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
    size_t num_batches = 0;
    for (size_t i = 0; i < GPR_ARRAY_SIZE(pending_batches_); ++i) {
      if (pending_batches_[i].batch != nullptr) ++num_batches;
    }
    gpr_log(GPR_INFO,
            "chand=%p retrying_call=%p: failing %" PRIuPTR
            " pending batches: %s",
            chand_, this, num_batches, grpc_error_string(error));
  }
  CallCombinerClosureList closures;
  for (size_t i = 0; i < GPR_ARRAY_SIZE(pending_batches_); ++i) {
    PendingBatch* pending = &pending_batches_[i];
    grpc_transport_stream_op_batch* batch = pending->batch;
    if (batch != nullptr) {
      batch->handler_private.extra_arg = this;
      GRPC_CLOSURE_INIT(&batch->handler_private.closure,
                        FailPendingBatchInCallCombiner, batch,
                        grpc_schedule_on_exec_ctx);
      closures.Add(&batch->handler_private.closure, GRPC_ERROR_REF(error),
                   "PendingBatchesFail");
      PendingBatchClear(pending);
    }
  }
  if (yield_call_combiner_predicate(closures)) {
    closures.RunClosures(call_combiner_);
  } else {
    closures.RunClosuresWithoutYielding(call_combiner_);
  }
  GRPC_ERROR_UNREF(error);
}

// This is called via the call combiner, so access to calld is synchronized.
void RetryingCall::ResumePendingBatchInCallCombiner(void* arg,
                                                    grpc_error* /*ignored*/) {
  grpc_transport_stream_op_batch* batch =
      static_cast<grpc_transport_stream_op_batch*>(arg);
  auto* lb_call =
      static_cast<LoadBalancedCall*>(batch->handler_private.extra_arg);
  // Note: This will release the call combiner.
  lb_call->StartTransportStreamOpBatch(batch);
}

// This is called via the call combiner, so access to calld is synchronized.
void RetryingCall::PendingBatchesResume() {
  if (enable_retries_) {
    StartRetriableSubchannelBatches(this, GRPC_ERROR_NONE);
    return;
  }
  // Retries not enabled; send down batches as-is.
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
    size_t num_batches = 0;
    for (size_t i = 0; i < GPR_ARRAY_SIZE(pending_batches_); ++i) {
      if (pending_batches_[i].batch != nullptr) ++num_batches;
    }
    gpr_log(GPR_INFO,
            "chand=%p retrying_call=%p: starting %" PRIuPTR
            " pending batches on lb_call=%p",
            chand_, this, num_batches, lb_call_.get());
  }
  CallCombinerClosureList closures;
  for (size_t i = 0; i < GPR_ARRAY_SIZE(pending_batches_); ++i) {
    PendingBatch* pending = &pending_batches_[i];
    grpc_transport_stream_op_batch* batch = pending->batch;
    if (batch != nullptr) {
      batch->handler_private.extra_arg = lb_call_.get();
      GRPC_CLOSURE_INIT(&batch->handler_private.closure,
                        ResumePendingBatchInCallCombiner, batch, nullptr);
      closures.Add(&batch->handler_private.closure, GRPC_ERROR_NONE,
                   "PendingBatchesResume");
      PendingBatchClear(pending);
    }
  }
  // Note: This will release the call combiner.
  closures.RunClosures(call_combiner_);
}

template <typename Predicate>
RetryingCall::PendingBatch* RetryingCall::PendingBatchFind(
    const char* log_message, Predicate predicate) {
  for (size_t i = 0; i < GPR_ARRAY_SIZE(pending_batches_); ++i) {
    PendingBatch* pending = &pending_batches_[i];
    grpc_transport_stream_op_batch* batch = pending->batch;
    if (batch != nullptr && predicate(batch)) {
      if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
        gpr_log(
            GPR_INFO,
            "chand=%p retrying_call=%p: %s pending batch at index %" PRIuPTR,
            chand_, this, log_message, i);
      }
      return pending;
    }
  }
  return nullptr;
}

//
// retry code
//

void RetryingCall::RetryCommit(SubchannelCallRetryState* retry_state) {
  if (retry_committed_) return;
  retry_committed_ = true;
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
    gpr_log(GPR_INFO, "chand=%p retrying_call=%p: committing retries", chand_,
            this);
  }
  if (retry_state != nullptr) {
    FreeCachedSendOpDataAfterCommit(retry_state);
  }
}

void RetryingCall::DoRetry(SubchannelCallRetryState* retry_state,
                           grpc_millis server_pushback_ms) {
  GPR_ASSERT(retry_policy_ != nullptr);
  // Reset LB call.
  lb_call_.reset();
  // Compute backoff delay.
  grpc_millis next_attempt_time;
  if (server_pushback_ms >= 0) {
    next_attempt_time = ExecCtx::Get()->Now() + server_pushback_ms;
    last_attempt_got_server_pushback_ = true;
  } else {
    if (num_attempts_completed_ == 1 || last_attempt_got_server_pushback_) {
      last_attempt_got_server_pushback_ = false;
    }
    next_attempt_time = retry_backoff_.NextAttemptTime();
  }
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
    gpr_log(GPR_INFO,
            "chand=%p retrying_call=%p: retrying failed call in %" PRId64 " ms",
            chand_, this, next_attempt_time - ExecCtx::Get()->Now());
  }
  // Schedule retry after computed delay.
  GRPC_CLOSURE_INIT(&retry_closure_, CreateLbCall, this, nullptr);
  grpc_timer_init(&retry_timer_, next_attempt_time, &retry_closure_);
  // Update bookkeeping.
  if (retry_state != nullptr) retry_state->retry_dispatched = true;
}

bool RetryingCall::MaybeRetry(SubchannelCallBatchData* batch_data,
                              grpc_status_code status,
                              grpc_mdelem* server_pushback_md) {
  // Get retry policy.
  if (retry_policy_ == nullptr) return false;
  // If we've already dispatched a retry from this call, return true.
  // This catches the case where the batch has multiple callbacks
  // (i.e., it includes either recv_message or recv_initial_metadata).
  SubchannelCallRetryState* retry_state = nullptr;
  if (batch_data != nullptr) {
    retry_state = static_cast<SubchannelCallRetryState*>(
        batch_data->lb_call->GetParentData());
    if (retry_state->retry_dispatched) {
      if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
        gpr_log(GPR_INFO, "chand=%p retrying_call=%p: retry already dispatched",
                chand_, this);
      }
      return true;
    }
  }
  // Check status.
  if (GPR_LIKELY(status == GRPC_STATUS_OK)) {
    if (retry_throttle_data_ != nullptr) {
      retry_throttle_data_->RecordSuccess();
    }
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
      gpr_log(GPR_INFO, "chand=%p retrying_call=%p: call succeeded", chand_,
              this);
    }
    return false;
  }
  // Status is not OK.  Check whether the status is retryable.
  if (!retry_policy_->retryable_status_codes.Contains(status)) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
      gpr_log(
          GPR_INFO,
          "chand=%p retrying_call=%p: status %s not configured as retryable",
          chand_, this, grpc_status_code_to_string(status));
    }
    return false;
  }
  // Record the failure and check whether retries are throttled.
  // Note that it's important for this check to come after the status
  // code check above, since we should only record failures whose statuses
  // match the configured retryable status codes, so that we don't count
  // things like failures due to malformed requests (INVALID_ARGUMENT).
  // Conversely, it's important for this to come before the remaining
  // checks, so that we don't fail to record failures due to other factors.
  if (retry_throttle_data_ != nullptr &&
      !retry_throttle_data_->RecordFailure()) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
      gpr_log(GPR_INFO, "chand=%p retrying_call=%p: retries throttled", chand_,
              this);
    }
    return false;
  }
  // Check whether the call is committed.
  if (retry_committed_) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
      gpr_log(GPR_INFO, "chand=%p retrying_call=%p: retries already committed",
              chand_, this);
    }
    return false;
  }
  // Check whether we have retries remaining.
  ++num_attempts_completed_;
  if (num_attempts_completed_ >= retry_policy_->max_attempts) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
      gpr_log(GPR_INFO, "chand=%p retrying_call=%p: exceeded %d retry attempts",
              chand_, this, retry_policy_->max_attempts);
    }
    return false;
  }
  // If the call was cancelled from the surface, don't retry.
  if (cancel_error_ != GRPC_ERROR_NONE) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
      gpr_log(GPR_INFO,
              "chand=%p retrying_call=%p: call cancelled from surface, not "
              "retrying",
              chand_, this);
    }
    return false;
  }
  // Check server push-back.
  grpc_millis server_pushback_ms = -1;
  if (server_pushback_md != nullptr) {
    // If the value is "-1" or any other unparseable string, we do not retry.
    uint32_t ms;
    if (!grpc_parse_slice_to_uint32(GRPC_MDVALUE(*server_pushback_md), &ms)) {
      if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
        gpr_log(
            GPR_INFO,
            "chand=%p retrying_call=%p: not retrying due to server push-back",
            chand_, this);
      }
      return false;
    } else {
      if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
        gpr_log(GPR_INFO,
                "chand=%p retrying_call=%p: server push-back: retry in %u ms",
                chand_, this, ms);
      }
      server_pushback_ms = static_cast<grpc_millis>(ms);
    }
  }
  DoRetry(retry_state, server_pushback_ms);
  return true;
}

//
// RetryingCall::SubchannelCallBatchData
//

RetryingCall::SubchannelCallBatchData*
RetryingCall::SubchannelCallBatchData::Create(RetryingCall* call, int refcount,
                                              bool set_on_complete) {
  return call->arena_->New<SubchannelCallBatchData>(call, refcount,
                                                    set_on_complete);
}

RetryingCall::SubchannelCallBatchData::SubchannelCallBatchData(
    RetryingCall* call, int refcount, bool set_on_complete)
    : call(call), lb_call(call->lb_call_) {
  SubchannelCallRetryState* retry_state =
      static_cast<SubchannelCallRetryState*>(lb_call->GetParentData());
  batch.payload = &retry_state->batch_payload;
  gpr_ref_init(&refs, refcount);
  if (set_on_complete) {
    GRPC_CLOSURE_INIT(&on_complete, RetryingCall::OnComplete, this,
                      grpc_schedule_on_exec_ctx);
    batch.on_complete = &on_complete;
  }
  GRPC_CALL_STACK_REF(call->owning_call_, "batch_data");
}

void RetryingCall::SubchannelCallBatchData::Destroy() {
  SubchannelCallRetryState* retry_state =
      static_cast<SubchannelCallRetryState*>(lb_call->GetParentData());
  if (batch.send_initial_metadata) {
    grpc_metadata_batch_destroy(&retry_state->send_initial_metadata);
  }
  if (batch.send_trailing_metadata) {
    grpc_metadata_batch_destroy(&retry_state->send_trailing_metadata);
  }
  if (batch.recv_initial_metadata) {
    grpc_metadata_batch_destroy(&retry_state->recv_initial_metadata);
  }
  if (batch.recv_trailing_metadata) {
    grpc_metadata_batch_destroy(&retry_state->recv_trailing_metadata);
  }
  lb_call.reset();
  GRPC_CALL_STACK_UNREF(call->owning_call_, "batch_data");
}

//
// recv_initial_metadata callback handling
//

void RetryingCall::InvokeRecvInitialMetadataCallback(void* arg,
                                                     grpc_error* error) {
  SubchannelCallBatchData* batch_data =
      static_cast<SubchannelCallBatchData*>(arg);
  // Find pending batch.
  PendingBatch* pending = batch_data->call->PendingBatchFind(
      "invoking recv_initial_metadata_ready for",
      [](grpc_transport_stream_op_batch* batch) {
        return batch->recv_initial_metadata &&
               batch->payload->recv_initial_metadata
                       .recv_initial_metadata_ready != nullptr;
      });
  GPR_ASSERT(pending != nullptr);
  // Return metadata.
  SubchannelCallRetryState* retry_state =
      static_cast<SubchannelCallRetryState*>(
          batch_data->lb_call->GetParentData());
  grpc_metadata_batch_move(
      &retry_state->recv_initial_metadata,
      pending->batch->payload->recv_initial_metadata.recv_initial_metadata);
  // Update bookkeeping.
  // Note: Need to do this before invoking the callback, since invoking
  // the callback will result in yielding the call combiner.
  grpc_closure* recv_initial_metadata_ready =
      pending->batch->payload->recv_initial_metadata
          .recv_initial_metadata_ready;
  pending->batch->payload->recv_initial_metadata.recv_initial_metadata_ready =
      nullptr;
  batch_data->call->MaybeClearPendingBatch(pending);
  batch_data->Unref();
  // Invoke callback.
  Closure::Run(DEBUG_LOCATION, recv_initial_metadata_ready,
               GRPC_ERROR_REF(error));
}

void RetryingCall::RecvInitialMetadataReady(void* arg, grpc_error* error) {
  SubchannelCallBatchData* batch_data =
      static_cast<SubchannelCallBatchData*>(arg);
  RetryingCall* call = batch_data->call;
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
    gpr_log(
        GPR_INFO,
        "chand=%p retrying_call=%p: got recv_initial_metadata_ready, error=%s",
        call->chand_, call, grpc_error_string(error));
  }
  SubchannelCallRetryState* retry_state =
      static_cast<SubchannelCallRetryState*>(
          batch_data->lb_call->GetParentData());
  retry_state->completed_recv_initial_metadata = true;
  // If a retry was already dispatched, then we're not going to use the
  // result of this recv_initial_metadata op, so do nothing.
  if (retry_state->retry_dispatched) {
    GRPC_CALL_COMBINER_STOP(
        call->call_combiner_,
        "recv_initial_metadata_ready after retry dispatched");
    return;
  }
  // If we got an error or a Trailers-Only response and have not yet gotten
  // the recv_trailing_metadata_ready callback, then defer propagating this
  // callback back to the surface.  We can evaluate whether to retry when
  // recv_trailing_metadata comes back.
  if (GPR_UNLIKELY((retry_state->trailing_metadata_available ||
                    error != GRPC_ERROR_NONE) &&
                   !retry_state->completed_recv_trailing_metadata)) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
      gpr_log(
          GPR_INFO,
          "chand=%p retrying_call=%p: deferring recv_initial_metadata_ready "
          "(Trailers-Only)",
          call->chand_, call);
    }
    retry_state->recv_initial_metadata_ready_deferred_batch = batch_data;
    retry_state->recv_initial_metadata_error = GRPC_ERROR_REF(error);
    if (!retry_state->started_recv_trailing_metadata) {
      // recv_trailing_metadata not yet started by application; start it
      // ourselves to get status.
      call->StartInternalRecvTrailingMetadata();
    } else {
      GRPC_CALL_COMBINER_STOP(
          call->call_combiner_,
          "recv_initial_metadata_ready trailers-only or error");
    }
    return;
  }
  // Received valid initial metadata, so commit the call.
  call->RetryCommit(retry_state);
  // Invoke the callback to return the result to the surface.
  // Manually invoking a callback function; it does not take ownership of error.
  call->InvokeRecvInitialMetadataCallback(batch_data, error);
}

//
// recv_message callback handling
//

void RetryingCall::InvokeRecvMessageCallback(void* arg, grpc_error* error) {
  SubchannelCallBatchData* batch_data =
      static_cast<SubchannelCallBatchData*>(arg);
  RetryingCall* call = batch_data->call;
  // Find pending op.
  PendingBatch* pending = call->PendingBatchFind(
      "invoking recv_message_ready for",
      [](grpc_transport_stream_op_batch* batch) {
        return batch->recv_message &&
               batch->payload->recv_message.recv_message_ready != nullptr;
      });
  GPR_ASSERT(pending != nullptr);
  // Return payload.
  SubchannelCallRetryState* retry_state =
      static_cast<SubchannelCallRetryState*>(
          batch_data->lb_call->GetParentData());
  *pending->batch->payload->recv_message.recv_message =
      std::move(retry_state->recv_message);
  // Update bookkeeping.
  // Note: Need to do this before invoking the callback, since invoking
  // the callback will result in yielding the call combiner.
  grpc_closure* recv_message_ready =
      pending->batch->payload->recv_message.recv_message_ready;
  pending->batch->payload->recv_message.recv_message_ready = nullptr;
  call->MaybeClearPendingBatch(pending);
  batch_data->Unref();
  // Invoke callback.
  Closure::Run(DEBUG_LOCATION, recv_message_ready, GRPC_ERROR_REF(error));
}

void RetryingCall::RecvMessageReady(void* arg, grpc_error* error) {
  SubchannelCallBatchData* batch_data =
      static_cast<SubchannelCallBatchData*>(arg);
  RetryingCall* call = batch_data->call;
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
    gpr_log(GPR_INFO,
            "chand=%p retrying_call=%p: got recv_message_ready, error=%s",
            call->chand_, call, grpc_error_string(error));
  }
  SubchannelCallRetryState* retry_state =
      static_cast<SubchannelCallRetryState*>(
          batch_data->lb_call->GetParentData());
  ++retry_state->completed_recv_message_count;
  // If a retry was already dispatched, then we're not going to use the
  // result of this recv_message op, so do nothing.
  if (retry_state->retry_dispatched) {
    GRPC_CALL_COMBINER_STOP(call->call_combiner_,
                            "recv_message_ready after retry dispatched");
    return;
  }
  // If we got an error or the payload was nullptr and we have not yet gotten
  // the recv_trailing_metadata_ready callback, then defer propagating this
  // callback back to the surface.  We can evaluate whether to retry when
  // recv_trailing_metadata comes back.
  if (GPR_UNLIKELY(
          (retry_state->recv_message == nullptr || error != GRPC_ERROR_NONE) &&
          !retry_state->completed_recv_trailing_metadata)) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
      gpr_log(
          GPR_INFO,
          "chand=%p retrying_call=%p: deferring recv_message_ready (nullptr "
          "message and recv_trailing_metadata pending)",
          call->chand_, call);
    }
    retry_state->recv_message_ready_deferred_batch = batch_data;
    retry_state->recv_message_error = GRPC_ERROR_REF(error);
    if (!retry_state->started_recv_trailing_metadata) {
      // recv_trailing_metadata not yet started by application; start it
      // ourselves to get status.
      call->StartInternalRecvTrailingMetadata();
    } else {
      GRPC_CALL_COMBINER_STOP(call->call_combiner_, "recv_message_ready null");
    }
    return;
  }
  // Received a valid message, so commit the call.
  call->RetryCommit(retry_state);
  // Invoke the callback to return the result to the surface.
  // Manually invoking a callback function; it does not take ownership of error.
  call->InvokeRecvMessageCallback(batch_data, error);
}

//
// recv_trailing_metadata handling
//

void RetryingCall::GetCallStatus(grpc_metadata_batch* md_batch,
                                 grpc_error* error, grpc_status_code* status,
                                 grpc_mdelem** server_pushback_md) {
  if (error != GRPC_ERROR_NONE) {
    grpc_error_get_status(error, deadline_, status, nullptr, nullptr, nullptr);
  } else {
    GPR_ASSERT(md_batch->idx.named.grpc_status != nullptr);
    *status =
        grpc_get_status_code_from_metadata(md_batch->idx.named.grpc_status->md);
    if (server_pushback_md != nullptr &&
        md_batch->idx.named.grpc_retry_pushback_ms != nullptr) {
      *server_pushback_md = &md_batch->idx.named.grpc_retry_pushback_ms->md;
    }
  }
  GRPC_ERROR_UNREF(error);
}

void RetryingCall::AddClosureForRecvTrailingMetadataReady(
    SubchannelCallBatchData* batch_data, grpc_error* error,
    CallCombinerClosureList* closures) {
  // Find pending batch.
  PendingBatch* pending = PendingBatchFind(
      "invoking recv_trailing_metadata for",
      [](grpc_transport_stream_op_batch* batch) {
        return batch->recv_trailing_metadata &&
               batch->payload->recv_trailing_metadata
                       .recv_trailing_metadata_ready != nullptr;
      });
  // If we generated the recv_trailing_metadata op internally via
  // StartInternalRecvTrailingMetadata(), then there will be no pending batch.
  if (pending == nullptr) {
    GRPC_ERROR_UNREF(error);
    return;
  }
  // Return metadata.
  SubchannelCallRetryState* retry_state =
      static_cast<SubchannelCallRetryState*>(
          batch_data->lb_call->GetParentData());
  grpc_metadata_batch_move(
      &retry_state->recv_trailing_metadata,
      pending->batch->payload->recv_trailing_metadata.recv_trailing_metadata);
  // Add closure.
  closures->Add(pending->batch->payload->recv_trailing_metadata
                    .recv_trailing_metadata_ready,
                error, "recv_trailing_metadata_ready for pending batch");
  // Update bookkeeping.
  pending->batch->payload->recv_trailing_metadata.recv_trailing_metadata_ready =
      nullptr;
  MaybeClearPendingBatch(pending);
}

void RetryingCall::AddClosuresForDeferredRecvCallbacks(
    SubchannelCallBatchData* batch_data, SubchannelCallRetryState* retry_state,
    CallCombinerClosureList* closures) {
  if (batch_data->batch.recv_trailing_metadata) {
    // Add closure for deferred recv_initial_metadata_ready.
    if (GPR_UNLIKELY(retry_state->recv_initial_metadata_ready_deferred_batch !=
                     nullptr)) {
      GRPC_CLOSURE_INIT(&retry_state->recv_initial_metadata_ready,
                        InvokeRecvInitialMetadataCallback,
                        retry_state->recv_initial_metadata_ready_deferred_batch,
                        grpc_schedule_on_exec_ctx);
      closures->Add(&retry_state->recv_initial_metadata_ready,
                    retry_state->recv_initial_metadata_error,
                    "resuming recv_initial_metadata_ready");
      retry_state->recv_initial_metadata_ready_deferred_batch = nullptr;
    }
    // Add closure for deferred recv_message_ready.
    if (GPR_UNLIKELY(retry_state->recv_message_ready_deferred_batch !=
                     nullptr)) {
      GRPC_CLOSURE_INIT(&retry_state->recv_message_ready,
                        InvokeRecvMessageCallback,
                        retry_state->recv_message_ready_deferred_batch,
                        grpc_schedule_on_exec_ctx);
      closures->Add(&retry_state->recv_message_ready,
                    retry_state->recv_message_error,
                    "resuming recv_message_ready");
      retry_state->recv_message_ready_deferred_batch = nullptr;
    }
  }
}

bool RetryingCall::PendingBatchIsUnstarted(
    PendingBatch* pending, SubchannelCallRetryState* retry_state) {
  if (pending->batch == nullptr || pending->batch->on_complete == nullptr) {
    return false;
  }
  if (pending->batch->send_initial_metadata &&
      !retry_state->started_send_initial_metadata) {
    return true;
  }
  if (pending->batch->send_message &&
      retry_state->started_send_message_count < send_messages_.size()) {
    return true;
  }
  if (pending->batch->send_trailing_metadata &&
      !retry_state->started_send_trailing_metadata) {
    return true;
  }
  return false;
}

void RetryingCall::AddClosuresToFailUnstartedPendingBatches(
    SubchannelCallRetryState* retry_state, grpc_error* error,
    CallCombinerClosureList* closures) {
  for (size_t i = 0; i < GPR_ARRAY_SIZE(pending_batches_); ++i) {
    PendingBatch* pending = &pending_batches_[i];
    if (PendingBatchIsUnstarted(pending, retry_state)) {
      if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
        gpr_log(GPR_INFO,
                "chand=%p retrying_call=%p: failing unstarted pending batch at "
                "index "
                "%" PRIuPTR,
                chand_, this, i);
      }
      closures->Add(pending->batch->on_complete, GRPC_ERROR_REF(error),
                    "failing on_complete for pending batch");
      pending->batch->on_complete = nullptr;
      MaybeClearPendingBatch(pending);
    }
  }
  GRPC_ERROR_UNREF(error);
}

void RetryingCall::RunClosuresForCompletedCall(
    SubchannelCallBatchData* batch_data, grpc_error* error) {
  SubchannelCallRetryState* retry_state =
      static_cast<SubchannelCallRetryState*>(
          batch_data->lb_call->GetParentData());
  // Construct list of closures to execute.
  CallCombinerClosureList closures;
  // First, add closure for recv_trailing_metadata_ready.
  AddClosureForRecvTrailingMetadataReady(batch_data, GRPC_ERROR_REF(error),
                                         &closures);
  // If there are deferred recv_initial_metadata_ready or recv_message_ready
  // callbacks, add them to closures.
  AddClosuresForDeferredRecvCallbacks(batch_data, retry_state, &closures);
  // Add closures to fail any pending batches that have not yet been started.
  AddClosuresToFailUnstartedPendingBatches(retry_state, GRPC_ERROR_REF(error),
                                           &closures);
  // Don't need batch_data anymore.
  batch_data->Unref();
  // Schedule all of the closures identified above.
  // Note: This will release the call combiner.
  closures.RunClosures(call_combiner_);
  GRPC_ERROR_UNREF(error);
}

void RetryingCall::RecvTrailingMetadataReady(void* arg, grpc_error* error) {
  SubchannelCallBatchData* batch_data =
      static_cast<SubchannelCallBatchData*>(arg);
  RetryingCall* call = batch_data->call;
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
    gpr_log(
        GPR_INFO,
        "chand=%p retrying_call=%p: got recv_trailing_metadata_ready, error=%s",
        call->chand_, call, grpc_error_string(error));
  }
  SubchannelCallRetryState* retry_state =
      static_cast<SubchannelCallRetryState*>(
          batch_data->lb_call->GetParentData());
  retry_state->completed_recv_trailing_metadata = true;
  // Get the call's status and check for server pushback metadata.
  grpc_status_code status = GRPC_STATUS_OK;
  grpc_mdelem* server_pushback_md = nullptr;
  grpc_metadata_batch* md_batch =
      batch_data->batch.payload->recv_trailing_metadata.recv_trailing_metadata;
  call->GetCallStatus(md_batch, GRPC_ERROR_REF(error), &status,
                      &server_pushback_md);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
    gpr_log(GPR_INFO, "chand=%p retrying_call=%p: call finished, status=%s",
            call->chand_, call, grpc_status_code_to_string(status));
  }
  // Check if we should retry.
  if (call->MaybeRetry(batch_data, status, server_pushback_md)) {
    // Unref batch_data for deferred recv_initial_metadata_ready or
    // recv_message_ready callbacks, if any.
    if (retry_state->recv_initial_metadata_ready_deferred_batch != nullptr) {
      batch_data->Unref();
      GRPC_ERROR_UNREF(retry_state->recv_initial_metadata_error);
    }
    if (retry_state->recv_message_ready_deferred_batch != nullptr) {
      batch_data->Unref();
      GRPC_ERROR_UNREF(retry_state->recv_message_error);
    }
    batch_data->Unref();
    return;
  }
  // Not retrying, so commit the call.
  call->RetryCommit(retry_state);
  // Run any necessary closures.
  call->RunClosuresForCompletedCall(batch_data, GRPC_ERROR_REF(error));
}

//
// on_complete callback handling
//

void RetryingCall::AddClosuresForCompletedPendingBatch(
    SubchannelCallBatchData* batch_data, grpc_error* error,
    CallCombinerClosureList* closures) {
  PendingBatch* pending = PendingBatchFind(
      "completed", [batch_data](grpc_transport_stream_op_batch* batch) {
        // Match the pending batch with the same set of send ops as the
        // subchannel batch we've just completed.
        return batch->on_complete != nullptr &&
               batch_data->batch.send_initial_metadata ==
                   batch->send_initial_metadata &&
               batch_data->batch.send_message == batch->send_message &&
               batch_data->batch.send_trailing_metadata ==
                   batch->send_trailing_metadata;
      });
  // If batch_data is a replay batch, then there will be no pending
  // batch to complete.
  if (pending == nullptr) {
    GRPC_ERROR_UNREF(error);
    return;
  }
  // Add closure.
  closures->Add(pending->batch->on_complete, error,
                "on_complete for pending batch");
  pending->batch->on_complete = nullptr;
  MaybeClearPendingBatch(pending);
}

void RetryingCall::AddClosuresForReplayOrPendingSendOps(
    SubchannelCallBatchData* batch_data, SubchannelCallRetryState* retry_state,
    CallCombinerClosureList* closures) {
  bool have_pending_send_message_ops =
      retry_state->started_send_message_count < send_messages_.size();
  bool have_pending_send_trailing_metadata_op =
      seen_send_trailing_metadata_ &&
      !retry_state->started_send_trailing_metadata;
  if (!have_pending_send_message_ops &&
      !have_pending_send_trailing_metadata_op) {
    for (size_t i = 0; i < GPR_ARRAY_SIZE(pending_batches_); ++i) {
      PendingBatch* pending = &pending_batches_[i];
      grpc_transport_stream_op_batch* batch = pending->batch;
      if (batch == nullptr || pending->send_ops_cached) continue;
      if (batch->send_message) have_pending_send_message_ops = true;
      if (batch->send_trailing_metadata) {
        have_pending_send_trailing_metadata_op = true;
      }
    }
  }
  if (have_pending_send_message_ops || have_pending_send_trailing_metadata_op) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
      gpr_log(GPR_INFO,
              "chand=%p retrying_call=%p: starting next batch for pending send "
              "op(s)",
              chand_, this);
    }
    GRPC_CLOSURE_INIT(&batch_data->batch.handler_private.closure,
                      StartRetriableSubchannelBatches, this,
                      grpc_schedule_on_exec_ctx);
    closures->Add(&batch_data->batch.handler_private.closure, GRPC_ERROR_NONE,
                  "starting next batch for send_* op(s)");
  }
}

void RetryingCall::OnComplete(void* arg, grpc_error* error) {
  SubchannelCallBatchData* batch_data =
      static_cast<SubchannelCallBatchData*>(arg);
  RetryingCall* call = batch_data->call;
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
    gpr_log(GPR_INFO,
            "chand=%p retrying_call=%p: got on_complete, error=%s, batch=%s",
            call->chand_, call, grpc_error_string(error),
            grpc_transport_stream_op_batch_string(&batch_data->batch).c_str());
  }
  SubchannelCallRetryState* retry_state =
      static_cast<SubchannelCallRetryState*>(
          batch_data->lb_call->GetParentData());
  // Update bookkeeping in retry_state.
  if (batch_data->batch.send_initial_metadata) {
    retry_state->completed_send_initial_metadata = true;
  }
  if (batch_data->batch.send_message) {
    ++retry_state->completed_send_message_count;
  }
  if (batch_data->batch.send_trailing_metadata) {
    retry_state->completed_send_trailing_metadata = true;
  }
  // If the call is committed, free cached data for send ops that we've just
  // completed.
  if (call->retry_committed_) {
    call->FreeCachedSendOpDataForCompletedBatch(batch_data, retry_state);
  }
  // Construct list of closures to execute.
  CallCombinerClosureList closures;
  // If a retry was already dispatched, that means we saw
  // recv_trailing_metadata before this, so we do nothing here.
  // Otherwise, invoke the callback to return the result to the surface.
  if (!retry_state->retry_dispatched) {
    // Add closure for the completed pending batch, if any.
    call->AddClosuresForCompletedPendingBatch(batch_data, GRPC_ERROR_REF(error),
                                              &closures);
    // If needed, add a callback to start any replay or pending send ops on
    // the subchannel call.
    if (!retry_state->completed_recv_trailing_metadata) {
      call->AddClosuresForReplayOrPendingSendOps(batch_data, retry_state,
                                                 &closures);
    }
  }
  // Track number of pending subchannel send batches and determine if this
  // was the last one.
  --call->num_pending_retriable_subchannel_send_batches_;
  const bool last_send_batch_complete =
      call->num_pending_retriable_subchannel_send_batches_ == 0;
  // Don't need batch_data anymore.
  batch_data->Unref();
  // Schedule all of the closures identified above.
  // Note: This yeilds the call combiner.
  closures.RunClosures(call->call_combiner_);
  // If this was the last subchannel send batch, unref the call stack.
  if (last_send_batch_complete) {
    GRPC_CALL_STACK_UNREF(call->owning_call_, "subchannel_send_batches");
  }
}

//
// subchannel batch construction
//

void RetryingCall::StartBatchInCallCombiner(void* arg,
                                            grpc_error* /*ignored*/) {
  grpc_transport_stream_op_batch* batch =
      static_cast<grpc_transport_stream_op_batch*>(arg);
  auto* lb_call =
      static_cast<LoadBalancedCall*>(batch->handler_private.extra_arg);
  // Note: This will release the call combiner.
  lb_call->StartTransportStreamOpBatch(batch);
}

void RetryingCall::AddClosureForSubchannelBatch(
    grpc_transport_stream_op_batch* batch, CallCombinerClosureList* closures) {
  batch->handler_private.extra_arg = lb_call_.get();
  GRPC_CLOSURE_INIT(&batch->handler_private.closure, StartBatchInCallCombiner,
                    batch, grpc_schedule_on_exec_ctx);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
    gpr_log(GPR_INFO,
            "chand=%p retrying_call=%p: starting subchannel batch: %s", chand_,
            this, grpc_transport_stream_op_batch_string(batch).c_str());
  }
  closures->Add(&batch->handler_private.closure, GRPC_ERROR_NONE,
                "start_subchannel_batch");
}

void RetryingCall::AddRetriableSendInitialMetadataOp(
    SubchannelCallRetryState* retry_state,
    SubchannelCallBatchData* batch_data) {
  // Maps the number of retries to the corresponding metadata value slice.
  const grpc_slice* retry_count_strings[] = {&GRPC_MDSTR_1, &GRPC_MDSTR_2,
                                             &GRPC_MDSTR_3, &GRPC_MDSTR_4};
  // We need to make a copy of the metadata batch for each attempt, since
  // the filters in the subchannel stack may modify this batch, and we don't
  // want those modifications to be passed forward to subsequent attempts.
  //
  // If we've already completed one or more attempts, add the
  // grpc-retry-attempts header.
  retry_state->send_initial_metadata_storage =
      static_cast<grpc_linked_mdelem*>(arena_->Alloc(
          sizeof(grpc_linked_mdelem) *
          (send_initial_metadata_.list.count + (num_attempts_completed_ > 0))));
  grpc_metadata_batch_copy(&send_initial_metadata_,
                           &retry_state->send_initial_metadata,
                           retry_state->send_initial_metadata_storage);
  if (GPR_UNLIKELY(retry_state->send_initial_metadata.idx.named
                       .grpc_previous_rpc_attempts != nullptr)) {
    grpc_metadata_batch_remove(&retry_state->send_initial_metadata,
                               GRPC_BATCH_GRPC_PREVIOUS_RPC_ATTEMPTS);
  }
  if (GPR_UNLIKELY(num_attempts_completed_ > 0)) {
    grpc_mdelem retry_md = grpc_mdelem_create(
        GRPC_MDSTR_GRPC_PREVIOUS_RPC_ATTEMPTS,
        *retry_count_strings[num_attempts_completed_ - 1], nullptr);
    grpc_error* error = grpc_metadata_batch_add_tail(
        &retry_state->send_initial_metadata,
        &retry_state
             ->send_initial_metadata_storage[send_initial_metadata_.list.count],
        retry_md, GRPC_BATCH_GRPC_PREVIOUS_RPC_ATTEMPTS);
    if (GPR_UNLIKELY(error != GRPC_ERROR_NONE)) {
      gpr_log(GPR_ERROR, "error adding retry metadata: %s",
              grpc_error_string(error));
      GPR_ASSERT(false);
    }
  }
  retry_state->started_send_initial_metadata = true;
  batch_data->batch.send_initial_metadata = true;
  batch_data->batch.payload->send_initial_metadata.send_initial_metadata =
      &retry_state->send_initial_metadata;
  batch_data->batch.payload->send_initial_metadata.send_initial_metadata_flags =
      send_initial_metadata_flags_;
  batch_data->batch.payload->send_initial_metadata.peer_string = peer_string_;
}

void RetryingCall::AddRetriableSendMessageOp(
    SubchannelCallRetryState* retry_state,
    SubchannelCallBatchData* batch_data) {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
    gpr_log(GPR_INFO,
            "chand=%p retrying_call=%p: starting calld->send_messages[%" PRIuPTR
            "]",
            chand_, this, retry_state->started_send_message_count);
  }
  ByteStreamCache* cache =
      send_messages_[retry_state->started_send_message_count];
  ++retry_state->started_send_message_count;
  retry_state->send_message.Init(cache);
  batch_data->batch.send_message = true;
  batch_data->batch.payload->send_message.send_message.reset(
      retry_state->send_message.get());
}

void RetryingCall::AddRetriableSendTrailingMetadataOp(
    SubchannelCallRetryState* retry_state,
    SubchannelCallBatchData* batch_data) {
  // We need to make a copy of the metadata batch for each attempt, since
  // the filters in the subchannel stack may modify this batch, and we don't
  // want those modifications to be passed forward to subsequent attempts.
  retry_state->send_trailing_metadata_storage =
      static_cast<grpc_linked_mdelem*>(arena_->Alloc(
          sizeof(grpc_linked_mdelem) * send_trailing_metadata_.list.count));
  grpc_metadata_batch_copy(&send_trailing_metadata_,
                           &retry_state->send_trailing_metadata,
                           retry_state->send_trailing_metadata_storage);
  retry_state->started_send_trailing_metadata = true;
  batch_data->batch.send_trailing_metadata = true;
  batch_data->batch.payload->send_trailing_metadata.send_trailing_metadata =
      &retry_state->send_trailing_metadata;
}

void RetryingCall::AddRetriableRecvInitialMetadataOp(
    SubchannelCallRetryState* retry_state,
    SubchannelCallBatchData* batch_data) {
  retry_state->started_recv_initial_metadata = true;
  batch_data->batch.recv_initial_metadata = true;
  grpc_metadata_batch_init(&retry_state->recv_initial_metadata);
  batch_data->batch.payload->recv_initial_metadata.recv_initial_metadata =
      &retry_state->recv_initial_metadata;
  batch_data->batch.payload->recv_initial_metadata.trailing_metadata_available =
      &retry_state->trailing_metadata_available;
  GRPC_CLOSURE_INIT(&retry_state->recv_initial_metadata_ready,
                    RecvInitialMetadataReady, batch_data,
                    grpc_schedule_on_exec_ctx);
  batch_data->batch.payload->recv_initial_metadata.recv_initial_metadata_ready =
      &retry_state->recv_initial_metadata_ready;
}

void RetryingCall::AddRetriableRecvMessageOp(
    SubchannelCallRetryState* retry_state,
    SubchannelCallBatchData* batch_data) {
  ++retry_state->started_recv_message_count;
  batch_data->batch.recv_message = true;
  batch_data->batch.payload->recv_message.recv_message =
      &retry_state->recv_message;
  GRPC_CLOSURE_INIT(&retry_state->recv_message_ready, RecvMessageReady,
                    batch_data, grpc_schedule_on_exec_ctx);
  batch_data->batch.payload->recv_message.recv_message_ready =
      &retry_state->recv_message_ready;
}

void RetryingCall::AddRetriableRecvTrailingMetadataOp(
    SubchannelCallRetryState* retry_state,
    SubchannelCallBatchData* batch_data) {
  retry_state->started_recv_trailing_metadata = true;
  batch_data->batch.recv_trailing_metadata = true;
  grpc_metadata_batch_init(&retry_state->recv_trailing_metadata);
  batch_data->batch.payload->recv_trailing_metadata.recv_trailing_metadata =
      &retry_state->recv_trailing_metadata;
  batch_data->batch.payload->recv_trailing_metadata.collect_stats =
      &retry_state->collect_stats;
  GRPC_CLOSURE_INIT(&retry_state->recv_trailing_metadata_ready,
                    RecvTrailingMetadataReady, batch_data,
                    grpc_schedule_on_exec_ctx);
  batch_data->batch.payload->recv_trailing_metadata
      .recv_trailing_metadata_ready =
      &retry_state->recv_trailing_metadata_ready;
}

void RetryingCall::StartInternalRecvTrailingMetadata() {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
    gpr_log(
        GPR_INFO,
        "chand=%p retrying_call=%p: call failed but recv_trailing_metadata not "
        "started; starting it internally",
        chand_, this);
  }
  SubchannelCallRetryState* retry_state =
      static_cast<SubchannelCallRetryState*>(lb_call_->GetParentData());
  // Create batch_data with 2 refs, since this batch will be unreffed twice:
  // once for the recv_trailing_metadata_ready callback when the subchannel
  // batch returns, and again when we actually get a recv_trailing_metadata
  // op from the surface.
  SubchannelCallBatchData* batch_data =
      SubchannelCallBatchData::Create(this, 2, false /* set_on_complete */);
  AddRetriableRecvTrailingMetadataOp(retry_state, batch_data);
  retry_state->recv_trailing_metadata_internal_batch = batch_data;
  // Note: This will release the call combiner.
  lb_call_->StartTransportStreamOpBatch(&batch_data->batch);
}

// If there are any cached send ops that need to be replayed on the
// current subchannel call, creates and returns a new subchannel batch
// to replay those ops.  Otherwise, returns nullptr.
RetryingCall::SubchannelCallBatchData*
RetryingCall::MaybeCreateSubchannelBatchForReplay(
    SubchannelCallRetryState* retry_state) {
  SubchannelCallBatchData* replay_batch_data = nullptr;
  // send_initial_metadata.
  if (seen_send_initial_metadata_ &&
      !retry_state->started_send_initial_metadata &&
      !pending_send_initial_metadata_) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
      gpr_log(GPR_INFO,
              "chand=%p retrying_call=%p: replaying previously completed "
              "send_initial_metadata op",
              chand_, this);
    }
    replay_batch_data =
        SubchannelCallBatchData::Create(this, 1, true /* set_on_complete */);
    AddRetriableSendInitialMetadataOp(retry_state, replay_batch_data);
  }
  // send_message.
  // Note that we can only have one send_message op in flight at a time.
  if (retry_state->started_send_message_count < send_messages_.size() &&
      retry_state->started_send_message_count ==
          retry_state->completed_send_message_count &&
      !pending_send_message_) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
      gpr_log(GPR_INFO,
              "chand=%p retrying_call=%p: replaying previously completed "
              "send_message op",
              chand_, this);
    }
    if (replay_batch_data == nullptr) {
      replay_batch_data =
          SubchannelCallBatchData::Create(this, 1, true /* set_on_complete */);
    }
    AddRetriableSendMessageOp(retry_state, replay_batch_data);
  }
  // send_trailing_metadata.
  // Note that we only add this op if we have no more send_message ops
  // to start, since we can't send down any more send_message ops after
  // send_trailing_metadata.
  if (seen_send_trailing_metadata_ &&
      retry_state->started_send_message_count == send_messages_.size() &&
      !retry_state->started_send_trailing_metadata &&
      !pending_send_trailing_metadata_) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
      gpr_log(GPR_INFO,
              "chand=%p retrying_call=%p: replaying previously completed "
              "send_trailing_metadata op",
              chand_, this);
    }
    if (replay_batch_data == nullptr) {
      replay_batch_data =
          SubchannelCallBatchData::Create(this, 1, true /* set_on_complete */);
    }
    AddRetriableSendTrailingMetadataOp(retry_state, replay_batch_data);
  }
  return replay_batch_data;
}

void RetryingCall::AddSubchannelBatchesForPendingBatches(
    SubchannelCallRetryState* retry_state, CallCombinerClosureList* closures) {
  for (size_t i = 0; i < GPR_ARRAY_SIZE(pending_batches_); ++i) {
    PendingBatch* pending = &pending_batches_[i];
    grpc_transport_stream_op_batch* batch = pending->batch;
    if (batch == nullptr) continue;
    // Skip any batch that either (a) has already been started on this
    // subchannel call or (b) we can't start yet because we're still
    // replaying send ops that need to be completed first.
    // TODO(roth): Note that if any one op in the batch can't be sent
    // yet due to ops that we're replaying, we don't start any of the ops
    // in the batch.  This is probably okay, but it could conceivably
    // lead to increased latency in some cases -- e.g., we could delay
    // starting a recv op due to it being in the same batch with a send
    // op.  If/when we revamp the callback protocol in
    // transport_stream_op_batch, we may be able to fix this.
    if (batch->send_initial_metadata &&
        retry_state->started_send_initial_metadata) {
      continue;
    }
    if (batch->send_message && retry_state->completed_send_message_count <
                                   retry_state->started_send_message_count) {
      continue;
    }
    // Note that we only start send_trailing_metadata if we have no more
    // send_message ops to start, since we can't send down any more
    // send_message ops after send_trailing_metadata.
    if (batch->send_trailing_metadata &&
        (retry_state->started_send_message_count + batch->send_message <
             send_messages_.size() ||
         retry_state->started_send_trailing_metadata)) {
      continue;
    }
    if (batch->recv_initial_metadata &&
        retry_state->started_recv_initial_metadata) {
      continue;
    }
    if (batch->recv_message && retry_state->completed_recv_message_count <
                                   retry_state->started_recv_message_count) {
      continue;
    }
    if (batch->recv_trailing_metadata &&
        retry_state->started_recv_trailing_metadata) {
      // If we previously completed a recv_trailing_metadata op
      // initiated by StartInternalRecvTrailingMetadata(), use the
      // result of that instead of trying to re-start this op.
      if (GPR_UNLIKELY((retry_state->recv_trailing_metadata_internal_batch !=
                        nullptr))) {
        // If the batch completed, then trigger the completion callback
        // directly, so that we return the previously returned results to
        // the application.  Otherwise, just unref the internally
        // started subchannel batch, since we'll propagate the
        // completion when it completes.
        if (retry_state->completed_recv_trailing_metadata) {
          // Batches containing recv_trailing_metadata always succeed.
          closures->Add(
              &retry_state->recv_trailing_metadata_ready, GRPC_ERROR_NONE,
              "re-executing recv_trailing_metadata_ready to propagate "
              "internally triggered result");
        } else {
          retry_state->recv_trailing_metadata_internal_batch->Unref();
        }
        retry_state->recv_trailing_metadata_internal_batch = nullptr;
      }
      continue;
    }
    // If we're not retrying, just send the batch as-is.
    // TODO(roth): This condition doesn't seem exactly right -- maybe need a
    // notion of "draining" once we've committed and are done replaying?
    if (retry_policy_ == nullptr || retry_committed_) {
      AddClosureForSubchannelBatch(batch, closures);
      PendingBatchClear(pending);
      continue;
    }
    // Create batch with the right number of callbacks.
    const bool has_send_ops = batch->send_initial_metadata ||
                              batch->send_message ||
                              batch->send_trailing_metadata;
    const int num_callbacks = has_send_ops + batch->recv_initial_metadata +
                              batch->recv_message +
                              batch->recv_trailing_metadata;
    SubchannelCallBatchData* batch_data = SubchannelCallBatchData::Create(
        this, num_callbacks, has_send_ops /* set_on_complete */);
    // Cache send ops if needed.
    MaybeCacheSendOpsForBatch(pending);
    // send_initial_metadata.
    if (batch->send_initial_metadata) {
      AddRetriableSendInitialMetadataOp(retry_state, batch_data);
    }
    // send_message.
    if (batch->send_message) {
      AddRetriableSendMessageOp(retry_state, batch_data);
    }
    // send_trailing_metadata.
    if (batch->send_trailing_metadata) {
      AddRetriableSendTrailingMetadataOp(retry_state, batch_data);
    }
    // recv_initial_metadata.
    if (batch->recv_initial_metadata) {
      // recv_flags is only used on the server side.
      GPR_ASSERT(batch->payload->recv_initial_metadata.recv_flags == nullptr);
      AddRetriableRecvInitialMetadataOp(retry_state, batch_data);
    }
    // recv_message.
    if (batch->recv_message) {
      AddRetriableRecvMessageOp(retry_state, batch_data);
    }
    // recv_trailing_metadata.
    if (batch->recv_trailing_metadata) {
      AddRetriableRecvTrailingMetadataOp(retry_state, batch_data);
    }
    AddClosureForSubchannelBatch(&batch_data->batch, closures);
    // Track number of pending subchannel send batches.
    // If this is the first one, take a ref to the call stack.
    if (batch->send_initial_metadata || batch->send_message ||
        batch->send_trailing_metadata) {
      if (num_pending_retriable_subchannel_send_batches_ == 0) {
        GRPC_CALL_STACK_REF(owning_call_, "subchannel_send_batches");
      }
      ++num_pending_retriable_subchannel_send_batches_;
    }
  }
}

void RetryingCall::StartRetriableSubchannelBatches(void* arg,
                                                   grpc_error* /*ignored*/) {
  RetryingCall* call = static_cast<RetryingCall*>(arg);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
    gpr_log(GPR_INFO,
            "chand=%p retrying_call=%p: constructing retriable batches",
            call->chand_, call);
  }
  SubchannelCallRetryState* retry_state =
      static_cast<SubchannelCallRetryState*>(call->lb_call_->GetParentData());
  // Construct list of closures to execute, one for each pending batch.
  CallCombinerClosureList closures;
  // Replay previously-returned send_* ops if needed.
  SubchannelCallBatchData* replay_batch_data =
      call->MaybeCreateSubchannelBatchForReplay(retry_state);
  if (replay_batch_data != nullptr) {
    call->AddClosureForSubchannelBatch(&replay_batch_data->batch, &closures);
    // Track number of pending subchannel send batches.
    // If this is the first one, take a ref to the call stack.
    if (call->num_pending_retriable_subchannel_send_batches_ == 0) {
      GRPC_CALL_STACK_REF(call->owning_call_, "subchannel_send_batches");
    }
    ++call->num_pending_retriable_subchannel_send_batches_;
  }
  // Now add pending batches.
  call->AddSubchannelBatchesForPendingBatches(retry_state, &closures);
  // Start batches on subchannel call.
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
    gpr_log(GPR_INFO,
            "chand=%p retrying_call=%p: starting %" PRIuPTR
            " retriable batches on lb_call=%p",
            call->chand_, call, closures.size(), call->lb_call_.get());
  }
  // Note: This will yield the call combiner.
  closures.RunClosures(call->call_combiner_);
}

void RetryingCall::CreateLbCall(void* arg, grpc_error* /*error*/) {
  auto* call = static_cast<RetryingCall*>(arg);
  const size_t parent_data_size =
      call->enable_retries_ ? sizeof(SubchannelCallRetryState) : 0;
  grpc_call_element_args args = {call->owning_call_,     nullptr,
                                 call->call_context_,    call->path_,
                                 call->call_start_time_, call->deadline_,
                                 call->arena_,           call->call_combiner_};
  call->lb_call_ = LoadBalancedCall::Create(call->chand_, args, call->pollent_,
                                            parent_data_size);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
    gpr_log(GPR_INFO, "chand=%p retrying_call=%p: create lb_call=%p",
            call->chand_, call, call->lb_call_.get());
  }
  if (parent_data_size > 0) {
    new (call->lb_call_->GetParentData())
        SubchannelCallRetryState(call->call_context_);
  }
  call->PendingBatchesResume();
}

//
// LoadBalancedCall::Metadata
//

class LoadBalancedCall::Metadata
    : public LoadBalancingPolicy::MetadataInterface {
 public:
  Metadata(LoadBalancedCall* lb_call, grpc_metadata_batch* batch)
      : lb_call_(lb_call), batch_(batch) {}

  void Add(absl::string_view key, absl::string_view value) override {
    grpc_linked_mdelem* linked_mdelem = static_cast<grpc_linked_mdelem*>(
        lb_call_->arena_->Alloc(sizeof(grpc_linked_mdelem)));
    linked_mdelem->md = grpc_mdelem_from_slices(
        ExternallyManagedSlice(key.data(), key.size()),
        ExternallyManagedSlice(value.data(), value.size()));
    GPR_ASSERT(grpc_metadata_batch_link_tail(batch_, linked_mdelem) ==
               GRPC_ERROR_NONE);
  }

  iterator begin() const override {
    static_assert(sizeof(grpc_linked_mdelem*) <= sizeof(intptr_t),
                  "iterator size too large");
    return iterator(
        this, reinterpret_cast<intptr_t>(MaybeSkipEntry(batch_->list.head)));
  }
  iterator end() const override {
    static_assert(sizeof(grpc_linked_mdelem*) <= sizeof(intptr_t),
                  "iterator size too large");
    return iterator(this, 0);
  }

  iterator erase(iterator it) override {
    grpc_linked_mdelem* linked_mdelem =
        reinterpret_cast<grpc_linked_mdelem*>(GetIteratorHandle(it));
    intptr_t handle = reinterpret_cast<intptr_t>(linked_mdelem->next);
    grpc_metadata_batch_remove(batch_, linked_mdelem);
    return iterator(this, handle);
  }

 private:
  grpc_linked_mdelem* MaybeSkipEntry(grpc_linked_mdelem* entry) const {
    if (entry != nullptr && batch_->idx.named.path == entry) {
      return entry->next;
    }
    return entry;
  }

  intptr_t IteratorHandleNext(intptr_t handle) const override {
    grpc_linked_mdelem* linked_mdelem =
        reinterpret_cast<grpc_linked_mdelem*>(handle);
    return reinterpret_cast<intptr_t>(MaybeSkipEntry(linked_mdelem->next));
  }

  std::pair<absl::string_view, absl::string_view> IteratorHandleGet(
      intptr_t handle) const override {
    grpc_linked_mdelem* linked_mdelem =
        reinterpret_cast<grpc_linked_mdelem*>(handle);
    return std::make_pair(StringViewFromSlice(GRPC_MDKEY(linked_mdelem->md)),
                          StringViewFromSlice(GRPC_MDVALUE(linked_mdelem->md)));
  }

  LoadBalancedCall* lb_call_;
  grpc_metadata_batch* batch_;
};

//
// LoadBalancedCall::LbCallState
//

class LoadBalancedCall::LbCallState : public LoadBalancingPolicy::CallState {
 public:
  explicit LbCallState(LoadBalancedCall* lb_call) : lb_call_(lb_call) {}

  void* Alloc(size_t size) override { return lb_call_->arena_->Alloc(size); }

  const LoadBalancingPolicy::BackendMetricData* GetBackendMetricData()
      override {
    if (lb_call_->backend_metric_data_ == nullptr) {
      grpc_linked_mdelem* md = lb_call_->recv_trailing_metadata_->idx.named
                                   .x_endpoint_load_metrics_bin;
      if (md != nullptr) {
        lb_call_->backend_metric_data_ =
            ParseBackendMetricData(GRPC_MDVALUE(md->md), lb_call_->arena_);
      }
    }
    return lb_call_->backend_metric_data_;
  }

  absl::string_view ExperimentalGetCallAttribute(const char* key) override {
    auto* service_config_call_data = static_cast<ServiceConfigCallData*>(
        lb_call_->call_context_[GRPC_CONTEXT_SERVICE_CONFIG_CALL_DATA].value);
    auto& call_attributes = service_config_call_data->call_attributes();
    auto it = call_attributes.find(key);
    if (it == call_attributes.end()) return absl::string_view();
    return it->second;
  }

 private:
  LoadBalancedCall* lb_call_;
};

//
// LoadBalancedCall
//

RefCountedPtr<LoadBalancedCall> LoadBalancedCall::Create(
    ChannelData* chand, const grpc_call_element_args& args,
    grpc_polling_entity* pollent, size_t parent_data_size) {
  const size_t alloc_size =
      parent_data_size > 0
          ? (GPR_ROUND_UP_TO_ALIGNMENT_SIZE(sizeof(LoadBalancedCall)) +
             parent_data_size)
          : sizeof(LoadBalancedCall);
  auto* lb_call = static_cast<LoadBalancedCall*>(args.arena->Alloc(alloc_size));
  new (lb_call) LoadBalancedCall(chand, args, pollent);
  return lb_call;
}

LoadBalancedCall::LoadBalancedCall(ChannelData* chand,
                                   const grpc_call_element_args& args,
                                   grpc_polling_entity* pollent)
    : refs_(1, GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)
                   ? "LoadBalancedCall"
                   : nullptr),
      chand_(chand),
      path_(grpc_slice_ref_internal(args.path)),
      call_start_time_(args.start_time),
      deadline_(args.deadline),
      arena_(args.arena),
      owning_call_(args.call_stack),
      call_combiner_(args.call_combiner),
      call_context_(args.context),
      pollent_(pollent) {}

LoadBalancedCall::~LoadBalancedCall() {
  grpc_slice_unref_internal(path_);
  GRPC_ERROR_UNREF(cancel_error_);
  if (backend_metric_data_ != nullptr) {
    backend_metric_data_
        ->LoadBalancingPolicy::BackendMetricData::~BackendMetricData();
  }
  // Make sure there are no remaining pending batches.
  for (size_t i = 0; i < GPR_ARRAY_SIZE(pending_batches_); ++i) {
    GPR_ASSERT(pending_batches_[i] == nullptr);
  }
}

RefCountedPtr<LoadBalancedCall> LoadBalancedCall::Ref() {
  IncrementRefCount();
  return RefCountedPtr<LoadBalancedCall>(this);
}

RefCountedPtr<LoadBalancedCall> LoadBalancedCall::Ref(
    const DebugLocation& location, const char* reason) {
  IncrementRefCount(location, reason);
  return RefCountedPtr<LoadBalancedCall>(this);
}

void LoadBalancedCall::Unref() {
  if (GPR_UNLIKELY(refs_.Unref())) {
    this->~LoadBalancedCall();
  }
}

void LoadBalancedCall::Unref(const DebugLocation& location,
                             const char* reason) {
  if (GPR_UNLIKELY(refs_.Unref(location, reason))) {
    this->~LoadBalancedCall();
  }
}

void LoadBalancedCall::IncrementRefCount() { refs_.Ref(); }

void LoadBalancedCall::IncrementRefCount(const DebugLocation& location,
                                         const char* reason) {
  refs_.Ref(location, reason);
}

void* LoadBalancedCall::GetParentData() {
  return reinterpret_cast<char*>(this) +
         GPR_ROUND_UP_TO_ALIGNMENT_SIZE(sizeof(LoadBalancedCall));
}

size_t LoadBalancedCall::GetBatchIndex(grpc_transport_stream_op_batch* batch) {
  // Note: It is important the send_initial_metadata be the first entry
  // here, since the code in pick_subchannel_locked() assumes it will be.
  if (batch->send_initial_metadata) return 0;
  if (batch->send_message) return 1;
  if (batch->send_trailing_metadata) return 2;
  if (batch->recv_initial_metadata) return 3;
  if (batch->recv_message) return 4;
  if (batch->recv_trailing_metadata) return 5;
  GPR_UNREACHABLE_CODE(return (size_t)-1);
}

// This is called via the call combiner, so access to calld is synchronized.
void LoadBalancedCall::PendingBatchesAdd(
    grpc_transport_stream_op_batch* batch) {
  const size_t idx = GetBatchIndex(batch);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
    gpr_log(GPR_INFO,
            "chand=%p lb_call=%p: adding pending batch at index %" PRIuPTR,
            chand_, this, idx);
  }
  GPR_ASSERT(pending_batches_[idx] == nullptr);
  pending_batches_[idx] = batch;
}

// This is called via the call combiner, so access to calld is synchronized.
void LoadBalancedCall::FailPendingBatchInCallCombiner(void* arg,
                                                      grpc_error* error) {
  grpc_transport_stream_op_batch* batch =
      static_cast<grpc_transport_stream_op_batch*>(arg);
  auto* self = static_cast<LoadBalancedCall*>(batch->handler_private.extra_arg);
  // Note: This will release the call combiner.
  grpc_transport_stream_op_batch_finish_with_failure(
      batch, GRPC_ERROR_REF(error), self->call_combiner_);
}

// This is called via the call combiner, so access to calld is synchronized.
void LoadBalancedCall::PendingBatchesFail(
    grpc_error* error,
    YieldCallCombinerPredicate yield_call_combiner_predicate) {
  GPR_ASSERT(error != GRPC_ERROR_NONE);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
    size_t num_batches = 0;
    for (size_t i = 0; i < GPR_ARRAY_SIZE(pending_batches_); ++i) {
      if (pending_batches_[i] != nullptr) ++num_batches;
    }
    gpr_log(GPR_INFO,
            "chand=%p lb_call=%p: failing %" PRIuPTR " pending batches: %s",
            chand_, this, num_batches, grpc_error_string(error));
  }
  CallCombinerClosureList closures;
  for (size_t i = 0; i < GPR_ARRAY_SIZE(pending_batches_); ++i) {
    grpc_transport_stream_op_batch*& batch = pending_batches_[i];
    if (batch != nullptr) {
      batch->handler_private.extra_arg = this;
      GRPC_CLOSURE_INIT(&batch->handler_private.closure,
                        FailPendingBatchInCallCombiner, batch,
                        grpc_schedule_on_exec_ctx);
      closures.Add(&batch->handler_private.closure, GRPC_ERROR_REF(error),
                   "PendingBatchesFail");
      batch = nullptr;
    }
  }
  if (yield_call_combiner_predicate(closures)) {
    closures.RunClosures(call_combiner_);
  } else {
    closures.RunClosuresWithoutYielding(call_combiner_);
  }
  GRPC_ERROR_UNREF(error);
}

// This is called via the call combiner, so access to calld is synchronized.
void LoadBalancedCall::ResumePendingBatchInCallCombiner(
    void* arg, grpc_error* /*ignored*/) {
  grpc_transport_stream_op_batch* batch =
      static_cast<grpc_transport_stream_op_batch*>(arg);
  SubchannelCall* subchannel_call =
      static_cast<SubchannelCall*>(batch->handler_private.extra_arg);
  // Note: This will release the call combiner.
  subchannel_call->StartTransportStreamOpBatch(batch);
}

// This is called via the call combiner, so access to calld is synchronized.
void LoadBalancedCall::PendingBatchesResume() {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
    size_t num_batches = 0;
    for (size_t i = 0; i < GPR_ARRAY_SIZE(pending_batches_); ++i) {
      if (pending_batches_[i] != nullptr) ++num_batches;
    }
    gpr_log(GPR_INFO,
            "chand=%p lb_call=%p: starting %" PRIuPTR
            " pending batches on subchannel_call=%p",
            chand_, this, num_batches, subchannel_call_.get());
  }
  CallCombinerClosureList closures;
  for (size_t i = 0; i < GPR_ARRAY_SIZE(pending_batches_); ++i) {
    grpc_transport_stream_op_batch*& batch = pending_batches_[i];
    if (batch != nullptr) {
      batch->handler_private.extra_arg = subchannel_call_.get();
      GRPC_CLOSURE_INIT(&batch->handler_private.closure,
                        ResumePendingBatchInCallCombiner, batch,
                        grpc_schedule_on_exec_ctx);
      closures.Add(&batch->handler_private.closure, GRPC_ERROR_NONE,
                   "PendingBatchesResume");
      batch = nullptr;
    }
  }
  // Note: This will release the call combiner.
  closures.RunClosures(call_combiner_);
}

void LoadBalancedCall::StartTransportStreamOpBatch(
    grpc_transport_stream_op_batch* batch) {
  // Intercept recv_trailing_metadata_ready for LB callback.
  if (batch->recv_trailing_metadata) {
    InjectRecvTrailingMetadataReadyForLoadBalancingPolicy(batch);
  }
  // If we've previously been cancelled, immediately fail any new batches.
  if (GPR_UNLIKELY(cancel_error_ != GRPC_ERROR_NONE)) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
      gpr_log(GPR_INFO, "chand=%p lb_call=%p: failing batch with error: %s",
              chand_, this, grpc_error_string(cancel_error_));
    }
    // Note: This will release the call combiner.
    grpc_transport_stream_op_batch_finish_with_failure(
        batch, GRPC_ERROR_REF(cancel_error_), call_combiner_);
    return;
  }
  // Handle cancellation.
  if (GPR_UNLIKELY(batch->cancel_stream)) {
    // Stash a copy of cancel_error in our call data, so that we can use
    // it for subsequent operations.  This ensures that if the call is
    // cancelled before any batches are passed down (e.g., if the deadline
    // is in the past when the call starts), we can return the right
    // error to the caller when the first batch does get passed down.
    GRPC_ERROR_UNREF(cancel_error_);
    cancel_error_ = GRPC_ERROR_REF(batch->payload->cancel_stream.cancel_error);
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
      gpr_log(GPR_INFO, "chand=%p lb_call=%p: recording cancel_error=%s",
              chand_, this, grpc_error_string(cancel_error_));
    }
    // If we do not have a subchannel call (i.e., a pick has not yet
    // been started), fail all pending batches.  Otherwise, send the
    // cancellation down to the subchannel call.
    if (subchannel_call_ == nullptr) {
      PendingBatchesFail(GRPC_ERROR_REF(cancel_error_), NoYieldCallCombiner);
      // Note: This will release the call combiner.
      grpc_transport_stream_op_batch_finish_with_failure(
          batch, GRPC_ERROR_REF(cancel_error_), call_combiner_);
    } else {
      // Note: This will release the call combiner.
      subchannel_call_->StartTransportStreamOpBatch(batch);
    }
    return;
  }
  // Add the batch to the pending list.
  PendingBatchesAdd(batch);
  // Check if we've already gotten a subchannel call.
  // Note that once we have picked a subchannel, we do not need to acquire
  // the channel's data plane mutex, which is more efficient (especially for
  // streaming calls).
  if (subchannel_call_ != nullptr) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
      gpr_log(GPR_INFO,
              "chand=%p lb_call=%p: starting batch on subchannel_call=%p",
              chand_, this, subchannel_call_.get());
    }
    PendingBatchesResume();
    return;
  }
  // We do not yet have a subchannel call.
  // For batches containing a send_initial_metadata op, acquire the
  // channel's data plane mutex to pick a subchannel.
  if (GPR_LIKELY(batch->send_initial_metadata)) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
      gpr_log(GPR_INFO,
              "chand=%p lb_call=%p: grabbing data plane mutex to perform pick",
              chand_, this);
    }
    PickSubchannel(this, GRPC_ERROR_NONE);
  } else {
    // For all other batches, release the call combiner.
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_call_trace)) {
      gpr_log(GPR_INFO,
              "chand=%p lb_call=%p: saved batch, yielding call combiner",
              chand_, this);
    }
    GRPC_CALL_COMBINER_STOP(call_combiner_,
                            "batch does not include send_initial_metadata");
  }
}

void LoadBalancedCall::RecvTrailingMetadataReadyForLoadBalancingPolicy(
    void* arg, grpc_error* error) {
  auto* self = static_cast<LoadBalancedCall*>(arg);
  if (self->lb_recv_trailing_metadata_ready_ != nullptr) {
    // Set error if call did not succeed.
    grpc_error* error_for_lb = GRPC_ERROR_NONE;
    if (error != GRPC_ERROR_NONE) {
      error_for_lb = error;
    } else {
      const auto& fields = self->recv_trailing_metadata_->idx.named;
      GPR_ASSERT(fields.grpc_status != nullptr);
      grpc_status_code status =
          grpc_get_status_code_from_metadata(fields.grpc_status->md);
      std::string msg;
      if (status != GRPC_STATUS_OK) {
        error_for_lb = grpc_error_set_int(
            GRPC_ERROR_CREATE_FROM_STATIC_STRING("call failed"),
            GRPC_ERROR_INT_GRPC_STATUS, status);
        if (fields.grpc_message != nullptr) {
          error_for_lb = grpc_error_set_str(
              error_for_lb, GRPC_ERROR_STR_GRPC_MESSAGE,
              grpc_slice_ref_internal(GRPC_MDVALUE(fields.grpc_message->md)));
        }
      }
    }
    // Invoke callback to LB policy.
    Metadata trailing_metadata(self, self->recv_trailing_metadata_);
    LbCallState lb_call_state(self);
    self->lb_recv_trailing_metadata_ready_(error_for_lb, &trailing_metadata,
                                           &lb_call_state);
    if (error == GRPC_ERROR_NONE) GRPC_ERROR_UNREF(error_for_lb);
  }
  // Chain to original callback.
  Closure::Run(DEBUG_LOCATION, self->original_recv_trailing_metadata_ready_,
               GRPC_ERROR_REF(error));
}

// TODO(roth): Consider not intercepting this callback unless we
// actually need to, if this causes a performance problem.
void LoadBalancedCall::InjectRecvTrailingMetadataReadyForLoadBalancingPolicy(
    grpc_transport_stream_op_batch* batch) {
  recv_trailing_metadata_ =
      batch->payload->recv_trailing_metadata.recv_trailing_metadata;
  original_recv_trailing_metadata_ready_ =
      batch->payload->recv_trailing_metadata.recv_trailing_metadata_ready;
  GRPC_CLOSURE_INIT(&recv_trailing_metadata_ready_,
                    RecvTrailingMetadataReadyForLoadBalancingPolicy, this,
                    grpc_schedule_on_exec_ctx);
  batch->payload->recv_trailing_metadata.recv_trailing_metadata_ready =
      &recv_trailing_metadata_ready_;
}

void LoadBalancedCall::CreateSubchannelCall() {
  SubchannelCall::Args call_args = {
      std::move(connected_subchannel_), pollent_, path_, call_start_time_,
      deadline_, arena_,
      // TODO(roth): When we implement hedging support, we will probably
      // need to use a separate call context for each subchannel call.
      call_context_, call_combiner_};
  grpc_error* error = GRPC_ERROR_NONE;
  subchannel_call_ = SubchannelCall::Create(std::move(call_args), &error);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
    gpr_log(GPR_INFO,
            "chand=%p lb_call=%p: create subchannel_call=%p: error=%s", chand_,
            this, subchannel_call_.get(), grpc_error_string(error));
  }
  if (GPR_UNLIKELY(error != GRPC_ERROR_NONE)) {
    PendingBatchesFail(error, YieldCallCombiner);
  } else {
    PendingBatchesResume();
  }
}

// A class to handle the call combiner cancellation callback for a
// queued pick.
// TODO(roth): When we implement hedging support, we won't be able to
// register a call combiner cancellation closure for each LB pick,
// because there may be multiple LB picks happening in parallel.
// Instead, we will probably need to maintain a list in the CallData
// object of pending LB picks to be cancelled when the closure runs.
class LoadBalancedCall::LbQueuedCallCanceller {
 public:
  explicit LbQueuedCallCanceller(RefCountedPtr<LoadBalancedCall> lb_call)
      : lb_call_(std::move(lb_call)) {
    GRPC_CALL_STACK_REF(lb_call_->owning_call_, "LbQueuedCallCanceller");
    GRPC_CLOSURE_INIT(&closure_, &CancelLocked, this, nullptr);
    lb_call_->call_combiner_->SetNotifyOnCancel(&closure_);
  }

 private:
  static void CancelLocked(void* arg, grpc_error* error) {
    auto* self = static_cast<LbQueuedCallCanceller*>(arg);
    auto* lb_call = self->lb_call_.get();
    auto* chand = lb_call->chand_;
    {
      MutexLock lock(chand->data_plane_mu());
      if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
        gpr_log(GPR_INFO,
                "chand=%p lb_call=%p: cancelling queued pick: "
                "error=%s self=%p calld->pick_canceller=%p",
                chand, lb_call, grpc_error_string(error), self,
                lb_call->lb_call_canceller_);
      }
      if (lb_call->lb_call_canceller_ == self && error != GRPC_ERROR_NONE) {
        // Remove pick from list of queued picks.
        lb_call->MaybeRemoveCallFromLbQueuedCallsLocked();
        // Fail pending batches on the call.
        lb_call->PendingBatchesFail(GRPC_ERROR_REF(error),
                                    YieldCallCombinerIfPendingBatchesFound);
      }
    }
    GRPC_CALL_STACK_UNREF(lb_call->owning_call_, "LbQueuedCallCanceller");
    delete self;
  }

  RefCountedPtr<LoadBalancedCall> lb_call_;
  grpc_closure closure_;
};

void LoadBalancedCall::MaybeRemoveCallFromLbQueuedCallsLocked() {
  if (!queued_pending_lb_pick_) return;
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
    gpr_log(GPR_INFO, "chand=%p lb_call=%p: removing from queued picks list",
            chand_, this);
  }
  chand_->RemoveLbQueuedCall(&queued_call_, pollent_);
  queued_pending_lb_pick_ = false;
  // Lame the call combiner canceller.
  lb_call_canceller_ = nullptr;
}

void LoadBalancedCall::MaybeAddCallToLbQueuedCallsLocked() {
  if (queued_pending_lb_pick_) return;
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
    gpr_log(GPR_INFO, "chand=%p lb_call=%p: adding to queued picks list",
            chand_, this);
  }
  queued_pending_lb_pick_ = true;
  queued_call_.lb_call = this;
  chand_->AddLbQueuedCall(&queued_call_, pollent_);
  // Register call combiner cancellation callback.
  lb_call_canceller_ = new LbQueuedCallCanceller(Ref());
}

void LoadBalancedCall::AsyncPickDone(grpc_error* error) {
  GRPC_CLOSURE_INIT(&pick_closure_, PickDone, this, grpc_schedule_on_exec_ctx);
  ExecCtx::Run(DEBUG_LOCATION, &pick_closure_, error);
}

void LoadBalancedCall::PickDone(void* arg, grpc_error* error) {
  auto* self = static_cast<LoadBalancedCall*>(arg);
  if (error != GRPC_ERROR_NONE) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
      gpr_log(GPR_INFO,
              "chand=%p lb_call=%p: failed to pick subchannel: error=%s",
              self->chand_, self, grpc_error_string(error));
    }
    self->PendingBatchesFail(GRPC_ERROR_REF(error), YieldCallCombiner);
    return;
  }
  self->CreateSubchannelCall();
}

const char* PickResultTypeName(
    LoadBalancingPolicy::PickResult::ResultType type) {
  switch (type) {
    case LoadBalancingPolicy::PickResult::PICK_COMPLETE:
      return "COMPLETE";
    case LoadBalancingPolicy::PickResult::PICK_QUEUE:
      return "QUEUE";
    case LoadBalancingPolicy::PickResult::PICK_FAILED:
      return "FAILED";
  }
  GPR_UNREACHABLE_CODE(return "UNKNOWN");
}

void LoadBalancedCall::PickSubchannel(void* arg, grpc_error* error) {
  auto* self = static_cast<LoadBalancedCall*>(arg);
  bool pick_complete;
  {
    MutexLock lock(self->chand_->data_plane_mu());
    pick_complete = self->PickSubchannelLocked(&error);
  }
  if (pick_complete) {
    PickDone(self, error);
    GRPC_ERROR_UNREF(error);
  }
}

bool LoadBalancedCall::PickSubchannelLocked(grpc_error** error) {
  GPR_ASSERT(connected_subchannel_ == nullptr);
  GPR_ASSERT(subchannel_call_ == nullptr);
  // Grab initial metadata.
  auto& send_initial_metadata =
      pending_batches_[0]->payload->send_initial_metadata;
  grpc_metadata_batch* initial_metadata_batch =
      send_initial_metadata.send_initial_metadata;
  const uint32_t send_initial_metadata_flags =
      send_initial_metadata.send_initial_metadata_flags;
  // Perform LB pick.
  LoadBalancingPolicy::PickArgs pick_args;
  pick_args.path = StringViewFromSlice(path_);
  LbCallState lb_call_state(this);
  pick_args.call_state = &lb_call_state;
  Metadata initial_metadata(this, initial_metadata_batch);
  pick_args.initial_metadata = &initial_metadata;
  auto result = chand_->picker()->Pick(pick_args);
  if (GRPC_TRACE_FLAG_ENABLED(grpc_client_channel_routing_trace)) {
    gpr_log(
        GPR_INFO,
        "chand=%p lb_call=%p: LB pick returned %s (subchannel=%p, error=%s)",
        chand_, this, PickResultTypeName(result.type), result.subchannel.get(),
        grpc_error_string(result.error));
  }
  switch (result.type) {
    case LoadBalancingPolicy::PickResult::PICK_FAILED: {
      // If we're shutting down, fail all RPCs.
      grpc_error* disconnect_error = chand_->disconnect_error();
      if (disconnect_error != GRPC_ERROR_NONE) {
        GRPC_ERROR_UNREF(result.error);
        MaybeRemoveCallFromLbQueuedCallsLocked();
        *error = GRPC_ERROR_REF(disconnect_error);
        return true;
      }
      // If wait_for_ready is false, then the error indicates the RPC
      // attempt's final status.
      if ((send_initial_metadata_flags &
           GRPC_INITIAL_METADATA_WAIT_FOR_READY) == 0) {
        grpc_error* new_error =
            GRPC_ERROR_CREATE_REFERENCING_FROM_STATIC_STRING(
                "Failed to pick subchannel", &result.error, 1);
        GRPC_ERROR_UNREF(result.error);
        *error = new_error;
        MaybeRemoveCallFromLbQueuedCallsLocked();
        return true;
      }
      // If wait_for_ready is true, then queue to retry when we get a new
      // picker.
      GRPC_ERROR_UNREF(result.error);
    }
    // Fallthrough
    case LoadBalancingPolicy::PickResult::PICK_QUEUE:
      MaybeAddCallToLbQueuedCallsLocked();
      return false;
    default:  // PICK_COMPLETE
      MaybeRemoveCallFromLbQueuedCallsLocked();
      // Handle drops.
      if (GPR_UNLIKELY(result.subchannel == nullptr)) {
        result.error = grpc_error_set_int(
            GRPC_ERROR_CREATE_FROM_STATIC_STRING(
                "Call dropped by load balancing policy"),
            GRPC_ERROR_INT_GRPC_STATUS, GRPC_STATUS_UNAVAILABLE);
      } else {
        // Grab a ref to the connected subchannel while we're still
        // holding the data plane mutex.
        connected_subchannel_ =
            chand_->GetConnectedSubchannelInDataPlane(result.subchannel.get());
        GPR_ASSERT(connected_subchannel_ != nullptr);
      }
      lb_recv_trailing_metadata_ready_ = result.recv_trailing_metadata_ready;
      *error = result.error;
      return true;
  }
}

}  // namespace
}  // namespace grpc_core

/*************************************************************************
 * EXPORTED SYMBOLS
 */

using grpc_core::CallData;
using grpc_core::ChannelData;

const grpc_channel_filter grpc_client_channel_filter = {
    CallData::StartTransportStreamOpBatch,
    ChannelData::StartTransportOp,
    sizeof(CallData),
    CallData::Init,
    CallData::SetPollent,
    CallData::Destroy,
    sizeof(ChannelData),
    ChannelData::Init,
    ChannelData::Destroy,
    ChannelData::GetChannelInfo,
    "client-channel",
};

grpc_connectivity_state grpc_client_channel_check_connectivity_state(
    grpc_channel_element* elem, int try_to_connect) {
  auto* chand = static_cast<ChannelData*>(elem->channel_data);
  return chand->CheckConnectivityState(try_to_connect);
}

int grpc_client_channel_num_external_connectivity_watchers(
    grpc_channel_element* elem) {
  auto* chand = static_cast<ChannelData*>(elem->channel_data);
  return chand->NumExternalConnectivityWatchers();
}

void grpc_client_channel_watch_connectivity_state(
    grpc_channel_element* elem, grpc_polling_entity pollent,
    grpc_connectivity_state* state, grpc_closure* on_complete,
    grpc_closure* watcher_timer_init) {
  auto* chand = static_cast<ChannelData*>(elem->channel_data);
  if (state == nullptr) {
    // Handle cancellation.
    GPR_ASSERT(watcher_timer_init == nullptr);
    chand->RemoveExternalConnectivityWatcher(on_complete, /*cancel=*/true);
    return;
  }
  // Handle addition.
  return chand->AddExternalConnectivityWatcher(pollent, state, on_complete,
                                               watcher_timer_init);
}

void grpc_client_channel_start_connectivity_watch(
    grpc_channel_element* elem, grpc_connectivity_state initial_state,
    grpc_core::OrphanablePtr<grpc_core::AsyncConnectivityStateWatcherInterface>
        watcher) {
  auto* chand = static_cast<ChannelData*>(elem->channel_data);
  chand->AddConnectivityWatcher(initial_state, std::move(watcher));
}

void grpc_client_channel_stop_connectivity_watch(
    grpc_channel_element* elem,
    grpc_core::AsyncConnectivityStateWatcherInterface* watcher) {
  auto* chand = static_cast<ChannelData*>(elem->channel_data);
  chand->RemoveConnectivityWatcher(watcher);
}
