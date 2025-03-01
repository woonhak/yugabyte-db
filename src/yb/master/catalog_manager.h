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

#ifndef YB_MASTER_CATALOG_MANAGER_H
#define YB_MASTER_CATALOG_MANAGER_H

#include <list>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <boost/optional/optional_fwd.hpp>
#include <boost/functional/hash.hpp>
#include <gtest/internal/gtest-internal.h>

#include "yb/common/constants.h"
#include "yb/common/entity_ids.h"
#include "yb/common/index.h"
#include "yb/common/partition.h"
#include "yb/common/transaction.h"
#include "yb/client/client_fwd.h"
#include "yb/gutil/macros.h"
#include "yb/gutil/ref_counted.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/gutil/thread_annotations.h"

#include "yb/master/catalog_entity_info.h"
#include "yb/master/catalog_manager_if.h"
#include "yb/master/catalog_manager_util.h"
#include "yb/master/master_dcl.fwd.h"
#include "yb/master/master_encryption.fwd.h"
#include "yb/master/master_defaults.h"
#include "yb/master/sys_catalog_initialization.h"
#include "yb/master/scoped_leader_shared_lock.h"
#include "yb/master/system_tablet.h"
#include "yb/master/tablet_split_candidate_filter.h"
#include "yb/master/tablet_split_driver.h"
#include "yb/master/tablet_split_manager.h"
#include "yb/master/ts_descriptor.h"
#include "yb/master/ts_manager.h"
#include "yb/master/ysql_tablespace_manager.h"
#include "yb/master/xcluster_split_driver.h"

#include "yb/rpc/rpc.h"
#include "yb/rpc/scheduler.h"
#include "yb/server/monitored_task.h"
#include "yb/tserver/tablet_peer_lookup.h"

#include "yb/util/debug/lock_debug.h"
#include "yb/util/locks.h"
#include "yb/util/monotime.h"
#include "yb/util/net/net_util.h"
#include "yb/util/pb_util.h"
#include "yb/util/promise.h"
#include "yb/util/random.h"
#include "yb/util/rw_mutex.h"
#include "yb/util/status_fwd.h"
#include "yb/util/test_macros.h"
#include "yb/util/version_tracker.h"

namespace yb {

class Schema;
class ThreadPool;

template<class T>
class AtomicGauge;

#define CALL_GTEST_TEST_CLASS_NAME_(...) GTEST_TEST_CLASS_NAME_(__VA_ARGS__)
namespace pgwrapper {
class CALL_GTEST_TEST_CLASS_NAME_(PgMiniTest, YB_DISABLE_TEST_IN_TSAN(DropDBMarkDeleted));
class CALL_GTEST_TEST_CLASS_NAME_(PgMiniTest, YB_DISABLE_TEST_IN_TSAN(DropDBUpdateSysTablet));
class CALL_GTEST_TEST_CLASS_NAME_(PgMiniTest, YB_DISABLE_TEST_IN_TSAN(DropDBWithTables));
}

class CALL_GTEST_TEST_CLASS_NAME_(MasterPartitionedTest, VerifyOldLeaderStepsDown);
#undef CALL_GTEST_TEST_CLASS_NAME_

namespace tablet {

struct TableInfo;
enum RaftGroupStatePB;

}

namespace master {

struct DeferredAssignmentActions;

using PlacementId = std::string;

typedef std::unordered_map<TabletId, TabletServerId> TabletToTabletServerMap;

typedef std::unordered_set<TableId> TableIdSet;

typedef std::unordered_map<TablespaceId, boost::optional<ReplicationInfoPB>>
  TablespaceIdToReplicationInfoMap;

typedef std::unordered_map<TableId, boost::optional<TablespaceId>> TableToTablespaceIdMap;

YB_STRONGLY_TYPED_BOOL(HideOnly);

typedef std::unordered_map<TableId, vector<scoped_refptr<TabletInfo>>> TableToTabletInfos;

// The component of the master which tracks the state and location
// of tables/tablets in the cluster.
//
// This is the master-side counterpart of TSTabletManager, which tracks
// the state of each tablet on a given tablet-server.
//
// Thread-safe.
class CatalogManager :
    public tserver::TabletPeerLookupIf,
    public TabletSplitCandidateFilterIf,
    public TabletSplitDriverIf,
    public CatalogManagerIf,
    public XClusterSplitDriverIf {
  typedef std::unordered_map<NamespaceName, scoped_refptr<NamespaceInfo> > NamespaceInfoMap;

  class NamespaceNameMapper {
   public:
    NamespaceInfoMap& operator[](YQLDatabase db_type);
    const NamespaceInfoMap& operator[](YQLDatabase db_type) const;
    void clear();

   private:
    std::array<NamespaceInfoMap, 4> typed_maps_;
  };

 public:
  explicit CatalogManager(Master *master);
  virtual ~CatalogManager();

  CHECKED_STATUS Init();

  bool StartShutdown();
  void CompleteShutdown();

  // Create Postgres sys catalog table.
  CHECKED_STATUS CreateYsqlSysTable(const CreateTableRequestPB* req, CreateTableResponsePB* resp);

  CHECKED_STATUS ReplicatePgMetadataChange(const tablet::ChangeMetadataRequestPB* req);

  // Reserve Postgres oids for a Postgres database.
  CHECKED_STATUS ReservePgsqlOids(const ReservePgsqlOidsRequestPB* req,
                                  ReservePgsqlOidsResponsePB* resp,
                                  rpc::RpcContext* rpc);

  // Get the info (current only version) for the ysql system catalog.
  CHECKED_STATUS GetYsqlCatalogConfig(const GetYsqlCatalogConfigRequestPB* req,
                                      GetYsqlCatalogConfigResponsePB* resp,
                                      rpc::RpcContext* rpc);

  // Copy Postgres sys catalog tables into a new namespace.
  CHECKED_STATUS CopyPgsqlSysTables(const NamespaceId& namespace_id,
                                    const std::vector<scoped_refptr<TableInfo>>& tables);

  // Create a new Table with the specified attributes.
  //
  // The RPC context is provided for logging/tracing purposes,
  // but this function does not itself respond to the RPC.
  CHECKED_STATUS CreateTable(const CreateTableRequestPB* req,
                             CreateTableResponsePB* resp,
                             rpc::RpcContext* rpc) override;

  // Create a new transaction status table.
  CHECKED_STATUS CreateTransactionStatusTable(const CreateTransactionStatusTableRequestPB* req,
                                              CreateTransactionStatusTableResponsePB* resp,
                                              rpc::RpcContext *rpc);

  // Create a transaction status table with the given name.
  CHECKED_STATUS CreateTransactionStatusTableInternal(rpc::RpcContext* rpc,
                                                      const string& table_name,
                                                      const TablespaceId* tablespace_id);

  // Check if there is a transaction table whose tablespace id matches the given tablespace id.
  bool DoesTransactionTableExistForTablespace(
      const TablespaceId& tablespace_id) EXCLUDES(mutex_);

  // Create a local transaction status table for a tablespace if needed
  // (i.e. if it does not exist already).
  //
  // This is called during CreateTable if the table has transactions enabled and is part
  // of a tablespace with a placement set.
  CHECKED_STATUS CreateLocalTransactionStatusTableIfNeeded(
      rpc::RpcContext *rpc, const TablespaceId& tablespace_id) EXCLUDES(mutex_);

  // Create the global transaction status table if needed (i.e. if it does not exist already).
  //
  // This is called at the end of CreateTable if the table has transactions enabled.
  CHECKED_STATUS CreateGlobalTransactionStatusTableIfNeeded(rpc::RpcContext *rpc);

  // Get tablet ids of the global transaction status table.
  CHECKED_STATUS GetGlobalTransactionStatusTablets(
      GetTransactionStatusTabletsResponsePB* resp) EXCLUDES(mutex_);

  // Get ids of transaction status tables matching a given placement.
  Result<std::vector<TableId>> GetPlacementLocalTransactionStatusTables(
      const CloudInfoPB& placement) EXCLUDES(mutex_);

  // Get tablet ids of local transaction status tables matching a given placement.
  CHECKED_STATUS GetPlacementLocalTransactionStatusTablets(
      const CloudInfoPB& placement,
      GetTransactionStatusTabletsResponsePB* resp) EXCLUDES(mutex_);

  // Get tablet ids of the global transaction status table and local transaction status tables
  // matching a given placement.
  CHECKED_STATUS GetTransactionStatusTablets(const GetTransactionStatusTabletsRequestPB* req,
                                             GetTransactionStatusTabletsResponsePB* resp,
                                             rpc::RpcContext *rpc) EXCLUDES(mutex_);

  // Create the metrics snapshots table if needed (i.e. if it does not exist already).
  //
  // This is called at the end of CreateTable.
  CHECKED_STATUS CreateMetricsSnapshotsTableIfNeeded(rpc::RpcContext *rpc);

  // Get the information about an in-progress create operation.
  CHECKED_STATUS IsCreateTableDone(const IsCreateTableDoneRequestPB* req,
                                   IsCreateTableDoneResponsePB* resp) override;

  CHECKED_STATUS IsCreateTableInProgress(const TableId& table_id,
                                         CoarseTimePoint deadline,
                                         bool* create_in_progress);

  CHECKED_STATUS WaitForCreateTableToFinish(const TableId& table_id);

  // Check if the transaction status table creation is done.
  //
  // This is called at the end of IsCreateTableDone if the table has transactions enabled.
  CHECKED_STATUS IsTransactionStatusTableCreated(IsCreateTableDoneResponsePB* resp);

  // Check if the metrics snapshots table creation is done.
  //
  // This is called at the end of IsCreateTableDone.
  CHECKED_STATUS IsMetricsSnapshotsTableCreated(IsCreateTableDoneResponsePB* resp);

  // Called when transaction associated with table create finishes. Verifies postgres layer present.
  CHECKED_STATUS VerifyTablePgLayer(scoped_refptr<TableInfo> table, bool txn_query_succeeded);

  // Truncate the specified table.
  //
  // The RPC context is provided for logging/tracing purposes,
  // but this function does not itself respond to the RPC.
  CHECKED_STATUS TruncateTable(const TruncateTableRequestPB* req,
                               TruncateTableResponsePB* resp,
                               rpc::RpcContext* rpc);

  // Get the information about an in-progress truncate operation.
  CHECKED_STATUS IsTruncateTableDone(const IsTruncateTableDoneRequestPB* req,
                                     IsTruncateTableDoneResponsePB* resp);

  // Backfill the specified index.  Currently only supported for YSQL.  YCQL does not need this as
  // master automatically runs backfill according to the DocDB permissions.
  CHECKED_STATUS BackfillIndex(const BackfillIndexRequestPB* req,
                               BackfillIndexResponsePB* resp,
                               rpc::RpcContext* rpc);

  // Gets the backfill jobs state associated with the requested table.
  CHECKED_STATUS GetBackfillJobs(const GetBackfillJobsRequestPB* req,
                                      GetBackfillJobsResponsePB* resp,
                                      rpc::RpcContext* rpc);

  // Backfill the indexes for the specified table.
  // Used for backfilling YCQL defered indexes when triggered from yb-admin.
  CHECKED_STATUS LaunchBackfillIndexForTable(const LaunchBackfillIndexForTableRequestPB* req,
                                             LaunchBackfillIndexForTableResponsePB* resp,
                                             rpc::RpcContext* rpc);

  // Delete the specified table.
  //
  // The RPC context is provided for logging/tracing purposes,
  // but this function does not itself respond to the RPC.
  CHECKED_STATUS DeleteTable(const DeleteTableRequestPB* req,
                             DeleteTableResponsePB* resp,
                             rpc::RpcContext* rpc);
  CHECKED_STATUS DeleteTableInternal(
      const DeleteTableRequestPB* req, DeleteTableResponsePB* resp, rpc::RpcContext* rpc);

  // Get the information about an in-progress delete operation.
  CHECKED_STATUS IsDeleteTableDone(const IsDeleteTableDoneRequestPB* req,
                                   IsDeleteTableDoneResponsePB* resp);

  // Alter the specified table.
  //
  // The RPC context is provided for logging/tracing purposes,
  // but this function does not itself respond to the RPC.
  CHECKED_STATUS AlterTable(const AlterTableRequestPB* req,
                            AlterTableResponsePB* resp,
                            rpc::RpcContext* rpc);

  // Get the information about an in-progress alter operation.
  CHECKED_STATUS IsAlterTableDone(const IsAlterTableDoneRequestPB* req,
                                  IsAlterTableDoneResponsePB* resp);

  // Get the information about the specified table.
  CHECKED_STATUS GetTableSchema(const GetTableSchemaRequestPB* req,
                                GetTableSchemaResponsePB* resp) override;
  CHECKED_STATUS GetTableSchemaInternal(const GetTableSchemaRequestPB* req,
                                        GetTableSchemaResponsePB* resp,
                                        bool get_fully_applied_indexes = false);

  // Get the information about the specified tablegroup.
  CHECKED_STATUS GetTablegroupSchema(const GetTablegroupSchemaRequestPB* req,
                                     GetTablegroupSchemaResponsePB* resp);

  // Get the information about the specified colocated databsae.
  CHECKED_STATUS GetColocatedTabletSchema(const GetColocatedTabletSchemaRequestPB* req,
                                          GetColocatedTabletSchemaResponsePB* resp);

  // List all the running tables.
  CHECKED_STATUS ListTables(const ListTablesRequestPB* req,
                            ListTablesResponsePB* resp) override;

  // Find the tablegroup associated with the given table.
  boost::optional<TablegroupId> FindTablegroupByTableId(const TableId& table_id);

  CHECKED_STATUS GetTableLocations(const GetTableLocationsRequestPB* req,
                                   GetTableLocationsResponsePB* resp) override;

  // Lookup tablet by ID, then call GetTabletLocations below.
  CHECKED_STATUS GetTabletLocations(
      const TabletId& tablet_id,
      TabletLocationsPB* locs_pb,
      IncludeInactive include_inactive) override;

  // Look up the locations of the given tablet. The locations
  // vector is overwritten (not appended to).
  // If the tablet is not found, returns Status::NotFound.
  // If the tablet is not running, returns Status::ServiceUnavailable.
  // Otherwise, returns Status::OK and puts the result in 'locs_pb'.
  // This only returns tablets which are in RUNNING state.
  CHECKED_STATUS GetTabletLocations(
      scoped_refptr<TabletInfo> tablet_info,
      TabletLocationsPB* locs_pb,
      IncludeInactive include_inactive) override;

  // Returns the system tablet in catalog manager by the id.
  Result<std::shared_ptr<tablet::AbstractTablet>> GetSystemTablet(const TabletId& id) override;

  // Handle a tablet report from the given tablet server.
  //
  // The RPC context is provided for logging/tracing purposes,
  // but this function does not itself respond to the RPC.
  CHECKED_STATUS ProcessTabletReport(TSDescriptor* ts_desc,
                                     const TabletReportPB& report,
                                     TabletReportUpdatesPB *report_update,
                                     rpc::RpcContext* rpc);

  // Create a new Namespace with the specified attributes.
  //
  // The RPC context is provided for logging/tracing purposes,
  // but this function does not itself respond to the RPC.
  CHECKED_STATUS CreateNamespace(const CreateNamespaceRequestPB* req,
                                 CreateNamespaceResponsePB* resp,
                                 rpc::RpcContext* rpc) override;
  // Get the information about an in-progress create operation.
  CHECKED_STATUS IsCreateNamespaceDone(const IsCreateNamespaceDoneRequestPB* req,
                                       IsCreateNamespaceDoneResponsePB* resp);

  // Delete the specified Namespace.
  //
  // The RPC context is provided for logging/tracing purposes,
  // but this function does not itself respond to the RPC.
  CHECKED_STATUS DeleteNamespace(const DeleteNamespaceRequestPB* req,
                                 DeleteNamespaceResponsePB* resp,
                                 rpc::RpcContext* rpc);
  // Get the information about an in-progress delete operation.
  CHECKED_STATUS IsDeleteNamespaceDone(const IsDeleteNamespaceDoneRequestPB* req,
                                       IsDeleteNamespaceDoneResponsePB* resp);

  // Alter the specified Namespace.
  CHECKED_STATUS AlterNamespace(const AlterNamespaceRequestPB* req,
                                AlterNamespaceResponsePB* resp,
                                rpc::RpcContext* rpc);

  // User API to Delete YSQL database tables.
  CHECKED_STATUS DeleteYsqlDatabase(const DeleteNamespaceRequestPB* req,
                                    DeleteNamespaceResponsePB* resp,
                                    rpc::RpcContext* rpc);

  // Work to delete YSQL database tables, handled asynchronously from the User API call.
  void DeleteYsqlDatabaseAsync(scoped_refptr<NamespaceInfo> database);

  // Work to delete YCQL database, handled asynchronously from the User API call.
  void DeleteYcqlDatabaseAsync(scoped_refptr<NamespaceInfo> database);

  // Delete all tables in YSQL database.
  CHECKED_STATUS DeleteYsqlDBTables(const scoped_refptr<NamespaceInfo>& database);

  // List all the current namespaces.
  CHECKED_STATUS ListNamespaces(const ListNamespacesRequestPB* req,
                                ListNamespacesResponsePB* resp);

  // Get information about a namespace.
  CHECKED_STATUS GetNamespaceInfo(const GetNamespaceInfoRequestPB* req,
                                  GetNamespaceInfoResponsePB* resp,
                                  rpc::RpcContext* rpc);

  // Set Redis Config
  CHECKED_STATUS RedisConfigSet(const RedisConfigSetRequestPB* req,
                                RedisConfigSetResponsePB* resp,
                                rpc::RpcContext* rpc);

  // Get Redis Config
  CHECKED_STATUS RedisConfigGet(const RedisConfigGetRequestPB* req,
                                RedisConfigGetResponsePB* resp,
                                rpc::RpcContext* rpc);

  CHECKED_STATUS CreateTablegroup(const CreateTablegroupRequestPB* req,
                                  CreateTablegroupResponsePB* resp,
                                  rpc::RpcContext* rpc);

  CHECKED_STATUS DeleteTablegroup(const DeleteTablegroupRequestPB* req,
                                  DeleteTablegroupResponsePB* resp,
                                  rpc::RpcContext* rpc);

  // List all the current tablegroups for a namespace.
  CHECKED_STATUS ListTablegroups(const ListTablegroupsRequestPB* req,
                                 ListTablegroupsResponsePB* resp,
                                 rpc::RpcContext* rpc);

  bool HasTablegroups() override;

  // Create a new User-Defined Type with the specified attributes.
  //
  // The RPC context is provided for logging/tracing purposes,
  // but this function does not itself respond to the RPC.
  CHECKED_STATUS CreateUDType(const CreateUDTypeRequestPB* req,
                              CreateUDTypeResponsePB* resp,
                              rpc::RpcContext* rpc);

  // Delete the specified UDType.
  //
  // The RPC context is provided for logging/tracing purposes,
  // but this function does not itself respond to the RPC.
  CHECKED_STATUS DeleteUDType(const DeleteUDTypeRequestPB* req,
                              DeleteUDTypeResponsePB* resp,
                              rpc::RpcContext* rpc);

  // List all user defined types in given namespaces.
  CHECKED_STATUS ListUDTypes(const ListUDTypesRequestPB* req,
                             ListUDTypesResponsePB* resp);

  // Get the info (id, name, namespace, fields names, field types) of a (user-defined) type.
  CHECKED_STATUS GetUDTypeInfo(const GetUDTypeInfoRequestPB* req,
                               GetUDTypeInfoResponsePB* resp,
                               rpc::RpcContext* rpc);

  // Delete CDC streams for a table.
  virtual CHECKED_STATUS DeleteCDCStreamsForTable(const TableId& table_id) EXCLUDES(mutex_);
  virtual CHECKED_STATUS DeleteCDCStreamsForTables(const vector<TableId>& table_ids)
      EXCLUDES(mutex_);

  virtual CHECKED_STATUS ChangeEncryptionInfo(const ChangeEncryptionInfoRequestPB* req,
                                              ChangeEncryptionInfoResponsePB* resp);

  CHECKED_STATUS UpdateXClusterConsumerOnTabletSplit(
      const TableId& consumer_table_id, const SplitTabletIds& split_tablet_ids) override {
    // Default value.
    return Status::OK();
  }

  CHECKED_STATUS UpdateXClusterProducerOnTabletSplit(
      const TableId& producer_table_id, const SplitTabletIds& split_tablet_ids) override {
    // Default value.
    return Status::OK();
  }

  Result<uint64_t> IncrementYsqlCatalogVersion() override;

  // Records the fact that initdb has succesfully completed.
  CHECKED_STATUS InitDbFinished(Status initdb_status, int64_t term);

  // Check if the initdb operation has been completed. This is intended for use by whoever wants
  // to wait for the cluster to be fully initialized, e.g. minicluster, YugaWare, etc.
  CHECKED_STATUS IsInitDbDone(
      const IsInitDbDoneRequestPB* req, IsInitDbDoneResponsePB* resp) override;

  CHECKED_STATUS GetYsqlCatalogVersion(
      uint64_t* catalog_version, uint64_t* last_breaking_version) override;

  CHECKED_STATUS InitializeTransactionTablesConfig(int64_t term);

  CHECKED_STATUS IncrementTransactionTablesVersion();

  uint64_t GetTransactionTablesVersion() override;

  virtual CHECKED_STATUS FillHeartbeatResponse(const TSHeartbeatRequestPB* req,
                                               TSHeartbeatResponsePB* resp);

  SysCatalogTable* sys_catalog() override { return sys_catalog_.get(); }

  // Tablet peer for the sys catalog tablet's peer.
  std::shared_ptr<tablet::TabletPeer> tablet_peer() const override;

  ClusterLoadBalancer* load_balancer() override { return load_balance_policy_.get(); }

  TabletSplitManager* tablet_split_manager() override { return &tablet_split_manager_; }

  // Dump all of the current state about tables and tablets to the
  // given output stream. This is verbose, meant for debugging.
  void DumpState(std::ostream* out, bool on_disk_dump = false) const override;

  void SetLoadBalancerEnabled(bool is_enabled);

  bool IsLoadBalancerEnabled() override;

  // Return the table info for the table with the specified UUID, if it exists.
  TableInfoPtr GetTableInfo(const TableId& table_id) override;
  TableInfoPtr GetTableInfoUnlocked(const TableId& table_id) REQUIRES_SHARED(mutex_);

  // Get Table info given namespace id and table name.
  // Does not work for YSQL tables because of possible ambiguity.
  scoped_refptr<TableInfo> GetTableInfoFromNamespaceNameAndTableName(
      YQLDatabase db_type, const NamespaceName& namespace_name,
      const TableName& table_name) override;

  // Return TableInfos according to specified mode.
  std::vector<TableInfoPtr> GetTables(GetTablesMode mode) override;

  // Return all the available NamespaceInfo. The flag 'includeOnlyRunningNamespaces' determines
  // whether to retrieve all Namespaces irrespective of their state or just 'RUNNING' namespaces.
  // To retrieve all live tables in the system, you should set this flag to true.
  void GetAllNamespaces(std::vector<scoped_refptr<NamespaceInfo> >* namespaces,
                        bool include_only_running_namespaces = false) override;

  // Return all the available (user-defined) types.
  void GetAllUDTypes(std::vector<scoped_refptr<UDTypeInfo>>* types) override;

  // Return the recent tasks.
  std::vector<std::shared_ptr<server::MonitoredTask>> GetRecentTasks() override;

  // Return the recent user-initiated jobs.
  std::vector<std::shared_ptr<server::MonitoredTask>> GetRecentJobs() override;

  NamespaceName GetNamespaceNameUnlocked(const NamespaceId& id) const REQUIRES_SHARED(mutex_);
  NamespaceName GetNamespaceName(const NamespaceId& id) const override;

  NamespaceName GetNamespaceNameUnlocked(const scoped_refptr<TableInfo>& table) const
      REQUIRES_SHARED(mutex_);
  NamespaceName GetNamespaceName(const scoped_refptr<TableInfo>& table) const;

  // Is the table a system table?
  bool IsSystemTable(const TableInfo& table) const override;

  // Is the table a user created table?
  bool IsUserTable(const TableInfo& table) const override;
  bool IsUserTableUnlocked(const TableInfo& table) const REQUIRES_SHARED(mutex_);

  // Is the table a user created index?
  bool IsUserIndex(const TableInfo& table) const override;
  bool IsUserIndexUnlocked(const TableInfo& table) const REQUIRES_SHARED(mutex_);

  // Is the table a special sequences system table?
  bool IsSequencesSystemTable(const TableInfo& table) const;

  // Is the table id from a tablegroup?
  bool IsTablegroupParentTableId(const TableId& table_id) const;

  // Is the table id from a table created for colocated database?
  bool IsColocatedParentTableId(const TableId& table_id) const;

  // Is the table created by user?
  // Note that table can be regular table or index in this case.
  bool IsUserCreatedTable(const TableInfo& table) const override;
  bool IsUserCreatedTableUnlocked(const TableInfo& table) const REQUIRES_SHARED(mutex_);

  // Let the catalog manager know that we have received a response for a delete tablet request,
  // and that we either deleted the tablet successfully, or we received a fatal error.
  //
  // Async tasks should call this when they finish. The last such tablet peer notification will
  // trigger trying to transition the table from DELETING to DELETED state.
  void NotifyTabletDeleteFinished(
      const TabletServerId& tserver_uuid, const TableId& table_id,
      const TableInfoPtr& table) override;

  // For a DeleteTable, we first mark tables as DELETING then move them to DELETED once all
  // outstanding tasks are complete and the TS side tablets are deleted.
  // For system tables or colocated tables, we just need outstanding tasks to be done.
  //
  // If all conditions are met, returns a locked write lock on this table.
  // Otherwise lock is default constructed, i.e. not locked.
  TableInfo::WriteLock MaybeTransitionTableToDeleted(const TableInfoPtr& table);

  // Used by ConsensusService to retrieve the TabletPeer for a system
  // table specified by 'tablet_id'.
  //
  // See also: TabletPeerLookupIf, ConsensusServiceImpl.
  CHECKED_STATUS GetTabletPeer(
      const TabletId& tablet_id,
      std::shared_ptr<tablet::TabletPeer>* tablet_peer) const override;

  const NodeInstancePB& NodeInstance() const override;

  CHECKED_STATUS GetRegistration(ServerRegistrationPB* reg) const override;

  bool IsInitialized() const;

  virtual CHECKED_STATUS StartRemoteBootstrap(const consensus::StartRemoteBootstrapRequestPB& req)
      override;

  // Checks that placement info can be accommodated by available ts_descs.
  CHECKED_STATUS CheckValidPlacementInfo(const PlacementInfoPB& placement_info,
                                         const TSDescriptorVector& ts_descs,
                                         ValidateReplicationInfoResponsePB* resp);

  // Loops through the table's placement infos and populates the corresponding config from
  // each placement.
  CHECKED_STATUS HandlePlacementUsingReplicationInfo(
      const ReplicationInfoPB& replication_info,
      const TSDescriptorVector& all_ts_descs,
      consensus::RaftConfigPB* config,
      CMPerTableLoadState* per_table_state,
      CMGlobalLoadState* global_state);

  // Handles the config creation for a given placement.
  CHECKED_STATUS HandlePlacementUsingPlacementInfo(const PlacementInfoPB& placement_info,
                                                   const TSDescriptorVector& ts_descs,
                                                   consensus::PeerMemberType member_type,
                                                   consensus::RaftConfigPB* config,
                                                   CMPerTableLoadState* per_table_state,
                                                   CMGlobalLoadState* global_state);

  // Populates ts_descs with all tservers belonging to a certain placement.
  void GetTsDescsFromPlacementInfo(const PlacementInfoPB& placement_info,
                                   const TSDescriptorVector& all_ts_descs,
                                   TSDescriptorVector* ts_descs);

    // Set the current committed config.
  CHECKED_STATUS GetCurrentConfig(consensus::ConsensusStatePB *cpb) const override;

  // Return OK if this CatalogManager is a leader in a consensus configuration and if
  // the required leader state (metadata for tables and tablets) has
  // been successfully loaded into memory. CatalogManager must be
  // initialized before calling this method.
  CHECKED_STATUS CheckIsLeaderAndReady() const override;

  // Returns this CatalogManager's role in a consensus configuration. CatalogManager
  // must be initialized before calling this method.
  PeerRole Role() const;

  CHECKED_STATUS PeerStateDump(const vector<consensus::RaftPeerPB>& masters_raft,
                               const DumpMasterStateRequestPB* req,
                               DumpMasterStateResponsePB* resp);

  // If we get removed from an existing cluster, leader might ask us to detach ourselves from the
  // cluster. So we enter a shell mode equivalent state, with no bg tasks and no tablet peer
  // nor consensus.
  CHECKED_STATUS GoIntoShellMode();

  // Setters and getters for the cluster config item.
  //
  // To change the cluster config, a client would need to do a client-side read-modify-write by
  // issuing a get for the latest config, obtaining the current valid config (together with its
  // respective version number), modify the values it wants of said config and issuing a write
  // afterwards, without changing the version number. In case the version number does not match
  // on the server, the change will fail and the client will have to retry the get, as someone
  // must havGetTableInfoe updated the config in the meantime.
  CHECKED_STATUS GetClusterConfig(GetMasterClusterConfigResponsePB* resp) override;
  CHECKED_STATUS GetClusterConfig(SysClusterConfigEntryPB* config) override;

  CHECKED_STATUS SetClusterConfig(
      const ChangeMasterClusterConfigRequestPB* req,
      ChangeMasterClusterConfigResponsePB* resp) override;


  // Validator for placement information with respect to cluster configuration
  CHECKED_STATUS ValidateReplicationInfo(
      const ValidateReplicationInfoRequestPB* req, ValidateReplicationInfoResponsePB* resp);

  CHECKED_STATUS SetPreferredZones(
      const SetPreferredZonesRequestPB* req, SetPreferredZonesResponsePB* resp);

  Result<size_t> GetReplicationFactor() override;
  Result<size_t> GetReplicationFactorForTablet(const scoped_refptr<TabletInfo>& tablet);

  void GetExpectedNumberOfReplicas(int* num_live_replicas, int* num_read_replicas);

  // Get the percentage of tablets that have been moved off of the black-listed tablet servers.
  CHECKED_STATUS GetLoadMoveCompletionPercent(GetLoadMovePercentResponsePB* resp);

  // Get the percentage of leaders that have been moved off of the leader black-listed tablet
  // servers.
  CHECKED_STATUS GetLeaderBlacklistCompletionPercent(GetLoadMovePercentResponsePB* resp);

  // Get the percentage of leaders/tablets that have been moved off of the (leader) black-listed
  // tablet servers.
  CHECKED_STATUS GetLoadMoveCompletionPercent(GetLoadMovePercentResponsePB* resp,
      bool blacklist_leader);

  // API to check if all the live tservers have similar tablet workload.
  CHECKED_STATUS IsLoadBalanced(const IsLoadBalancedRequestPB* req,
                                IsLoadBalancedResponsePB* resp) override;

  CHECKED_STATUS IsLoadBalancerIdle(const IsLoadBalancerIdleRequestPB* req,
                                    IsLoadBalancerIdleResponsePB* resp);

  // API to check that all tservers that shouldn't have leader load do not.
  CHECKED_STATUS AreLeadersOnPreferredOnly(const AreLeadersOnPreferredOnlyRequestPB* req,
                                           AreLeadersOnPreferredOnlyResponsePB* resp);

  // Return the placement uuid of the primary cluster containing this master.
  string placement_uuid() const;

  // Clears out the existing metadata ('table_names_map_', 'table_ids_map_',
  // and 'tablet_map_'), loads tables metadata into memory and if successful
  // loads the tablets metadata.
  CHECKED_STATUS VisitSysCatalog(int64_t term) override;
  virtual CHECKED_STATUS RunLoaders(int64_t term) REQUIRES(mutex_);

  // Waits for the worker queue to finish processing, returns OK if worker queue is idle before
  // the provided timeout, TimedOut Status otherwise.
  CHECKED_STATUS WaitForWorkerPoolTests(
      const MonoDelta& timeout = MonoDelta::FromSeconds(10)) const override;

  Result<scoped_refptr<NamespaceInfo>> FindNamespaceUnlocked(
      const NamespaceIdentifierPB& ns_identifier) const REQUIRES_SHARED(mutex_);

  Result<scoped_refptr<NamespaceInfo>> FindNamespace(
      const NamespaceIdentifierPB& ns_identifier) const EXCLUDES(mutex_);

  Result<scoped_refptr<NamespaceInfo>> FindNamespaceById(
      const NamespaceId& id) const override EXCLUDES(mutex_);

  Result<scoped_refptr<NamespaceInfo>> FindNamespaceByIdUnlocked(
      const NamespaceId& id) const REQUIRES_SHARED(mutex_);

  Result<scoped_refptr<TableInfo>> FindTableUnlocked(
      const TableIdentifierPB& table_identifier) const REQUIRES_SHARED(mutex_);

  Result<scoped_refptr<TableInfo>> FindTable(
      const TableIdentifierPB& table_identifier) const override EXCLUDES(mutex_);

  Result<scoped_refptr<TableInfo>> FindTableById(
      const TableId& table_id) const override EXCLUDES(mutex_);

  Result<scoped_refptr<TableInfo>> FindTableByIdUnlocked(
      const TableId& table_id) const REQUIRES_SHARED(mutex_);

  Result<bool> TableExists(
      const std::string& namespace_name, const std::string& table_name) const EXCLUDES(mutex_);

  Result<TableDescription> DescribeTable(
      const TableIdentifierPB& table_identifier, bool succeed_if_create_in_progress);

  Result<TableDescription> DescribeTable(
      const TableInfoPtr& table_info, bool succeed_if_create_in_progress);

  Result<std::string> GetPgSchemaName(const TableInfoPtr& table_info) REQUIRES_SHARED(mutex_);

  void AssertLeaderLockAcquiredForReading() const override {
    leader_lock_.AssertAcquiredForReading();
  }

  std::string GenerateId() override {
    return GenerateId(boost::none);
  }

  std::string GenerateId(boost::optional<const SysRowEntryType> entity_type);
  std::string GenerateIdUnlocked(boost::optional<const SysRowEntryType> entity_type = boost::none)
      REQUIRES_SHARED(mutex_);

  ThreadPool* AsyncTaskPool() override { return async_task_pool_.get(); }

  PermissionsManager* permissions_manager() override {
    return permissions_manager_.get();
  }

  intptr_t tablets_version() const override NO_THREAD_SAFETY_ANALYSIS {
    // This method should not hold the lock, because Version method is thread safe.
    return tablet_map_.Version() + table_ids_map_.Version();
  }

  intptr_t tablet_locations_version() const override {
    return tablet_locations_version_.load(std::memory_order_acquire);
  }

  EncryptionManager& encryption_manager() {
    return *encryption_manager_;
  }

  client::UniverseKeyClient& universe_key_client() {
    return *universe_key_client_;
  }

  CHECKED_STATUS FlushSysCatalog(const FlushSysCatalogRequestPB* req,
                                 FlushSysCatalogResponsePB* resp,
                                 rpc::RpcContext* rpc);

  CHECKED_STATUS CompactSysCatalog(const CompactSysCatalogRequestPB* req,
                                   CompactSysCatalogResponsePB* resp,
                                   rpc::RpcContext* rpc);

  CHECKED_STATUS SplitTablet(const TabletId& tablet_id, bool select_all_tablets_for_split) override;

  // Splits tablet specified in the request using middle of the partition as a split point.
  CHECKED_STATUS SplitTablet(
      const SplitTabletRequestPB* req, SplitTabletResponsePB* resp, rpc::RpcContext* rpc);

  // Deletes a tablet that is no longer serving user requests. This would require that the tablet
  // has been split and both of its children are now in RUNNING state and serving user requests
  // instead.
  CHECKED_STATUS DeleteNotServingTablet(
      const DeleteNotServingTabletRequestPB* req, DeleteNotServingTabletResponsePB* resp,
      rpc::RpcContext* rpc);

  CHECKED_STATUS DdlLog(
      const DdlLogRequestPB* req, DdlLogResponsePB* resp, rpc::RpcContext* rpc);

  // Test wrapper around protected DoSplitTablet method.
  CHECKED_STATUS TEST_SplitTablet(
      const scoped_refptr<TabletInfo>& source_tablet_info,
      docdb::DocKeyHash split_hash_code) override;

  CHECKED_STATUS TEST_SplitTablet(
      const TabletId& tablet_id, const std::string& split_encoded_key,
      const std::string& split_partition_key) override;

  CHECKED_STATUS TEST_IncrementTablePartitionListVersion(const TableId& table_id) override;

  // Schedule a task to run on the async task thread pool.
  CHECKED_STATUS ScheduleTask(std::shared_ptr<RetryingTSRpcTask> task) override;

  // Time since this peer became master leader. Caller should verify that it is leader before.
  MonoDelta TimeSinceElectedLeader();

  Result<std::vector<TableDescription>> CollectTables(
      const google::protobuf::RepeatedPtrField<TableIdentifierPB>& table_identifiers,
      bool add_indexes,
      bool include_parent_colocated_table = false) override;

  Result<std::vector<TableDescription>> CollectTables(
      const google::protobuf::RepeatedPtrField<TableIdentifierPB>& table_identifiers,
      CollectFlags flags,
      std::unordered_set<NamespaceId>* namespaces = nullptr);

  // Returns 'table_replication_info' itself if set. Else looks up placement info for its
  // 'tablespace_id'. If neither is set, returns the cluster level replication info.
  Result<ReplicationInfoPB> GetTableReplicationInfo(
      const ReplicationInfoPB& table_replication_info,
      const TablespaceId& tablespace_id) override;

  Result<boost::optional<TablespaceId>> GetTablespaceForTable(
      const scoped_refptr<TableInfo>& table) override;

  void ProcessTabletStorageMetadata(
      const std::string& ts_uuid,
      const TabletDriveStorageMetadataPB& storage_metadata);

  void CheckTableDeleted(const TableInfoPtr& table) override;

  bool ShouldSplitValidCandidate(
      const TabletInfo& tablet_info, const TabletReplicaDriveInfo& drive_info) const override;

  BlacklistSet BlacklistSetFromPB() const override;

  std::vector<std::string> GetMasterAddresses();

 protected:
  // TODO Get rid of these friend classes and introduce formal interface.
  friend class TableLoader;
  friend class TabletLoader;
  friend class NamespaceLoader;
  friend class UDTypeLoader;
  friend class ClusterConfigLoader;
  friend class RoleLoader;
  friend class RedisConfigLoader;
  friend class SysConfigLoader;
  friend class ::yb::master::ScopedLeaderSharedLock;
  friend class PermissionsManager;
  friend class MultiStageAlterTable;
  friend class BackfillTable;
  friend class BackfillTablet;

  FRIEND_TEST(SysCatalogTest, TestCatalogManagerTasksTracker);
  FRIEND_TEST(SysCatalogTest, TestPrepareDefaultClusterConfig);
  FRIEND_TEST(SysCatalogTest, TestSysCatalogTablesOperations);
  FRIEND_TEST(SysCatalogTest, TestSysCatalogTabletsOperations);
  FRIEND_TEST(SysCatalogTest, TestTableInfoCommit);

  FRIEND_TEST(MasterTest, TestTabletsDeletedWhenTableInDeletingState);
  FRIEND_TEST(yb::MasterPartitionedTest, VerifyOldLeaderStepsDown);

  // Called by SysCatalog::SysCatalogStateChanged when this node
  // becomes the leader of a consensus configuration.
  //
  // Executes LoadSysCatalogDataTask below and marks the current time as time since leader.
  CHECKED_STATUS ElectedAsLeaderCb();

  // Loops and sleeps until one of the following conditions occurs:
  // 1. The current node is the leader master in the current term
  //    and at least one op from the current term is committed. Returns OK.
  // 2. The current node is not the leader master.
  //    Returns IllegalState.
  // 3. The provided timeout expires. Returns TimedOut.
  //
  // This method is intended to ensure that all operations replicated by
  // previous masters are committed and visible to the local node before
  // reading that data, to ensure consistency across failovers.
  CHECKED_STATUS WaitUntilCaughtUpAsLeader(const MonoDelta& timeout) override;

  // This method is submitted to 'leader_initialization_pool_' by
  // ElectedAsLeaderCb above. It:
  // 1) Acquired 'lock_'
  // 2) Runs the various Visitors defined below
  // 3) Releases 'lock_' and if successful, updates 'leader_ready_term_'
  // to true (under state_lock_).
  void LoadSysCatalogDataTask();

  // This method checks that resource such as keyspace is available for GrantRevokePermission
  // request.
  // Since this method takes lock on mutex_, it is separated out of permissions manager
  // so that the thread safety relationship between the two managers is easy to reason about.
  CHECKED_STATUS CheckResource(const GrantRevokePermissionRequestPB* req,
                               GrantRevokePermissionResponsePB* resp);

  // Generated the default entry for the cluster config, that is written into sys_catalog on very
  // first leader election of the cluster.
  //
  // Sets the version field of the SysClusterConfigEntryPB to 0.
  CHECKED_STATUS PrepareDefaultClusterConfig(int64_t term) REQUIRES(mutex_);

  // Sets up various system configs.
  CHECKED_STATUS PrepareDefaultSysConfig(int64_t term) REQUIRES(mutex_);

  // Starts an asynchronous run of initdb. Errors are handled in the callback. Returns true
  // if started running initdb, false if decided that it is not needed.
  bool StartRunningInitDbIfNeeded(int64_t term) REQUIRES_SHARED(mutex_);

  CHECKED_STATUS PrepareDefaultNamespaces(int64_t term) REQUIRES(mutex_);

  CHECKED_STATUS PrepareSystemTables(int64_t term) REQUIRES(mutex_);

  CHECKED_STATUS PrepareSysCatalogTable(int64_t term) REQUIRES(mutex_);

  template <class T>
  CHECKED_STATUS PrepareSystemTableTemplate(const TableName& table_name,
                                            const NamespaceName& namespace_name,
                                            const NamespaceId& namespace_id,
                                            int64_t term) REQUIRES(mutex_);

  CHECKED_STATUS PrepareSystemTable(const TableName& table_name,
                                    const NamespaceName& namespace_name,
                                    const NamespaceId& namespace_id,
                                    const Schema& schema,
                                    int64_t term,
                                    YQLVirtualTable* vtable) REQUIRES(mutex_);

  CHECKED_STATUS PrepareNamespace(YQLDatabase db_type,
                                  const NamespaceName& name,
                                  const NamespaceId& id,
                                  int64_t term) REQUIRES(mutex_);

  void ProcessPendingNamespace(NamespaceId id,
                               std::vector<scoped_refptr<TableInfo>> template_tables,
                               TransactionMetadata txn);

  // Called when transaction associated with NS create finishes. Verifies postgres layer present.
  CHECKED_STATUS VerifyNamespacePgLayer(scoped_refptr<NamespaceInfo> ns, bool txn_query_succeeded);

  CHECKED_STATUS ConsensusStateToTabletLocations(const consensus::ConsensusStatePB& cstate,
                                                 TabletLocationsPB* locs_pb);

  // Creates the table and associated tablet objects in-memory and updates the appropriate
  // catalog manager maps.
  CHECKED_STATUS CreateTableInMemory(const CreateTableRequestPB& req,
                                     const Schema& schema,
                                     const PartitionSchema& partition_schema,
                                     const NamespaceId& namespace_id,
                                     const NamespaceName& namespace_name,
                                     const vector<Partition>& partitions,
                                     IndexInfoPB* index_info,
                                     TabletInfos* tablets,
                                     CreateTableResponsePB* resp,
                                     scoped_refptr<TableInfo>* table) REQUIRES(mutex_);

  Result<TabletInfos> CreateTabletsFromTable(const vector<Partition>& partitions,
                                             const TableInfoPtr& table) REQUIRES(mutex_);

  // Helper for creating copartitioned table.
  CHECKED_STATUS CreateCopartitionedTable(const CreateTableRequestPB& req,
                                          CreateTableResponsePB* resp,
                                          rpc::RpcContext* rpc,
                                          Schema schema,
                                          scoped_refptr<NamespaceInfo> ns);

  // Check that local host is present in master addresses for normal master process start.
  // On error, it could imply that master_addresses is incorrectly set for shell master startup
  // or that this master host info was missed in the master addresses and it should be
  // participating in the very first quorum setup.
  CHECKED_STATUS CheckLocalHostInMasterAddresses();

  // Helper for initializing 'sys_catalog_'. After calling this
  // method, the caller should call WaitUntilRunning() on sys_catalog_
  // WITHOUT holding 'lock_' to wait for consensus to start for
  // sys_catalog_.
  //
  // This method is thread-safe.
  CHECKED_STATUS InitSysCatalogAsync();

  Result<ReplicationInfoPB> GetTableReplicationInfo(const TabletInfo& tablet_info) const;

  // Helper for creating the initial TableInfo state
  // Leaves the table "write locked" with the new info in the
  // "dirty" state field.
  scoped_refptr<TableInfo> CreateTableInfo(const CreateTableRequestPB& req,
                                           const Schema& schema,
                                           const PartitionSchema& partition_schema,
                                           const NamespaceId& namespace_id,
                                           const NamespaceName& namespace_name,
                                           IndexInfoPB* index_info) REQUIRES(mutex_);

  // Helper for creating the initial TabletInfo state.
  // Leaves the tablet "write locked" with the new info in the
  // "dirty" state field.
  TabletInfoPtr CreateTabletInfo(TableInfo* table,
                                 const PartitionPB& partition) REQUIRES(mutex_);

  // Remove the specified entries from the protobuf field table_ids of a TabletInfo.
  Status RemoveTableIdsFromTabletInfo(
      TabletInfoPtr tablet_info, std::unordered_set<TableId> tables_to_remove);

  // Add index info to the indexed table.
  CHECKED_STATUS AddIndexInfoToTable(const scoped_refptr<TableInfo>& indexed_table,
                                     const IndexInfoPB& index_info,
                                     CreateTableResponsePB* resp);

  // Delete index info from the indexed table.
  CHECKED_STATUS MarkIndexInfoFromTableForDeletion(
      const TableId& indexed_table_id, const TableId& index_table_id, bool multi_stage,
      DeleteTableResponsePB* resp);

  // Delete index info from the indexed table.
  CHECKED_STATUS DeleteIndexInfoFromTable(
      const TableId& indexed_table_id, const TableId& index_table_id);

  // Builds the TabletLocationsPB for a tablet based on the provided TabletInfo.
  // Populates locs_pb and returns true on success.
  // Returns Status::ServiceUnavailable if tablet is not running.
  // Set include_inactive to true in order to also get information about hidden tablets.
  CHECKED_STATUS BuildLocationsForTablet(
      const scoped_refptr<TabletInfo>& tablet,
      TabletLocationsPB* locs_pb,
      IncludeInactive include_inactive = IncludeInactive::kFalse);

  // Check whether the tservers in the current replica map differs from those in the cstate when
  // processing a tablet report. Ignore the roles reported by the cstate, just compare the
  // tservers.
  bool ReplicaMapDiffersFromConsensusState(const scoped_refptr<TabletInfo>& tablet,
                                           const consensus::ConsensusStatePB& consensus_state);

  void ReconcileTabletReplicasInLocalMemoryWithReport(
      const scoped_refptr<TabletInfo>& tablet,
      const std::string& sender_uuid,
      const consensus::ConsensusStatePB& consensus_state,
      const ReportedTabletPB& report);

  // Register a tablet server whenever it heartbeats with a consensus configuration. This is
  // needed because we have logic in the Master that states that if a tablet
  // server that is part of a consensus configuration has not heartbeated to the Master yet, we
  // leave it out of the consensus configuration reported to clients.
  // TODO: See if we can remove this logic, as it seems confusing.
  void UpdateTabletReplicaInLocalMemory(TSDescriptor* ts_desc,
                                        const consensus::ConsensusStatePB* consensus_state,
                                        const ReportedTabletPB& report,
                                        const scoped_refptr<TabletInfo>& tablet_to_update);

  static void CreateNewReplicaForLocalMemory(TSDescriptor* ts_desc,
                                             const consensus::ConsensusStatePB* consensus_state,
                                             const ReportedTabletPB& report,
                                             TabletReplica* new_replica);

  // Extract the set of tablets that can be deleted and the set of tablets
  // that must be processed because not running yet.
  // Returns a map of table_id -> {tablet_info1, tablet_info2, etc.}.
  void ExtractTabletsToProcess(TabletInfos *tablets_to_delete,
                               TableToTabletInfos *tablets_to_process);

  // Determine whether any tables are in the DELETING state.
  bool AreTablesDeleting() override;

  // Task that takes care of the tablet assignments/creations.
  // Loops through the "not created" tablets and sends a CreateTablet() request.
  CHECKED_STATUS ProcessPendingAssignmentsPerTable(
      const TableId& table_id, const TabletInfos& tablets, CMGlobalLoadState* global_load_state);

  // Select a tablet server from 'ts_descs' on which to place a new replica.
  // Any tablet servers in 'excluded' are not considered.
  // REQUIRES: 'ts_descs' must include at least one non-excluded server.
  std::shared_ptr<TSDescriptor> SelectReplica(
      const TSDescriptorVector& ts_descs,
      std::set<TabletServerId>* excluded,
      CMPerTableLoadState* per_table_state, CMGlobalLoadState* global_state);

  // Select N Replicas from online tablet servers (as specified by
  // 'ts_descs') for the specified tablet and populate the consensus configuration
  // object. If 'ts_descs' does not specify enough online tablet
  // servers to select the N replicas, return Status::InvalidArgument.
  //
  // This method is called by "ProcessPendingAssignmentsPerTable()".
  CHECKED_STATUS SelectReplicasForTablet(
      const TSDescriptorVector& ts_descs, TabletInfo* tablet,
      CMPerTableLoadState* per_table_state, CMGlobalLoadState* global_state);

  // Select N Replicas from the online tablet servers that have been chosen to respect the
  // placement information provided. Populate the consensus configuration object with choices and
  // also update the set of selected tablet servers, to not place several replicas on the same TS.
  // member_type indicated what type of replica to select for.
  //
  // This method is called by "SelectReplicasForTablet".
  void SelectReplicas(
      const TSDescriptorVector& ts_descs,
      size_t nreplicas, consensus::RaftConfigPB* config,
      std::set<TabletServerId>* already_selected_ts,
      consensus::PeerMemberType member_type,
      CMPerTableLoadState* per_table_state,
      CMGlobalLoadState* global_state);

  void HandleAssignPreparingTablet(TabletInfo* tablet,
                                   DeferredAssignmentActions* deferred);

  // Assign tablets and send CreateTablet RPCs to tablet servers.
  // The out param 'new_tablets' should have any newly-created TabletInfo
  // objects appended to it.
  void HandleAssignCreatingTablet(TabletInfo* tablet,
                                  DeferredAssignmentActions* deferred,
                                  TabletInfos* new_tablets);

  CHECKED_STATUS HandleTabletSchemaVersionReport(
      TabletInfo *tablet, uint32_t version,
      const scoped_refptr<TableInfo>& table = nullptr) override;

  // Send the create tablet requests to the selected peers of the consensus configurations.
  // The creation is async, and at the moment there is no error checking on the
  // caller side. We rely on the assignment timeout. If we don't see the tablet
  // after the timeout, we regenerate a new one and proceed with a new
  // assignment/creation.
  //
  // This method is part of the "ProcessPendingAssignmentsPerTable()"
  //
  // This must be called after persisting the tablet state as
  // CREATING to ensure coherent state after Master failover.
  CHECKED_STATUS SendCreateTabletRequests(const std::vector<TabletInfo*>& tablets);

  // Send the "alter table request" to all tablets of the specified table.
  //
  // Also, initiates the required AlterTable requests to backfill the Index.
  // Initially the index is set to be in a INDEX_PERM_DELETE_ONLY state, then
  // updated to INDEX_PERM_WRITE_AND_DELETE state; followed by backfilling. Once
  // all the tablets have completed backfilling, the index will be updated
  // to be in INDEX_PERM_READ_WRITE_AND_DELETE state.
  CHECKED_STATUS SendAlterTableRequest(const scoped_refptr<TableInfo>& table,
                                       const AlterTableRequestPB* req = nullptr);

  // Start the background task to send the CopartitionTable() RPC to the leader for this
  // tablet.
  void SendCopartitionTabletRequest(const scoped_refptr<TabletInfo>& tablet,
                                    const scoped_refptr<TableInfo>& table);

  // Starts the background task to send the SplitTablet RPC to the leader for the specified tablet.
  CHECKED_STATUS SendSplitTabletRequest(
      const scoped_refptr<TabletInfo>& tablet, std::array<TabletId, kNumSplitParts> new_tablet_ids,
      const std::string& split_encoded_key, const std::string& split_partition_key);

  // Send the "truncate table request" to all tablets of the specified table.
  void SendTruncateTableRequest(const scoped_refptr<TableInfo>& table);

  // Start the background task to send the TruncateTable() RPC to the leader for this tablet.
  void SendTruncateTabletRequest(const scoped_refptr<TabletInfo>& tablet);

  // Truncate the specified table/index.
  CHECKED_STATUS TruncateTable(const TableId& table_id,
                               TruncateTableResponsePB* resp,
                               rpc::RpcContext* rpc);

  struct DeletingTableData {
    TableInfoPtr info;
    TableInfo::WriteLock write_lock;
    RepeatedBytes retained_by_snapshot_schedules;
    bool remove_from_name_map;
  };

  // Delete the specified table in memory. The TableInfo, DeletedTableInfo and lock of the deleted
  // table are appended to the lists. The caller will be responsible for committing the change and
  // deleting the actual table and tablets.
  CHECKED_STATUS DeleteTableInMemory(
      const TableIdentifierPB& table_identifier,
      bool is_index_table,
      bool update_indexed_table,
      const SnapshotSchedulesToObjectIdsMap& schedules_to_tables_map,
      std::vector<DeletingTableData>* tables,
      DeleteTableResponsePB* resp,
      rpc::RpcContext* rpc);

  // Request tablet servers to delete all replicas of the tablet.
  void DeleteTabletReplicas(TabletInfo* tablet, const std::string& msg, HideOnly hide_only);

  // Returns error if and only if it is forbidden to both:
  // 1) Delete single tablet from table.
  // 2) Delete the whole table.
  // This is used for pre-checks in both `DeleteTablet` and `DeleteTabletsAndSendRequests`.
  CHECKED_STATUS CheckIfForbiddenToDeleteTabletOf(const scoped_refptr<TableInfo>& table);

  // Marks each of the tablets in the given table as deleted and triggers requests to the tablet
  // servers to delete them. The table parameter is expected to be given "write locked".
  CHECKED_STATUS DeleteTabletsAndSendRequests(
      const TableInfoPtr& table, const RepeatedBytes& retained_by_snapshot_schedules);

  // Marks each tablet as deleted and triggers requests to the tablet servers to delete them.
  CHECKED_STATUS DeleteTabletListAndSendRequests(
      const std::vector<scoped_refptr<TabletInfo>>& tablets, const std::string& deletion_msg,
      const RepeatedBytes& retained_by_snapshot_schedules);

  // Send the "delete tablet request" to the specified TS/tablet.
  // The specified 'reason' will be logged on the TS.
  void SendDeleteTabletRequest(const TabletId& tablet_id,
                               tablet::TabletDataState delete_type,
                               const boost::optional<int64_t>& cas_config_opid_index_less_or_equal,
                               const scoped_refptr<TableInfo>& table,
                               TSDescriptor* ts_desc,
                               const std::string& reason,
                               bool hide_only = false);

  // Start a task to request the specified tablet leader to step down and optionally to remove
  // the server that is over-replicated. A new tablet server can be specified to start an election
  // immediately to become the new leader. If new_leader_ts_uuid is empty, the election will be run
  // following the protocol's default mechanism.
  void SendLeaderStepDownRequest(
      const scoped_refptr<TabletInfo>& tablet, const consensus::ConsensusStatePB& cstate,
      const string& change_config_ts_uuid, bool should_remove,
      const string& new_leader_ts_uuid = "");

  // Start a task to change the config to remove a certain voter because the specified tablet is
  // over-replicated.
  void SendRemoveServerRequest(
      const scoped_refptr<TabletInfo>& tablet, const consensus::ConsensusStatePB& cstate,
      const string& change_config_ts_uuid);

  // Start a task to change the config to add an additional voter because the
  // specified tablet is under-replicated.
  void SendAddServerRequest(
      const scoped_refptr<TabletInfo>& tablet, consensus::PeerMemberType member_type,
      const consensus::ConsensusStatePB& cstate, const string& change_config_ts_uuid);

  void GetPendingServerTasksUnlocked(const TableId &table_uuid,
                                     TabletToTabletServerMap *add_replica_tasks_map,
                                     TabletToTabletServerMap *remove_replica_tasks_map,
                                     TabletToTabletServerMap *stepdown_leader_tasks)
      REQUIRES_SHARED(mutex_);

  // Abort creation of 'table': abort all mutation for TabletInfo and
  // TableInfo objects (releasing all COW locks), abort all pending
  // tasks associated with the table, and erase any state related to
  // the table we failed to create from the in-memory maps
  // ('table_names_map_', 'table_ids_map_', 'tablet_map_' below).
  CHECKED_STATUS AbortTableCreation(TableInfo* table,
                                    const TabletInfos& tablets,
                                    const Status& s,
                                    CreateTableResponsePB* resp);

  CHECKED_STATUS CreateTransactionStatusTablesForTablespaces(
      const TablespaceIdToReplicationInfoMap& tablespace_info,
      const TableToTablespaceIdMap& table_to_tablespace_map);

  void StartTablespaceBgTaskIfStopped();

  std::shared_ptr<YsqlTablespaceManager> GetTablespaceManager() const;

  Result<boost::optional<ReplicationInfoPB>> GetTablespaceReplicationInfoWithRetry(
      const TablespaceId& tablespace_id);

  // Report metrics.
  void ReportMetrics();

  // Reset metrics.
  void ResetMetrics();

  // Conventional "T xxx P yyy: " prefix for logging.
  std::string LogPrefix() const;

  // Aborts all tasks belonging to 'tables' and waits for them to finish.
  void AbortAndWaitForAllTasks(const std::vector<scoped_refptr<TableInfo>>& tables);

  // Can be used to create background_tasks_ field for this master.
  // Used on normal master startup or when master comes out of the shell mode.
  CHECKED_STATUS EnableBgTasks();

  // Helper function for RebuildYQLSystemPartitions to get the system.partitions tablet.
  Status GetYQLPartitionsVTable(std::shared_ptr<SystemTablet>* tablet);
  // Background task for automatically rebuilding system.partitions every
  // partitions_vtable_cache_refresh_secs seconds.
  void RebuildYQLSystemPartitions();

  // Registers new split tablet with `partition` for the same table as `source_tablet_info` tablet.
  // Does not change any other tablets and their partitions.
  // Returns TabletInfo for registered tablet.
  Result<TabletInfoPtr> RegisterNewTabletForSplit(
      TabletInfo* source_tablet_info, const PartitionPB& partition,
      TableInfo::WriteLock* table_write_lock, TabletInfo::WriteLock* tablet_write_lock);

  Result<scoped_refptr<TabletInfo>> GetTabletInfo(const TabletId& tablet_id) override;

  CHECKED_STATUS DoSplitTablet(
      const scoped_refptr<TabletInfo>& source_tablet_info, std::string split_encoded_key,
      std::string split_partition_key, bool select_all_tablets_for_split);

  // Splits tablet using specified split_hash_code as a split point.
  CHECKED_STATUS DoSplitTablet(
      const scoped_refptr<TabletInfo>& source_tablet_info, docdb::DocKeyHash split_hash_code,
      bool select_all_tablets_for_split);

  // Calculate the total number of replicas which are being handled by servers in state.
  int64_t GetNumRelevantReplicas(const BlacklistPB& state, bool leaders_only);

  int64_t leader_ready_term() override EXCLUDES(state_lock_) {
    std::lock_guard<simple_spinlock> l(state_lock_);
    return leader_ready_term_;
  }

  // Delete tables from internal map by id, if it has no more active tasks and tablets.
  // This function should only be called from the bg_tasks thread, in a single threaded fashion!
  void CleanUpDeletedTables();

  // Called when a new table id is added to table_ids_map_.
  void HandleNewTableId(const TableId& id);

  // Creates a new TableInfo object.
  scoped_refptr<TableInfo> NewTableInfo(TableId id) override;

  // Register the tablet server with the ts manager using the Raft config. This is called for
  // servers that are part of the Raft config but haven't registered as yet.
  CHECKED_STATUS RegisterTsFromRaftConfig(const consensus::RaftPeerPB& peer);

  template <class Loader>
  CHECKED_STATUS Load(const std::string& title, const int64_t term);

  virtual void Started() {}

  virtual void SysCatalogLoaded(int64_t term) {}

  // Respect leader affinity with master sys catalog tablet by stepping down if we don't match
  // the cluster config affinity specification.
  CHECKED_STATUS SysCatalogRespectLeaderAffinity();

  virtual Result<bool> IsTablePartOfSomeSnapshotSchedule(const TableInfo& table_info) override {
    // Default value.
    return false;
  }

  virtual bool IsCdcEnabled(const TableInfo& table_info) const override {
    // Default value.
    return false;
  }

  virtual bool IsTableCdcProducer(const TableInfo& table_info) const REQUIRES_SHARED(mutex_) {
    // Default value.
    return false;
  }

  virtual Result<SnapshotSchedulesToObjectIdsMap> MakeSnapshotSchedulesToObjectIdsMap(
      SysRowEntryType type) {
    return SnapshotSchedulesToObjectIdsMap();
  }

  Status DoDeleteNamespace(const DeleteNamespaceRequestPB* req,
                           DeleteNamespaceResponsePB* resp,
                           rpc::RpcContext* rpc);

  // TODO: the maps are a little wasteful of RAM, since the TableInfo/TabletInfo
  // objects have a copy of the string key. But STL doesn't make it
  // easy to make a "gettable set".

  // Lock protecting the various in memory storage structures.
  using MutexType = rw_spinlock;
  using SharedLock = NonRecursiveSharedLock<MutexType>;
  using LockGuard = std::lock_guard<MutexType>;
  mutable MutexType mutex_;

  // Note: Namespaces and tables for YSQL databases are identified by their ids only and therefore
  // are not saved in the name maps below.

  // Table map: table-id -> TableInfo
  VersionTracker<TableInfoMap> table_ids_map_ GUARDED_BY(mutex_);

  // Table map: [namespace-id, table-name] -> TableInfo
  // Don't have to use VersionTracker for it, since table_ids_map_ already updated at the same time.
  // Note that this map isn't used for YSQL tables.
  TableInfoByNameMap table_names_map_ GUARDED_BY(mutex_);

  // Set of table ids that are transaction status tables.
  // Don't have to use VersionTracker for it, since table_ids_map_ already updated at the same time.
  TableIdSet transaction_table_ids_set_ GUARDED_BY(mutex_);

  // Don't have to use VersionTracker for it, since table_ids_map_ already updated at the same time.
  // Tablet maps: tablet-id -> TabletInfo
  VersionTracker<TabletInfoMap> tablet_map_ GUARDED_BY(mutex_);

  // Tablets that was hidden instead of deleting, used to cleanup such tablets when time comes.
  std::vector<TabletInfoPtr> hidden_tablets_ GUARDED_BY(mutex_);

  // Namespace maps: namespace-id -> NamespaceInfo and namespace-name -> NamespaceInfo
  NamespaceInfoMap namespace_ids_map_ GUARDED_BY(mutex_);
  NamespaceNameMapper namespace_names_mapper_ GUARDED_BY(mutex_);

  // User-Defined type maps: udtype-id -> UDTypeInfo and udtype-name -> UDTypeInfo
  UDTypeInfoMap udtype_ids_map_ GUARDED_BY(mutex_);
  UDTypeInfoByNameMap udtype_names_map_ GUARDED_BY(mutex_);

  // RedisConfig map: RedisConfigKey -> RedisConfigInfo
  typedef std::unordered_map<RedisConfigKey, scoped_refptr<RedisConfigInfo>> RedisConfigInfoMap;
  RedisConfigInfoMap redis_config_map_ GUARDED_BY(mutex_);

  // Config information.
  scoped_refptr<ClusterConfigInfo> cluster_config_ = nullptr; // No GUARD, only write on Load.

  // YSQL Catalog information.
  scoped_refptr<SysConfigInfo> ysql_catalog_config_ = nullptr; // No GUARD, only write on Load.

  // Transaction tables information.
  scoped_refptr<SysConfigInfo> transaction_tables_config_ =
      nullptr; // No GUARD, only write on Load.

  Master *master_;
  Atomic32 closing_;

  std::unique_ptr<SysCatalogTable> sys_catalog_;

  // Mutex to avoid concurrent remote bootstrap sessions.
  std::mutex remote_bootstrap_mtx_;

  // Set to true if this master has received at least the superblock from a remote master.
  bool tablet_exists_;

  // Background thread, used to execute the catalog manager tasks
  // like the assignment and cleaner.
  friend class CatalogManagerBgTasks;
  std::unique_ptr<CatalogManagerBgTasks> background_tasks_;

  // Background threadpool, newer features use this (instead of the Background thread)
  // to execute time-lenient catalog manager tasks.
  std::unique_ptr<yb::ThreadPool> background_tasks_thread_pool_;

  // TODO: convert this to YB_DEFINE_ENUM for automatic pretty-printing.
  enum State {
    kConstructed,
    kStarting,
    kRunning,
    kClosing
  };

  // Lock protecting state_, leader_ready_term_
  mutable simple_spinlock state_lock_;
  State state_ GUARDED_BY(state_lock_);

  // Used to defer Master<->TabletServer work from reactor threads onto a thread where
  // blocking behavior is permissible.
  //
  // NOTE: Presently, this thread pool must contain only a single
  // thread (to correctly serialize invocations of ElectedAsLeaderCb
  // upon closely timed consecutive elections).
  std::unique_ptr<ThreadPool> leader_initialization_pool_;

  // Thread pool to do the async RPC task work.
  std::unique_ptr<ThreadPool> async_task_pool_;

  // This field is updated when a node becomes leader master,
  // waits for all outstanding uncommitted metadata (table and tablet metadata)
  // in the sys catalog to commit, and then reads that metadata into in-memory
  // data structures. This is used to "fence" client and tablet server requests
  // that depend on the in-memory state until this master can respond
  // correctly.
  int64_t leader_ready_term_ GUARDED_BY(state_lock_);

  // Lock used to fence operations and leader elections. All logical operations
  // (i.e. create table, alter table, etc.) should acquire this lock for
  // reading. Following an election where this master is elected leader, it
  // should acquire this lock for writing before reloading the metadata.
  //
  // Readers should not acquire this lock directly; use ScopedLeadershipLock
  // instead.
  //
  // Always acquire this lock before state_lock_.
  RWMutex leader_lock_;

  // Async operations are accessing some private methods
  // (TODO: this stuff should be deferred and done in the background thread)
  friend class AsyncAlterTable;

  // Number of live tservers metric.
  scoped_refptr<AtomicGauge<uint32_t>> metric_num_tablet_servers_live_;

  // Number of dead tservers metric.
  scoped_refptr<AtomicGauge<uint32_t>> metric_num_tablet_servers_dead_;

  friend class ClusterLoadBalancer;

  // Policy for load balancing tablets on tablet servers.
  std::unique_ptr<ClusterLoadBalancer> load_balance_policy_;

  // Use the Raft config that has been bootstrapped to update the in-memory state of master options
  // and also the on-disk state of the consensus meta object.
  CHECKED_STATUS UpdateMastersListInMemoryAndDisk();

  // Tablets of system tables on the master indexed by the tablet id.
  std::unordered_map<std::string, std::shared_ptr<tablet::AbstractTablet>> system_tablets_;

  // Tablet of colocated namespaces indexed by the namespace id.
  std::unordered_map<NamespaceId, scoped_refptr<TabletInfo>> colocated_tablet_ids_map_
      GUARDED_BY(mutex_);

  typedef std::unordered_map<TablegroupId, scoped_refptr<TabletInfo>> TablegroupTabletMap;

  std::unordered_map<NamespaceId, TablegroupTabletMap> tablegroup_tablet_ids_map_
      GUARDED_BY(mutex_);

  std::unordered_map<TablegroupId, scoped_refptr<TablegroupInfo>> tablegroup_ids_map_
      GUARDED_BY(mutex_);

  std::unordered_map<TableId, TableId> matview_pg_table_ids_map_
      GUARDED_BY(mutex_);

  boost::optional<std::future<Status>> initdb_future_;
  boost::optional<InitialSysCatalogSnapshotWriter> initial_snapshot_writer_;

  std::unique_ptr<PermissionsManager> permissions_manager_;

  // This is used for tracking that initdb has started running previously.
  std::atomic<bool> pg_proc_exists_{false};

  // Tracks most recent async tasks.
  scoped_refptr<TasksTracker> tasks_tracker_;

  // Tracks most recent user initiated jobs.
  scoped_refptr<TasksTracker> jobs_tracker_;

  std::unique_ptr<EncryptionManager> encryption_manager_;

  std::unique_ptr<client::UniverseKeyClient> universe_key_client_;

  // A pointer to the system.partitions tablet for the RebuildYQLSystemPartitions bg task.
  std::shared_ptr<SystemTablet> system_partitions_tablet_ = nullptr;

  // Handles querying and processing YSQL DDL Transactions as a catalog manager background task.
  std::unique_ptr<YsqlTransactionDdl> ysql_transaction_;

  MonoTime time_elected_leader_;

  std::unique_ptr<client::YBClient> cdc_state_client_;

  // Mutex to avoid simultaneous creation of transaction tables for a tablespace.
  std::mutex tablespace_transaction_table_creation_mutex_;

  void StartElectionIfReady(
      const consensus::ConsensusStatePB& cstate, TabletInfo* tablet);

 private:
  // Performs the provided action with the sys catalog shared tablet instance, or sets up an error
  // if the tablet is not found.
  template <class Req, class Resp, class F>
  CHECKED_STATUS PerformOnSysCatalogTablet(
      const Req& req, Resp* resp, const F& f);

  virtual bool CDCStreamExistsUnlocked(const CDCStreamId& id) REQUIRES_SHARED(mutex_);

  CHECKED_STATUS CollectTable(
      const TableDescription& table_description,
      CollectFlags flags,
      std::vector<TableDescription>* all_tables,
      std::unordered_set<NamespaceId>* parent_colocated_table_ids);

  void SplitTabletWithKey(
      const scoped_refptr<TabletInfo>& tablet, const std::string& split_encoded_key,
      const std::string& split_partition_key, bool select_all_tablets_for_split);

  // From the list of TServers in 'ts_descs', return the ones that match any placement policy
  // in 'placement_info'. Returns error if there are insufficient TServers to match the
  // required replication factor in placement_info.
  // NOTE: This function will only check whether the total replication factor can be
  // satisfied, and not the individual min_num_replicas in each placement block.
  Result<TSDescriptorVector> FindTServersForPlacementInfo(
      const PlacementInfoPB& placement_info,
      const TSDescriptorVector& ts_descs) const;

  // Using the TServer info in 'ts_descs', return the TServers that match 'pplacement_block'.
  // Returns error if there aren't enough TServers to fulfill the min_num_replicas requirement
  // outlined in 'placement_block'.
  Result<TSDescriptorVector> FindTServersForPlacementBlock(
      const PlacementBlockPB& placement_block,
      const TSDescriptorVector& ts_descs);

  bool IsReplicationInfoSet(const ReplicationInfoPB& replication_info);

  CHECKED_STATUS ValidateTableReplicationInfo(const ReplicationInfoPB& replication_info);

  // Return the id of the tablespace associated with a transaction status table, if any.
  boost::optional<TablespaceId> GetTransactionStatusTableTablespace(
      const scoped_refptr<TableInfo>& table) REQUIRES_SHARED(mutex_);

  // Clears tablespace id for a transaction status table, reverting it back to cluster default
  // if no placement has been set explicitly.
  void ClearTransactionStatusTableTablespace(
      const scoped_refptr<TableInfo>& table) REQUIRES(mutex_);

  // Checks if there are any transaction tables with tablespace id set for a tablespace not in
  // the given tablespace info map.
  bool CheckTransactionStatusTablesWithMissingTablespaces(
      const TablespaceIdToReplicationInfoMap& tablespace_info) EXCLUDES(mutex_);

  // Updates transaction tables' tablespace ids for tablespaces that don't exist.
  CHECKED_STATUS UpdateTransactionStatusTableTablespaces(
      const TablespaceIdToReplicationInfoMap& tablespace_info) EXCLUDES(mutex_);

  // Return the tablespaces in the system and their associated replication info from
  // pg catalog tables.
  Result<std::shared_ptr<TablespaceIdToReplicationInfoMap>> GetYsqlTablespaceInfo();

  // Return the table->tablespace mapping by reading the pg catalog tables.
  Result<std::shared_ptr<TableToTablespaceIdMap>> GetYsqlTableToTablespaceMap(
      const TablespaceIdToReplicationInfoMap& tablespace_info) EXCLUDES(mutex_);

  // Background task that refreshes the in-memory state for YSQL tables with their associated
  // tablespace info.
  // Note: This function should only ever be called by StartTablespaceBgTaskIfStopped().
  void RefreshTablespaceInfoPeriodically();

  // Helper function to schedule the next iteration of the tablespace info task.
  void ScheduleRefreshTablespaceInfoTask(const bool schedule_now = false);

  // Helper function to refresh the tablespace info.
  CHECKED_STATUS DoRefreshTablespaceInfo();

  // Processes committed consensus state for specified tablet from ts_desc.
  // Returns true if tablet was mutated.
  bool ProcessCommittedConsensusState(
      TSDescriptor* ts_desc,
      bool is_incremental,
      const ReportedTabletPB& report,
      const TableInfo::WriteLock& table_lock,
      const TabletInfoPtr& tablet,
      const TabletInfo::WriteLock& tablet_lock,
      std::vector<RetryingTSRpcTaskPtr>* rpcs);

  struct ReportedTablet {
    TabletId tablet_id;
    TabletInfoPtr info;
    const ReportedTabletPB* report;
  };
  using ReportedTablets = std::vector<ReportedTablet>;

  // Process tablets batch while processing tablet report.
  CHECKED_STATUS ProcessTabletReportBatch(
      TSDescriptor* ts_desc,
      bool is_incremental,
      ReportedTablets::const_iterator begin,
      ReportedTablets::const_iterator end,
      TabletReportUpdatesPB* full_report_update,
      std::vector<RetryingTSRpcTaskPtr>* rpcs);

  size_t GetNumLiveTServersForPlacement(const PlacementId& placement_id);

  TSDescriptorVector GetAllLiveNotBlacklistedTServers() const;

  const YQLPartitionsVTable& GetYqlPartitionsVtable() const;

  void InitializeTableLoadState(
      const TableId& table_id, TSDescriptorVector ts_descs, CMPerTableLoadState* state);

  void InitializeGlobalLoadState(
      TSDescriptorVector ts_descs, CMGlobalLoadState* state);

  // Should be bumped up when tablet locations are changed.
  std::atomic<uintptr_t> tablet_locations_version_{0};

  rpc::ScheduledTaskTracker refresh_yql_partitions_task_;

  mutable MutexType tablespace_mutex_;

  // The tablespace_manager_ encapsulates two maps that are periodically updated by a background
  // task that reads tablespace information from the PG catalog tables. The task creates a new
  // manager instance, populates it with the information read from the catalog tables and updates
  // this shared_ptr. The maps themselves are thus never updated (no inserts/deletes/updates)
  // once populated and are garbage collected once all references to them go out of scope.
  // No clients are expected to update the manager, they take a lock merely to copy the
  // shared_ptr and read from it.
  std::shared_ptr<YsqlTablespaceManager> tablespace_manager_ GUARDED_BY(tablespace_mutex_);

  // Whether the periodic job to update tablespace info is running.
  std::atomic<bool> tablespace_bg_task_running_;

  rpc::ScheduledTaskTracker refresh_ysql_tablespace_info_task_;

  ServerRegistrationPB server_registration_;

  TabletSplitManager tablet_split_manager_;

  DISALLOW_COPY_AND_ASSIGN(CatalogManager);
};

}  // namespace master
}  // namespace yb

#endif // YB_MASTER_CATALOG_MANAGER_H
