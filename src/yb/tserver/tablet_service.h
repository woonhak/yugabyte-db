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
#ifndef YB_TSERVER_TABLET_SERVICE_H_
#define YB_TSERVER_TABLET_SERVICE_H_

#include <functional>
#include <future>
#include <memory>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/range/iterator_range.hpp>

#include "yb/common/common_fwd.h"

#include "yb/consensus/consensus.service.h"

#include "yb/gutil/ref_counted.h"

#include "yb/tablet/tablet_fwd.h"

#include "yb/tserver/read_query.h"
#include "yb/tserver/tserver_util_fwd.h"
#include "yb/tserver/tserver_fwd.h"
#include "yb/tserver/tserver_admin.pb.h"
#include "yb/tserver/tserver_admin.service.h"
#include "yb/tserver/tserver_forward_service.service.h"
#include "yb/tserver/tserver_service.service.h"

namespace yb {
class Schema;
class Status;
class HybridTime;

namespace tserver {

class TabletPeerLookupIf;
class TabletServer;

class TabletServiceImpl : public TabletServerServiceIf, public ReadTabletProvider {
 public:
  typedef std::vector<tablet::TabletPeerPtr> TabletPeers;

  explicit TabletServiceImpl(TabletServerIf* server);

  void Write(const WriteRequestPB* req, WriteResponsePB* resp, rpc::RpcContext context) override;

  void Read(const ReadRequestPB* req, ReadResponsePB* resp, rpc::RpcContext context) override;

  void VerifyTableRowRange(
      const VerifyTableRowRangeRequestPB* req, VerifyTableRowRangeResponsePB* resp,
      rpc::RpcContext context) override;

  void NoOp(const NoOpRequestPB* req, NoOpResponsePB* resp, rpc::RpcContext context) override;

  void Publish(
      const PublishRequestPB* req, PublishResponsePB* resp, rpc::RpcContext context) override;

  void ListTablets(const ListTabletsRequestPB* req,
                   ListTabletsResponsePB* resp,
                   rpc::RpcContext context) override;

  void GetMasterAddresses(const GetMasterAddressesRequestPB* req,
                          GetMasterAddressesResponsePB* resp,
                          rpc::RpcContext context) override;

  void ListTabletsForTabletServer(const ListTabletsForTabletServerRequestPB* req,
                                  ListTabletsForTabletServerResponsePB* resp,
                                  rpc::RpcContext context) override;

  void GetLogLocation(
      const GetLogLocationRequestPB* req,
      GetLogLocationResponsePB* resp,
      rpc::RpcContext context) override;

  void Checksum(const ChecksumRequestPB* req,
                ChecksumResponsePB* resp,
                rpc::RpcContext context) override;

  void ImportData(const ImportDataRequestPB* req,
                  ImportDataResponsePB* resp,
                  rpc::RpcContext context) override;

  void UpdateTransaction(const UpdateTransactionRequestPB* req,
                         UpdateTransactionResponsePB* resp,
                         rpc::RpcContext context) override;

  void GetTransactionStatus(const GetTransactionStatusRequestPB* req,
                            GetTransactionStatusResponsePB* resp,
                            rpc::RpcContext context) override;

  void GetTransactionStatusAtParticipant(const GetTransactionStatusAtParticipantRequestPB* req,
                                         GetTransactionStatusAtParticipantResponsePB* resp,
                                         rpc::RpcContext context) override;

  void AbortTransaction(const AbortTransactionRequestPB* req,
                        AbortTransactionResponsePB* resp,
                        rpc::RpcContext context) override;

  void Truncate(const TruncateRequestPB* req,
                TruncateResponsePB* resp,
                rpc::RpcContext context) override;

  void GetTabletStatus(const GetTabletStatusRequestPB* req,
                       GetTabletStatusResponsePB* resp,
                       rpc::RpcContext context) override;

  void IsTabletServerReady(const IsTabletServerReadyRequestPB* req,
                           IsTabletServerReadyResponsePB* resp,
                           rpc::RpcContext context) override;

  void GetSplitKey(
      const GetSplitKeyRequestPB* req,
      GetSplitKeyResponsePB* resp,
      rpc::RpcContext context) override;

  void TakeTransaction(const TakeTransactionRequestPB* req,
                       TakeTransactionResponsePB* resp,
                       rpc::RpcContext context) override;

  void GetSharedData(const GetSharedDataRequestPB* req,
                     GetSharedDataResponsePB* resp,
                     rpc::RpcContext context) override;

  void Shutdown() override;

 private:
  Result<std::shared_ptr<tablet::AbstractTablet>> GetTabletForRead(
    const TabletId& tablet_id, tablet::TabletPeerPtr tablet_peer,
    YBConsistencyLevel consistency_level, tserver::AllowSplitTablet allow_split_tablet) override;

  template<class Resp>
  bool CheckWriteThrottlingOrRespond(
      double score, tablet::TabletPeer* tablet_peer, Resp* resp, rpc::RpcContext* context);

  template <class Req, class Resp, class F>
  void PerformAtLeader(const Req& req, Resp* resp, rpc::RpcContext* context, const F& f);

  Result<uint64_t> DoChecksum(const ChecksumRequestPB* req, CoarseTimePoint deadline);

  TabletServerIf *const server_;
};

class TabletServiceAdminImpl : public TabletServerAdminServiceIf {
 public:
  typedef std::vector<tablet::TabletPeerPtr> TabletPeers;

  explicit TabletServiceAdminImpl(TabletServer* server);
  void CreateTablet(const CreateTabletRequestPB* req,
                    CreateTabletResponsePB* resp,
                    rpc::RpcContext context) override;

  void DeleteTablet(const DeleteTabletRequestPB* req,
                    DeleteTabletResponsePB* resp,
                    rpc::RpcContext context) override;

  void AlterSchema(const tablet::ChangeMetadataRequestPB* req,
                   ChangeMetadataResponsePB* resp,
                   rpc::RpcContext context) override;

  void CopartitionTable(const CopartitionTableRequestPB* req,
                        CopartitionTableResponsePB* resp,
                        rpc::RpcContext context) override;

  void FlushTablets(const FlushTabletsRequestPB* req,
                    FlushTabletsResponsePB* resp,
                    rpc::RpcContext context) override;

  void CountIntents(const CountIntentsRequestPB* req,
                    CountIntentsResponsePB* resp,
                    rpc::RpcContext context) override;

  void AddTableToTablet(const AddTableToTabletRequestPB* req,
                        AddTableToTabletResponsePB* resp,
                        rpc::RpcContext context) override;

  void RemoveTableFromTablet(const RemoveTableFromTabletRequestPB* req,
                             RemoveTableFromTabletResponsePB* resp,
                             rpc::RpcContext context) override;

  // Called on the Indexed table to choose time to read.
  void GetSafeTime(
      const GetSafeTimeRequestPB* req, GetSafeTimeResponsePB* resp,
      rpc::RpcContext context) override;

  // Called on the Indexed table to backfill the index table(s).
  void BackfillIndex(
      const BackfillIndexRequestPB* req, BackfillIndexResponsePB* resp,
      rpc::RpcContext context) override;

  // Called on the Index table(s) once the backfill is complete.
  void BackfillDone(
      const tablet::ChangeMetadataRequestPB* req, ChangeMetadataResponsePB* resp,
      rpc::RpcContext context) override;

  // Starts tablet splitting by adding split tablet Raft operation into Raft log of the source
  // tablet.
  void SplitTablet(
      const tablet::SplitTabletRequestPB* req,
      SplitTabletResponsePB* resp,
      rpc::RpcContext context) override;

  // Upgrade YSQL cluster (all databases) to the latest version, applying necessary migrations.
  void UpgradeYsql(
      const UpgradeYsqlRequestPB* req,
      UpgradeYsqlResponsePB* resp,
      rpc::RpcContext context) override;

 private:
  TabletServer* server_;

  CHECKED_STATUS DoCreateTablet(const CreateTabletRequestPB* req, CreateTabletResponsePB* resp);

  // Used to implement wait/signal mechanism for backfill requests.
  // Since the number of concurrently allowed backfill requests is
  // limited.
  mutable std::mutex backfill_lock_;
  std::condition_variable backfill_cond_;
  std::atomic<int32_t> num_tablets_backfilling_{0};
};

class ConsensusServiceImpl : public consensus::ConsensusServiceIf {
 public:
  ConsensusServiceImpl(const scoped_refptr<MetricEntity>& metric_entity,
                       TabletPeerLookupIf* tablet_manager_);

  virtual ~ConsensusServiceImpl();

  virtual void UpdateConsensus(const consensus::ConsensusRequestPB *req,
                               consensus::ConsensusResponsePB *resp,
                               rpc::RpcContext context) override;

  virtual void MultiRaftUpdateConsensus(const consensus::MultiRaftConsensusRequestPB *req,
                                        consensus::MultiRaftConsensusResponsePB *resp,
                                        rpc::RpcContext context) override;

  virtual void RequestConsensusVote(const consensus::VoteRequestPB* req,
                                    consensus::VoteResponsePB* resp,
                                    rpc::RpcContext context) override;

  virtual void ChangeConfig(const consensus::ChangeConfigRequestPB* req,
                            consensus::ChangeConfigResponsePB* resp,
                            rpc::RpcContext context) override;

  virtual void UnsafeChangeConfig(const consensus::UnsafeChangeConfigRequestPB* req,
                                  consensus::UnsafeChangeConfigResponsePB* resp,
                                  rpc::RpcContext context) override;

  virtual void GetNodeInstance(const consensus::GetNodeInstanceRequestPB* req,
                               consensus::GetNodeInstanceResponsePB* resp,
                               rpc::RpcContext context) override;

  virtual void RunLeaderElection(const consensus::RunLeaderElectionRequestPB* req,
                                 consensus::RunLeaderElectionResponsePB* resp,
                                 rpc::RpcContext context) override;

  virtual void LeaderElectionLost(const consensus::LeaderElectionLostRequestPB *req,
                                  consensus::LeaderElectionLostResponsePB *resp,
                                  ::yb::rpc::RpcContext context) override;

  virtual void LeaderStepDown(const consensus::LeaderStepDownRequestPB* req,
                              consensus::LeaderStepDownResponsePB* resp,
                              rpc::RpcContext context) override;

  virtual void GetLastOpId(const consensus::GetLastOpIdRequestPB *req,
                           consensus::GetLastOpIdResponsePB *resp,
                           rpc::RpcContext context) override;

  virtual void GetConsensusState(const consensus::GetConsensusStateRequestPB *req,
                                 consensus::GetConsensusStateResponsePB *resp,
                                 rpc::RpcContext context) override;

  virtual void StartRemoteBootstrap(const consensus::StartRemoteBootstrapRequestPB* req,
                                    consensus::StartRemoteBootstrapResponsePB* resp,
                                    rpc::RpcContext context) override;

 private:
  void CompleteUpdateConsensusResponse(std::shared_ptr<tablet::TabletPeer> tablet_peer,
                                       consensus::ConsensusResponsePB* resp);
  TabletPeerLookupIf* tablet_manager_;
};

class TabletServerForwardServiceImpl : public TabletServerForwardServiceIf {
 public:
  TabletServerForwardServiceImpl(TabletServiceImpl *impl,
                                 TabletServerIf* server);

  void Write(const WriteRequestPB* req, WriteResponsePB* resp, rpc::RpcContext context) override;

  void Read(const ReadRequestPB* req, ReadResponsePB* resp, rpc::RpcContext context) override;

 private:
  TabletServerIf *const server_;
};

}  // namespace tserver
}  // namespace yb

#endif  // YB_TSERVER_TABLET_SERVICE_H_
