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

#ifndef GRPC_CORE_LIB_SURFACE_IDLE_ACCOUNTING_H
#define GRPC_CORE_LIB_SURFACE_IDLE_ACCOUNTING_H

#include <grpc/support/port_platform.h>

#include "src/core/lib/channel/channel_stack.h"
#include "src/core/lib/channel/context.h"
#include "src/core/lib/gprpp/arena.h"
#include "src/core/lib/surface/api_trace.h"

#include <grpc/grpc.h>
#include <grpc/support/time.h>

namespace grpc_core {

enum IdleReason {
  WALL_TIME,
  WAITING_FOR_PICK,
  WAITING_FOR_TRANSPORT_FC,
  WAITING_FOR_STREAM_FC,
  WAITING_FOR_WRITEABLE,
  WAITING_FOR_READABLE,
  WAITING_FOR_CLIENT_AUTH,
  NUM_IDLE_REASONS,
};

class IdleAccount {
 public:
  explicit IdleAccount() {}
  ~IdleAccount() {}

  void start(IdleReason reason) {
    if (idle_totals_[reason].wall_time_active_) {
      gpr_log(GPR_ERROR, "idle_account:%s wall time %d already active", name_, reason);
      abort();
    }
    idle_totals_[reason].wall_time_active_ = true;
    idle_totals_[reason] = gpr_now(GPR_CLOCK_MONOTONIC);
  }
  void stop(IdleReason reason) {
    if (idle_totals_[reason].wall_time_active_) {
      gpr_log(GPR_ERROR, "idle_account:%s wall time %d not active", name_, reason);
      abort();
    }
    idle_totals_[reason].wall_time_active_ = false;
    idle_totals_[reason].total_us += gpr_timespec_to_micros(gpr_time_sub(gpr_now(GPR_CLOCK_MONOTONIC), idle_totals_[reason].cur_wall_time_start_));
  }
  double get_total_wall_us() {
    return idle_totals_[IdleReason.WALL_TIME];
  }
  double get_total_idle_us() {
    double out = 0;
    for (int i = 0; i < IdleReason.NUM_IDLE_REASONS; i++) {
      if (i != IdleReason.WALL_TIME) {
        out += idle_totals_[i];
      }
    }
    return out;
  }
  std::string as_string() {
    char* out;
    gpr_asprint(&out, "idle_account:%s wall_time:%lf waiting_for_pick:%lf waiting_for_transport_fc:%lf waiting_for_stream_fc:%lf waiting_for_writeable:%lf waiting_for_readable:%lf waiting_for_client_auth:%lf",
                idle_totals_[WALL_TIME],
                idle_totals_[WAITING_FOR_PICK],
                idle_totals_[WAITING_FOR_TRANSPORT_FC],
                idle_totals_[WAITING_FOR_STREAM_FC],
                idle_totals_[WAITING_FOR_WRITEABLE],
                idle_totals_[WAITING_FOR_READABLE],
                idle_totals_[WAITING_FOR_CLIENT_AUTH]);
    auto ret = std::string(out);
    gpr_free(out);
    return ret;
  }

 private:
  const char* name_;

  struct IdleTotal {
    bool wall_time_active_;
    gpr_timespec cur_wall_time_start_;
    double total_us;
  };

  IdleTotal[IdleReason.NUM_IDLE_REASONS] idle_totals_;
};

} // namespace grpc_core

#endif /* GRPC_CORE_LIB_SURFACE_IDLE_ACCOUNTING_H */
