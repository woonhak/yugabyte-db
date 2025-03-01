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
package org.yb.pgsql;

import java.io.File;
import java.lang.Math;
import java.sql.Connection;
import java.sql.Statement;
import java.time.Instant;
import java.time.temporal.ChronoUnit;
import java.util.Arrays;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.json.JSONObject;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import org.apache.commons.io.FileUtils;
import org.yb.minicluster.MiniYBCluster;
import org.yb.minicluster.MiniYBClusterBuilder;
import org.yb.util.TableProperties;
import org.yb.util.YBBackupException;
import org.yb.util.YBBackupUtil;
import org.yb.util.YBTestRunnerNonSanitizersOrMac;

import com.google.common.collect.ImmutableMap;

import static org.yb.AssertionWrappers.assertArrayEquals;
import static org.yb.AssertionWrappers.assertEquals;
import static org.yb.AssertionWrappers.assertFalse;
import static org.yb.AssertionWrappers.assertLessThan;
import static org.yb.AssertionWrappers.assertTrue;
import static org.yb.AssertionWrappers.fail;

@RunWith(value=YBTestRunnerNonSanitizersOrMac.class)
public class TestYbBackup extends BasePgSQLTest {
  private static final Logger LOG = LoggerFactory.getLogger(TestYbBackup.class);

  @Before
  public void initYBBackupUtil() {
    YBBackupUtil.setTSAddresses(miniCluster.getTabletServers());
    YBBackupUtil.setMasterAddresses(masterAddresses);
    YBBackupUtil.setPostgresContactPoint(miniCluster.getPostgresContactPoints().get(0));
  }

  @Override
  protected void customizeMiniClusterBuilder(MiniYBClusterBuilder builder) {
    super.customizeMiniClusterBuilder(builder);

    List<Map<String, String>> perTserverZonePlacementFlags = Arrays.asList(
        ImmutableMap.of("placement_cloud", "cloud1",
                        "placement_region", "region1",
                        "placement_zone", "zone1"),
        ImmutableMap.of("placement_cloud", "cloud2",
                        "placement_region", "region2",
                        "placement_zone", "zone2"),
        ImmutableMap.of("placement_cloud", "cloud3",
                        "placement_region", "region3",
                        "placement_zone", "zone3"));
    builder.perTServerFlags(perTserverZonePlacementFlags);
  }

  @Override
  public int getTestMethodTimeoutSec() {
    return 600; // Usual time for a test ~90 seconds. But can be much more on Jenkins.
  }

  @Override
  protected int getNumShardsPerTServer() {
    return 2;
  }

  public void doAlteredTableBackup(String dbName, TableProperties tp) throws Exception {
    String colocString = tp.isColocated() ? "TRUE" : "FALSE";
    String initialDBName = dbName + "1";
    String restoreDBName = dbName + "2";

    try (Statement stmt = connection.createStatement()) {
      stmt.execute(String.format("CREATE DATABASE %s COLOCATED=%s", initialDBName, colocString));
    }
    try (Connection connection2 = getConnectionBuilder().withDatabase(initialDBName).connect();
         Statement stmt = connection2.createStatement()) {
      stmt.execute("CREATE TABLE test_tbl (h INT PRIMARY KEY, a INT, b FLOAT) " +
                   String.format("WITH (colocated = %s)", colocString));

      for (int i = 1; i <= 2000; ++i) {
        stmt.execute("INSERT INTO test_tbl (h, a, b) VALUES" +
          " (" + String.valueOf(i) +                     // h
          ", " + String.valueOf(100 + i) +               // a
          ", " + String.valueOf(2.14 + (float)i) + ")"); // b
      }

      stmt.execute("ALTER TABLE test_tbl DROP a");
      String backupDir = YBBackupUtil.getTempBackupDir();
      String output = YBBackupUtil.runYbBackupCreate("--backup_location", backupDir,
          "--keyspace", "ysql." + initialDBName);
      backupDir = new JSONObject(output).getString("snapshot_url");

      stmt.execute("INSERT INTO test_tbl (h, b) VALUES (9999, 8.9)");

      YBBackupUtil.runYbBackupRestore(backupDir, "--keyspace", "ysql." + restoreDBName);

      assertQuery(stmt, "SELECT * FROM test_tbl WHERE h=1", new Row(1, 3.14));
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE h=2000", new Row(2000, 2002.14));
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE h=9999", new Row(9999, 8.9));
    }

    try (Connection connection2 = getConnectionBuilder().withDatabase(restoreDBName).connect();
         Statement stmt = connection2.createStatement()) {
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE h=1", new Row(1, 3.14));
      assertQuery(stmt, "SELECT b FROM test_tbl WHERE h=1", new Row(3.14));
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE h=2000", new Row(2000, 2002.14));
      assertQuery(stmt, "SELECT h FROM test_tbl WHERE h=2000", new Row(2000));
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE h=9999");
    }

    // Cleanup.
    try (Statement stmt = connection.createStatement()) {
      stmt.execute("DROP DATABASE " + initialDBName);
      stmt.execute("DROP DATABASE " + restoreDBName);
    }
  }

  @Test
  public void testAlteredTable() throws Exception {
    doAlteredTableBackup("altered_db", new TableProperties(
        TableProperties.TP_YSQL | TableProperties.TP_NON_COLOCATED));
  }

  @Test
  public void testAlteredTableInColocatedDB() throws Exception {
    doAlteredTableBackup("altered_colocated_db", new TableProperties(
        TableProperties.TP_YSQL | TableProperties.TP_COLOCATED));
  }

  @Test
  public void testMixedColocatedDatabase() throws Exception {
    String initialDBName = "yb_colocated";
    String restoreDBName = "yb_colocated2";

    try (Statement stmt = connection.createStatement()) {
      stmt.execute(String.format("CREATE DATABASE %s COLOCATED=TRUE", initialDBName));
    }

    try (Connection connection2 = getConnectionBuilder().withDatabase(initialDBName).connect();
         Statement stmt = connection2.createStatement()) {
      // Create 3 tables, 2 colocated and 1 non colocated but in the same db.
      stmt.execute("CREATE TABLE test_tbl1 (h INT PRIMARY KEY, a INT, b FLOAT) " +
                   "WITH (COLOCATED=TRUE)");
      stmt.execute("CREATE TABLE test_tbl2 (h INT PRIMARY KEY, a INT, b FLOAT) " +
                   "WITH (COLOCATED=TRUE)");
      stmt.execute("CREATE TABLE test_tbl3 (h INT PRIMARY KEY, a INT, b FLOAT) " +
                   "WITH (COLOCATED=FALSE)");

      // Insert random rows/values for tables to snapshot
      for (int j = 1; j <= 3; ++j) {
        for (int i = 1; i <= 2000; ++i) {
          stmt.execute("INSERT INTO test_tbl" + String.valueOf(j) + " (h, a, b) VALUES" +
            " (" + String.valueOf(i * j) +                       // h
            ", " + String.valueOf((100 + i) * j) +               // a
            ", " + String.valueOf((2.14 + (float)i) * j) + ")"); // b
        }
      }

      String backupDir = YBBackupUtil.getTempBackupDir();
      String output = YBBackupUtil.runYbBackupCreate("--backup_location", backupDir,
          "--keyspace", "ysql." + initialDBName);
      backupDir = new JSONObject(output).getString("snapshot_url");

      // Insert more rows after taking the snapshot.
      for (int j = 1; j <= 3; ++j) {
        stmt.execute("INSERT INTO test_tbl" + String.valueOf(j) + " (h, a, b) VALUES" +
        " (" + String.valueOf(9999 * j) +                        // h
        ", " + String.valueOf((100 + 9999) * j) +                // a
        ", " + String.valueOf((2.14 + 9999f) * j) + ")");        // b
      }

      YBBackupUtil.runYbBackupRestore(backupDir, "--keyspace", "ysql." + restoreDBName);

      // Verify the original database has the rows we previously inserted.
      for (int j = 1; j <= 3; ++j) {
        for (int i : new int[] {1, 2000, 9999}) {
          assertQuery(stmt, String.format("SELECT * FROM test_tbl%d WHERE h=%d", j, i * j),
            new Row(i * j, (100 + i) * j, (2.14 + (float)i) * j));
        }
      }
    }

    // Verify that the new database and tables are properly configured.
    List<String> tbl1Tablets = YBBackupUtil.getTabletsForTable("ysql." + restoreDBName,
                                                                "test_tbl1");
    List<String> tbl2Tablets = YBBackupUtil.getTabletsForTable("ysql." + restoreDBName,
                                                                "test_tbl2");
    List<String> tbl3Tablets = YBBackupUtil.getTabletsForTable("ysql." + restoreDBName,
                                                                "test_tbl3");
    // test_tbl1 and test_tbl2 are colocated and so should share the exact same tablet.
    assertEquals("test_tbl1 is not colocated", 1, tbl1Tablets.size());
    assertEquals("test_tbl2 is not colocated", 1, tbl2Tablets.size());
    assertArrayEquals("test_tbl1 and test_tbl2 do not share the same colocated tablet",
                      tbl1Tablets.toArray(), tbl2Tablets.toArray());
    // test_tbl3 is not colocated so it should have more tablets.
    assertTrue("test_tbl3 should have more than 1 tablet", tbl3Tablets.size() > 1);
    assertFalse("test_tbl3 uses the colocated tablet", tbl3Tablets.contains(tbl1Tablets.get(0)));

    try (Connection connection2 = getConnectionBuilder().withDatabase(restoreDBName).connect();
         Statement stmt = connection2.createStatement()) {
      // Verify the new database contains all the rows we inserted before taking the snapshot.
      for (int j = 1; j <= 3; ++j) {
        for (int i : new int[] {1, 500, 2000}) {
          assertQuery(stmt, String.format("SELECT * FROM test_tbl%d WHERE h=%d", j, i * j),
            new Row(i * j, (100 + i) * j, (2.14 + (float)i) * j));
          assertQuery(stmt, String.format("SELECT h FROM test_tbl%d WHERE h=%d", j, i * j),
            new Row(i * j));
          assertQuery(stmt, String.format("SELECT a FROM test_tbl%d WHERE h=%d", j, i * j),
            new Row((100 + i) * j));
          assertQuery(stmt, String.format("SELECT b FROM test_tbl%d WHERE h=%d", j, i * j),
            new Row((2.14 + (float)i) * j));
        }
        // Assert that this returns no rows.
        assertQuery(stmt, String.format("SELECT * FROM test_tbl%d WHERE h=%d", j, 9999 * j));
      }
    }

    // Cleanup.
    try (Statement stmt = connection.createStatement()) {
      stmt.execute(String.format("DROP DATABASE %s", initialDBName));
      stmt.execute(String.format("DROP DATABASE %s", restoreDBName));
    }
  }

  @Test
  public void testAlteredTableInOriginalCluster() throws Exception {
    try (Statement stmt = connection.createStatement()) {
      stmt.execute("CREATE TABLE  test_tbl (h INT PRIMARY KEY, a INT, b FLOAT)");

      for (int i = 1; i <= 2000; ++i) {
        stmt.execute("INSERT INTO test_tbl (h, a, b) VALUES" +
          " (" + String.valueOf(i) +                     // h
          ", " + String.valueOf(100 + i) +               // a
          ", " + String.valueOf(2.14 + (float)i) + ")"); // b
      }

      String backupDir = YBBackupUtil.getTempBackupDir();
      String output = YBBackupUtil.runYbBackupCreate("--backup_location", backupDir,
          "--keyspace", "ysql.yugabyte");
      backupDir = new JSONObject(output).getString("snapshot_url");
      stmt.execute("ALTER TABLE test_tbl DROP a");
      stmt.execute("INSERT INTO test_tbl (h, b) VALUES (9999, 8.9)");

      try {
        YBBackupUtil.runYbBackupRestore(backupDir);
        fail("Backup restoring did not fail as expected");
      } catch (YBBackupException ex) {
        LOG.info("Expected exception", ex);
      }

      YBBackupUtil.runYbBackupRestore(backupDir, "--keyspace", "ysql.yb2");
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE h=1", new Row(1, 3.14));
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE h=2000", new Row(2000, 2002.14));
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE h=9999", new Row(9999, 8.9));
    }

    try (Connection connection2 = getConnectionBuilder().withDatabase("yb2").connect();
         Statement stmt = connection2.createStatement()) {
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE h=1", new Row(1, 101, 3.14));
      assertQuery(stmt, "SELECT b FROM test_tbl WHERE h=1", new Row(3.14));
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE h=2000", new Row(2000, 2100, 2002.14));
      assertQuery(stmt, "SELECT h FROM test_tbl WHERE h=2000", new Row(2000));
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE h=9999");
    }
  }

  @Test
  public void testColocatedWithTableOidAlreadySet() throws Exception {
    try (Statement stmt = connection.createStatement()) {
      stmt.execute("CREATE DATABASE yb1 COLOCATED=TRUE");
    }
    try (Connection connection2 = getConnectionBuilder().withDatabase("yb1").connect();
         Statement stmt = connection2.createStatement()) {
      // Create a table with a set table_oid.
      stmt.execute("SET yb_enable_create_with_table_oid = true");
      stmt.execute("CREATE TABLE test_tbl (h INT PRIMARY KEY, a INT, b FLOAT) " +
                   "WITH (table_oid = 123456)");
      stmt.execute("INSERT INTO test_tbl (h, a, b) VALUES (1, 101, 3.14)");

      // Check that backup and restore works fine.
      String backupDir = YBBackupUtil.getTempBackupDir();
      String output = YBBackupUtil.runYbBackupCreate("--backup_location", backupDir,
          "--keyspace", "ysql.yb1");
      backupDir = new JSONObject(output).getString("snapshot_url");
      YBBackupUtil.runYbBackupRestore(backupDir, "--keyspace", "ysql.yb2");
    }
    // Verify data is correct.
    try (Connection connection2 = getConnectionBuilder().withDatabase("yb2").connect();
         Statement stmt = connection2.createStatement()) {
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE h=1", new Row(1, 101, 3.14));
      assertQuery(stmt, "SELECT b FROM test_tbl WHERE h=1", new Row(3.14));

      // Now try to do a backup/restore of the restored db.
      String backupDir = YBBackupUtil.getTempBackupDir();
      String output = YBBackupUtil.runYbBackupCreate("--backup_location", backupDir,
          "--keyspace", "ysql.yb2");
      backupDir = new JSONObject(output).getString("snapshot_url");
      YBBackupUtil.runYbBackupRestore(backupDir, "--keyspace", "ysql.yb3");
    }
    // Verify data is correct.
    try (Connection connection2 = getConnectionBuilder().withDatabase("yb3").connect();
         Statement stmt = connection2.createStatement()) {
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE h=1", new Row(1, 101, 3.14));
      assertQuery(stmt, "SELECT b FROM test_tbl WHERE h=1", new Row(3.14));
    }
    // Cleanup.
    try (Statement stmt = connection.createStatement()) {
      stmt.execute("DROP DATABASE yb1");
      stmt.execute("DROP DATABASE yb2");
      stmt.execute("DROP DATABASE yb3");
    }
  }

  @Test
  public void testAlteredTableWithNotNull() throws Exception {
    try (Statement stmt = connection.createStatement()) {
      stmt.execute("CREATE TABLE test_tbl(id int)");

      stmt.execute("ALTER TABLE test_tbl ADD a int NOT NULL");
      stmt.execute("ALTER TABLE test_tbl ADD b int NULL");

      stmt.execute("INSERT INTO test_tbl(id, a, b) VALUES (1, 2, 3)");
      stmt.execute("INSERT INTO test_tbl(id, a) VALUES (2, 4)");

      runInvalidQuery(
          stmt, "INSERT INTO test_tbl(id, b) VALUES(3, 6)",
          "null value in column \"a\" violates not-null constraint");

      String backupDir = YBBackupUtil.getTempBackupDir();
      String output = YBBackupUtil.runYbBackupCreate("--backup_location", backupDir,
          "--keyspace", "ysql.yugabyte");
      backupDir = new JSONObject(output).getString("snapshot_url");
      stmt.execute("INSERT INTO test_tbl (id, a, b) VALUES (9999, 9, 9)");

      YBBackupUtil.runYbBackupRestore(backupDir, "--keyspace", "ysql.yb2");
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE id=1", new Row(1, 2, 3));
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE id=2", new Row(2, 4, (Integer) null));
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE id=3");
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE id=9999", new Row(9999, 9, 9));
    }

    try (Connection connection2 = getConnectionBuilder().withDatabase("yb2").connect();
         Statement stmt = connection2.createStatement()) {
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE id=1", new Row(1, 2, 3));
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE id=2", new Row(2, 4, (Integer) null));
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE id=3");
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE id=9999");
    }

    // Cleanup.
    try (Statement stmt = connection.createStatement()) {
      stmt.execute("DROP DATABASE yb2");
    }
  }

  @Test
  public void testIndex() throws Exception {
    String backupDir = null;
    try (Statement stmt = connection.createStatement()) {
      stmt.execute("CREATE TABLE test_tbl (h INT PRIMARY KEY, a INT, b FLOAT)");
      stmt.execute("CREATE INDEX test_idx ON test_tbl (a)");

      for (int i = 1; i <= 1000; ++i) {
        stmt.execute("INSERT INTO test_tbl (h, a, b) VALUES" +
          " (" + String.valueOf(i) +                     // h
          ", " + String.valueOf(100 + i) +               // a
          ", " + String.valueOf(2.14 + (float)i) + ")"); // b
      }

      backupDir = YBBackupUtil.getTempBackupDir();
      String output = YBBackupUtil.runYbBackupCreate("--backup_location", backupDir,
          "--keyspace", "ysql.yugabyte");
      backupDir = new JSONObject(output).getString("snapshot_url");

      stmt.execute("INSERT INTO test_tbl (h, a, b) VALUES (9999, 8888, 8.9)");

      assertQuery(stmt, "SELECT * FROM test_tbl WHERE h=1", new Row(1, 101, 3.14));
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE h=1000", new Row(1000, 1100, 1002.14));
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE h=9999", new Row(9999, 8888, 8.9));

      // Select via the index.
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE a=101", new Row(1, 101, 3.14));
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE a=1100", new Row(1000, 1100, 1002.14));
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE a=8888", new Row(9999, 8888, 8.9));
    }

    // Add a new node.
    miniCluster.startTServer(getTServerFlags());
    // Wait for node list refresh.
    Thread.sleep(MiniYBCluster.CQL_NODE_LIST_REFRESH_SECS * 2 * 1000);
    YBBackupUtil.setTSAddresses(miniCluster.getTabletServers());
    YBBackupUtil.runYbBackupRestore(backupDir, "--keyspace", "ysql.yb2");

    try (Connection connection2 = getConnectionBuilder().withDatabase("yb2").connect();
         Statement stmt = connection2.createStatement()) {
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE h=1", new Row(1, 101, 3.14));
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE h=1000", new Row(1000, 1100, 1002.14));
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE h=9999");

      // Select via the index.
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE a=101", new Row(1, 101, 3.14));
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE a=1100", new Row(1000, 1100, 1002.14));
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE a=8888");
    }

    // Cleanup.
    try (Statement stmt = connection.createStatement()) {
      stmt.execute("DROP DATABASE yb2");
    }
  }

  @Test
  public void testIndexTypes() throws Exception {
    String backupDir = null;
    try (Statement stmt = connection.createStatement()) {
      stmt.execute("CREATE TABLE test_tbl (h INT PRIMARY KEY, c1 INT, c2 INT, c3 INT, c4 INT)");
      stmt.execute("CREATE INDEX test_idx1 ON test_tbl (c1)");
      stmt.execute("CREATE INDEX test_idx2 ON test_tbl (c2 HASH)");
      stmt.execute("CREATE INDEX test_idx3 ON test_tbl (c3 ASC)");
      stmt.execute("CREATE INDEX test_idx4 ON test_tbl (c4 DESC)");

      stmt.execute("INSERT INTO test_tbl (h, c1, c2, c3, c4) VALUES (1, 11, 12, 13, 14)");

      backupDir = YBBackupUtil.getTempBackupDir();
      String output = YBBackupUtil.runYbBackupCreate("--backup_location", backupDir,
          "--keyspace", "ysql.yugabyte");
      backupDir = new JSONObject(output).getString("snapshot_url");

      stmt.execute("INSERT INTO test_tbl (h, c1, c2, c3, c4) VALUES (9, 21, 22, 23, 24)");

      assertQuery(stmt, "SELECT * FROM test_tbl WHERE h=1", new Row(1, 11, 12, 13, 14));
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE c1=11", new Row(1, 11, 12, 13, 14));
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE c2=12", new Row(1, 11, 12, 13, 14));
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE c3=13", new Row(1, 11, 12, 13, 14));
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE c4=14", new Row(1, 11, 12, 13, 14));

      assertQuery(stmt, "SELECT * FROM test_tbl WHERE h=9", new Row(9, 21, 22, 23, 24));
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE c1=21", new Row(9, 21, 22, 23, 24));
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE c2=22", new Row(9, 21, 22, 23, 24));
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE c3=23", new Row(9, 21, 22, 23, 24));
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE c4=24", new Row(9, 21, 22, 23, 24));
    }

    YBBackupUtil.runYbBackupRestore(backupDir, "--keyspace", "ysql.yb2");

    try (Connection connection2 = getConnectionBuilder().withDatabase("yb2").connect();
         Statement stmt = connection2.createStatement()) {
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE h=1", new Row(1, 11, 12, 13, 14));
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE c1=11", new Row(1, 11, 12, 13, 14));
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE c2=12", new Row(1, 11, 12, 13, 14));
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE c3=13", new Row(1, 11, 12, 13, 14));
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE c4=14", new Row(1, 11, 12, 13, 14));

      assertQuery(stmt, "SELECT * FROM test_tbl WHERE h=9");
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE c1=21");
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE c2=22");
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE c3=23");
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE c4=24");
    }

    // Cleanup.
    try (Statement stmt = connection.createStatement()) {
      stmt.execute("DROP DATABASE yb2");
    }
  }

  private void verifyCollationIndexData(Statement stmt, int test_step) throws Exception {
    Row expectedLowerCaseRow = new Row("a", "b", "c", "d", "e");
    Row expectedUpperCaseRow = new Row("A", "B", "C", "D", "E");
    List<Row> expectedRows = test_step == 2 ? Arrays.asList(new Row("a", "b", "c", "d", "e"),
                                                            new Row("A", "B", "C", "D", "E"),
                                                            new Row("f", "g", "h", "i", "j"))
                                            : Arrays.asList(new Row("a", "b", "c", "d", "e"),
                                                            new Row("A", "B", "C", "D", "E"));
    assertRowList(stmt, "SELECT * FROM test_tbl ORDER BY h", expectedRows);
    assertRowList(stmt, "SELECT * FROM test_tbl ORDER BY c1", expectedRows);
    assertRowList(stmt, "SELECT * FROM test_tbl ORDER BY c2", expectedRows);
    assertRowList(stmt, "SELECT * FROM test_tbl ORDER BY c3", expectedRows);
    assertRowList(stmt, "SELECT * FROM test_tbl ORDER BY c4", expectedRows);
    assertQuery(stmt, "SELECT * FROM test_tbl WHERE h='a'", expectedLowerCaseRow);
    assertQuery(stmt, "SELECT * FROM test_tbl WHERE c1='b'", expectedLowerCaseRow);
    assertQuery(stmt, "SELECT * FROM test_tbl WHERE c2='c'", expectedLowerCaseRow);
    assertQuery(stmt, "SELECT * FROM test_tbl WHERE c3='d'", expectedLowerCaseRow);
    assertQuery(stmt, "SELECT * FROM test_tbl WHERE c4='e'", expectedLowerCaseRow);
    assertQuery(stmt, "SELECT * FROM test_tbl WHERE h='A'", expectedUpperCaseRow);
    assertQuery(stmt, "SELECT * FROM test_tbl WHERE c1='B'", expectedUpperCaseRow);
    assertQuery(stmt, "SELECT * FROM test_tbl WHERE c2='C'", expectedUpperCaseRow);
    assertQuery(stmt, "SELECT * FROM test_tbl WHERE c3='D'", expectedUpperCaseRow);
    assertQuery(stmt, "SELECT * FROM test_tbl WHERE c4='E'", expectedUpperCaseRow);
    if (test_step == 2) {
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE h='f'", new Row("f", "g", "h", "i", "j"));
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE c1='g'", new Row("f", "g", "h", "i", "j"));
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE c2='h'", new Row("f", "g", "h", "i", "j"));
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE c3='i'", new Row("f", "g", "h", "i", "j"));
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE c4='j'", new Row("f", "g", "h", "i", "j"));
    } else {
      assertNoRows(stmt, "SELECT * FROM test_tbl WHERE h='f'");
      assertNoRows(stmt, "SELECT * FROM test_tbl WHERE c1='g'");
      assertNoRows(stmt, "SELECT * FROM test_tbl WHERE c2='h'");
      assertNoRows(stmt, "SELECT * FROM test_tbl WHERE c3='i'");
      assertNoRows(stmt, "SELECT * FROM test_tbl WHERE c4='j'");
    }
  }

  private void testCollationIndexTypesHelper(String collName) throws Exception {
    String backupDir = null;
    try (Statement stmt = connection.createStatement()) {
      stmt.execute("DROP TABLE IF EXISTS test_tbl");
      stmt.execute("CREATE TABLE test_tbl (h TEXT PRIMARY KEY COLLATE \"" + collName + "\", " +
                   "c1 TEXT COLLATE \"" + collName + "\", " +
                   "c2 TEXT COLLATE \"" + collName + "\", " +
                   "c3 TEXT COLLATE \"" + collName + "\", " +
                   "c4 TEXT COLLATE \"" + collName + "\")");
      stmt.execute("CREATE INDEX test_idx1 ON test_tbl (c1)");
      stmt.execute("CREATE INDEX test_idx2 ON test_tbl (c2 HASH)");
      stmt.execute("CREATE INDEX test_idx3 ON test_tbl (c3 ASC)");
      stmt.execute("CREATE INDEX test_idx4 ON test_tbl (c4 DESC)");

      stmt.execute("INSERT INTO test_tbl (h, c1, c2, c3, c4) VALUES ('a', 'b', 'c', 'd', 'e')");
      stmt.execute("INSERT INTO test_tbl (h, c1, c2, c3, c4) VALUES ('A', 'B', 'C', 'D', 'E')");

      verifyCollationIndexData(stmt, 1);

      backupDir = YBBackupUtil.getTempBackupDir();
      String output = YBBackupUtil.runYbBackupCreate("--backup_location", backupDir,
          "--keyspace", "ysql.yugabyte");
      backupDir = new JSONObject(output).getString("snapshot_url");

      stmt.execute("INSERT INTO test_tbl (h, c1, c2, c3, c4) VALUES ('f', 'g', 'h', 'i', 'j')");

      verifyCollationIndexData(stmt, 2);
    }

    YBBackupUtil.runYbBackupRestore(backupDir, "--keyspace", "ysql.yb2");

    try (Connection connection2 = getConnectionBuilder().withDatabase("yb2").connect();
         Statement stmt = connection2.createStatement()) {
      verifyCollationIndexData(stmt, 3);
    }

    // Cleanup.
    try (Statement stmt = connection.createStatement()) {
      stmt.execute("DROP DATABASE yb2");
    }
  }

  @Test
  public void testCollationIndexTypesEn() throws Exception {
    testCollationIndexTypesHelper("en-US-x-icu");
  }

  @Test
  public void testCollationIndexTypesSv() throws Exception {
    testCollationIndexTypesHelper("sv-x-icu");
  }

  @Test
  public void testCollationIndexTypesTr() throws Exception {
    testCollationIndexTypesHelper("tr-x-icu");
  }

  private void verifyPartialIndexData(Statement stmt) throws Exception {
    assertQuery(stmt, "SELECT * FROM test_tbl WHERE h=1", new Row(1, 11, 22));
    assertQuery(stmt, "SELECT * FROM test_tbl WHERE c1=11", new Row(1, 11, 22));
    assertQuery(stmt, "SELECT * FROM test_tbl WHERE c2=22", new Row(1, 11, 22));

    assertQuery(stmt, "SELECT * FROM \"WHERE_tbl\" WHERE h=1", new Row(1, 11, 22));
    assertQuery(stmt, "SELECT * FROM \"WHERE_tbl\" WHERE \"WHERE_c1\"=11", new Row(1, 11, 22));
    assertQuery(stmt, "SELECT * FROM \"WHERE_tbl\" WHERE \" WHERE c2\"=22", new Row(1, 11, 22));
  }

  @Test
  public void testPartialIndexes() throws Exception {
    String backupDir = null;
    try (Statement stmt = connection.createStatement()) {
      stmt.execute("CREATE TABLE test_tbl (h INT PRIMARY KEY, c1 INT, c2 INT)");
      stmt.execute("CREATE INDEX test_idx1 ON test_tbl (c1) WHERE (c1 IS NOT NULL)");
      stmt.execute("CREATE INDEX test_idx2 ON test_tbl (c2 ASC) WHERE (c2 IS NOT NULL)");

      stmt.execute("INSERT INTO test_tbl (h, c1, c2) VALUES (1, 11, 22)");

      stmt.execute("CREATE TABLE \"WHERE_tbl\" " +
                     "(h INT PRIMARY KEY, \"WHERE_c1\" INT, \" WHERE c2\" INT)");
      stmt.execute("CREATE INDEX ON \"WHERE_tbl\" " +
                     "(\"WHERE_c1\") WHERE (\"WHERE_c1\" IS NOT NULL)");
      stmt.execute("CREATE INDEX ON \"WHERE_tbl\" " +
                     "(\" WHERE c2\") WHERE (\" WHERE c2\" IS NOT NULL)");

      stmt.execute("INSERT INTO \"WHERE_tbl\" (h, \"WHERE_c1\", \" WHERE c2\") VALUES (1, 11, 22)");

      backupDir = YBBackupUtil.getTempBackupDir();
      String output = YBBackupUtil.runYbBackupCreate("--backup_location", backupDir,
          "--keyspace", "ysql.yugabyte");
      backupDir = new JSONObject(output).getString("snapshot_url");
      verifyPartialIndexData(stmt);
    }

    YBBackupUtil.runYbBackupRestore(backupDir, "--keyspace", "ysql.yb2");

    try (Connection connection2 = getConnectionBuilder().withDatabase("yb2").connect();
         Statement stmt = connection2.createStatement()) {
      verifyPartialIndexData(stmt);
    }

    // Cleanup.
    try (Statement stmt = connection.createStatement()) {
      stmt.execute("DROP DATABASE yb2");
    }
  }

  @Test
  public void testRestoreWithRestoreTime() throws Exception {
    try (Statement stmt = connection.createStatement()) {
      stmt.execute("CREATE TABLE  test_tbl (h INT PRIMARY KEY, a INT, b FLOAT)");

      for (int i = 1; i <= 2000; ++i) {
        stmt.execute("INSERT INTO test_tbl (h, a, b) VALUES" +
            " (" + String.valueOf(i) +                     // h
            ", " + String.valueOf(100 + i) +               // a
            ", " + String.valueOf(2.14 + (float)i) + ")"); // b
      }

      // Get the current timestamp in microseconds.
      String ts = Long.toString(ChronoUnit.MICROS.between(Instant.EPOCH, Instant.now()));

      // Insert additional values into the table before taking the backup.
      stmt.execute("INSERT INTO test_tbl (h, a, b) VALUES (9999, 789, 8.9)");

      String backupDir = YBBackupUtil.getTempBackupDir();
      String output = YBBackupUtil.runYbBackupCreate("--backup_location", backupDir,
          "--keyspace", "ysql.yugabyte");
      backupDir = new JSONObject(output).getString("snapshot_url");

      // Backup using --restore_time.
      YBBackupUtil.runYbBackupRestore(backupDir, "--keyspace", "ysql.yb2", "--restore_time", ts);
    }

    // Verify we only restore the original rows.
    try (Connection connection2 = getConnectionBuilder().withDatabase("yb2").connect();
        Statement stmt = connection2.createStatement()) {
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE h=1", new Row(1, 101, 3.14));
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE h=2000", new Row(2000, 2100, 2002.14));
      assertQuery(stmt, "SELECT * FROM test_tbl WHERE h=9999");  // Should not exist.
    }
  }

  @Test
  public void testBackupCreateGetBackupSize() throws Exception {
    try (Statement stmt = connection.createStatement()) {
      stmt.execute("CREATE TABLE  test_tbl (h INT PRIMARY KEY, a INT, b FLOAT)");

      for (int i = 1; i <= 2000; ++i) {
        stmt.execute("INSERT INTO test_tbl (h, a, b) VALUES" +
            " (" + String.valueOf(i) +                     // h
            ", " + String.valueOf(100 + i) +               // a
            ", " + String.valueOf(2.14 + (float)i) + ")"); // b
      }

      String backupDir = YBBackupUtil.getTempBackupDir();
      String output = YBBackupUtil.runYbBackupCreate("--backup_location", backupDir,
          "--keyspace", "ysql.yugabyte", "--pg_based_backup", "--disable_checksum");
      JSONObject json = new JSONObject(output);
      long expectedBackupSize = json.getLong("backup_size_in_bytes");
      long actualBackupSize = FileUtils.sizeOfDirectory(new File(json.getString("snapshot_url")));
      long allowedDelta = 1 * 1024;     // 1 KB
      assertLessThan(Math.abs(expectedBackupSize - actualBackupSize), allowedDelta);
    }
  }

  @Test
  public void testSedRegExpForYSQLDump() throws Exception {
    try (Statement stmt = connection.createStatement()) {
      stmt.execute("CREATE ROLE  admin");
      // Default DB & table owner is ROLE 'yugabyte'.
      stmt.execute("CREATE TABLE  test_tbl (h INT PRIMARY KEY, a INT)");

      String backupDir = YBBackupUtil.getTempBackupDir();
      String output = YBBackupUtil.runYbBackupCreate("--backup_location", backupDir,
          "--keyspace", "ysql.yugabyte");
      backupDir = new JSONObject(output).getString("snapshot_url");

      // Restore with the table owner renaming on fly.
      YBBackupUtil.runYbBackupRestore(backupDir, "--keyspace", "ysql.yb2",
          "--edit_ysql_dump_sed_reg_exp", "s|OWNER TO yugabyte_test|OWNER TO admin|");

      // In this DB the table owner was not changed.
      assertEquals("yugabyte_test", getOwnerForTable(stmt, "test_tbl"));
    }

    // Verify the changed table owner for the restored table.
    try (Connection connection2 = getConnectionBuilder().withDatabase("yb2").connect();
         Statement stmt = connection2.createStatement()) {
      assertEquals("admin", getOwnerForTable(stmt, "test_tbl"));
    }

    // Cleanup.
    try (Statement stmt = connection.createStatement()) {
      stmt.execute("DROP DATABASE yb2");
    }
  }

  public Set<String> subDirs(String path) throws Exception {
    Set<String> dirs = new HashSet<String>();
    for (File f : new File(path).listFiles(File::isDirectory)) {
      dirs.add(f.getName());
    }
    return dirs;
  }

  public void checkTabletsInDir(String path, List<String>... tabletLists) throws Exception {
    Set<String> dirs = subDirs(path);
    for(List<String> tablets : tabletLists) {
      for (String tabletID : tablets) {
        assertTrue(dirs.contains("tablet-" + tabletID));
      }
    }
  }

  public void doTestGeoPartitionedBackup(
      String targetDB, int numRegions, boolean useTablespaces) throws Exception {
    try (Statement stmt = connection.createStatement()) {
      stmt.execute(
          " CREATE TABLESPACE region1_ts " +
          "  WITH (replica_placement=" +
          "'{\"num_replicas\":1, \"placement_blocks\":" +
          "[{\"cloud\":\"cloud1\",\"region\":\"region1\",\"zone\":\"zone1\"," +
          "\"min_num_replicas\":1}]}')");
      stmt.execute(
          " CREATE TABLESPACE region2_ts " +
          "  WITH (replica_placement=" +
          "'{\"num_replicas\":1, \"placement_blocks\":" +
          "[{\"cloud\":\"cloud2\",\"region\":\"region2\",\"zone\":\"zone2\"," +
          "\"min_num_replicas\":1}]}')");
      stmt.execute(
          " CREATE TABLESPACE region3_ts " +
          "  WITH (replica_placement=" +
          "'{\"num_replicas\":1, \"placement_blocks\":" +
          "[{\"cloud\":\"cloud3\",\"region\":\"region3\",\"zone\":\"zone3\"," +
          "\"min_num_replicas\":1}]}')");

      stmt.execute("CREATE TABLE tbl (id INT, geo VARCHAR) PARTITION BY LIST (geo)");
      stmt.execute("CREATE TABLE tbl_r1 PARTITION OF tbl (id, geo, PRIMARY KEY (id HASH, geo))" +
                   "FOR VALUES IN ('R1') TABLESPACE region1_ts");
      stmt.execute("CREATE TABLE tbl_r2 PARTITION OF tbl (id, geo, PRIMARY KEY (id HASH, geo))" +
                   "FOR VALUES IN ('R2') TABLESPACE region2_ts");
      stmt.execute("CREATE TABLE tbl_r3 PARTITION OF tbl (id, geo, PRIMARY KEY (id HASH, geo))" +
                   "FOR VALUES IN ('R3') TABLESPACE region3_ts");
      // Check tablespaces for tables.
      assertEquals(null, getTablespaceForTable(stmt, "tbl"));
      assertEquals("region1_ts", getTablespaceForTable(stmt, "tbl_r1"));
      assertEquals("region2_ts", getTablespaceForTable(stmt, "tbl_r2"));
      assertEquals("region3_ts", getTablespaceForTable(stmt, "tbl_r3"));

      for (int i = 1; i <= 2000; ++i) {
        stmt.execute("INSERT INTO tbl (id, geo) VALUES" +
          " (" + String.valueOf(i) +                  // id
          ", 'R" + String.valueOf(1 + i % 3) + "')"); // geo
      }

      List<String> tblTablets = getTabletsForTable("yugabyte", "tbl");
      List<String> tblR1Tablets = getTabletsForTable("yugabyte", "tbl_r1");
      List<String> tblR2Tablets = getTabletsForTable("yugabyte", "tbl_r2");
      List<String> tblR3Tablets = getTabletsForTable("yugabyte", "tbl_r3");

      String backupDir = YBBackupUtil.getTempBackupDir(), output = null;
      List<String> args = new ArrayList<>(Arrays.asList("--keyspace", "ysql.yugabyte"));
      if (useTablespaces) {
        args.add("--use_tablespaces");
      }

      switch (numRegions) {
        case 0:
          args.addAll(Arrays.asList("--backup_location", backupDir));
          output = YBBackupUtil.runYbBackupCreate(args);
          backupDir = new JSONObject(output).getString("snapshot_url");
          checkTabletsInDir(backupDir, tblTablets, tblR1Tablets, tblR2Tablets, tblR3Tablets);
          break;
        case 1:
          args.addAll(Arrays.asList(
              "--region", "region1", "--region_location", backupDir + "_reg1",
              "--backup_location", backupDir));
          output = YBBackupUtil.runYbBackupCreate(args);
          backupDir = new JSONObject(output).getString("snapshot_url");
          checkTabletsInDir(backupDir, tblR2Tablets, tblR3Tablets);
          checkTabletsInDir(backupDir + "_reg1", tblR1Tablets);
          break;
        case 3:
          args.addAll(Arrays.asList(
            "--region", "region1", "--region_location", backupDir + "_reg1",
            "--region", "region2", "--region_location", backupDir + "_reg2",
            "--region", "region3", "--region_location", backupDir + "_reg3",
            "--backup_location", backupDir));
          output = YBBackupUtil.runYbBackupCreate(args);
          backupDir = new JSONObject(output).getString("snapshot_url");
          assertTrue(subDirs(backupDir).isEmpty());
          checkTabletsInDir(backupDir + "_reg1", tblR1Tablets);
          checkTabletsInDir(backupDir + "_reg2", tblR2Tablets);
          checkTabletsInDir(backupDir + "_reg3", tblR3Tablets);
          break;
        default:
          throw new IllegalArgumentException("Unexpected numRegions: " + numRegions);
      }

      stmt.execute("INSERT INTO tbl (id, geo) VALUES (9999, 'R1')");
      assertQuery(stmt, "SELECT * FROM tbl WHERE id=1", new Row(1, "R2"));
      assertQuery(stmt, "SELECT * FROM tbl WHERE id=2000", new Row(2000, "R3"));
      assertQuery(stmt, "SELECT * FROM tbl WHERE id=9999", new Row(9999, "R1"));
      assertQuery(stmt, "SELECT COUNT(*) FROM tbl", new Row(2001));

      args.clear();
      if (useTablespaces) {
        args.add("--use_tablespaces");
      }

      if (!targetDB.equals("yugabyte")) {
        // Drop TABLEs and TABLESPACEs.
        stmt.execute("DROP TABLE tbl_r1");
        stmt.execute("DROP TABLE tbl_r2");
        stmt.execute("DROP TABLE tbl_r3");
        stmt.execute("DROP TABLE tbl");
        stmt.execute("DROP TABLESPACE region1_ts");
        stmt.execute("DROP TABLESPACE region2_ts");
        stmt.execute("DROP TABLESPACE region3_ts");

        // Check global TABLESPACEs.
        assertRowSet(stmt, "SELECT spcname FROM pg_tablespace",
            asSet(new Row("pg_default"), new Row("pg_global")));

        args.addAll(Arrays.asList("--keyspace", "ysql." + targetDB));
      }
      // else - overwriting existing tables in DB "yugabyte".

      YBBackupUtil.runYbBackupRestore(backupDir, args);
    }

    try (Connection connection2 = getConnectionBuilder().withDatabase(targetDB).connect();
         Statement stmt = connection2.createStatement()) {
      assertQuery(stmt, "SELECT * FROM tbl WHERE id=1", new Row(1, "R2"));
      assertQuery(stmt, "SELECT * FROM tbl WHERE id=2000", new Row(2000, "R3"));
      assertQuery(stmt, "SELECT COUNT(*) FROM tbl", new Row(2000));
      // This row was inserted after backup so it is absent here.
      assertNoRows(stmt, "SELECT * FROM tbl WHERE id=9999");

      assertEquals(null, getTablespaceForTable(stmt, "tbl"));
      // Check global TABLESPACEs.
      Set<Row> expectedTablespaces = asSet(new Row("pg_default"), new Row("pg_global"));
      if (useTablespaces || targetDB.equals("yugabyte")) {
        assertEquals("region1_ts", getTablespaceForTable(stmt, "tbl_r1"));
        assertEquals("region2_ts", getTablespaceForTable(stmt, "tbl_r2"));
        assertEquals("region3_ts", getTablespaceForTable(stmt, "tbl_r3"));

        expectedTablespaces.addAll(
            asSet(new Row("region1_ts"), new Row("region2_ts"), new Row("region3_ts")));
      } else {
        assertEquals(null, getTablespaceForTable(stmt, "tbl_r1"));
        assertEquals(null, getTablespaceForTable(stmt, "tbl_r2"));
        assertEquals(null, getTablespaceForTable(stmt, "tbl_r3"));
      }
      assertRowSet(stmt, "SELECT spcname FROM pg_tablespace", expectedTablespaces);
    }

    if (!targetDB.equals("yugabyte")) {
      // Cleanup.
      try (Statement stmt = connection.createStatement()) {
        stmt.execute("DROP DATABASE " + targetDB);
      }
    }
  }

  @Test
  public void testGeoPartitioning() throws Exception {
    doTestGeoPartitionedBackup("db2", 3, false);
  }

  @Test
  public void testGeoPartitioningNoRegions() throws Exception {
    doTestGeoPartitionedBackup("db2", 0, false);
  }

  @Test
  public void testGeoPartitioningOneRegion() throws Exception {
    doTestGeoPartitionedBackup("db2", 1, false);
  }

  @Test
  public void testGeoPartitioningRestoringIntoExisting() throws Exception {
    doTestGeoPartitionedBackup("yugabyte", 3, false);
  }

  @Test
  public void testGeoPartitioningWithTablespaces() throws Exception {
    doTestGeoPartitionedBackup("db2", 3, true);
  }

  @Test
  public void testGeoPartitioningNoRegionsWithTablespaces() throws Exception {
    doTestGeoPartitionedBackup("db2", 0, true);
  }

  @Test
  public void testGeoPartitioningOneRegionWithTablespaces() throws Exception {
    doTestGeoPartitionedBackup("db2", 1, true);
  }

  @Test
  public void testGeoPartitioningRestoringIntoExistingWithTablespaces() throws Exception {
    doTestGeoPartitionedBackup("yugabyte", 3, true);
  }

  @Test
  public void testUserDefinedTypes() throws Exception {
    // TODO(myang): Add ALTER TYPE test after #1893 is fixed.
    String backupDir = null;
    try (Statement stmt = connection.createStatement()) {
      // A enum type.
      stmt.execute("CREATE TYPE e_t AS ENUM('c', 'b', 'a')");

      // Table column of enum type.
      stmt.execute("CREATE TABLE test_tb1(c1 e_t)");
      stmt.execute("INSERT INTO test_tb1 VALUES ('b'), ('c')");

      // Table column of enum type with default value.
      stmt.execute("CREATE TABLE test_tb2(c1 INT, c2 e_t DEFAULT 'a')");
      stmt.execute("INSERT INTO test_tb2 VALUES(1)");

      // A user-defined type.
      stmt.execute("CREATE TYPE udt1 AS (f1 INT, f2 TEXT, f3 e_t)");

      // Table column of user-defined type.
      stmt.execute("CREATE TABLE test_tb3(c1 udt1)");
      stmt.execute("INSERT INTO test_tb3 VALUES((1, '1', 'a'))");
      stmt.execute("INSERT INTO test_tb3 VALUES((1, '2', 'b'))");
      stmt.execute("INSERT INTO test_tb3 VALUES((1, '2', 'c'))");

      // Table column of user-defined type and enum type with default values.
      stmt.execute("CREATE TABLE test_tb4(c1 INT, c2 udt1 DEFAULT (1, '2', 'b'), " +
                   "c3 e_t DEFAULT 'b')");
      stmt.execute("INSERT INTO test_tb4 VALUES (1)");

      // Table column of enum array type.
      stmt.execute("CREATE TABLE test_tb5 (c1 e_t[])");
      stmt.execute("INSERT INTO test_tb5 VALUES (ARRAY['a', 'b', 'c']::e_t[])");

      // nested user-defined type and enum type.
      stmt.execute("CREATE TYPE udt2 AS (f1 INT, f2 udt1, f3 e_t)");

      // Table column of nested user-defined type and enum type.
      stmt.execute("CREATE TABLE test_tb6(c1 INT, c2 udt2, c3 e_t)");
      stmt.execute("INSERT INTO test_tb6 VALUES (1, (1, (1, '1', 'a'), 'b'), 'c')");

      // Table column of array of nested user-defined type and enum type.
      stmt.execute("CREATE TABLE test_tb7 (c1 udt2[])");
      stmt.execute("INSERT INTO test_tb7 VALUES (ARRAY[" +
                   "(1, (1, (1, '1', 'a'), 'b'), 'c')," +
                   "(2, (2, (2, '2', 'b'), 'a'), 'c')," +
                   "(3, (3, (3, '3', 'a'), 'c'), 'b')]::udt2[])");

      // A domain type.
      stmt.execute("CREATE DOMAIN dom AS TEXT " +
                   "check(value ~ '^\\d{5}$'or value ~ '^\\d{5}-\\d{4}$')");
      // Table column of array of domain type.
      stmt.execute("CREATE TABLE test_tb8(c1 dom[])");
      stmt.execute("INSERT INTO test_tb8 VALUES (ARRAY['32768', '65536']::dom[])");

      // A range type.
      stmt.execute("CREATE TYPE inetrange AS RANGE(subtype = inet)");
      // Table column of range type.
      stmt.execute("CREATE TABLE test_tb9(c1 inetrange)");
      stmt.execute("INSERT INTO test_tb9 VALUES ('[10.0.0.1,10.0.0.2]'::inetrange)");
      // Table column of array of range type.
      stmt.execute("CREATE TABLE test_tb10(c1 inetrange[])");
      stmt.execute("INSERT INTO test_tb10 VALUES (ARRAY[" +
                   "'[10.0.0.1,10.0.0.2]'::inetrange, '[10.0.0.3,10.0.0.8]'::inetrange])");

      // Test drop column in the middle.
      stmt.execute("CREATE TABLE test_tb11(c1 e_t, c2 e_t, c3 e_t, c4 e_t)");
      stmt.execute("INSERT INTO test_tb11 VALUES (" +
                   "'a', 'b', 'c', 'a'), ('a', 'c', 'b', 'b'), ('b', 'a', 'c', 'c')");
      stmt.execute("ALTER TABLE test_tb11 DROP COLUMN c2");

      backupDir = YBBackupUtil.getTempBackupDir();
      String output = YBBackupUtil.runYbBackupCreate("--backup_location", backupDir,
          "--keyspace", "ysql.yugabyte");
      backupDir = new JSONObject(output).getString("snapshot_url");

      stmt.execute("INSERT INTO test_tb1 VALUES ('a')");
      stmt.execute("INSERT INTO test_tb2 VALUES(2)");
      stmt.execute("INSERT INTO test_tb3 VALUES((2, '1', 'a'))");
      stmt.execute("INSERT INTO test_tb3 VALUES((2, '2', 'b'))");
      stmt.execute("INSERT INTO test_tb3 VALUES((2, '2', 'c'))");
      stmt.execute("INSERT INTO test_tb4 VALUES (2)");
      stmt.execute("INSERT INTO test_tb5 VALUES (ARRAY['c', 'b', 'a']::e_t[])");
      stmt.execute("INSERT INTO test_tb6 VALUES (2, (2, (2, '2', 'c'), 'b'), 'a')");
      stmt.execute("INSERT INTO test_tb7 VALUES (ARRAY[" +
                   "(4, (4, (4, '4', 'c'), 'b'), 'a')]::udt2[])");
      stmt.execute("INSERT INTO test_tb8 VALUES (ARRAY['16384', '81920']::dom[])");
      stmt.execute("INSERT INTO test_tb9 VALUES ('[10.0.0.3,10.0.0.8]'::inetrange)");
      stmt.execute("INSERT INTO test_tb10 VALUES (array['[10.0.0.9,10.0.0.12]'::inetrange])");
      stmt.execute("ALTER TABLE test_tb11 DROP COLUMN c3");

      List<Row> expectedRows1 = Arrays.asList(new Row("c"),
                                              new Row("b"),
                                              new Row("a"));
      List<Row> expectedRows2 = Arrays.asList(new Row(1, "a"),
                                              new Row(2, "a"));
      List<Row> expectedRows3 = Arrays.asList(new Row("(1,1,a)"),
                                              new Row("(1,2,c)"),
                                              new Row("(1,2,b)"),
                                              new Row("(2,1,a)"),
                                              new Row("(2,2,c)"),
                                              new Row("(2,2,b)"));
      List<Row> expectedRows4 = Arrays.asList(new Row(1, "(1,2,b)", "b"),
                                              new Row(2, "(1,2,b)", "b"));
      List<Row> expectedRows5 = Arrays.asList(new Row("{a,b,c}"),
                                              new Row("{c,b,a}"));
      List<Row> expectedRows6 = Arrays.asList(new Row(1, "(1,\"(1,1,a)\",b)", "c"),
                                              new Row(2, "(2,\"(2,2,c)\",b)", "a"));
      List<Row> expectedRows7 = Arrays.asList(
        new Row("{\"(1,\\\"(1,\\\"\\\"(1,1,a)\\\"\\\",b)\\\",c)\"," +
                 "\"(2,\\\"(2,\\\"\\\"(2,2,b)\\\"\\\",a)\\\",c)\"," +
                 "\"(3,\\\"(3,\\\"\\\"(3,3,a)\\\"\\\",c)\\\",b)\"}"),
        new Row("{\"(4,\\\"(4,\\\"\\\"(4,4,c)\\\"\\\",b)\\\",a)\"}"));
      List<Row> expectedRows8 = Arrays.asList(new Row("{16384,81920}"),
                                              new Row("{32768,65536}"));
      List<Row> expectedRows9 = Arrays.asList(new Row("[10.0.0.1,10.0.0.2]"),
                                              new Row("[10.0.0.3,10.0.0.8]"));
      List<Row> expectedRows10 = Arrays.asList(
        new Row("{\"[10.0.0.1,10.0.0.2]\",\"[10.0.0.3,10.0.0.8]\"}"),
        new Row("{\"[10.0.0.9,10.0.0.12]\"}"));

      // Column c2 and c3 are dropped from test_tbl1 so we only expect to see rows for
      // column c1 and c4.
      List<Row> expectedRows11 = Arrays.asList(new Row("b", "c"),
                                               new Row("a", "b"),
                                               new Row("a", "a"));
      assertRowList(stmt, "SELECT * FROM test_tb1 ORDER BY c1", expectedRows1);
      assertRowList(stmt, "SELECT * FROM test_tb1 ORDER BY c1", expectedRows1);
      assertRowList(stmt, "SELECT * FROM test_tb2 ORDER BY c1", expectedRows2);
      assertRowList(stmt, "SELECT * FROM test_tb3 ORDER BY c1", expectedRows3);
      assertRowList(stmt, "SELECT * FROM test_tb4 ORDER BY c1", expectedRows4);
      assertRowList(stmt, "SELECT c1::TEXT FROM test_tb5 ORDER BY c1", expectedRows5);
      assertRowList(stmt, "SELECT * FROM test_tb6 ORDER BY c1", expectedRows6);
      assertRowList(stmt, "SELECT c1::TEXT FROM test_tb7 ORDER BY c1", expectedRows7);
      assertRowList(stmt, "SELECT c1::TEXT FROM test_tb8 ORDER BY c1", expectedRows8);
      assertRowList(stmt, "SELECT * FROM test_tb9 ORDER BY c1", expectedRows9);
      assertRowList(stmt, "SELECT c1::TEXT FROM test_tb10 ORDER BY c1", expectedRows10);
      assertRowList(stmt, "SELECT * FROM test_tb11 ORDER BY c1, c4", expectedRows11);
    }

    YBBackupUtil.runYbBackupRestore(backupDir, "--keyspace", "ysql.yb2");

    try (Connection connection2 = getConnectionBuilder().withDatabase("yb2").connect();
         Statement stmt = connection2.createStatement()) {
      List<Row> expectedRows1 = Arrays.asList(new Row("c"),
                                              new Row("b"));
      List<Row> expectedRows2 = Arrays.asList(new Row(1, "a"));
      List<Row> expectedRows3 = Arrays.asList(new Row("(1,1,a)"),
                                              new Row("(1,2,c)"),
                                              new Row("(1,2,b)"));
      List<Row> expectedRows4 = Arrays.asList(new Row(1, "(1,2,b)", "b"));
      List<Row> expectedRows5 = Arrays.asList(new Row("{a,b,c}"));
      List<Row> expectedRows6 = Arrays.asList(new Row(1, "(1,\"(1,1,a)\",b)", "c"));
      List<Row> expectedRows7 = Arrays.asList(
        new Row("{\"(1,\\\"(1,\\\"\\\"(1,1,a)\\\"\\\",b)\\\",c)\"," +
                 "\"(2,\\\"(2,\\\"\\\"(2,2,b)\\\"\\\",a)\\\",c)\"," +
                 "\"(3,\\\"(3,\\\"\\\"(3,3,a)\\\"\\\",c)\\\",b)\"}"));
      List<Row> expectedRows8 = Arrays.asList(new Row("{32768,65536}"));
      List<Row> expectedRows9 = Arrays.asList(new Row("[10.0.0.1,10.0.0.2]"));
      List<Row> expectedRows10 = Arrays.asList(
        new Row("{\"[10.0.0.1,10.0.0.2]\",\"[10.0.0.3,10.0.0.8]\"}"));

      // Only column c2 is dropped from test_tbl1 before backup, the column c3 was dropped
      // after backup and it should be restored. Therefore we expect to see rows for column
      // c1, c3 and c4.
      List<Row> expectedRows11 = Arrays.asList(new Row("b", "c", "c"),
                                               new Row("a", "c", "a"),
                                               new Row("a", "b", "b"));
      assertRowList(stmt, "SELECT * FROM test_tb1 ORDER BY c1", expectedRows1);
      assertRowList(stmt, "SELECT * FROM test_tb2 ORDER BY c1", expectedRows2);
      assertRowList(stmt, "SELECT * FROM test_tb3 ORDER BY c1", expectedRows3);
      assertRowList(stmt, "SELECT * FROM test_tb4 ORDER BY c1", expectedRows4);
      assertRowList(stmt, "SELECT c1::TEXT FROM test_tb5 ORDER BY c1", expectedRows5);
      assertRowList(stmt, "SELECT * FROM test_tb6 ORDER BY c1", expectedRows6);
      assertRowList(stmt, "SELECT c1::TEXT FROM test_tb7 ORDER BY c1", expectedRows7);
      assertRowList(stmt, "SELECT c1::TEXT FROM test_tb8 ORDER BY c1", expectedRows8);
      assertRowList(stmt, "SELECT * FROM test_tb9 ORDER BY c1", expectedRows9);
      assertRowList(stmt, "SELECT c1::TEXT FROM test_tb10 ORDER BY c1", expectedRows10);
      assertRowList(stmt, "SELECT * FROM test_tb11 ORDER BY c1, c3, c4", expectedRows11);
    }

    // Cleanup.
    try (Statement stmt = connection.createStatement()) {
      stmt.execute("DROP DATABASE yb2");
    }
  }

  private void testMaterializedViewsHelper(boolean matviewOnMatview) throws Exception {
    String backupDir = null;
    try (Statement stmt = connection.createStatement()) {
      stmt.execute("DROP TABLE IF EXISTS test_tbl");
      stmt.execute("CREATE TABLE test_tbl (t int)");
      stmt.execute("CREATE MATERIALIZED VIEW test_mv AS SELECT * FROM test_tbl");
      if (matviewOnMatview) {
        stmt.execute("CREATE MATERIALIZED VIEW test_mv_2 AS SELECT * FROM test_mv");
      }
      stmt.execute("INSERT INTO test_tbl VALUES (1)");
      stmt.execute("REFRESH MATERIALIZED VIEW test_mv");
      if (matviewOnMatview) {
        stmt.execute("REFRESH MATERIALIZED VIEW test_mv_2");
      }
      backupDir = YBBackupUtil.getTempBackupDir();
      String output = YBBackupUtil.runYbBackupCreate("--backup_location", backupDir,
          "--keyspace", "ysql.yugabyte");
      backupDir = new JSONObject(output).getString("snapshot_url");
    }

    YBBackupUtil.runYbBackupRestore(backupDir, "--keyspace", "ysql.yb2");

    try (Connection connection2 = getConnectionBuilder().withDatabase("yb2").connect();
         Statement stmt = connection2.createStatement()) {
        assertQuery(stmt, "SELECT * FROM test_mv WHERE t=1", new Row(1));
        if (matviewOnMatview) {
          assertQuery(stmt, "SELECT * FROM test_mv_2 WHERE t=1", new Row(1));
        }
    }
  }

  @Test
  public void testRefreshedMaterializedViewsBackup() throws Exception {
    testMaterializedViewsHelper(false);
  }

  @Test
  public void testRefreshedMaterializedViewsOnMaterializedViewsBackup() throws Exception {
    testMaterializedViewsHelper(true);
  }
}
