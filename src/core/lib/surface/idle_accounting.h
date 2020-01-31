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
#include <grpc/support/string_util.h>
#include <grpc/impl/codegen/log.h>

#include "src/core/lib/iomgr/exec_ctx.h"

namespace grpc_core {

enum IdleAccountMetric {
  SEND_WALL_TIME,
  RECV_WALL_TIME,
  WAITING_FOR_PICK,
  WAITING_FOR_CONCURRENT_STREAM,
  WAITING_FOR_TRANSPORT_FC,
  WAITING_FOR_STREAM_FC,
  WAITING_FOR_WRITABLE,
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
      gpr_log(GPR_DEBUG, "idle_account:%p metric %s start_ms: %ld", this, MetricToName(reason), metric_totals_[reason].cur_wall_time_start_);
    }
    for (int i = IdleAccountMetric::WAITING_FOR_PICK; i <= IdleAccountMetric::WAITING_FOR_CLIENT_AUTH; i++) {
      if (metric_totals_[i].cur_active_ > 1) {
        gpr_log(GPR_ERROR, "idle_account:%p metric %d max active is 1. have:%d", this, i, metric_totals_[i].cur_active_);
        abort();
      }
    }
  }

  void stop(IdleAccountMetric reason) {
    if (metric_totals_[reason].cur_active_ == 0) {
      gpr_log(GPR_ERROR, "idle_account:%p wall time %s not active", this, MetricToName(reason));
      abort();
    }
    if (--metric_totals_[reason].cur_active_ == 0) {
      grpc_millis now = grpc_core::ExecCtx::Get()->Now();
      metric_totals_[reason].total_ms += (now - metric_totals_[reason].cur_wall_time_start_);
      gpr_log(GPR_DEBUG, "idle_account:%p metric %s stopped at %ld total_ms: %ld", this, MetricToName(reason), now, metric_totals_[reason].total_ms);
    }
  }

  grpc_millis get_total_send_wall_ms() {
    return metric_totals_[IdleAccountMetric::SEND_WALL_TIME].total_ms;
  }

  grpc_millis get_total_recv_wall_ms() {
    return metric_totals_[IdleAccountMetric::RECV_WALL_TIME].total_ms;
  }

  grpc_millis get_total_send_idle_ms() {
    return metric_totals_[IdleAccountMetric::WAITING_FOR_PICK].total_ms +
        metric_totals_[IdleAccountMetric::WAITING_FOR_CONCURRENT_STREAM].total_ms +
        metric_totals_[IdleAccountMetric::WAITING_FOR_TRANSPORT_FC].total_ms +
        metric_totals_[IdleAccountMetric::WAITING_FOR_STREAM_FC].total_ms +
        metric_totals_[IdleAccountMetric::WAITING_FOR_WRITABLE].total_ms;
  }

  double get_total_recv_idle_ms() {
    return metric_totals_[IdleAccountMetric::WAITING_FOR_READABLE].total_ms;
  }

  std::string as_string() {
    std::string out = "";
    for (int i = 0; i < IdleAccountMetric::NUM_METRICS; i++) {
      out += " " + std::string(MetricToName(static_cast<IdleAccountMetric>(i))) + ":" + std::to_string(metric_totals_[i].total_ms);
    }
    return out;
  }

 private:
  const char* name_ = "idle_account";

  const char* MetricToName(IdleAccountMetric metric) {
    switch (metric) {
      case SEND_WALL_TIME:
        return "SEND_WALL_TIME";
      case RECV_WALL_TIME:
        return "RECV_WALL_TIME";
      case WAITING_FOR_PICK:
        return "WAITING_FOR_PICK";
      case WAITING_FOR_CONCURRENT_STREAM:
        return "WAITING_FOR_CONCURRENT_STREAM";
      case WAITING_FOR_TRANSPORT_FC:
        return "WAITING_FOR_TRANSPORT_FC";
      case WAITING_FOR_STREAM_FC:
        return "WAITING_FOR_STREAM_FC";
      case WAITING_FOR_CLIENT_AUTH:
        return "WAITING_FOR_CLIENT_AUTH";
      case WAITING_FOR_WRITABLE:
        return "WAITING_FOR_WRITABLE";
      case WAITING_FOR_READABLE:
        return "WAITING_FOR_READABLE";
      default:
        abort();
    }
  }

  struct MetricTotal {
    int cur_active_ = 0;
    grpc_millis cur_wall_time_start_ = 0;
    grpc_millis total_ms = 0;
  };

  std::vector<MetricTotal> metric_totals_;
};

} // namespace grpc_core

#endif /* GRPC_CORE_LIB_SURFACE_IDLE_ACCOUNTING_H */
