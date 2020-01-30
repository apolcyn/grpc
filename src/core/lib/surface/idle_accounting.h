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

#include <string>
#include <vector>

#include <grpc/grpc.h>
#include <grpc/support/time.h>
#include <grpc/impl/codegen/log.h>

#include "src/core/lib/iomgr/exec_ctx.h"

namespace grpc_core {

enum IdleAccountMetric {
  SEND_WALL_TIME,
  RECV_WALL_TIME,
  WAITING_FOR_PICK,
  WAITING_FOR_TRANSPORT_FC,
  WAITING_FOR_STREAM_FC,
  WAITING_FOR_WRITEABLE,
  WAITING_FOR_CLIENT_AUTH,
  WAITING_FOR_READABLE,
  NUM_METRICS,
};

class IdleAccount {
 public:
  explicit IdleAccount() {
    metric_totals_.resize(IdleAccountMetric::NUM_METRICS);
  }
  ~IdleAccount() {}

  void start(IdleAccountMetric reason) {
    if (metric_totals_[reason].cur_active_++ == 0) {
      metric_totals_[reason].cur_wall_time_start_ = grpc_core::ExecCtx::Get()->Now();
    }
    for (int i = IdleAccountMetric::WAITING_FOR_PICK; i <= IdleAccountMetric::WAITING_FOR_CLIENT_AUTH; i++) {
      if (metric_totals_[i].cur_active_ > 1) {
        gpr_log(GPR_ERROR, "metric %d max active is 1. have:%d", i, metric_totals_[i].cur_active_);
        abort();
      }
    }
  }

  void stop(IdleAccountMetric reason) {
    if (metric_totals_[reason].cur_active_ == 0) {
      gpr_log(GPR_ERROR, "idle_account:%p wall time %d not active", reason);
      abort();
    }
    if (--metric_totals_[reason].cur_active_ == 0) {
      metric_totals_[reason].total_ms += grpc_core::ExecCtx::Get()->Now() - metric_totals_[reason].cur_wall_time_start_;
    }
  }

  double get_total_send_wall_ms() {
    return metric_totals_[IdleAccountMetric::SEND_WALL_TIME].total_ms;
  }

  double get_total_recv_wall_ms() {
    return metric_totals_[IdleAccountMetric::RECV_WALL_TIME].total_ms;
  }

  double get_total_send_idle_ms() {
    return metric_totals_[IdleAccountMetric::WAITING_FOR_PICK].total_ms +
        metric_totals_[IdleAccountMetric::WAITING_FOR_TRANSPORT_FC].total_ms +
        metric_totals_[IdleAccountMetric::WAITING_FOR_STREAM_FC].total_ms +
        metric_totals_[IdleAccountMetric::WAITING_FOR_WRITEABLE].total_ms;
  }

  double get_total_recv_idle_ms() {
    return metric_totals_[IdleAccountMetric::WAITING_FOR_READABLE].total_ms;
  }

  std::string as_string() {
    char* out;
    gpr_asprintf(&out, "idle_account:%p send_wall_time:%lf recv_wall_time:%lf waiting_for_pick:%lf waiting_for_transport_fc:%lf waiting_for_stream_fc:%lf waiting_for_writeable:%lf waiting_for_readable:%lf waiting_for_client_auth:%lf",
                this,
                metric_totals_[SEND_WALL_TIME],
                metric_totals_[RECV_WALL_TIME],
                metric_totals_[WAITING_FOR_PICK],
                metric_totals_[WAITING_FOR_TRANSPORT_FC],
                metric_totals_[WAITING_FOR_STREAM_FC],
                metric_totals_[WAITING_FOR_WRITEABLE],
                metric_totals_[WAITING_FOR_READABLE],
                metric_totals_[WAITING_FOR_CLIENT_AUTH]);
    auto ret = std::string(out);
    gpr_free(out);
    return ret;
  }

 private:
  struct MetricTotal {
    int cur_active_ = 0;
    grpc_millis cur_wall_time_start_ = 0;
    double total_ms = 0;
  };

  std::vector<MetricTotal> metric_totals_;
};

} // namespace grpc_core

#endif /* GRPC_CORE_LIB_SURFACE_IDLE_ACCOUNTING_H */
