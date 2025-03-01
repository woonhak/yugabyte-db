// Copyright (c) YugaByte, Inc.
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

#include "yb/yql/pggate/pg_op.h"

#include "yb/client/table.h"
#include "yb/client/yb_op.h"

#include "yb/common/partition.h"
#include "yb/common/pgsql_protocol.pb.h"
#include "yb/common/ql_scanspec.h"
#include "yb/common/ql_value.h"
#include "yb/common/schema.h"

#include "yb/docdb/doc_key.h"
#include "yb/docdb/doc_scanspec_util.h"
#include "yb/docdb/primitive_value_util.h"

#include "yb/yql/pggate/pg_tabledesc.h"

#include "yb/util/scope_exit.h"

namespace yb {
namespace pggate {

Status ReviewResponsePagingState(const PgTableDesc& table, PgsqlReadOp* op) {
  auto& response = op->response();
  if (table.num_hash_key_columns() > 0 ||
      op->read_request().is_forward_scan() ||
      !response.has_paging_state() ||
      !response.paging_state().has_next_partition_key() ||
      response.paging_state().has_next_row_key()) {
    return Status::OK();
  }
  // Backward scan of range key only table. next_row_key is not specified in paging state.
  // In this case next_partition_key must be corrected as now it points to the partition start key
  // of already scanned tablet. Partition start key of the preceding tablet must be used instead.
  // Also lower bound is checked here because DocDB can check upper bound only.
  const auto& current_next_partition_key = response.paging_state().next_partition_key();
  std::vector<docdb::PrimitiveValue> lower_bound, upper_bound;
  RETURN_NOT_OK(client::GetRangePartitionBounds(
      table.schema(), op->read_request(), &lower_bound, &upper_bound));
  if (!lower_bound.empty()) {
    docdb::DocKey current_key(table.schema());
    VERIFY_RESULT(current_key.DecodeFrom(
        current_next_partition_key, docdb::DocKeyPart::kWholeDocKey, docdb::AllowSpecial::kTrue));
    if (current_key.CompareTo(docdb::DocKey(std::move(lower_bound))) < 0) {
      response.clear_paging_state();
      return Status::OK();
    }
  }
  const auto& partitions = table.GetPartitions();
  const auto idx = client::FindPartitionStartIndex(partitions, current_next_partition_key);
  SCHECK_GT(
      idx, 0ULL,
      IllegalState, "Paging state for backward scan cannot point to first partition");
  SCHECK_EQ(
      partitions[idx], current_next_partition_key,
      IllegalState, "Paging state for backward scan must point to partition start key");
  const auto& next_partition_key = partitions[idx - 1];
  response.mutable_paging_state()->set_next_partition_key(next_partition_key);
  return Status::OK();
}

std::string PgsqlOp::ToString() const {
  return Format("{ $0 active: $1 read_time: $2 }",
                is_read() ? "READ" : "WRITE", active_, read_time_);
}

PgsqlReadOp::PgsqlReadOp(const PgTableDesc& desc) {
  read_request_.set_client(YQL_CLIENT_PGSQL);
  read_request_.set_table_id(desc.id().GetYBTableId());
  read_request_.set_schema_version(desc.schema_version());
  read_request_.set_stmt_id(reinterpret_cast<int64_t>(&read_request_));
}

CHECKED_STATUS PgsqlReadOp::InitPartitionKey(const PgTableDesc& table) {
  return client::InitPartitionKey(
       table.schema(), table.partition_schema(), table.LastPartition(), &read_request_);
}

CHECKED_STATUS PgsqlWriteOp::InitPartitionKey(const PgTableDesc& table) {
  return client::InitPartitionKey(table.schema(), table.partition_schema(), &write_request_);
}

}  // namespace pggate
}  // namespace yb
