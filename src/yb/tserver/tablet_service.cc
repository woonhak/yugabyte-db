// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#include "yb/tserver/tablet_service.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "yb/client/forward_rpc.h"
#include "yb/client/transaction.h"
#include "yb/client/transaction_pool.h"

#include "yb/common/ql_rowblock.h"
#include "yb/common/ql_value.h"
#include "yb/common/row_mark.h"
#include "yb/common/schema.h"
#include "yb/common/wire_protocol.h"
#include "yb/consensus/leader_lease.h"
#include "yb/consensus/consensus.pb.h"
#include "yb/consensus/raft_consensus.h"

#include "yb/docdb/cql_operation.h"
#include "yb/docdb/pgsql_operation.h"

#include "yb/gutil/bind.h"
#include "yb/gutil/casts.h"
#include "yb/gutil/stl_util.h"
#include "yb/gutil/stringprintf.h"
#include "yb/gutil/strings/escaping.h"

#include "yb/rpc/thread_pool.h"

#include "yb/server/hybrid_clock.h"

#include "yb/tablet/abstract_tablet.h"
#include "yb/tablet/metadata.pb.h"
#include "yb/tablet/operations/change_metadata_operation.h"
#include "yb/tablet/operations/split_operation.h"
#include "yb/tablet/operations/truncate_operation.h"
#include "yb/tablet/operations/update_txn_operation.h"
#include "yb/tablet/operations/write_operation.h"
#include "yb/tablet/read_result.h"
#include "yb/tablet/tablet.h"
#include "yb/tablet/tablet_bootstrap_if.h"
#include "yb/tablet/tablet_metadata.h"
#include "yb/tablet/tablet_metrics.h"
#include "yb/tablet/transaction_participant.h"
#include "yb/tablet/write_query.h"

#include "yb/tserver/read_query.h"
#include "yb/tserver/service_util.h"
#include "yb/tserver/tablet_server.h"
#include "yb/tserver/ts_tablet_manager.h"
#include "yb/tserver/tserver_error.h"

#include "yb/util/crc.h"
#include "yb/util/debug-util.h"
#include "yb/util/debug/long_operation_tracker.h"
#include "yb/util/debug/trace_event.h"
#include "yb/util/faststring.h"
#include "yb/util/flag_tags.h"
#include "yb/util/format.h"
#include "yb/util/logging.h"
#include "yb/util/math_util.h"
#include "yb/util/mem_tracker.h"
#include "yb/util/metrics.h"
#include "yb/util/monotime.h"
#include "yb/util/random_util.h"
#include "yb/util/scope_exit.h"
#include "yb/util/size_literals.h"
#include "yb/util/status.h"
#include "yb/util/status_callback.h"
#include "yb/util/status_format.h"
#include "yb/util/status_log.h"
#include "yb/util/string_util.h"
#include "yb/util/trace.h"

#include "yb/yql/pgwrapper/ysql_upgrade.h"

using namespace std::literals;  // NOLINT

DEFINE_int32(scanner_default_batch_size_bytes, 64 * 1024,
             "The default size for batches of scan results");
TAG_FLAG(scanner_default_batch_size_bytes, advanced);
TAG_FLAG(scanner_default_batch_size_bytes, runtime);

DEFINE_int32(scanner_max_batch_size_bytes, 8 * 1024 * 1024,
             "The maximum batch size that a client may request for "
             "scan results.");
TAG_FLAG(scanner_max_batch_size_bytes, advanced);
TAG_FLAG(scanner_max_batch_size_bytes, runtime);

DEFINE_int32(scanner_batch_size_rows, 100,
             "The number of rows to batch for servicing scan requests.");
TAG_FLAG(scanner_batch_size_rows, advanced);
TAG_FLAG(scanner_batch_size_rows, runtime);

// Fault injection flags.
DEFINE_test_flag(int32, scanner_inject_latency_on_each_batch_ms, 0,
                 "If set, the scanner will pause the specified number of milliesconds "
                 "before reading each batch of data on the tablet server.");

DEFINE_int32(max_wait_for_safe_time_ms, 5000,
             "Maximum time in milliseconds to wait for the safe time to advance when trying to "
             "scan at the given hybrid_time.");

DEFINE_int32(num_concurrent_backfills_allowed, -1,
             "Maximum number of concurrent backfill jobs that is allowed to run.");

DEFINE_test_flag(bool, tserver_noop_read_write, false, "Respond NOOP to read/write.");

DEFINE_uint64(index_backfill_upperbound_for_user_enforced_txn_duration_ms, 65000,
              "For Non-Txn tables, it is impossible to know at the tservers "
              "whether or not an 'old transaction' is still active. To avoid "
              "having such old transactions, we assume a bound on the duration "
              "of such transactions (during the backfill process) and wait "
              "it out. This flag denotes a conservative upper bound on the "
              "duration of such user enforced transactions.");
TAG_FLAG(index_backfill_upperbound_for_user_enforced_txn_duration_ms, evolving);
TAG_FLAG(index_backfill_upperbound_for_user_enforced_txn_duration_ms, runtime);

DEFINE_int32(index_backfill_additional_delay_before_backfilling_ms, 0,
             "Operations that are received by the tserver, and have decided how "
             "the indexes need to be updated (based on the IndexPermission), will "
             "not be added to the list of current transactions until they are "
             "replicated/applied. This delay allows for the GetSafeTime method "
             "to wait for such operations to be replicated/applied. Ideally, this "
             "value should be set to be something larger than the raft-heartbeat-interval "
             "but can be as high as the client_rpc_timeout if we want to be more conservative.");
TAG_FLAG(index_backfill_additional_delay_before_backfilling_ms, evolving);
TAG_FLAG(index_backfill_additional_delay_before_backfilling_ms, runtime);

DEFINE_int32(index_backfill_wait_for_old_txns_ms, 0,
             "Index backfill needs to wait for transactions that started before the "
             "WRITE_AND_DELETE phase to commit or abort before choosing a time for "
             "backfilling the index. This is the max time that the GetSafeTime call will "
             "wait for, before it resorts to attempt aborting old transactions. This is "
             "necessary to guard against the pathological active transaction that never "
             "commits from blocking the index backfill forever.");
TAG_FLAG(index_backfill_wait_for_old_txns_ms, evolving);
TAG_FLAG(index_backfill_wait_for_old_txns_ms, runtime);

DEFINE_test_flag(double, respond_write_failed_probability, 0.0,
                 "Probability to respond that write request is failed");

DEFINE_test_flag(bool, rpc_delete_tablet_fail, false, "Should delete tablet RPC fail.");

DECLARE_bool(disable_alter_vs_write_mutual_exclusion);
DECLARE_uint64(max_clock_skew_usec);
DECLARE_uint64(transaction_min_running_check_interval_ms);
DECLARE_int64(transaction_rpc_timeout_ms);

DEFINE_test_flag(int32, txn_status_table_tablet_creation_delay_ms, 0,
                 "Extra delay to slowdown creation of transaction status table tablet.");

DEFINE_test_flag(int32, leader_stepdown_delay_ms, 0,
                 "Amount of time to delay before starting a leader stepdown change.");

DEFINE_test_flag(int32, alter_schema_delay_ms, 0, "Delay before processing AlterSchema.");

DEFINE_test_flag(bool, disable_post_split_tablet_rbs_check, false,
                 "If true, bypass any checks made to reject remote boostrap requests for post "
                 "split tablets whose parent tablets are still present.");

DEFINE_test_flag(double, fail_tablet_split_probability, 0.0,
                 "Probability of failing in TabletServiceAdminImpl::SplitTablet.");

DEFINE_test_flag(bool, pause_tserver_get_split_key, false,
                 "Pause before processing a GetSplitKey request.");

DECLARE_int32(heartbeat_interval_ms);

DECLARE_int32(ysql_transaction_abort_timeout_ms);

DEFINE_test_flag(bool, fail_alter_schema_after_abort_transactions, false,
                 "If true, setup an error status in AlterSchema and respond success to rpc call. "
                 "This failure should not cause the TServer to crash but "
                 "instead return an error message on the YSQL connection.");

double TEST_delay_create_transaction_probability = 0;

namespace yb {
namespace tserver {

using client::internal::ForwardReadRpc;
using client::internal::ForwardWriteRpc;
using consensus::ChangeConfigRequestPB;
using consensus::ChangeConfigResponsePB;
using consensus::Consensus;
using consensus::CONSENSUS_CONFIG_ACTIVE;
using consensus::CONSENSUS_CONFIG_COMMITTED;
using consensus::ConsensusConfigType;
using consensus::ConsensusRequestPB;
using consensus::ConsensusResponsePB;
using consensus::GetLastOpIdRequestPB;
using consensus::GetNodeInstanceRequestPB;
using consensus::GetNodeInstanceResponsePB;
using consensus::LeaderLeaseStatus;
using consensus::LeaderStepDownRequestPB;
using consensus::LeaderStepDownResponsePB;
using consensus::RaftPeerPB;
using consensus::RunLeaderElectionRequestPB;
using consensus::RunLeaderElectionResponsePB;
using consensus::StartRemoteBootstrapRequestPB;
using consensus::StartRemoteBootstrapResponsePB;
using consensus::UnsafeChangeConfigRequestPB;
using consensus::UnsafeChangeConfigResponsePB;
using consensus::VoteRequestPB;
using consensus::VoteResponsePB;

using std::unique_ptr;
using google::protobuf::RepeatedPtrField;
using rpc::RpcContext;
using std::shared_ptr;
using std::vector;
using std::string;
using strings::Substitute;
using tablet::ChangeMetadataOperation;
using tablet::Tablet;
using tablet::TabletPeer;
using tablet::TabletPeerPtr;
using tablet::TabletStatusPB;
using tablet::TruncateOperation;
using tablet::OperationCompletionCallback;
using tablet::WriteOperation;

namespace {

Result<std::shared_ptr<consensus::RaftConsensus>> GetConsensus(const TabletPeerPtr& tablet_peer) {
  auto result = tablet_peer->shared_raft_consensus();
  if (!result) {
    Status s = STATUS(ServiceUnavailable, "Consensus unavailable. Tablet not running");
    return s.CloneAndAddErrorCode(TabletServerError(TabletServerErrorPB::TABLET_NOT_RUNNING));
  }
  return result;
}

template<class RespClass>
std::shared_ptr<consensus::RaftConsensus> GetConsensusOrRespond(const TabletPeerPtr& tablet_peer,
                                                                RespClass* resp,
                                                                rpc::RpcContext* context) {
  auto result = GetConsensus(tablet_peer);
  if (!result.ok()) {
    SetupErrorAndRespond(resp->mutable_error(), result.status(), context);
  }
  return result.get();
}

template<class RespClass>
bool GetConsensusOrRespond(const TabletPeerPtr& tablet_peer,
                           RespClass* resp,
                           rpc::RpcContext* context,
                           shared_ptr<Consensus>* consensus) {
  auto result = GetConsensus(tablet_peer);
  if (!result.ok()) {
    SetupErrorAndRespond(resp->mutable_error(), result.status(), context);
    return false;
  }
  return (*consensus = result.get()) != nullptr;
}

} // namespace

template<class Resp>
bool TabletServiceImpl::CheckWriteThrottlingOrRespond(
    double score, tablet::TabletPeer* tablet_peer, Resp* resp, rpc::RpcContext* context) {
  // Check for memory pressure; don't bother doing any additional work if we've
  // exceeded the limit.
  auto status = CheckWriteThrottling(score, tablet_peer);
  if (!status.ok()) {
    SetupErrorAndRespond(resp->mutable_error(), status, context);
    return false;
  }

  return true;
}

typedef ListTabletsResponsePB::StatusAndSchemaPB StatusAndSchemaPB;

class WriteQueryCompletionCallback {
 public:
  WriteQueryCompletionCallback(
      tablet::TabletPeerPtr tablet_peer,
      std::shared_ptr<rpc::RpcContext> context,
      WriteResponsePB* response,
      tablet::WriteQuery* query,
      const server::ClockPtr& clock,
      bool trace = false)
      : tablet_peer_(std::move(tablet_peer)),
        context_(std::move(context)),
        response_(response),
        query_(query),
        clock_(clock),
        include_trace_(trace),
        trace_(include_trace_ ? Trace::CurrentTrace() : nullptr) {}

  void operator()(Status status) const {
    VLOG(1) << __PRETTY_FUNCTION__ << " completing with status " << status;
    // When we don't need to return any data, we could return success on duplicate request.
    if (status.IsAlreadyPresent() &&
        query_->ql_write_ops()->empty() &&
        query_->pgsql_write_ops()->empty() &&
        query_->client_request()->redis_write_batch().empty()) {
      status = Status::OK();
    }

    TRACE("Write completing with status $0", yb::ToString(status));

    if (!status.ok()) {
      LOG(INFO) << tablet_peer_->LogPrefix() << "Write failed: " << status;
      if (include_trace_ && trace_) {
        response_->set_trace_buffer(trace_->DumpToString(true));
      }
      SetupErrorAndRespond(get_error(), status, context_.get());
      return;
    }

    // Retrieve the rowblocks returned from the QL write operations and return them as RPC
    // sidecars. Populate the row schema also.
    faststring rows_data;
    for (const auto& ql_write_op : *query_->ql_write_ops()) {
      const auto& ql_write_req = ql_write_op->request();
      auto* ql_write_resp = ql_write_op->response();
      const QLRowBlock* rowblock = ql_write_op->rowblock();
      SchemaToColumnPBs(rowblock->schema(), ql_write_resp->mutable_column_schemas());
      rows_data.clear();
      rowblock->Serialize(ql_write_req.client(), &rows_data);
      ql_write_resp->set_rows_data_sidecar(
          narrow_cast<int32_t>(context_->AddRpcSidecar(rows_data)));
    }

    if (!query_->pgsql_write_ops()->empty()) {
      // Retrieve the resultset returned from the PGSQL write operations and return them as RPC
      // sidecars.

      size_t sidecars_size = 0;
      for (const auto& pgsql_write_op : *query_->pgsql_write_ops()) {
        sidecars_size += pgsql_write_op->result_buffer().size();
      }

      if (sidecars_size != 0) {
        context_->ReserveSidecarSpace(sidecars_size);
        for (const auto& pgsql_write_op : *query_->pgsql_write_ops()) {
          auto* pgsql_write_resp = pgsql_write_op->response();
          const faststring& result_buffer = pgsql_write_op->result_buffer();
          if (!result_buffer.empty()) {
            pgsql_write_resp->set_rows_data_sidecar(
                narrow_cast<int32_t>(context_->AddRpcSidecar(result_buffer)));
          }
        }
      }
    }

    if (include_trace_ && trace_) {
      response_->set_trace_buffer(trace_->DumpToString(true));
    }
    response_->set_propagated_hybrid_time(clock_->Now().ToUint64());
    context_->RespondSuccess();
    VLOG(1) << __PRETTY_FUNCTION__ << " RespondedSuccess";
  }

 private:
  TabletServerErrorPB* get_error() const {
    return response_->mutable_error();
  }

  tablet::TabletPeerPtr tablet_peer_;
  const std::shared_ptr<rpc::RpcContext> context_;
  WriteResponsePB* const response_;
  tablet::WriteQuery* const query_;
  server::ClockPtr clock_;
  const bool include_trace_;
  scoped_refptr<Trace> trace_;
};

// Checksums the scan result.
class ScanResultChecksummer {
 public:
  ScanResultChecksummer() {}

  void HandleRow(const Schema& schema, const QLTableRow& row) {
    QLValue value;
    buffer_.clear();
    for (uint32_t col_index = 0; col_index != schema.num_columns(); ++col_index) {
      auto status = row.GetValue(schema.column_id(col_index), &value);
      if (!status.ok()) {
        LOG(WARNING) << "Column " << schema.column_id(col_index)
                     << " not found in " << row.ToString();
        continue;
      }
      buffer_.append(pointer_cast<const char*>(&col_index), sizeof(col_index));
      if (schema.column(col_index).is_nullable()) {
        uint8_t defined = value.IsNull() ? 0 : 1;
        buffer_.append(pointer_cast<const char*>(&defined), sizeof(defined));
      }
      if (!value.IsNull()) {
        value.value().AppendToString(&buffer_);
      }
    }
    crc_->Compute(buffer_.c_str(), buffer_.size(), &agg_checksum_, nullptr);
  }

  // Accessors for initializing / setting the checksum.
  uint64_t agg_checksum() const { return agg_checksum_; }

 private:
  crc::Crc* const crc_ = crc::GetCrc32cInstance();
  uint64_t agg_checksum_ = 0;
  std::string buffer_;
};

Result<std::shared_ptr<tablet::AbstractTablet>> TabletServiceImpl::GetTabletForRead(
  const TabletId& tablet_id, tablet::TabletPeerPtr tablet_peer,
  YBConsistencyLevel consistency_level, tserver::AllowSplitTablet allow_split_tablet) {
  return GetTablet(server_->tablet_peer_lookup(), tablet_id, std::move(tablet_peer),
                   consistency_level, allow_split_tablet);
}

TabletServiceImpl::TabletServiceImpl(TabletServerIf* server)
    : TabletServerServiceIf(server->MetricEnt()),
      server_(server) {
}

TabletServiceAdminImpl::TabletServiceAdminImpl(TabletServer* server)
    : TabletServerAdminServiceIf(server->MetricEnt()), server_(server) {}

void TabletServiceAdminImpl::BackfillDone(
    const tablet::ChangeMetadataRequestPB* req, ChangeMetadataResponsePB* resp,
    rpc::RpcContext context) {
  if (!CheckUuidMatchOrRespond(server_->tablet_manager(), "BackfillDone", req, resp, &context)) {
    return;
  }
  DVLOG(3) << "Received BackfillDone RPC: " << req->DebugString();

  server::UpdateClock(*req, server_->Clock());

  // For now, we shall only allow this RPC on the leader.
  auto tablet =
      LookupLeaderTabletOrRespond(server_->tablet_peer_lookup(), req->tablet_id(), resp, &context);
  if (!tablet) {
    return;
  }

  auto operation = std::make_unique<ChangeMetadataOperation>(
      tablet.peer->tablet(), tablet.peer->log(), req);

  operation->set_completion_callback(
      MakeRpcOperationCompletionCallback(std::move(context), resp, server_->Clock()));

  // Submit the alter schema op. The RPC will be responded to asynchronously.
  tablet.peer->Submit(std::move(operation), tablet.leader_term);
}

void TabletServiceAdminImpl::GetSafeTime(
    const GetSafeTimeRequestPB* req, GetSafeTimeResponsePB* resp, rpc::RpcContext context) {
  if (!CheckUuidMatchOrRespond(server_->tablet_manager(), "GetSafeTime", req, resp, &context)) {
    return;
  }
  DVLOG(3) << "Received GetSafeTime RPC: " << req->DebugString();

  server::UpdateClock(*req, server_->Clock());

  // For now, we shall only allow this RPC on the leader.
  auto tablet =
      LookupLeaderTabletOrRespond(server_->tablet_peer_lookup(), req->tablet_id(), resp, &context);
  if (!tablet) {
    return;
  }
  const CoarseTimePoint& deadline = context.GetClientDeadline();
  HybridTime min_hybrid_time(HybridTime::kMin);
  if (req->has_min_hybrid_time_for_backfill()) {
    min_hybrid_time = HybridTime(req->min_hybrid_time_for_backfill());
    // For Transactional tables, wait until there are no pending transactions that started
    // prior to min_hybrid_time. These may not have updated the index correctly, if they
    // happen to commit after the backfill scan, it is possible that they may miss updating
    // the index because the some operations may have taken place prior to min_hybrid_time.
    //
    // For Non-Txn tables, it is impossible to know at the tservers whether or not an "old
    // transaction" is still active. To avoid having such old transactions, we assume a
    // bound on the length of such transactions (during the backfill process) and wait it
    // out.
    if (!tablet.peer->tablet()->transaction_participant()) {
      min_hybrid_time = min_hybrid_time.AddMilliseconds(
          FLAGS_index_backfill_upperbound_for_user_enforced_txn_duration_ms);
      VLOG(2) << "GetSafeTime called on a user enforced transaction tablet "
              << tablet.peer->tablet_id() << " will wait until "
              << min_hybrid_time << " is safe.";
    } else {
      // Add some extra delay to wait for operations being replicated to be
      // applied.
      SleepFor(MonoDelta::FromMilliseconds(
          FLAGS_index_backfill_additional_delay_before_backfilling_ms));

      auto txn_particpant = tablet.peer->tablet()->transaction_participant();
      auto wait_until = CoarseMonoClock::Now() + FLAGS_index_backfill_wait_for_old_txns_ms * 1ms;
      HybridTime min_running_ht;
      for (;;) {
        min_running_ht = txn_particpant->MinRunningHybridTime();
        if ((min_running_ht && min_running_ht >= min_hybrid_time) ||
            CoarseMonoClock::Now() > wait_until) {
          break;
        }
        VLOG(2) << "MinRunningHybridTime is " << min_running_ht
                << " need to wait for " << min_hybrid_time;
        SleepFor(MonoDelta::FromMilliseconds(FLAGS_transaction_min_running_check_interval_ms));
      }

      VLOG(2) << "Finally MinRunningHybridTime is " << min_running_ht;
      if (min_running_ht < min_hybrid_time) {
        VLOG(2) << "Aborting Txns that started prior to " << min_hybrid_time;
        auto s = txn_particpant->StopActiveTxnsPriorTo(min_hybrid_time, deadline);
        if (!s.ok()) {
          SetupErrorAndRespond(resp->mutable_error(), s, &context);
          return;
        }
      }
    }
  }

  auto safe_time = tablet.peer->tablet()->SafeTime(
      tablet::RequireLease::kTrue, min_hybrid_time, deadline);
  if (!safe_time.ok()) {
    SetupErrorAndRespond(resp->mutable_error(), safe_time.status(), &context);
    return;
  }

  resp->set_safe_time(safe_time->ToUint64());
  resp->set_propagated_hybrid_time(server_->Clock()->Now().ToUint64());
  VLOG(1) << "Tablet " << tablet.peer->tablet_id()
          << " returning safe time " << yb::ToString(safe_time);

  context.RespondSuccess();
}

void TabletServiceAdminImpl::BackfillIndex(
    const BackfillIndexRequestPB* req, BackfillIndexResponsePB* resp, rpc::RpcContext context) {
  if (!CheckUuidMatchOrRespond(server_->tablet_manager(), "BackfillIndex", req, resp, &context)) {
    return;
  }
  DVLOG(3) << "Received BackfillIndex RPC: " << req->DebugString();

  server::UpdateClock(*req, server_->Clock());

  // For now, we shall only allow this RPC on the leader.
  auto tablet =
      LookupLeaderTabletOrRespond(server_->tablet_peer_lookup(), req->tablet_id(), resp, &context);
  if (!tablet) {
    return;
  }

  if (req->indexes().empty()) {
    SetupErrorAndRespond(
        resp->mutable_error(),
        STATUS(InvalidArgument, "No indexes given in request"),
        TabletServerErrorPB::OPERATION_NOT_SUPPORTED,
        &context);
    return;
  }

  const CoarseTimePoint &deadline = context.GetClientDeadline();
  const auto coarse_start = CoarseMonoClock::Now();
  {
    std::unique_lock<std::mutex> l(backfill_lock_);
    while (num_tablets_backfilling_ >= FLAGS_num_concurrent_backfills_allowed) {
      if (backfill_cond_.wait_until(l, deadline) == std::cv_status::timeout) {
        SetupErrorAndRespond(
            resp->mutable_error(),
            STATUS_FORMAT(ServiceUnavailable,
                          "Already running $0 backfill requests",
                          num_tablets_backfilling_),
            &context);
        return;
      }
    }
    num_tablets_backfilling_++;
  }
  auto se = ScopeExit([this] {
    std::unique_lock<std::mutex> l(this->backfill_lock_);
    this->num_tablets_backfilling_--;
    this->backfill_cond_.notify_all();
  });

  // Wait for SafeTime to get past read_at;
  const HybridTime read_at(req->read_at_hybrid_time());
  DVLOG(1) << "Waiting for safe time to be past " << read_at;
  const auto safe_time =
      tablet.peer->tablet()->SafeTime(tablet::RequireLease::kFalse, read_at, deadline);
  DVLOG(1) << "Got safe time " << safe_time.ToString();
  if (!safe_time.ok()) {
    LOG(ERROR) << "Could not get a good enough safe time " << safe_time.ToString();
    SetupErrorAndRespond(resp->mutable_error(), safe_time.status(), &context);
    return;
  }

  // Don't work on the request if we have had to wait more than 50%
  // of the time allocated to us for the RPC.
  // Backfill is a costly operation, we do not want to start working
  // on it if we expect the client (master) to time out the RPC and
  // force us to redo the work.
  const auto coarse_now = CoarseMonoClock::Now();
  if (deadline - coarse_now < coarse_now - coarse_start) {
    SetupErrorAndRespond(
        resp->mutable_error(),
        STATUS_FORMAT(
            ServiceUnavailable, "Not enough time left $0", deadline - coarse_now),
        &context);
    return;
  }

  bool all_at_backfill = true;
  bool all_past_backfill = true;
  bool is_pg_table = tablet.peer->tablet()->table_type() == TableType::PGSQL_TABLE_TYPE;
  const shared_ptr<IndexMap> index_map = tablet.peer->tablet_metadata()->index_map(
    req->indexed_table_id());
  std::vector<IndexInfo> indexes_to_backfill;
  std::vector<TableId> index_ids;
  for (const auto& idx : req->indexes()) {
    auto result = index_map->FindIndex(idx.table_id());
    if (result) {
      const IndexInfo* index_info = *result;
      indexes_to_backfill.push_back(*index_info);
      index_ids.push_back(index_info->table_id());

      IndexInfoPB idx_info_pb;
      index_info->ToPB(&idx_info_pb);
      if (!is_pg_table) {
        all_at_backfill &=
            idx_info_pb.index_permissions() == IndexPermissions::INDEX_PERM_DO_BACKFILL;
      } else {
        // YSQL tables don't use all the docdb permissions, so use this approximation.
        // TODO(jason): change this back to being like YCQL once we bring the docdb permission
        // DO_BACKFILL back (issue #6218).
        all_at_backfill &=
            idx_info_pb.index_permissions() == IndexPermissions::INDEX_PERM_WRITE_AND_DELETE;
      }
      all_past_backfill &=
          idx_info_pb.index_permissions() > IndexPermissions::INDEX_PERM_DO_BACKFILL;
    } else {
      LOG(WARNING) << "index " << idx.table_id() << " not found in tablet metadata";
      all_at_backfill = false;
      all_past_backfill = false;
    }
  }

  if (!all_at_backfill) {
    if (all_past_backfill) {
      // Change this to see if for all indexes: IndexPermission > DO_BACKFILL.
      LOG(WARNING) << "Received BackfillIndex RPC: " << req->DebugString()
                   << " after all indexes have moved past DO_BACKFILL. IndexMap is "
                   << ToString(index_map);
      // This is possible if this tablet completed the backfill. But the master failed over before
      // other tablets could complete.
      // The new master is redoing the backfill. We are safe to ignore this request.
      context.RespondSuccess();
      return;
    }

    uint32_t our_schema_version = tablet.peer->tablet_metadata()->schema_version();
    uint32_t their_schema_version = req->schema_version();
    DCHECK_NE(our_schema_version, their_schema_version);
    SetupErrorAndRespond(
        resp->mutable_error(),
        STATUS_SUBSTITUTE(
            InvalidArgument,
            "Tablet has a different schema $0 vs $1. "
            "Requested index is not ready to backfill. IndexMap: $2",
            our_schema_version, their_schema_version, ToString(index_map)),
        TabletServerErrorPB::MISMATCHED_SCHEMA, &context);
    return;
  }

  Status backfill_status;
  std::string backfilled_until;
  std::unordered_set<TableId> failed_indexes;
  size_t number_rows_processed = 0;
  if (is_pg_table) {
    if (!req->has_namespace_name()) {
      SetupErrorAndRespond(
          resp->mutable_error(),
          STATUS(
              InvalidArgument,
              "Attempted backfill on YSQL table without supplying database name"),
          TabletServerErrorPB::OPERATION_NOT_SUPPORTED,
          &context);
      return;
    }
    backfill_status = tablet.peer->tablet()->BackfillIndexesForYsql(
        indexes_to_backfill,
        req->start_key(),
        deadline,
        read_at,
        server_->pgsql_proxy_bind_address(),
        req->namespace_name(),
        server_->GetSharedMemoryPostgresAuthKey(),
        &number_rows_processed,
        &backfilled_until);
    if (backfill_status.IsIllegalState()) {
      DCHECK_EQ(failed_indexes.size(), 0) << "We don't support batching in YSQL yet";
      for (const auto& idx_info : indexes_to_backfill) {
        failed_indexes.insert(idx_info.table_id());
      }
      DCHECK_EQ(failed_indexes.size(), 1) << "We don't support batching in YSQL yet";
    }
  } else if (tablet.peer->tablet()->table_type() == TableType::YQL_TABLE_TYPE) {
    backfill_status = tablet.peer->tablet()->BackfillIndexes(
        indexes_to_backfill,
        req->start_key(),
        deadline,
        read_at,
        &number_rows_processed,
        &backfilled_until,
        &failed_indexes);
  } else {
    SetupErrorAndRespond(
        resp->mutable_error(),
        STATUS(InvalidArgument, "Attempted backfill on tablet of invalid table type"),
        TabletServerErrorPB::OPERATION_NOT_SUPPORTED,
        &context);
    return;
  }
  DVLOG(1) << "Tablet " << tablet.peer->tablet_id() << " backfilled indexes "
           << yb::ToString(index_ids) << " and got " << backfill_status
           << " backfilled until : " << backfilled_until;

  resp->set_backfilled_until(backfilled_until);
  resp->set_propagated_hybrid_time(server_->Clock()->Now().ToUint64());
  resp->set_number_rows_processed(number_rows_processed);

  if (!backfill_status.ok()) {
    VLOG(2) << " Failed indexes are " << yb::ToString(failed_indexes);
    for (const auto& idx : failed_indexes) {
      *resp->add_failed_index_ids() = idx;
    }
    SetupErrorAndRespond(
        resp->mutable_error(),
        backfill_status,
        (backfill_status.IsIllegalState()
            ? TabletServerErrorPB::OPERATION_NOT_SUPPORTED
            : TabletServerErrorPB::UNKNOWN_ERROR),
        &context);
    return;
  }

  context.RespondSuccess();
}

void TabletServiceAdminImpl::AlterSchema(const tablet::ChangeMetadataRequestPB* req,
                                         ChangeMetadataResponsePB* resp,
                                         rpc::RpcContext context) {
  if (!CheckUuidMatchOrRespond(server_->tablet_manager(), "ChangeMetadata", req, resp, &context)) {
    return;
  }
  VLOG(1) << "Received Change Metadata RPC: " << req->DebugString();
  if (FLAGS_TEST_alter_schema_delay_ms) {
    LOG(INFO) << __func__ << ": sleeping for " << FLAGS_TEST_alter_schema_delay_ms << "ms";
    SleepFor(MonoDelta::FromMilliseconds(FLAGS_TEST_alter_schema_delay_ms));
    LOG(INFO) << __func__ << ": done sleeping for " << FLAGS_TEST_alter_schema_delay_ms << "ms";
  }

  server::UpdateClock(*req, server_->Clock());

  auto tablet = LookupLeaderTabletOrRespond(
      server_->tablet_peer_lookup(), req->tablet_id(), resp, &context);
  if (!tablet) {
    return;
  }

  tablet::TableInfoPtr table_info;
  if (req->has_alter_table_id()) {
    auto result = tablet.peer->tablet_metadata()->GetTableInfo(req->alter_table_id());
    if (!result.ok()) {
      SetupErrorAndRespond(resp->mutable_error(), result.status(),
                           TabletServerErrorPB::INVALID_SCHEMA, &context);
      return;
    }
    table_info = *result;
  } else {
    table_info = tablet.peer->tablet_metadata()->primary_table_info();
  }
  const Schema& tablet_schema = *table_info->schema;
  uint32_t schema_version = table_info->schema_version;
  // Sanity check, to verify that the tablet should have the same schema
  // specified in the request.
  Schema req_schema;
  Status s = SchemaFromPB(req->schema(), &req_schema);
  if (!s.ok()) {
    SetupErrorAndRespond(resp->mutable_error(), s, TabletServerErrorPB::INVALID_SCHEMA, &context);
    return;
  }

  // If the schema was already applied, respond as succeeded.
  if (!req->has_wal_retention_secs() && schema_version == req->schema_version()) {

    if (req_schema.Equals(tablet_schema)) {
      context.RespondSuccess();
      return;
    }

    schema_version = tablet.peer->tablet_metadata()->schema_version(
        req->has_alter_table_id() ? req->alter_table_id() : "");
    if (schema_version == req->schema_version()) {
      LOG(ERROR) << "The current schema does not match the request schema."
                 << " version=" << schema_version
                 << " current-schema=" << tablet_schema.ToString()
                 << " request-schema=" << req_schema.ToString()
                 << " (corruption)";
      SetupErrorAndRespond(resp->mutable_error(),
                           STATUS(Corruption, "got a different schema for the same version number"),
                           TabletServerErrorPB::MISMATCHED_SCHEMA, &context);
      return;
    }
  }

  // If the current schema is newer than the one in the request reject the request.
  if (schema_version > req->schema_version()) {
    LOG(ERROR) << "Tablet " << req->tablet_id() << " has a newer schema"
               << " version=" << schema_version
               << " req->schema_version()=" << req->schema_version()
               << "\n current-schema=" << tablet_schema.ToString()
               << "\n request-schema=" << req_schema.ToString();
    SetupErrorAndRespond(
        resp->mutable_error(),
        STATUS_SUBSTITUTE(
            InvalidArgument, "Tablet has a newer schema Tab $0. Req $1 vs Existing version : $2",
            req->tablet_id(), req->schema_version(), schema_version),
        TabletServerErrorPB::TABLET_HAS_A_NEWER_SCHEMA, &context);
    return;
  }

  VLOG(1) << "Tablet updating schema from "
          << " version=" << schema_version << " current-schema=" << tablet_schema.ToString()
          << " to request-schema=" << req_schema.ToString()
          << " for table ID=" << table_info->table_id;
  ScopedRWOperationPause pause_writes;
  if ((tablet.peer->tablet()->table_type() == TableType::YQL_TABLE_TYPE &&
       !GetAtomicFlag(&FLAGS_disable_alter_vs_write_mutual_exclusion)) ||
      tablet.peer->tablet()->table_type() == TableType::PGSQL_TABLE_TYPE) {
    // For schema change operations we will have to pause the write operations
    // until the schema change is done. This will be done synchronously.
    pause_writes = tablet.peer->tablet()->PauseWritePermits(context.GetClientDeadline());
    if (!pause_writes.ok()) {
      SetupErrorAndRespond(
          resp->mutable_error(),
          STATUS(
              TryAgain, "Could not lock the tablet against write operations for schema change"),
          &context);
      return;
    }

    // After write operation is paused, active transactions will be aborted for YSQL transactions.
    if (tablet.peer->tablet()->table_type() == TableType::PGSQL_TABLE_TYPE &&
        req->should_abort_active_txns()) {
      DCHECK(req->has_transaction_id());
      if (tablet.peer->tablet()->transaction_participant() == nullptr) {
        auto status = STATUS(
            IllegalState, "Transaction participant is null for tablet " + req->tablet_id());
        LOG(ERROR) << status;
        SetupErrorAndRespond(
            resp->mutable_error(),
            status,
            &context);
        return;
      }
      HybridTime max_cutoff = HybridTime::kMax;
      CoarseTimePoint deadline =
          CoarseMonoClock::Now() +
          MonoDelta::FromMilliseconds(FLAGS_ysql_transaction_abort_timeout_ms);
      TransactionId txn_id = CHECK_RESULT(TransactionId::FromString(req->transaction_id()));
      LOG(INFO) << "Aborting transactions that started prior to " << max_cutoff
                << " for tablet id " << req->tablet_id()
                << " excluding transaction with id " << txn_id;
      // There could be a chance where a transaction does not appear by transaction_participant
      // but has already begun replicating through Raft. Such transactions might succeed rather
      // than get aborted. This race codnition is dismissable for this intermediate solution.
      Status status = tablet.peer->tablet()->transaction_participant()->StopActiveTxnsPriorTo(
            max_cutoff, deadline, &txn_id);
      if (!status.ok() || PREDICT_FALSE(FLAGS_TEST_fail_alter_schema_after_abort_transactions)) {
        auto status = STATUS(TryAgain, "Transaction abort failed for tablet " + req->tablet_id());
        LOG(WARNING) << status;
        SetupErrorAndRespond(
            resp->mutable_error(),
            status,
            &context);
        return;
      }
    }
  }
  auto operation = std::make_unique<ChangeMetadataOperation>(
      tablet.peer->tablet(), tablet.peer->log(), req);

  operation->set_completion_callback(
      MakeRpcOperationCompletionCallback(std::move(context), resp, server_->Clock()));
  operation->UsePermitToken(std::move(pause_writes));

  // Submit the alter schema op. The RPC will be responded to asynchronously.
  tablet.peer->Submit(std::move(operation), tablet.leader_term);
}

#define VERIFY_RESULT_OR_RETURN(expr) RESULT_CHECKER_HELPER( \
    expr, \
    if (!__result.ok()) { return; });

void TabletServiceImpl::VerifyTableRowRange(
    const VerifyTableRowRangeRequestPB* req,
    VerifyTableRowRangeResponsePB* resp,
    rpc::RpcContext context) {
  DVLOG(3) << "Received VerifyTableRowRange RPC: " << req->DebugString();

  server::UpdateClock(*req, server_->Clock());

  auto peer_tablet =
      LookupTabletPeerOrRespond(server_->tablet_peer_lookup(), req->tablet_id(), resp, &context);
  if (!peer_tablet) {
    return;
  }

  auto tablet = peer_tablet->tablet;
  bool is_pg_table = tablet->table_type() == TableType::PGSQL_TABLE_TYPE;
  if (is_pg_table) {
    SetupErrorAndRespond(
        resp->mutable_error(), STATUS(NotFound, "Verify operation not supported for PGSQL tables."),
        &context);
    return;
  }

  const CoarseTimePoint& deadline = context.GetClientDeadline();

  // Wait for SafeTime to get past read_at;
  const HybridTime read_at(req->read_time());
  DVLOG(1) << "Waiting for safe time to be past " << read_at;
  const auto safe_time = tablet->SafeTime(tablet::RequireLease::kFalse, read_at, deadline);
  DVLOG(1) << "Got safe time " << safe_time.ToString();
  if (!safe_time.ok()) {
    LOG(ERROR) << "Could not get a good enough safe time " << safe_time.ToString();
    SetupErrorAndRespond(resp->mutable_error(), safe_time.status(), &context);
    return;
  }

  auto valid_read_at = req->has_read_time() ? read_at : *safe_time;
  std::string verified_until = "";
  std::unordered_map<TableId, uint64> consistency_stats;

  if (peer_tablet->tablet_peer->tablet_metadata()->primary_table_info()->index_info) {
    auto index_info =
        *peer_tablet->tablet_peer->tablet_metadata()->primary_table_info()->index_info;
    const auto& table_id = index_info.indexed_table_id();
    Status verify_status = tablet->VerifyMainTableConsistencyForCQL(
        table_id, req->start_key(), req->num_rows(), deadline, valid_read_at, &consistency_stats,
        &verified_until);
    if (!verify_status.ok()) {
      SetupErrorAndRespond(resp->mutable_error(), verify_status, &context);
      return;
    }

    (*resp->mutable_consistency_stats())[table_id] = consistency_stats[table_id];
  } else {
    const IndexMap index_map =
        *peer_tablet->tablet_peer->tablet_metadata()->primary_table_info()->index_map;
    vector<IndexInfo> indexes;
    vector<TableId> index_ids;
    if (req->index_ids().empty()) {
      for (auto it = index_map.begin(); it != index_map.end(); it++) {
        indexes.push_back(it->second);
      }
    } else {
      for (const auto& idx : req->index_ids()) {
        auto result = index_map.FindIndex(idx);
        if (result) {
          const IndexInfo* index_info = *result;
          indexes.push_back(*index_info);
          index_ids.push_back(index_info->table_id());
        } else {
          LOG(WARNING) << "Index " << idx << " not found in tablet metadata";
        }
      }
    }

    Status verify_status = tablet->VerifyIndexTableConsistencyForCQL(
        indexes, req->start_key(), req->num_rows(), deadline, valid_read_at, &consistency_stats,
        &verified_until);
    if (!verify_status.ok()) {
      SetupErrorAndRespond(resp->mutable_error(), verify_status, &context);
      return;
    }

    for (const IndexInfo& index : indexes) {
      const auto& table_id = index.table_id();
      (*resp->mutable_consistency_stats())[table_id] = consistency_stats[table_id];
    }
  }
  resp->set_verified_until(verified_until);
  context.RespondSuccess();
}

void TabletServiceImpl::UpdateTransaction(const UpdateTransactionRequestPB* req,
                                          UpdateTransactionResponsePB* resp,
                                          rpc::RpcContext context) {
  TRACE("UpdateTransaction");

  if (req->state().status() == TransactionStatus::CREATED &&
      RandomActWithProbability(TEST_delay_create_transaction_probability)) {
    std::this_thread::sleep_for(
        (FLAGS_transaction_rpc_timeout_ms + RandomUniformInt(-200, 200)) * 1ms);
  }

  VLOG(1) << "UpdateTransaction: " << req->ShortDebugString()
          << ", context: " << context.ToString();
  LOG_IF(DFATAL, !req->has_propagated_hybrid_time())
      << __func__ << " missing propagated hybrid time for "
      << TransactionStatus_Name(req->state().status());
  UpdateClock(*req, server_->Clock());

  LeaderTabletPeer tablet;
  auto txn_status = req->state().status();
  auto cleanup = txn_status == TransactionStatus::IMMEDIATE_CLEANUP ||
                 txn_status == TransactionStatus::GRACEFUL_CLEANUP;
  if (cleanup) {
    auto peer_tablet = VERIFY_RESULT_OR_RETURN(LookupTabletPeerOrRespond(
        server_->tablet_peer_lookup(), req->tablet_id(), resp, &context));
    tablet.FillTabletPeer(std::move(peer_tablet));
    tablet.leader_term = OpId::kUnknownTerm;
  } else {
    tablet = LookupLeaderTabletOrRespond(
        server_->tablet_peer_lookup(), req->tablet_id(), resp, &context);
  }
  if (!tablet) {
    return;
  }

  auto state = std::make_unique<tablet::UpdateTxnOperation>(tablet.tablet.get(), &req->state());
  state->set_completion_callback(MakeRpcOperationCompletionCallback(
      std::move(context), resp, server_->Clock()));

  if (req->state().status() == TransactionStatus::APPLYING || cleanup) {
    auto* participant = tablet.tablet->transaction_participant();
    if (participant) {
      participant->Handle(std::move(state), tablet.leader_term);
    } else {
      state->CompleteWithStatus(STATUS_FORMAT(
          InvalidArgument, "Does not have transaction participant to process $0",
          req->state().status()));
    }
  } else {
    auto* coordinator = tablet.tablet->transaction_coordinator();
    if (coordinator) {
      coordinator->Handle(std::move(state), tablet.leader_term);
    } else {
      state->CompleteWithStatus(STATUS_FORMAT(
          InvalidArgument, "Does not have transaction coordinator to process $0",
          req->state().status()));
    }
  }
}

template <class Req, class Resp, class Action>
void TabletServiceImpl::PerformAtLeader(
    const Req& req, Resp* resp, rpc::RpcContext* context, const Action& action) {
  UpdateClock(*req, server_->Clock());

  auto tablet_peer = LookupLeaderTabletOrRespond(
      server_->tablet_peer_lookup(), req->tablet_id(), resp, context);

  if (!tablet_peer) {
    return;
  }

  auto status = action(tablet_peer);

  if (*context) {
    resp->set_propagated_hybrid_time(server_->Clock()->Now().ToUint64());
    if (status.ok()) {
      context->RespondSuccess();
    } else {
      SetupErrorAndRespond(resp->mutable_error(), status, context);
    }
  }
}

void TabletServiceImpl::GetTransactionStatus(const GetTransactionStatusRequestPB* req,
                                             GetTransactionStatusResponsePB* resp,
                                             rpc::RpcContext context) {
  TRACE("GetTransactionStatus");

  PerformAtLeader(req, resp, &context,
      [req, resp, &context](const LeaderTabletPeer& tablet_peer) {
    auto* transaction_coordinator = tablet_peer.tablet->transaction_coordinator();
    if (!transaction_coordinator) {
      return STATUS_FORMAT(
          InvalidArgument, "No transaction coordinator at tablet $0",
          tablet_peer.peer->tablet_id());
    }
    return transaction_coordinator->GetStatus(
        req->transaction_id(), context.GetClientDeadline(), resp);
  });
}

void TabletServiceImpl::GetTransactionStatusAtParticipant(
    const GetTransactionStatusAtParticipantRequestPB* req,
    GetTransactionStatusAtParticipantResponsePB* resp,
    rpc::RpcContext context) {
  TRACE("GetTransactionStatusAtParticipant");

  PerformAtLeader(req, resp, &context,
      [req, resp, &context](const LeaderTabletPeer& tablet_peer) -> Status {
    auto* transaction_participant = tablet_peer.peer->tablet()->transaction_participant();
    if (!transaction_participant) {
      return STATUS_FORMAT(
          InvalidArgument, "No transaction participant at tablet $0",
          tablet_peer.peer->tablet_id());
    }

    transaction_participant->GetStatus(
        VERIFY_RESULT(FullyDecodeTransactionId(req->transaction_id())),
        req->required_num_replicated_batches(), tablet_peer.leader_term, resp, &context);
    return Status::OK();
  });
}

void TabletServiceImpl::AbortTransaction(const AbortTransactionRequestPB* req,
                                         AbortTransactionResponsePB* resp,
                                         rpc::RpcContext context) {
  TRACE("AbortTransaction");

  UpdateClock(*req, server_->Clock());

  auto tablet = LookupLeaderTabletOrRespond(
      server_->tablet_peer_lookup(), req->tablet_id(), resp, &context);
  if (!tablet) {
    return;
  }

  server::ClockPtr clock(server_->Clock());
  auto context_ptr = std::make_shared<rpc::RpcContext>(std::move(context));
  tablet.peer->tablet()->transaction_coordinator()->Abort(
      req->transaction_id(),
      tablet.leader_term,
      [resp, context_ptr, clock, peer = tablet.peer](Result<TransactionStatusResult> result) {
        resp->set_propagated_hybrid_time(clock->Now().ToUint64());
        Status status;
        if (result.ok()) {
          auto leader_safe_time = peer->LeaderSafeTime();
          if (leader_safe_time.ok()) {
            resp->set_status(result->status);
            if (result->status_time.is_valid()) {
              resp->set_status_hybrid_time(result->status_time.ToUint64());
            }
            // See comment above WaitForSafeTime in TransactionStatusCache::DoGetCommitData
            // for details.
            resp->set_coordinator_safe_time(leader_safe_time->ToUint64());
            context_ptr->RespondSuccess();
            return;
          }

          status = leader_safe_time.status();
        } else {
          status = result.status();
        }
        SetupErrorAndRespond(resp->mutable_error(), status, context_ptr.get());
      });
}

void TabletServiceImpl::Truncate(const TruncateRequestPB* req,
                                 TruncateResponsePB* resp,
                                 rpc::RpcContext context) {
  TRACE("Truncate");

  UpdateClock(*req, server_->Clock());

  auto tablet = LookupLeaderTabletOrRespond(
      server_->tablet_peer_lookup(), req->tablet_id(), resp, &context);
  if (!tablet) {
    return;
  }

  auto operation = std::make_unique<TruncateOperation>(tablet.peer->tablet(), &req->truncate());

  operation->set_completion_callback(
      MakeRpcOperationCompletionCallback(std::move(context), resp, server_->Clock()));

  // Submit the truncate tablet op. The RPC will be responded to asynchronously.
  tablet.peer->Submit(std::move(operation), tablet.leader_term);
}

void TabletServiceAdminImpl::CreateTablet(const CreateTabletRequestPB* req,
                                          CreateTabletResponsePB* resp,
                                          rpc::RpcContext context) {
  if (!CheckUuidMatchOrRespond(server_->tablet_manager(), "CreateTablet", req, resp, &context)) {
    return;
  }
  auto status = DoCreateTablet(req, resp);
  if (!status.ok()) {
    SetupErrorAndRespond(resp->mutable_error(), status, &context);
  } else {
    context.RespondSuccess();
  }
}

Status TabletServiceAdminImpl::DoCreateTablet(const CreateTabletRequestPB* req,
                                              CreateTabletResponsePB* resp) {
  if (PREDICT_FALSE(FLAGS_TEST_txn_status_table_tablet_creation_delay_ms > 0 &&
                    req->table_type() == TableType::TRANSACTION_STATUS_TABLE_TYPE)) {
    std::this_thread::sleep_for(FLAGS_TEST_txn_status_table_tablet_creation_delay_ms * 1ms);
  }

  DVLOG(3) << "Received CreateTablet RPC: " << yb::ToString(*req);
  TRACE_EVENT1("tserver", "CreateTablet",
               "tablet_id", req->tablet_id());

  Schema schema;
  PartitionSchema partition_schema;
  auto status = SchemaFromPB(req->schema(), &schema);
  if (status.ok()) {
    DCHECK(schema.has_column_ids());
    status = PartitionSchema::FromPB(req->partition_schema(), schema, &partition_schema);
  }
  if (!status.ok()) {
    return status.CloneAndAddErrorCode(TabletServerError(TabletServerErrorPB::INVALID_SCHEMA));
  }

  Partition partition;
  Partition::FromPB(req->partition(), &partition);

  LOG(INFO) << "Processing CreateTablet for T " << req->tablet_id() << " P " << req->dest_uuid()
            << " (table=" << req->table_name()
            << " [id=" << req->table_id() << "]), partition="
            << partition_schema.PartitionDebugString(partition, schema);
  VLOG(1) << "Full request: " << req->DebugString();

  auto table_info = std::make_shared<tablet::TableInfo>(
      req->table_id(), req->namespace_name(), req->table_name(), req->table_type(), schema,
      IndexMap(),
      req->has_index_info() ? boost::optional<IndexInfo>(req->index_info()) : boost::none,
      0 /* schema_version */, partition_schema);
  std::vector<SnapshotScheduleId> snapshot_schedules;
  snapshot_schedules.reserve(req->snapshot_schedules().size());
  for (const auto& id : req->snapshot_schedules()) {
    snapshot_schedules.push_back(VERIFY_RESULT(FullyDecodeSnapshotScheduleId(id)));
  }
  status = ResultToStatus(server_->tablet_manager()->CreateNewTablet(
      table_info, req->tablet_id(), partition, req->config(), req->colocated(),
      snapshot_schedules));
  if (PREDICT_FALSE(!status.ok())) {
    return status.IsAlreadyPresent()
        ? status.CloneAndAddErrorCode(TabletServerError(TabletServerErrorPB::TABLET_ALREADY_EXISTS))
        : status;
  }
  return Status::OK();
}

void TabletServiceAdminImpl::DeleteTablet(const DeleteTabletRequestPB* req,
                                          DeleteTabletResponsePB* resp,
                                          rpc::RpcContext context) {
  if (PREDICT_FALSE(FLAGS_TEST_rpc_delete_tablet_fail)) {
    context.RespondFailure(STATUS(NetworkError, "Simulating network partition for test"));
    return;
  }

  if (!CheckUuidMatchOrRespond(server_->tablet_manager(), "DeleteTablet", req, resp, &context)) {
    return;
  }
  TRACE_EVENT2("tserver", "DeleteTablet",
               "tablet_id", req->tablet_id(),
               "reason", req->reason());

  tablet::TabletDataState delete_type = tablet::TABLET_DATA_UNKNOWN;
  if (req->has_delete_type()) {
    delete_type = req->delete_type();
  }
  LOG(INFO) << "T " << req->tablet_id() << " P " << server_->permanent_uuid()
            << ": Processing DeleteTablet with delete_type " << TabletDataState_Name(delete_type)
            << (req->has_reason() ? (" (" + req->reason() + ")") : "")
            << (req->hide_only() ? " (Hide only)" : "")
            << " from " << context.requestor_string();
  VLOG(1) << "Full request: " << req->DebugString();

  boost::optional<int64_t> cas_config_opid_index_less_or_equal;
  if (req->has_cas_config_opid_index_less_or_equal()) {
    cas_config_opid_index_less_or_equal = req->cas_config_opid_index_less_or_equal();
  }
  boost::optional<TabletServerErrorPB::Code> error_code;
  Status s = server_->tablet_manager()->DeleteTablet(req->tablet_id(),
                                                     delete_type,
                                                     cas_config_opid_index_less_or_equal,
                                                     req->hide_only(),
                                                     &error_code);
  if (PREDICT_FALSE(!s.ok())) {
    HandleErrorResponse(resp, &context, s, error_code);
    return;
  }
  context.RespondSuccess();
}

// TODO(sagnik): Modify this to actually create a copartitioned table
void TabletServiceAdminImpl::CopartitionTable(const CopartitionTableRequestPB* req,
                                              CopartitionTableResponsePB* resp,
                                              rpc::RpcContext context) {
  context.RespondSuccess();
  LOG(INFO) << "tserver doesn't support co-partitioning yet";
}

void TabletServiceAdminImpl::FlushTablets(const FlushTabletsRequestPB* req,
                                          FlushTabletsResponsePB* resp,
                                          rpc::RpcContext context) {
  if (!CheckUuidMatchOrRespond(server_->tablet_manager(), "FlushTablets", req, resp, &context)) {
    return;
  }

  if (!req->all_tablets() && req->tablet_ids_size() == 0) {
    const Status s = STATUS(InvalidArgument, "No tablet ids");
    SetupErrorAndRespond(resp->mutable_error(), s, &context);
    return;
  }

  server::UpdateClock(*req, server_->Clock());

  TRACE_EVENT1("tserver", "FlushTablets",
               "TS: ", req->dest_uuid());

  LOG(INFO) << "Processing FlushTablets from " << context.requestor_string();
  VLOG(1) << "Full FlushTablets request: " << req->DebugString();
  TabletPeers tablet_peers;
  TSTabletManager::TabletPtrs tablet_ptrs;

  if (req->all_tablets()) {
    tablet_peers = server_->tablet_manager()->GetTabletPeers(&tablet_ptrs);
  } else {
    for (const TabletId& id : req->tablet_ids()) {
      auto tablet_peer = VERIFY_RESULT_OR_RETURN(LookupTabletPeerOrRespond(
          server_->tablet_peer_lookup(), id, resp, &context));
      tablet_peers.push_back(std::move(tablet_peer.tablet_peer));
      auto tablet = tablet_peer.tablet;
      if (tablet != nullptr) {
        tablet_ptrs.push_back(std::move(tablet));
      }
    }
  }
  switch (req->operation()) {
    case FlushTabletsRequestPB::FLUSH:
      for (const tablet::TabletPtr& tablet : tablet_ptrs) {
        resp->set_failed_tablet_id(tablet->tablet_id());
        RETURN_UNKNOWN_ERROR_IF_NOT_OK(tablet->Flush(tablet::FlushMode::kAsync), resp, &context);
        resp->clear_failed_tablet_id();
      }

      // Wait for end of all flush operations.
      for (const tablet::TabletPtr& tablet : tablet_ptrs) {
        resp->set_failed_tablet_id(tablet->tablet_id());
        RETURN_UNKNOWN_ERROR_IF_NOT_OK(tablet->WaitForFlush(), resp, &context);
        resp->clear_failed_tablet_id();
      }
      break;
    case FlushTabletsRequestPB::COMPACT:
      RETURN_UNKNOWN_ERROR_IF_NOT_OK(
          server_->tablet_manager()->TriggerCompactionAndWait(tablet_ptrs), resp, &context);
      break;
    case FlushTabletsRequestPB::LOG_GC:
      for (const auto& tablet : tablet_peers) {
        resp->set_failed_tablet_id(tablet->tablet_id());
        RETURN_UNKNOWN_ERROR_IF_NOT_OK(tablet->RunLogGC(), resp, &context);
        resp->clear_failed_tablet_id();
      }
      break;
  }

  context.RespondSuccess();
}

void TabletServiceAdminImpl::CountIntents(
    const CountIntentsRequestPB* req,
    CountIntentsResponsePB* resp,
    rpc::RpcContext context) {
  TSTabletManager::TabletPtrs tablet_ptrs;
  TabletPeers tablet_peers = server_->tablet_manager()->GetTabletPeers(&tablet_ptrs);
  int64_t total_intents = 0;
  // TODO: do this in parallel.
  // TODO: per-tablet intent counts.
  for (const auto& tablet : tablet_ptrs) {
    auto num_intents = tablet->CountIntents();
    if (!num_intents.ok()) {
      SetupErrorAndRespond(resp->mutable_error(), num_intents.status(), &context);
      return;
    }
    total_intents += *num_intents;
  }
  resp->set_num_intents(total_intents);
  context.RespondSuccess();
}

void TabletServiceAdminImpl::AddTableToTablet(
    const AddTableToTabletRequestPB* req, AddTableToTabletResponsePB* resp,
    rpc::RpcContext context) {
  auto tablet_id = req->tablet_id();

  const auto tablet =
      LookupLeaderTabletOrRespond(server_->tablet_peer_lookup(), tablet_id, resp, &context);
  if (!tablet) {
    return;
  }
  DVLOG(3) << "Received AddTableToTablet RPC: " << yb::ToString(*req);

  tablet::ChangeMetadataRequestPB change_req;
  *change_req.mutable_add_table() = req->add_table();
  change_req.set_tablet_id(tablet_id);
  Status s = tablet::SyncReplicateChangeMetadataOperation(
      &change_req, tablet.peer.get(), tablet.leader_term);
  if (PREDICT_FALSE(!s.ok())) {
    SetupErrorAndRespond(resp->mutable_error(), s, &context);
    return;
  }
  context.RespondSuccess();
}

void TabletServiceAdminImpl::RemoveTableFromTablet(
    const RemoveTableFromTabletRequestPB* req,
    RemoveTableFromTabletResponsePB* resp,
    rpc::RpcContext context) {
  auto tablet =
      LookupLeaderTabletOrRespond(server_->tablet_peer_lookup(), req->tablet_id(), resp, &context);
  if (!tablet) {
    return;
  }

  tablet::ChangeMetadataRequestPB change_req;
  change_req.set_remove_table_id(req->remove_table_id());
  change_req.set_tablet_id(req->tablet_id());
  Status s = tablet::SyncReplicateChangeMetadataOperation(
      &change_req, tablet.peer.get(), tablet.leader_term);
  if (PREDICT_FALSE(!s.ok())) {
    SetupErrorAndRespond(resp->mutable_error(), s, &context);
    return;
  }
  context.RespondSuccess();
}

void TabletServiceAdminImpl::SplitTablet(
    const tablet::SplitTabletRequestPB* req, SplitTabletResponsePB* resp, rpc::RpcContext context) {
  if (!CheckUuidMatchOrRespond(server_->tablet_manager(), "SplitTablet", req, resp, &context)) {
    return;
  }
  if (PREDICT_FALSE(FLAGS_TEST_fail_tablet_split_probability > 0) &&
      RandomActWithProbability(FLAGS_TEST_fail_tablet_split_probability)) {
    return SetupErrorAndRespond(
        resp->mutable_error(),
        STATUS(InvalidArgument,  // Use InvalidArgument to hit IsDefinitelyPermanentError().
            "Failing tablet split due to FLAGS_TEST_fail_tablet_split_probability"),
        TabletServerErrorPB::UNKNOWN_ERROR,
        &context);
  }
  TRACE_EVENT1("tserver", "SplitTablet", "tablet_id", req->tablet_id());

  server::UpdateClock(*req, server_->Clock());
  auto leader_tablet_peer =
      LookupLeaderTabletOrRespond(server_->tablet_peer_lookup(), req->tablet_id(), resp, &context);
  if (!leader_tablet_peer) {
    return;
  }

  {
    auto tablet_data_state = leader_tablet_peer.peer->data_state();
    if (tablet_data_state != tablet::TABLET_DATA_READY) {
      auto s = tablet_data_state == tablet::TABLET_DATA_SPLIT_COMPLETED
                  ? STATUS_FORMAT(AlreadyPresent, "Tablet $0 is already split.", req->tablet_id())
                  : STATUS_FORMAT(
                        InvalidArgument, "Invalid tablet $0 data state: $1", req->tablet_id(),
                        tablet_data_state);
      SetupErrorAndRespond(
          resp->mutable_error(), s, TabletServerErrorPB::TABLET_NOT_RUNNING, &context);
      return;
    }
  }

  auto state = std::make_unique<tablet::SplitOperation>(
      leader_tablet_peer.peer->tablet(), server_->tablet_manager(), req);

  state->set_completion_callback(
      MakeRpcOperationCompletionCallback(std::move(context), resp, server_->Clock()));

  leader_tablet_peer.peer->Submit(std::move(state), leader_tablet_peer.leader_term);
}

void TabletServiceAdminImpl::UpgradeYsql(
    const UpgradeYsqlRequestPB* req,
    UpgradeYsqlResponsePB* resp,
    rpc::RpcContext context) {
  LOG(INFO) << "Starting YSQL upgrade";

  pgwrapper::YsqlUpgradeHelper upgrade_helper(server_->pgsql_proxy_bind_address(),
                                              server_->GetSharedMemoryPostgresAuthKey(),
                                              FLAGS_heartbeat_interval_ms);
  const auto status = upgrade_helper.Upgrade();
  if (!status.ok()) {
    LOG(INFO) << "YSQL upgrade failed: " << status;
    SetupErrorAndRespond(resp->mutable_error(), status, &context);
    return;
  }

  LOG(INFO) << "YSQL upgrade done successfully";
  context.RespondSuccess();
}


bool EmptyWriteBatch(const docdb::KeyValueWriteBatchPB& write_batch) {
  return write_batch.write_pairs().empty() && write_batch.apply_external_transactions().empty();
}

void TabletServiceImpl::Write(const WriteRequestPB* req,
                              WriteResponsePB* resp,
                              rpc::RpcContext context) {
  if (FLAGS_TEST_tserver_noop_read_write) {
    for (int i = 0; i < req->ql_write_batch_size(); ++i) {
      resp->add_ql_response_batch();
    }
    context.RespondSuccess();
    return;
  }
  TRACE("Start Write");
  TRACE_EVENT1("tserver", "TabletServiceImpl::Write",
               "tablet_id", req->tablet_id());
  VLOG(2) << "Received Write RPC: " << req->DebugString();
  UpdateClock(*req, server_->Clock());

  auto tablet = LookupLeaderTabletOrRespond(
      server_->tablet_peer_lookup(), req->tablet_id(), resp, &context);
  if (!tablet ||
      !CheckWriteThrottlingOrRespond(
          req->rejection_score(), tablet.peer.get(), resp, &context)) {
    return;
  }

  if (tablet.peer->tablet()->metadata()->hidden()) {
    auto status = STATUS(NotFound, "Tablet not found", req->tablet_id());
    SetupErrorAndRespond(
        resp->mutable_error(), status, TabletServerErrorPB::TABLET_NOT_FOUND, &context);
    return;
  }

#if defined(DUMP_WRITE)
  if (req->has_write_batch() && req->write_batch().has_transaction()) {
    VLOG(1) << "Write with transaction: " << req->write_batch().transaction().ShortDebugString();
    if (req->pgsql_write_batch_size() != 0) {
      auto txn_id = CHECK_RESULT(FullyDecodeTransactionId(
          req->write_batch().transaction().transaction_id()));
      for (const auto& entry : req->pgsql_write_batch()) {
        if (entry.stmt_type() == PgsqlWriteRequestPB::PGSQL_UPDATE) {
          auto key = entry.column_new_values(0).expr().value().int32_value();
          LOG(INFO) << txn_id << " UPDATE: " << key << " = "
                    << entry.column_new_values(1).expr().value().string_value();
        } else if (
            entry.stmt_type() == PgsqlWriteRequestPB::PGSQL_INSERT ||
            entry.stmt_type() == PgsqlWriteRequestPB::PGSQL_UPSERT) {
          docdb::DocKey doc_key;
          CHECK_OK(doc_key.FullyDecodeFrom(entry.ybctid_column_value().value().binary_value()));
          LOG(INFO) << txn_id << " INSERT: " << doc_key.hashed_group()[0].GetInt32() << " = "
                    << entry.column_values(0).expr().value().string_value();
        } else if (entry.stmt_type() == PgsqlWriteRequestPB::PGSQL_DELETE) {
          LOG(INFO) << txn_id << " DELETE: " << entry.ShortDebugString();
        }
      }
    }
  }
#endif

  if (PREDICT_FALSE(req->has_write_batch() && !req->has_external_hybrid_time() &&
      (!req->write_batch().write_pairs().empty() || !req->write_batch().read_pairs().empty()))) {
    Status s = STATUS(NotSupported, "Write Request contains write batch. This field should be "
        "used only for post-processed write requests during "
        "Raft replication.");
    SetupErrorAndRespond(resp->mutable_error(), s,
                         TabletServerErrorPB::INVALID_MUTATION,
                         &context);
    return;
  }

  bool has_operations = req->ql_write_batch_size() != 0 ||
                        req->redis_write_batch_size() != 0 ||
                        req->pgsql_write_batch_size() != 0 ||
                        (req->has_external_hybrid_time() && !EmptyWriteBatch(req->write_batch()));
  if (!has_operations && tablet.peer->tablet()->table_type() != TableType::REDIS_TABLE_TYPE) {
    // An empty request. This is fine, can just exit early with ok status instead of working hard.
    // This doesn't need to go to Raft log.
    MakeRpcOperationCompletionCallback<WriteResponsePB>(
        std::move(context), resp, server_->Clock())(Status::OK());
    return;
  }

  // For postgres requests check that the syscatalog version matches.
  if (tablet.peer->tablet()->table_type() == TableType::PGSQL_TABLE_TYPE) {
    uint64_t last_breaking_catalog_version = 0; // unset.
    for (const auto& pg_req : req->pgsql_write_batch()) {
      if (pg_req.has_ysql_catalog_version()) {
        if (last_breaking_catalog_version == 0) {
          // Initialize last breaking version if not yet set.
          server_->get_ysql_catalog_version(nullptr /* current_version */,
                                            &last_breaking_catalog_version);
        }
        if (pg_req.ysql_catalog_version() < last_breaking_catalog_version) {
          SetupErrorAndRespond(resp->mutable_error(),
              STATUS_SUBSTITUTE(QLError, "The catalog snapshot used for this "
                                         "transaction has been invalidated."),
              TabletServerErrorPB::MISMATCHED_SCHEMA, &context);
          return;
        }
      }
    }
  }

  auto query = std::make_unique<tablet::WriteQuery>(
      tablet.leader_term, context.GetClientDeadline(), tablet.peer.get(),
      tablet.peer->tablet(), resp);
  query->set_client_request(*req);

  auto context_ptr = std::make_shared<RpcContext>(std::move(context));
  if (RandomActWithProbability(GetAtomicFlag(&FLAGS_TEST_respond_write_failed_probability))) {
    LOG(INFO) << "Responding with a failure to " << req->DebugString();
    SetupErrorAndRespond(resp->mutable_error(), STATUS(LeaderHasNoLease, "TEST: Random failure"),
                         context_ptr.get());
  } else {
    query->set_callback(WriteQueryCompletionCallback(
        tablet.peer, context_ptr, resp, query.get(), server_->Clock(), req->include_trace()));
  }

  query->AdjustYsqlQueryTransactionality(req->pgsql_write_batch_size());

  tablet.peer->WriteAsync(std::move(query));
}

void TabletServiceImpl::Read(const ReadRequestPB* req,
                             ReadResponsePB* resp,
                             rpc::RpcContext context) {
  if (FLAGS_TEST_tserver_noop_read_write) {
    context.RespondSuccess();
    return;
  }

  PerformRead(server_, this, req, resp, std::move(context));
}

ConsensusServiceImpl::ConsensusServiceImpl(const scoped_refptr<MetricEntity>& metric_entity,
                                           TabletPeerLookupIf* tablet_manager)
    : ConsensusServiceIf(metric_entity),
      tablet_manager_(tablet_manager) {
}

ConsensusServiceImpl::~ConsensusServiceImpl() {
}

void ConsensusServiceImpl::CompleteUpdateConsensusResponse(
    std::shared_ptr<tablet::TabletPeer> tablet_peer,
    consensus::ConsensusResponsePB* resp) {
  auto tablet = tablet_peer->shared_tablet();
  if (tablet) {
    resp->set_num_sst_files(tablet->GetCurrentVersionNumSSTFiles());
  }
  resp->set_propagated_hybrid_time(tablet_peer->clock().Now().ToUint64());
}

void ConsensusServiceImpl::MultiRaftUpdateConsensus(
      const consensus::MultiRaftConsensusRequestPB *req,
      consensus::MultiRaftConsensusResponsePB *resp,
      rpc::RpcContext context) {
    DVLOG(3) << "Received Batch Consensus Update RPC: " << req->ShortDebugString();
    // Effectively performs ConsensusServiceImpl::UpdateConsensus for
    // each ConsensusRequestPB in the batch but does not fail the entire
    // batch if a single request fails.
    for (int i = 0; i < req->consensus_request_size(); i++) {
      // Unfortunately, we have to use const_cast here,
      // because the protobuf-generated interface only gives us a const request
      // but we need to be able to move messages out of the request for efficiency.
      auto consensus_req = const_cast<ConsensusRequestPB*>(&req->consensus_request(i));
      auto consensus_resp = resp->add_consensus_response();;

      auto uuid_match_res = CheckUuidMatch(tablet_manager_, "UpdateConsensus", consensus_req,
                                           context.requestor_string());
      if (!uuid_match_res.ok()) {
        SetupError(consensus_resp->mutable_error(), uuid_match_res.status());
        continue;
      }

      auto peer_tablet_res = LookupTabletPeer(tablet_manager_, consensus_req->tablet_id());
      if (!peer_tablet_res.ok()) {
        SetupError(consensus_resp->mutable_error(), peer_tablet_res.status());
        continue;
      }
      auto tablet_peer = peer_tablet_res.get().tablet_peer;

      // Submit the update directly to the TabletPeer's Consensus instance.
      auto consensus_res = GetConsensus(tablet_peer);
      if (!consensus_res.ok()) {
        SetupError(consensus_resp->mutable_error(), consensus_res.status());
        continue;
      }
      auto consensus = *consensus_res;

      Status s = consensus->Update(
        consensus_req, consensus_resp, context.GetClientDeadline());
      if (PREDICT_FALSE(!s.ok())) {
        // Clear the response first, since a partially-filled response could
        // result in confusing a caller, or in having missing required fields
        // in embedded optional messages.
        consensus_resp->Clear();
        SetupError(consensus_resp->mutable_error(), s);
        continue;
      }

      CompleteUpdateConsensusResponse(tablet_peer, consensus_resp);
    }
    context.RespondSuccess();
}

void ConsensusServiceImpl::UpdateConsensus(const ConsensusRequestPB* req,
                                           ConsensusResponsePB* resp,
                                           rpc::RpcContext context) {
  DVLOG(3) << "Received Consensus Update RPC: " << req->ShortDebugString();
  if (!CheckUuidMatchOrRespond(tablet_manager_, "UpdateConsensus", req, resp, &context)) {
    return;
  }
  auto peer_tablet = VERIFY_RESULT_OR_RETURN(LookupTabletPeerOrRespond(
      tablet_manager_, req->tablet_id(), resp, &context));
  auto tablet_peer = peer_tablet.tablet_peer;

  // Submit the update directly to the TabletPeer's Consensus instance.
  shared_ptr<Consensus> consensus;
  if (!GetConsensusOrRespond(tablet_peer, resp, &context, &consensus)) return;

  // Unfortunately, we have to use const_cast here, because the protobuf-generated interface only
  // gives us a const request, but we need to be able to move messages out of the request for
  // efficiency.
  Status s = consensus->Update(
      const_cast<ConsensusRequestPB*>(req), resp, context.GetClientDeadline());
  if (PREDICT_FALSE(!s.ok())) {
    // Clear the response first, since a partially-filled response could
    // result in confusing a caller, or in having missing required fields
    // in embedded optional messages.
    resp->Clear();

    SetupErrorAndRespond(resp->mutable_error(), s, &context);
    return;
  }

  CompleteUpdateConsensusResponse(tablet_peer, resp);

  context.RespondSuccess();
}

void ConsensusServiceImpl::RequestConsensusVote(const VoteRequestPB* req,
                                                VoteResponsePB* resp,
                                                rpc::RpcContext context) {
  DVLOG(3) << "Received Consensus Request Vote RPC: " << req->DebugString();
  if (!CheckUuidMatchOrRespond(tablet_manager_, "RequestConsensusVote", req, resp, &context)) {
    return;
  }
  auto peer_tablet = VERIFY_RESULT_OR_RETURN(LookupTabletPeerOrRespond(
      tablet_manager_, req->tablet_id(), resp, &context));
  auto tablet_peer = peer_tablet.tablet_peer;

  // Submit the vote request directly to the consensus instance.
  shared_ptr<Consensus> consensus;
  if (!GetConsensusOrRespond(tablet_peer, resp, &context, &consensus)) return;
  Status s = consensus->RequestVote(req, resp);
  RETURN_UNKNOWN_ERROR_IF_NOT_OK(s, resp, &context);
  context.RespondSuccess();
}

void ConsensusServiceImpl::ChangeConfig(const ChangeConfigRequestPB* req,
                                        ChangeConfigResponsePB* resp,
                                        RpcContext context) {
  VLOG(1) << "Received ChangeConfig RPC: " << req->ShortDebugString();
  // If the destination uuid is empty string, it means the client was retrying after a leader
  // stepdown and did not have a chance to update the uuid inside the request.
  // TODO: Note that this can be removed once Java YBClient will reset change config's uuid
  // correctly after leader step down.
  if (req->dest_uuid() != "" &&
      !CheckUuidMatchOrRespond(tablet_manager_, "ChangeConfig", req, resp, &context)) {
    return;
  }
  auto peer_tablet = VERIFY_RESULT_OR_RETURN(LookupTabletPeerOrRespond(
      tablet_manager_, req->tablet_id(), resp, &context));
  auto tablet_peer = peer_tablet.tablet_peer;

  shared_ptr<Consensus> consensus;
  if (!GetConsensusOrRespond(tablet_peer, resp, &context, &consensus)) return;
  boost::optional<TabletServerErrorPB::Code> error_code;
  std::shared_ptr<RpcContext> context_ptr = std::make_shared<RpcContext>(std::move(context));
  Status s = consensus->ChangeConfig(*req, BindHandleResponse(resp, context_ptr), &error_code);
  VLOG(1) << "Sent ChangeConfig req " << req->ShortDebugString() << " to consensus layer.";
  if (PREDICT_FALSE(!s.ok())) {
    HandleErrorResponse(resp, context_ptr.get(), s, error_code);
    return;
  }
  // The success case is handled when the callback fires.
}

void ConsensusServiceImpl::UnsafeChangeConfig(const UnsafeChangeConfigRequestPB* req,
                                              UnsafeChangeConfigResponsePB* resp,
                                              RpcContext context) {
  VLOG(1) << "Received UnsafeChangeConfig RPC: " << req->ShortDebugString();
  if (!CheckUuidMatchOrRespond(tablet_manager_, "UnsafeChangeConfig", req, resp, &context)) {
    return;
  }
  auto peer_tablet = VERIFY_RESULT_OR_RETURN(LookupTabletPeerOrRespond(
      tablet_manager_, req->tablet_id(), resp, &context));
  auto tablet_peer = peer_tablet.tablet_peer;

  shared_ptr<Consensus> consensus;
  if (!GetConsensusOrRespond(tablet_peer, resp, &context, &consensus)) {
    return;
  }
  boost::optional<TabletServerErrorPB::Code> error_code;
  const Status s = consensus->UnsafeChangeConfig(*req, &error_code);
  if (PREDICT_FALSE(!s.ok())) {
    SetupErrorAndRespond(resp->mutable_error(), s, &context);
    HandleErrorResponse(resp, &context, s, error_code);
    return;
  }
  context.RespondSuccess();
}

void ConsensusServiceImpl::GetNodeInstance(const GetNodeInstanceRequestPB* req,
                                           GetNodeInstanceResponsePB* resp,
                                           rpc::RpcContext context) {
  DVLOG(3) << "Received Get Node Instance RPC: " << req->DebugString();
  resp->mutable_node_instance()->CopyFrom(tablet_manager_->NodeInstance());
  auto status = tablet_manager_->GetRegistration(resp->mutable_registration());
  if (!status.ok()) {
    context.RespondFailure(status);
  } else {
    context.RespondSuccess();
  }
}

namespace {

class RpcScope {
 public:
  template<class Req, class Resp>
  RpcScope(TabletPeerLookupIf* tablet_manager,
           const char* method_name,
           const Req* req,
           Resp* resp,
           rpc::RpcContext* context)
      : context_(context) {
    if (!CheckUuidMatchOrRespond(tablet_manager, method_name, req, resp, context)) {
      return;
    }
    auto peer_tablet = VERIFY_RESULT_OR_RETURN(LookupTabletPeerOrRespond(
        tablet_manager, req->tablet_id(), resp, context));
    auto tablet_peer = peer_tablet.tablet_peer;

    if (!GetConsensusOrRespond(tablet_peer, resp, context, &consensus_)) {
      return;
    }
    responded_ = false;
  }

  ~RpcScope() {
    if (!responded_) {
      context_->RespondSuccess();
    }
  }

  template<class Resp>
  void CheckStatus(const Status& status, Resp* resp) {
    if (!status.ok()) {
      LOG(INFO) << "Status failed: " << status.ToString();
      SetupErrorAndRespond(resp->mutable_error(), status, context_);
      responded_ = true;
    }
  }

  Consensus* operator->() {
    return consensus_.get();
  }

  explicit operator bool() const {
    return !responded_;
  }

 private:
  rpc::RpcContext* context_;
  bool responded_ = true;
  shared_ptr<Consensus> consensus_;
};

} // namespace

void ConsensusServiceImpl::RunLeaderElection(const RunLeaderElectionRequestPB* req,
                                             RunLeaderElectionResponsePB* resp,
                                             rpc::RpcContext context) {
  VLOG(1) << "Received Run Leader Election RPC: " << req->DebugString();
  RpcScope scope(tablet_manager_, "RunLeaderElection", req, resp, &context);
  if (!scope) {
    return;
  }

  Status s = scope->StartElection(consensus::LeaderElectionData {
    .mode = consensus::ElectionMode::ELECT_EVEN_IF_LEADER_IS_ALIVE,
    .pending_commit = req->has_committed_index(),
    .must_be_committed_opid = OpId::FromPB(req->committed_index()),
    .originator_uuid = req->has_originator_uuid() ? req->originator_uuid() : std::string(),
    .suppress_vote_request = consensus::TEST_SuppressVoteRequest(req->suppress_vote_request()),
    .initial_election = req->initial_election() });
  scope.CheckStatus(s, resp);
}

void ConsensusServiceImpl::LeaderElectionLost(const consensus::LeaderElectionLostRequestPB *req,
                                              consensus::LeaderElectionLostResponsePB *resp,
                                              ::yb::rpc::RpcContext context) {
  LOG(INFO) << "LeaderElectionLost, req: " << req->ShortDebugString();
  RpcScope scope(tablet_manager_, "LeaderElectionLost", req, resp, &context);
  if (!scope) {
    return;
  }
  auto status = scope->ElectionLostByProtege(req->election_lost_by_uuid());
  scope.CheckStatus(status, resp);
  LOG(INFO) << "LeaderElectionLost, outcome: " << (scope ? "success" : "failure") << "req: "
            << req->ShortDebugString();
}

void ConsensusServiceImpl::LeaderStepDown(const LeaderStepDownRequestPB* req,
                                          LeaderStepDownResponsePB* resp,
                                          RpcContext context) {
  LOG(INFO) << "Received Leader stepdown RPC: " << req->ShortDebugString();

  if (PREDICT_FALSE(FLAGS_TEST_leader_stepdown_delay_ms > 0)) {
    LOG(INFO) << "Delaying leader stepdown for "
              << FLAGS_TEST_leader_stepdown_delay_ms << " ms.";
    SleepFor(MonoDelta::FromMilliseconds(FLAGS_TEST_leader_stepdown_delay_ms));
  }

  RpcScope scope(tablet_manager_, "LeaderStepDown", req, resp, &context);
  if (!scope) {
    return;
  }
  Status s = scope->StepDown(req, resp);
  LOG(INFO) << "Leader stepdown request " << req->ShortDebugString() << " success. Resp code="
            << TabletServerErrorPB::Code_Name(resp->error().code());
  scope.CheckStatus(s, resp);
}

void ConsensusServiceImpl::GetLastOpId(const consensus::GetLastOpIdRequestPB *req,
                                       consensus::GetLastOpIdResponsePB *resp,
                                       rpc::RpcContext context) {
  DVLOG(3) << "Received GetLastOpId RPC: " << req->DebugString();

  if (PREDICT_FALSE(req->opid_type() == consensus::UNKNOWN_OPID_TYPE)) {
    HandleErrorResponse(resp, &context,
                        STATUS(InvalidArgument, "Invalid opid_type specified to GetLastOpId()"));
    return;
  }

  if (!CheckUuidMatchOrRespond(tablet_manager_, "GetLastOpId", req, resp, &context)) {
    return;
  }
  auto peer_tablet = VERIFY_RESULT_OR_RETURN(LookupTabletPeerOrRespond(
      tablet_manager_, req->tablet_id(), resp, &context));
  auto tablet_peer = peer_tablet.tablet_peer;

  if (tablet_peer->state() != tablet::RUNNING) {
    SetupErrorAndRespond(resp->mutable_error(),
                         STATUS(ServiceUnavailable, "Tablet Peer not in RUNNING state"),
                         TabletServerErrorPB::TABLET_NOT_RUNNING, &context);
    return;
  }

  auto consensus = GetConsensusOrRespond(tablet_peer, resp, &context);
  if (!consensus) return;
  auto op_id = req->has_op_type()
      ? consensus->TEST_GetLastOpIdWithType(req->opid_type(), req->op_type())
      : consensus->GetLastOpId(req->opid_type());

  // RETURN_UNKNOWN_ERROR_IF_NOT_OK does not support Result, so have to add extra check here.
  if (!op_id.ok()) {
    RETURN_UNKNOWN_ERROR_IF_NOT_OK(op_id.status(), resp, &context);
  }
  op_id->ToPB(resp->mutable_opid());
  context.RespondSuccess();
}

void ConsensusServiceImpl::GetConsensusState(const consensus::GetConsensusStateRequestPB *req,
                                             consensus::GetConsensusStateResponsePB *resp,
                                             rpc::RpcContext context) {
  DVLOG(3) << "Received GetConsensusState RPC: " << req->DebugString();

  RpcScope scope(tablet_manager_, "GetConsensusState", req, resp, &context);
  if (!scope) {
    return;
  }
  ConsensusConfigType type = req->type();
  if (PREDICT_FALSE(type != CONSENSUS_CONFIG_ACTIVE && type != CONSENSUS_CONFIG_COMMITTED)) {
    HandleErrorResponse(resp, &context,
        STATUS(InvalidArgument, Substitute("Unsupported ConsensusConfigType $0 ($1)",
                                           ConsensusConfigType_Name(type), type)));
    return;
  }
  LeaderLeaseStatus leader_lease_status;
  *resp->mutable_cstate() = scope->ConsensusState(req->type(), &leader_lease_status);
  resp->set_leader_lease_status(leader_lease_status);
}

void ConsensusServiceImpl::StartRemoteBootstrap(const StartRemoteBootstrapRequestPB* req,
                                                StartRemoteBootstrapResponsePB* resp,
                                                rpc::RpcContext context) {
  if (!CheckUuidMatchOrRespond(tablet_manager_, "StartRemoteBootstrap", req, resp, &context)) {
    return;
  }
  if (req->has_split_parent_tablet_id()
      && !PREDICT_FALSE(FLAGS_TEST_disable_post_split_tablet_rbs_check)) {
    // For any tablet that was the result of a split, the raft group leader will always send the
    // split_parent_tablet_id. However, our local tablet manager should only know about the parent
    // if it was part of the raft group which committed the split to the parent, and if the parent
    // tablet has yet to be deleted across the cluster.
    TabletPeerTablet result;
    if (tablet_manager_->GetTabletPeer(req->split_parent_tablet_id(), &result.tablet_peer).ok()) {
      YB_LOG_EVERY_N_SECS(WARNING, 30)
          << "Start remote bootstrap rejected: parent tablet not yet split.";
      SetupErrorAndRespond(
          resp->mutable_error(),
          STATUS(Incomplete, "Rejecting bootstrap request while parent tablet is present."),
          TabletServerErrorPB::TABLET_SPLIT_PARENT_STILL_LIVE,
          &context);
      return;
    }
  }

  Status s = tablet_manager_->StartRemoteBootstrap(*req);
  if (!s.ok()) {
    // Using Status::AlreadyPresent for a remote bootstrap operation that is already in progress.
    if (s.IsAlreadyPresent()) {
      YB_LOG_EVERY_N_SECS(WARNING, 30) << "Start remote bootstrap failed: " << s;
      SetupErrorAndRespond(resp->mutable_error(), s, TabletServerErrorPB::ALREADY_IN_PROGRESS,
                           &context);
      return;
    } else {
      LOG(WARNING) << "Start remote bootstrap failed: " << s;
    }
  }

  RETURN_UNKNOWN_ERROR_IF_NOT_OK(s, resp, &context);
  context.RespondSuccess();
}

void TabletServiceImpl::NoOp(const NoOpRequestPB *req,
                             NoOpResponsePB *resp,
                             rpc::RpcContext context) {
  context.RespondSuccess();
}

void TabletServiceImpl::Publish(
    const PublishRequestPB* req, PublishResponsePB* resp, rpc::RpcContext context) {
  rpc::Publisher* publisher = server_->GetPublisher();
  resp->set_num_clients_forwarded_to(publisher ? (*publisher)(req->channel(), req->message()) : 0);
  context.RespondSuccess();
}

void TabletServiceImpl::ListTablets(const ListTabletsRequestPB* req,
                                    ListTabletsResponsePB* resp,
                                    rpc::RpcContext context) {
  TabletPeers peers = server_->tablet_manager()->GetTabletPeers();
  RepeatedPtrField<StatusAndSchemaPB>* peer_status = resp->mutable_status_and_schema();
  for (const TabletPeerPtr& peer : peers) {
    StatusAndSchemaPB* status = peer_status->Add();
    peer->GetTabletStatusPB(status->mutable_tablet_status());
    SchemaToPB(*peer->status_listener()->schema(), status->mutable_schema());
    peer->tablet_metadata()->partition_schema()->ToPB(status->mutable_partition_schema());
  }
  context.RespondSuccess();
}

void TabletServiceImpl::GetMasterAddresses(const GetMasterAddressesRequestPB* req,
                                           GetMasterAddressesResponsePB* resp,
                                           rpc::RpcContext context) {
  resp->set_master_addresses(server::MasterAddressesToString(
      *server_->tablet_manager()->server()->options().GetMasterAddresses()));
  context.RespondSuccess();
}

void TabletServiceImpl::GetLogLocation(
    const GetLogLocationRequestPB* req,
    GetLogLocationResponsePB* resp,
    rpc::RpcContext context) {
  resp->set_log_location(FLAGS_log_dir);
  context.RespondSuccess();
}

void TabletServiceImpl::ListTabletsForTabletServer(const ListTabletsForTabletServerRequestPB* req,
                                                   ListTabletsForTabletServerResponsePB* resp,
                                                   rpc::RpcContext context) {
  // Replicating logic from path-handlers.
  TabletPeers peers = server_->tablet_manager()->GetTabletPeers();
  for (const TabletPeerPtr& peer : peers) {
    TabletStatusPB status;
    peer->GetTabletStatusPB(&status);

    ListTabletsForTabletServerResponsePB::Entry* data_entry = resp->add_entries();
    data_entry->set_table_name(status.table_name());
    data_entry->set_tablet_id(status.tablet_id());

    std::shared_ptr<consensus::Consensus> consensus = peer->shared_consensus();
    data_entry->set_is_leader(consensus && consensus->role() == PeerRole::LEADER);
    data_entry->set_state(status.state());

    auto tablet = peer->shared_tablet();
    uint64_t num_sst_files = tablet ? tablet->GetCurrentVersionNumSSTFiles() : 0;
    data_entry->set_num_sst_files(num_sst_files);

    uint64_t num_log_segments = peer->GetNumLogSegments();
    data_entry->set_num_log_segments(num_log_segments);

    auto num_memtables = tablet ? tablet->GetNumMemtables() : std::make_pair(0, 0);
    data_entry->set_num_memtables_intents(num_memtables.first);
    data_entry->set_num_memtables_regular(num_memtables.second);
  }

  context.RespondSuccess();
}

namespace {

Result<uint64_t> CalcChecksum(tablet::Tablet* tablet, CoarseTimePoint deadline) {
  const shared_ptr<Schema> schema = tablet->metadata()->schema();
  auto client_schema = schema->CopyWithoutColumnIds();
  auto iter = tablet->NewRowIterator(client_schema, {}, "", deadline);
  RETURN_NOT_OK(iter);

  QLTableRow value_map;
  ScanResultChecksummer collector;

  while (VERIFY_RESULT((**iter).HasNext())) {
    RETURN_NOT_OK((**iter).NextRow(&value_map));
    collector.HandleRow(*schema, value_map);
  }

  return collector.agg_checksum();
}

} // namespace

Result<uint64_t> TabletServiceImpl::DoChecksum(
    const ChecksumRequestPB* req, CoarseTimePoint deadline) {
  auto abstract_tablet = VERIFY_RESULT(GetTablet(
      server_->tablet_peer_lookup(), req->tablet_id(), /* tablet_peer = */ nullptr,
      req->consistency_level(), AllowSplitTablet::kTrue));
  return CalcChecksum(down_cast<tablet::Tablet*>(abstract_tablet.get()), deadline);
}

void TabletServiceImpl::Checksum(const ChecksumRequestPB* req,
                                 ChecksumResponsePB* resp,
                                 rpc::RpcContext context) {
  VLOG(1) << "Full request: " << req->DebugString();

  auto checksum = DoChecksum(req, context.GetClientDeadline());
  if (!checksum.ok()) {
    SetupErrorAndRespond(resp->mutable_error(), checksum.status(), &context);
    return;
  }

  resp->set_checksum(*checksum);
  context.RespondSuccess();
}

void TabletServiceImpl::ImportData(const ImportDataRequestPB* req,
                                   ImportDataResponsePB* resp,
                                   rpc::RpcContext context) {
  auto peer = VERIFY_RESULT_OR_RETURN(LookupTabletPeerOrRespond(
      server_->tablet_peer_lookup(), req->tablet_id(), resp, &context));

  auto status = peer.tablet_peer->tablet()->ImportData(req->source_dir());
  if (!status.ok()) {
    SetupErrorAndRespond(resp->mutable_error(), status, &context);
    return;
  }
  context.RespondSuccess();
}

void TabletServiceImpl::GetTabletStatus(const GetTabletStatusRequestPB* req,
                                        GetTabletStatusResponsePB* resp,
                                        rpc::RpcContext context) {
  const Status s = server_->GetTabletStatus(req, resp);
  if (!s.ok()) {
    SetupErrorAndRespond(resp->mutable_error(), s,
                         s.IsNotFound() ? TabletServerErrorPB::TABLET_NOT_FOUND
                                        : TabletServerErrorPB::UNKNOWN_ERROR,
                         &context);
    return;
  }
  context.RespondSuccess();
}

void TabletServiceImpl::IsTabletServerReady(const IsTabletServerReadyRequestPB* req,
                                            IsTabletServerReadyResponsePB* resp,
                                            rpc::RpcContext context) {
  Status s = server_->tablet_manager()->GetNumTabletsPendingBootstrap(resp);
  if (!s.ok()) {
    SetupErrorAndRespond(resp->mutable_error(), s, &context);
    return;
  }
  context.RespondSuccess();
}

void TabletServiceImpl::TakeTransaction(const TakeTransactionRequestPB* req,
                                        TakeTransactionResponsePB* resp,
                                        rpc::RpcContext context) {
  auto transaction = server_->TransactionPool()->Take(
      client::ForceGlobalTransaction(req->has_is_global() && req->is_global()));
  auto metadata = transaction->Release();
  if (!metadata.ok()) {
    LOG(INFO) << "Take failed: " << metadata.status();
    context.RespondFailure(metadata.status());
    return;
  }
  metadata->ForceToPB(resp->mutable_metadata());
  VLOG(2) << "Taken metadata: " << metadata->ToString();
  context.RespondSuccess();
}

void TabletServiceImpl::GetSplitKey(
    const GetSplitKeyRequestPB* req, GetSplitKeyResponsePB* resp, RpcContext context) {
  TEST_PAUSE_IF_FLAG(TEST_pause_tserver_get_split_key);
  PerformAtLeader(req, resp, &context,
      [resp](const LeaderTabletPeer& leader_tablet_peer) -> Status {
        const auto& tablet = leader_tablet_peer.tablet;

        if (tablet->MayHaveOrphanedPostSplitData()) {
          return STATUS(IllegalState, "Tablet has orphaned post-split data");
        }
        const auto split_encoded_key = VERIFY_RESULT(tablet->GetEncodedMiddleSplitKey());
        resp->set_split_encoded_key(split_encoded_key);
        const auto doc_key_hash = VERIFY_RESULT(docdb::DecodeDocKeyHash(split_encoded_key));
        if (doc_key_hash.has_value()) {
          resp->set_split_partition_key(PartitionSchema::EncodeMultiColumnHashValue(
              doc_key_hash.value()));
        } else {
          resp->set_split_partition_key(split_encoded_key);
        }
        return Status::OK();
  });
}

void TabletServiceImpl::GetSharedData(const GetSharedDataRequestPB* req,
                                      GetSharedDataResponsePB* resp,
                                      rpc::RpcContext context) {
  auto& data = server_->SharedObject();
  resp->mutable_data()->assign(pointer_cast<const char*>(&data), sizeof(data));
  context.RespondSuccess();
}

void TabletServiceImpl::Shutdown() {
}

scoped_refptr<Histogram> TabletServer::GetMetricsHistogram(
    TabletServerServiceRpcMethodIndexes metric) {
  // Returns the metric Histogram by holding a lock to make sure tablet_server_service_ remains
  // unchanged during the operation.
  std::lock_guard<simple_spinlock> l(lock_);
  if (tablet_server_service_) {
    return tablet_server_service_->GetMetric(metric).handler_latency;
  }
  return nullptr;
}

TabletServerForwardServiceImpl::TabletServerForwardServiceImpl(TabletServiceImpl *impl,
                                                               TabletServerIf *server)
  : TabletServerForwardServiceIf(server->MetricEnt()),
    server_(server) {
}

void TabletServerForwardServiceImpl::Write(const WriteRequestPB* req,
                                           WriteResponsePB* resp,
                                           rpc::RpcContext context) {
  // Forward the rpc to the required Tserver.
  std::shared_ptr<ForwardWriteRpc> forward_rpc =
    std::make_shared<ForwardWriteRpc>(req, resp, std::move(context), server_->client());
  forward_rpc->SendRpc();
}

void TabletServerForwardServiceImpl::Read(const ReadRequestPB* req,
                                          ReadResponsePB* resp,
                                          rpc::RpcContext context) {
  std::shared_ptr<ForwardReadRpc> forward_rpc =
    std::make_shared<ForwardReadRpc>(req, resp, std::move(context), server_->client());
  forward_rpc->SendRpc();
}

}  // namespace tserver
}  // namespace yb
