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

#ifndef YB_YQL_PGWRAPPER_PG_MINI_TEST_BASE_H
#define YB_YQL_PGWRAPPER_PG_MINI_TEST_BASE_H

#include "yb/integration-tests/mini_cluster.h"
#include "yb/integration-tests/yb_mini_cluster_test_base.h"

#include "yb/yql/pgwrapper/libpq_utils.h"
#include "yb/yql/pgwrapper/pg_wrapper.h"

namespace yb {
namespace pgwrapper {

class PgMiniTestBase : public YBMiniClusterTestBase<MiniCluster> {
 protected:
  // This allows modifying flags before we start the postgres process in SetUp.
  virtual void BeforePgProcessStart() {
  }

  void DoTearDown() override;

  void SetUp() override;

  virtual size_t NumMasters() {
    return 1;
  }

  virtual size_t NumTabletServers() {
    return 3;
  }

  // This allows modifying the logic to decide which tablet server to run postgres on -
  // by default, randomly picked out of all the tablet servers.
  virtual const std::shared_ptr<tserver::MiniTabletServer> PickPgTabletServer(
     const MiniCluster::MiniTabletServers& servers);

  Result<PGConn> Connect() {
    return PGConn::Connect(pg_host_port_);
  }

  Result<PGConn> ConnectToDB(const std::string& dbname) {
    return PGConn::Connect(pg_host_port_, dbname);
  }

  CHECKED_STATUS RestartCluster();

  const HostPort& pg_host_port() const {
    return pg_host_port_;
  }

 private:
  Result<PgProcessConf> CreatePgProcessConf(uint16_t port);

  std::unique_ptr<PgSupervisor> pg_supervisor_;
  HostPort pg_host_port_;
};

} // namespace pgwrapper
} // namespace yb

#endif // YB_YQL_PGWRAPPER_PG_MINI_TEST_BASE_H
