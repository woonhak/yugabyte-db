/*--------------------------------------------------------------------------------------------------
 *
 * ybccmds.c
 *        YB commands for creating and altering table structures and settings
 *
 * Copyright (c) YugaByte, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
 * in compliance with the License.  You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed under the License
 * is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
 * or implied.  See the License for the specific language governing permissions and limitations
 * under the License.
 *
 * IDENTIFICATION
 *        src/backend/commands/ybccmds.c
 *
 *------------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"
#include "access/sysattr.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "catalog/pg_am.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_class.h"
#include "catalog/pg_database.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_type.h"
#include "catalog/pg_type_d.h"
#include "catalog/yb_type.h"
#include "commands/dbcommands.h"
#include "commands/tablegroup.h"
#include "commands/tablecmds.h"
#include "commands/ybccmds.h"

#include "access/htup_details.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/relcache.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "executor/executor.h"
#include "executor/tuptable.h"
#include "executor/ybcExpr.h"

#include "yb/yql/pggate/ybc_pggate.h"
#include "pg_yb_utils.h"

#include "access/nbtree.h"
#include "commands/defrem.h"
#include "nodes/nodeFuncs.h"
#include "parser/parser.h"
#include "parser/parse_coerce.h"
#include "parser/parse_type.h"
#include "parser/parse_utilcmd.h"

/* Utility function to calculate column sorting options */
static void
ColumnSortingOptions(SortByDir dir, SortByNulls nulls, bool* is_desc, bool* is_nulls_first)
{
  if (dir == SORTBY_DESC) {
	/*
	 * From postgres doc NULLS FIRST is the default for DESC order.
	 * So SORTBY_NULLS_DEFAULT is equal to SORTBY_NULLS_FIRST here.
	 */
	*is_desc = true;
	*is_nulls_first = (nulls != SORTBY_NULLS_LAST);
  } else {
	/*
	 * From postgres doc ASC is the default sort order and NULLS LAST is the default for it.
	 * So SORTBY_DEFAULT is equal to SORTBY_ASC and SORTBY_NULLS_DEFAULT is equal
	 * to SORTBY_NULLS_LAST here.
	 */
	*is_desc = false;
	*is_nulls_first = (nulls == SORTBY_NULLS_FIRST);
  }
}

/* -------------------------------------------------------------------------- */
/*  Database Functions. */

void
YBCCreateDatabase(Oid dboid, const char *dbname, Oid src_dboid, Oid next_oid, bool colocated)
{
	YBCPgStatement handle;

	HandleYBStatus(YBCPgNewCreateDatabase(dbname,
										  dboid,
										  src_dboid,
										  next_oid,
										  colocated,
										  &handle));
	HandleYBStatus(YBCPgExecCreateDatabase(handle));
}

void
YBCDropDatabase(Oid dboid, const char *dbname)
{
	YBCPgStatement handle;

	HandleYBStatus(YBCPgNewDropDatabase(dbname,
										dboid,
										&handle));
	bool not_found = false;
	HandleYBStatusIgnoreNotFound(YBCPgExecDropDatabase(handle), &not_found);
}

void
YBCReserveOids(Oid dboid, Oid next_oid, uint32 count, Oid *begin_oid, Oid *end_oid)
{
	HandleYBStatus(YBCPgReserveOids(dboid,
									next_oid,
									count,
									begin_oid,
									end_oid));
}

/* ------------------------------------------------------------------------- */
/*  Tablegroup Functions. */
void
YBCCreateTablegroup(Oid grp_oid, Oid tablespace_oid)
{
	YBCPgStatement handle;
	char *db_name = get_database_name(MyDatabaseId);

	HandleYBStatus(YBCPgNewCreateTablegroup(db_name, MyDatabaseId,
											grp_oid, tablespace_oid, &handle));
	HandleYBStatus(YBCPgExecCreateTablegroup(handle));
}

void
YBCDropTablegroup(Oid grpoid)
{
	YBCPgStatement handle;

	HandleYBStatus(YBCPgNewDropTablegroup(MyDatabaseId, grpoid, &handle));
	YBSaveDdlHandle(handle);
}


/* ------------------------------------------------------------------------- */
/*  Table Functions. */
static void CreateTableAddColumn(YBCPgStatement handle,
								 Form_pg_attribute att,
								 bool is_hash,
								 bool is_primary,
								 bool is_desc,
								 bool is_nulls_first)
{
	const AttrNumber attnum = att->attnum;
	const YBCPgTypeEntity *col_type = YbDataTypeFromOidMod(attnum,
															att->atttypid);
	HandleYBStatus(YBCPgCreateTableAddColumn(handle,
											 NameStr(att->attname),
											 attnum,
											 col_type,
											 is_hash,
											 is_primary,
											 is_desc,
											 is_nulls_first));
}

/* Utility function to add columns to the YB create statement
 * Columns need to be sent in order first hash columns, then rest of primary
 * key columns, then regular columns.
 */
static void CreateTableAddColumns(YBCPgStatement handle,
								  TupleDesc desc,
								  Constraint *primary_key,
								  const bool colocated,
								  Oid tablegroupId)
{
	ListCell  *cell;
	IndexElem *index_elem;

	/* For tables created WITH (oids = true), we expect oid column to be the only PK. */
	if (desc->tdhasoid)
	{
		if (!primary_key ||
			list_length(primary_key->yb_index_params) != 1 ||
			strcmp(linitial_node(IndexElem, primary_key->yb_index_params)->name,
				   "oid") != 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("OID should be the only primary key column")));

		index_elem = linitial_node(IndexElem, primary_key->yb_index_params);
		SortByDir order = index_elem->ordering;
		/*
		 * We can only have OID columns on system catalog tables
		 * and we disallow hash partitioning on those, so OID is not allowed
		 * to be a hash column - but that will be caught normally.
		 */
		bool is_hash = (order == SORTBY_HASH ||
						(order == SORTBY_DEFAULT &&
						 !colocated && tablegroupId == InvalidOid));
		bool is_desc = false;
		bool is_nulls_first = false;
		ColumnSortingOptions(order,
							 index_elem->nulls_ordering,
							 &is_desc,
							 &is_nulls_first);
		const YBCPgTypeEntity *col_type =
			YbDataTypeFromOidMod(ObjectIdAttributeNumber, OIDOID);
		HandleYBStatus(YBCPgCreateTableAddColumn(handle,
												 "oid",
												 ObjectIdAttributeNumber,
												 col_type,
												 is_hash,
												 true /* is_primary */,
												 is_desc,
												 is_nulls_first));
	}
	else if (primary_key != NULL)
	{
		/* Add all key columns first with respect to compound key order */
		foreach(cell, primary_key->yb_index_params)
		{
			index_elem = lfirst_node(IndexElem, cell);
			bool column_found = false;
			for (int i = 0; i < desc->natts; ++i)
			{
				Form_pg_attribute att = TupleDescAttr(desc, i);
				if (strcmp(NameStr(att->attname), index_elem->name) == 0)
				{
					if (!YbDataTypeIsValidForKey(att->atttypid))
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("PRIMARY KEY containing column of type"
										" '%s' not yet supported",
										YBPgTypeOidToStr(att->atttypid))));
					SortByDir order = index_elem->ordering;
					/* In YB mode, the first column defaults to HASH if it is
					 * not set and its table is not colocated */
					const bool is_first_key =
						cell == list_head(primary_key->yb_index_params);
					bool is_hash = (order == SORTBY_HASH ||
									(is_first_key &&
									 order == SORTBY_DEFAULT &&
									 !colocated && tablegroupId == InvalidOid));
					bool is_desc = false;
					bool is_nulls_first = false;
					ColumnSortingOptions(order,
										 index_elem->nulls_ordering,
										 &is_desc,
										 &is_nulls_first);
					CreateTableAddColumn(handle,
										 att,
										 is_hash,
										 true /* is_primary */,
										 is_desc,
										 is_nulls_first);
					column_found = true;
					break;
				}
			}
			if (!column_found)
				ereport(FATAL,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("Column '%s' not found in table",
								index_elem->name)));
		}
	}

	/* Add all non-key columns */
	for (int i = 0; i < desc->natts; ++i)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);
		bool is_key = false;
		if (primary_key)
			foreach(cell, primary_key->yb_index_params)
			{
				IndexElem *index_elem = (IndexElem *) lfirst(cell);
				if (strcmp(NameStr(att->attname), index_elem->name) == 0)
				{
					is_key = true;
					break;
				}
			}
		if (!is_key)
			CreateTableAddColumn(handle,
								 att,
								 false /* is_hash */,
								 false /* is_primary */,
								 false /* is_desc */,
								 false /* is_nulls_first */);
	}
}

static void
YBTransformPartitionSplitPoints(YBCPgStatement yb_stmt,
								List *split_points,
								Form_pg_attribute *attrs,
								int attr_count)
{
	/* Parser state for type conversion and validation */
	ParseState *pstate = make_parsestate(NULL);

	/* Construct values */
	PartitionRangeDatum *datums[INDEX_MAX_KEYS];
	int datum_count = 0;
	ListCell *lc;
	foreach(lc, split_points) {
		YBTransformPartitionSplitValue(pstate, castNode(List, lfirst(lc)), attrs, attr_count,
										datums, &datum_count);

		/* Convert the values to yugabyte format and bind to statement. */
		YBCPgExpr exprs[INDEX_MAX_KEYS];
		int idx;
		for (idx = 0; idx < datum_count; idx++) {
			switch (datums[idx]->kind)
			{
				case PARTITION_RANGE_DATUM_VALUE:
				{
					/* Given value is not null. Convert it to YugaByte format. */
					Const *value = castNode(Const, datums[idx]->value);
					/*
					 * Use attr->attcollation because the split value will be compared against
					 * collation-encoded strings that are encoded using the column collation.
					 * We assume collation-encoding will likely to retain the similar key
					 * distribution as the original text values.
					 */
					Form_pg_attribute attr = attrs[idx];
					exprs[idx] = YBCNewConstant(yb_stmt, value->consttype, attr->attcollation,
												value->constvalue, false /* is_null */);
					break;
				}

				case PARTITION_RANGE_DATUM_MAXVALUE:
				{
					/* Create MINVALUE in YugaByte format */
					Form_pg_attribute attr = attrs[idx];
					exprs[idx] = YBCNewConstantVirtual(yb_stmt, attr->atttypid,
													   YB_YQL_DATUM_LIMIT_MAX);
					break;
				}

				case PARTITION_RANGE_DATUM_MINVALUE:
				{
					/* Create MINVALUE in YugaByte format */
					Form_pg_attribute attr = attrs[idx];
					exprs[idx] = YBCNewConstantVirtual(yb_stmt, attr->atttypid,
													   YB_YQL_DATUM_LIMIT_MIN);
					break;
				}
			}
		}

		/* Defaulted to MINVALUE for the rest of the columns that are not assigned a value */
		for (; idx < attr_count; idx++) {
			Form_pg_attribute attr = attrs[idx];
			exprs[idx] = YBCNewConstantVirtual(yb_stmt, attr->atttypid,
											   YB_YQL_DATUM_LIMIT_MIN);
		}

		/* Add the split boundary to CREATE statement */
		HandleYBStatus(YBCPgAddSplitBoundary(yb_stmt, exprs, attr_count));
	}
}

/* Utility function to handle split points */
static void CreateTableHandleSplitOptions(YBCPgStatement handle,
										  TupleDesc desc,
										  OptSplit *split_options,
										  Constraint *primary_key,
										  Oid namespaceId,
										  const bool colocated,
										  YBCPgOid tablegroup_id)
{
	/* Address both types of split options */
	switch (split_options->split_type)
	{
		case NUM_TABLETS:
		{
			/* Make sure we have HASH columns */
			bool hashable = true;
			if (primary_key) {
				/* If a primary key exists, we utilize it to check its ordering */
				ListCell *head = list_head(primary_key->yb_index_params);
				IndexElem *index_elem = (IndexElem*) lfirst(head);

				if (!index_elem ||
				   !(index_elem->ordering == SORTBY_HASH ||
				   index_elem->ordering == SORTBY_DEFAULT))
					hashable = false;
			} else {
				/* In the abscence of a primary key, we use ybrowid as the PK to hash partition */
				bool is_pg_catalog_table_ =
					IsSystemNamespace(namespaceId) && IsToastNamespace(namespaceId);
				/*
				 * Checking if  table_oid is valid simple means if the table is
				 * part of a tablegroup.
				 */
				hashable = !is_pg_catalog_table_ && !colocated && tablegroup_id == kInvalidOid;
			}

			if (!hashable)
				ereport(ERROR, (errmsg("HASH columns must be present to "
							"split by number of tablets")));
			/* Tell pggate about it */
			HandleYBStatus(YBCPgCreateTableSetNumTablets(handle, split_options->num_tablets));
			break;
		}

		case SPLIT_POINTS:
		{
			if (primary_key == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("Cannot split table that does not have primary key")));

			/* Find the column descriptions for primary key (split columns). */
			Form_pg_attribute attrs[INDEX_MAX_KEYS];
			ListCell *lc;
			int attr_count = 0;
			foreach(lc, primary_key->yb_index_params)
			{
				const char *col_name = castNode(IndexElem, lfirst(lc))->name;
				for (int i = 0; i < desc->natts; i++)
				{
					Form_pg_attribute att = TupleDescAttr(desc, i);
					if (strcmp(NameStr(att->attname), col_name) == 0)
					{
						attrs[attr_count++] = att;
						break;
					}
				}
			}

			/* Analyze split_points and add them to CREATE statement */
			YBTransformPartitionSplitPoints(handle, split_options->split_points, attrs, attr_count);
			break;
		}

		default:
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("Invalid split options")));
	}
}

void
YBCCreateTable(CreateStmt *stmt, char relkind, TupleDesc desc,
							 Oid relationId, Oid namespaceId, Oid tablegroupId, Oid tablespaceId,
							 Oid matviewPgTableId)
{
	if (relkind != RELKIND_RELATION && relkind != RELKIND_PARTITIONED_TABLE &&
		relkind != RELKIND_MATVIEW)
	{
		return;
	}

	if (stmt->relation->relpersistence == RELPERSISTENCE_TEMP)
	{
		return; /* Nothing to do. */
	}

	YBCPgStatement handle = NULL;
	ListCell       *listptr;
	bool           is_shared_relation = tablespaceId == GLOBALTABLESPACE_OID;
	Oid            databaseId         = YBCGetDatabaseOidFromShared(is_shared_relation);

	char *db_name = get_database_name(databaseId);
	char *schema_name = stmt->relation->schemaname;
	if (schema_name == NULL)
	{
		schema_name = get_namespace_name(namespaceId);
	}
	if (!IsBootstrapProcessingMode())
		YBC_LOG_INFO("Creating Table %s.%s.%s",
					 db_name,
					 schema_name,
					 stmt->relation->relname);

	Constraint *primary_key = NULL;

	foreach(listptr, stmt->constraints)
	{
		Constraint *constraint = lfirst(listptr);

		if (constraint->contype == CONSTR_PRIMARY)
		{
			primary_key = constraint;
		}
	}

	/*
	 * If this is a partition table, check whether it needs to inherit the same
	 * primary key as the parent partitioned table.
	 */
	if (primary_key == NULL && stmt->partbound)
	{
		/*
		 * This relation is not created yet and not visible to other
		 * backends. It doesn't really matter what lock we take here.
		 */
		Relation rel = relation_open(relationId, AccessShareLock);

		/* Find the parent partitioned table */
		RangeVar   *rv = (RangeVar *) lfirst(list_head(stmt->inhRelations));
		Oid	parentOid = RangeVarGetRelid(rv, NoLock, false);

		Relation parentRel = heap_open(parentOid, NoLock);
		List *idxlist = RelationGetIndexList(parentRel);
		ListCell *cell;
		foreach(cell, idxlist)
		{
			Relation    idxRel = index_open(lfirst_oid(cell), AccessShareLock);
			/* Fetch pg_index tuple for source index from relcache entry */
			if (!((Form_pg_index) GETSTRUCT(idxRel->rd_indextuple))->indisprimary)
			{
				/*
				 * This is not a primary key index so this doesn't matter.
				 */
				relation_close(idxRel, AccessShareLock);
				continue;
			}
			AttrNumber *attmap;
			IndexStmt  *idxstmt;
			Oid         constraintOid;

			attmap = convert_tuples_by_name_map(RelationGetDescr(rel),
								RelationGetDescr(parentRel),
								gettext_noop("could not convert row type"));
			idxstmt =
				generateClonedIndexStmt(NULL, RelationGetRelid(rel), idxRel,
						attmap, RelationGetDescr(rel)->natts,
						&constraintOid);

			primary_key = makeNode(Constraint);
			primary_key->contype      = CONSTR_PRIMARY;
			primary_key->conname      = idxstmt->idxname;
			primary_key->options      = idxstmt->options;
			primary_key->indexspace   = idxstmt->tableSpace;

			ListCell *idxcell;
			foreach(idxcell, idxstmt->indexParams)
			{
				IndexElem* ielem = lfirst(idxcell);
				primary_key->keys =
					lappend(primary_key->keys, makeString(ielem->name));
				primary_key->yb_index_params =
					lappend(primary_key->yb_index_params, ielem);
			}

			relation_close(idxRel, AccessShareLock);
		}
		heap_close(parentRel, NoLock);
		heap_close(rel, AccessShareLock);
	}

	/* By default, inherit the colocated option from the database */
	bool colocated = MyDatabaseColocated;

	/* Handle user-supplied colocated reloption */
	ListCell *opt_cell;
	foreach(opt_cell, stmt->options)
	{
		DefElem *def = (DefElem *) lfirst(opt_cell);

		if (strcmp(def->defname, "colocated") == 0)
		{
			if (tablegroupId != InvalidOid)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot use \'colocated=true/false\' with tablegroup")));

			bool colocated_relopt = defGetBoolean(def);
			if (MyDatabaseColocated)
				colocated = colocated_relopt;
			else if (colocated_relopt)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot set colocated true on a non-colocated"
								" database")));
			/* The following break is fine because there should only be one
			 * colocated reloption at this point due to checks in
			 * parseRelOptions */
			break;
		}
	}

	if (colocated && stmt->tablespacename)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("cannot create colocated table with a tablespace")));

	HandleYBStatus(YBCPgNewCreateTable(db_name,
									   schema_name,
									   stmt->relation->relname,
									   databaseId,
									   relationId,
									   is_shared_relation,
									   false, /* if_not_exists */
									   primary_key == NULL /* add_primary_key */,
									   colocated,
									   tablegroupId,
									   tablespaceId,
									   matviewPgTableId,
									   &handle));

	CreateTableAddColumns(handle, desc, primary_key, colocated, tablegroupId);

	/* Handle SPLIT statement, if present */
	OptSplit *split_options = stmt->split_options;
	if (split_options)
		CreateTableHandleSplitOptions(
			handle, desc, split_options, primary_key, namespaceId, colocated, tablegroupId);

	/* Create the table. */
	HandleYBStatus(YBCPgExecCreateTable(handle));
}

void
YBCDropTable(Oid relationId)
{
	YBCPgStatement  handle     = NULL;
	Oid             databaseId = YBCGetDatabaseOidByRelid(relationId);
	bool            colocated  = false;

	/* Determine if table is colocated */
	if (MyDatabaseColocated)
	{
		bool not_found = false;
		HandleYBStatusIgnoreNotFound(YBCPgIsTableColocated(databaseId,
														   relationId,
														   &colocated),
									 &not_found);
	}

	/* Create table-level tombstone for colocated tables / tables in a tablegroup */
	Oid tablegroupId = InvalidOid;
	if (YbTablegroupCatalogExists)
		tablegroupId = get_tablegroup_oid_by_table_oid(relationId);
	if (colocated || tablegroupId != InvalidOid)
	{
		bool not_found = false;
		HandleYBStatusIgnoreNotFound(YBCPgNewTruncateColocated(databaseId,
															   relationId,
															   false,
															   &handle),
									 &not_found);
		/* Since the creation of the handle could return a 'NotFound' error,
		 * execute the statement only if the handle is valid.
		 */
		const bool valid_handle = !not_found;
		if (valid_handle)
		{
			HandleYBStatusIgnoreNotFound(YBCPgDmlBindTable(handle), &not_found);
			int rows_affected_count = 0;
			HandleYBStatusIgnoreNotFound(YBCPgDmlExecWriteOp(handle, &rows_affected_count),
										 &not_found);
		}
	}

	/* Drop the table */
	{
		bool not_found = false;
		Relation relation = relation_open(relationId, AccessExclusiveLock);
		HandleYBStatusIgnoreNotFound(YBCPgNewDropTable(databaseId,
													   YbGetStorageRelid(relation),
													   false, /* if_exists */
													   &handle),
													   &not_found);
		relation_close(relation, AccessExclusiveLock);
		const bool valid_handle = !not_found;
		if (valid_handle)
		{
			/*
			 * We cannot abort drop in DocDB so postpone the execution until
			 * the rest of the statement/txn is finished executing.
			 */
			YBSaveDdlHandle(handle);
		}
	}
}

void
YBCTruncateTable(Relation rel) {
	YBCPgStatement  handle;
	Oid             relationId = RelationGetRelid(rel);
	Oid             databaseId = YBCGetDatabaseOid(rel);
	bool            colocated  = false;

	/* Determine if table is colocated */
	if (MyDatabaseColocated)
		HandleYBStatus(YBCPgIsTableColocated(databaseId,
											 relationId,
											 &colocated));
	Oid tablegroupId = InvalidOid;
	if (YbTablegroupCatalogExists)
		tablegroupId = get_tablegroup_oid_by_table_oid(relationId);
	if (colocated || tablegroupId != InvalidOid)
	{
		/* Create table-level tombstone for colocated tables / tables in tablegroups */
		HandleYBStatus(YBCPgNewTruncateColocated(databaseId,
												 relationId,
												 false,
												 &handle));
		HandleYBStatus(YBCPgDmlBindTable(handle));
		int rows_affected_count = 0;
		HandleYBStatus(YBCPgDmlExecWriteOp(handle, &rows_affected_count));
	}
	else
	{
		/* Send truncate table RPC to master for non-colocated tables */
		HandleYBStatus(YBCPgNewTruncateTable(databaseId,
											 relationId,
											 &handle));
		HandleYBStatus(YBCPgExecTruncateTable(handle));
	}

	if (!rel->rd_rel->relhasindex)
		return;

	/* Truncate the associated secondary indexes */
	List	 *indexlist = RelationGetIndexList(rel);
	ListCell *lc;

	foreach(lc, indexlist)
	{
		Oid indexId = lfirst_oid(lc);

		if (indexId == rel->rd_pkindex)
			continue;

		/* Determine if index is colocated */
		if (MyDatabaseColocated)
			HandleYBStatus(YBCPgIsTableColocated(databaseId,
												indexId,
												&colocated));

		tablegroupId = InvalidOid;
		if (YbTablegroupCatalogExists)
			tablegroupId = get_tablegroup_oid_by_table_oid(indexId);
		if (colocated || tablegroupId != InvalidOid)
		{
			/* Create index-level tombstone for colocated indexes / indexes in tablegroups */
			HandleYBStatus(YBCPgNewTruncateColocated(databaseId,
													 indexId,
													 false,
													 &handle));
			HandleYBStatus(YBCPgDmlBindTable(handle));
			int rows_affected_count = 0;
			HandleYBStatus(YBCPgDmlExecWriteOp(handle, &rows_affected_count));
		}
		else
		{
			/* Send truncate table RPC to master for non-colocated tables */
			HandleYBStatus(YBCPgNewTruncateTable(databaseId,
												 indexId,
												 &handle));
			HandleYBStatus(YBCPgExecTruncateTable(handle));
		}
	}

	list_free(indexlist);
}

/* Utility function to handle split points */
static void
CreateIndexHandleSplitOptions(YBCPgStatement handle,
                              TupleDesc desc,
                              OptSplit *split_options,
                              int16 * coloptions)
{
	/* Address both types of split options */
	switch (split_options->split_type)
	{
		case NUM_TABLETS:
			/* Make sure we have HASH columns */
			if (!(coloptions[0] & INDOPTION_HASH))
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("HASH columns must be present to split by number of tablets")));

			HandleYBStatus(YBCPgCreateIndexSetNumTablets(handle, split_options->num_tablets));
			break;

		case SPLIT_POINTS:
		{
			/* Construct array to SPLIT column datatypes */
			Form_pg_attribute attrs[INDEX_MAX_KEYS];
			int attr_count;
			for (attr_count = 0; attr_count < desc->natts; ++attr_count)
			{
				attrs[attr_count] = TupleDescAttr(desc, attr_count);
			}

			/* Analyze split_points and add them to CREATE statement */
			YBTransformPartitionSplitPoints(handle, split_options->split_points, attrs, attr_count);
			break;
		}

		default:
			ereport(ERROR, (errmsg("Illegal memory state for SPLIT options")));
			break;
	}
}

void
YBCCreateIndex(const char *indexName,
			   IndexInfo *indexInfo,
			   TupleDesc indexTupleDesc,
			   int16 *coloptions,
			   Datum reloptions,
			   Oid indexId,
			   Relation rel,
			   OptSplit *split_options,
			   const bool skip_index_backfill,
			   Oid tablegroupId,
			   Oid tablespaceId)
{
	char *db_name	  = get_database_name(YBCGetDatabaseOid(rel));
	char *schema_name = get_namespace_name(RelationGetNamespace(rel));

	if (!IsBootstrapProcessingMode())
		YBC_LOG_INFO("Creating index %s.%s.%s",
					 db_name,
					 schema_name,
					 indexName);

	YBCPgStatement handle = NULL;

	HandleYBStatus(YBCPgNewCreateIndex(db_name,
									   schema_name,
									   indexName,
									   YBCGetDatabaseOid(rel),
									   indexId,
									   YbGetStorageRelid(rel),
									   rel->rd_rel->relisshared,
									   indexInfo->ii_Unique,
									   skip_index_backfill,
									   false, /* if_not_exists */
									   tablegroupId,
									   tablespaceId,
									   &handle));

	for (int i = 0; i < indexTupleDesc->natts; i++)
	{
		Form_pg_attribute     att         = TupleDescAttr(indexTupleDesc, i);
		char                  *attname    = NameStr(att->attname);
		AttrNumber            attnum      = att->attnum;
		const YBCPgTypeEntity *col_type   = YbDataTypeFromOidMod(attnum, att->atttypid);
		const bool            is_key      = (i < indexInfo->ii_NumIndexKeyAttrs);

		if (is_key)
		{
			if (!YbDataTypeIsValidForKey(att->atttypid))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("INDEX on column of type '%s' not yet supported",
								YBPgTypeOidToStr(att->atttypid))));
		}

		const int16 options        = coloptions[i];
		const bool  is_hash        = options & INDOPTION_HASH;
		const bool  is_desc        = options & INDOPTION_DESC;
		const bool  is_nulls_first = options & INDOPTION_NULLS_FIRST;

		HandleYBStatus(YBCPgCreateIndexAddColumn(handle,
												 attname,
												 attnum,
												 col_type,
												 is_hash,
												 is_key,
												 is_desc,
												 is_nulls_first));
	}

	/* Handle SPLIT statement, if present */
	if (split_options)
		CreateIndexHandleSplitOptions(handle, indexTupleDesc, split_options, coloptions);

	/* Create the index. */
	HandleYBStatus(YBCPgExecCreateIndex(handle));
}

static void
YBCPrepareAlterTableCmd(AlterTableCmd* cmd, Relation rel, YBCPgStatement handle,
						int* col, bool* needsYBAlter,
						YBCPgStatement* rollbackHandle,
						bool isPartitionOfAlteredTable)
{
	if (isPartitionOfAlteredTable)
	{
		/*
		 * This function was invoked on a child partition table to reflect
		 * the effects of Alter on its parent.
		 */
		switch (cmd->subtype)
		{
			case AT_AddColumnRecurse:
			case AT_DropColumnRecurse:
			case AT_AddConstraintRecurse:
			case AT_DropConstraintRecurse:
				break;
			default:
				/*
				 * This is not an alter command on a partitioned table that
				 * needs to trickle down to its child partitions. Nothing to
				 * do.
				 */
				return;
		}
	}
	Oid relationId = RelationGetRelid(rel);
	switch (cmd->subtype)
	{
		case AT_AddColumn:
		case AT_AddColumnToView:
		case AT_AddColumnRecurse:
		{
			ColumnDef* colDef = (ColumnDef *) cmd->def;
			Oid			typeOid;
			int32		typmod;
			HeapTuple	typeTuple;
			int order;

			/* Skip yb alter for IF NOT EXISTS with existing column */
			if (cmd->missing_ok)
			{
				HeapTuple tuple = SearchSysCacheAttName(relationId, colDef->colname);
				if (HeapTupleIsValid(tuple)) {
					ReleaseSysCache(tuple);
					break;
				}
			}

			typeTuple = typenameType(NULL, colDef->typeName, &typmod);
			typeOid = HeapTupleGetOid(typeTuple);
			ReleaseSysCache(typeTuple);
			order = RelationGetNumberOfAttributes(rel) + *col;
			const YBCPgTypeEntity *col_type = YbDataTypeFromOidMod(order, typeOid);

			HandleYBStatus(YBCPgAlterTableAddColumn(handle, colDef->colname,
						   order, col_type));
			++(*col);
			*needsYBAlter = true;

			/*
			 * Prepare the handle that will be used to rollback
			 * this change at the DocDB side. This is an add column
			 * statement, thus the equivalent rollback operation
			 * will be to drop the column.
			 */
			if (*rollbackHandle == NULL)
			{
				HandleYBStatus(YBCPgNewAlterTable(YBCGetDatabaseOid(rel),
												  relationId,
												  rollbackHandle));
			}
			HandleYBStatus(YBCPgAlterTableDropColumn(*rollbackHandle,
													 colDef->colname));
			break;
		}

		case AT_DropColumn:
		case AT_DropColumnRecurse:
		{
			/* Skip yb alter for IF EXISTS with non-existent column */
			if (cmd->missing_ok)
			{
				HeapTuple tuple = SearchSysCacheAttName(relationId, cmd->name);
				if (!HeapTupleIsValid(tuple))
					break;
				ReleaseSysCache(tuple);
			}

			HandleYBStatus(YBCPgAlterTableDropColumn(handle, cmd->name));
			*needsYBAlter = true;

			break;
		}

		case AT_AddIndex:
		case AT_AddIndexConstraint:
		{
			IndexStmt *index = (IndexStmt *) cmd->def;
			/* Only allow adding indexes when it is a unique or primary key constraint */
			if (!(index->unique || index->primary) || !index->isconstraint)
			{
				ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("This ALTER TABLE command is not yet supported.")));
			}

			break;
		}

		case AT_AlterColumnType:
		{
			/*
			 * Only supports variants that don't require on-disk changes.
			 * For now, that is just varchar and varbit.
			 */
			ColumnDef*			colDef = (ColumnDef *) cmd->def;
			HeapTuple			typeTuple;
			Form_pg_attribute	attTup;
			Oid					curTypId;
			Oid					newTypId;
			int32				curTypMod;
			int32				newTypMod;

			/* Get current typid and typmod of the column. */
			typeTuple = SearchSysCacheAttName(relationId, cmd->name);
			if (!HeapTupleIsValid(typeTuple))
			{
				ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN),
						errmsg("column \"%s\" of relation \"%s\" does not exist",
								cmd->name, RelationGetRelationName(rel))));
			}
			attTup = (Form_pg_attribute) GETSTRUCT(typeTuple);
			curTypId = attTup->atttypid;
			curTypMod = attTup->atttypmod;
			ReleaseSysCache(typeTuple);

			/* Get the new typid and typmod of the column. */
			typenameTypeIdAndMod(NULL, colDef->typeName, &newTypId, &newTypMod);

			/* Only varbit and varchar don't cause on-disk changes. */
			switch (newTypId)
			{
				case VARCHAROID:
				case VARBITOID:
				{
					/*
					* Check for type equality, and that the new size is greater than or equal
					* to the old size, unless the current size is infinite (-1).
					*/
					if (newTypId != curTypId ||
						(newTypMod < curTypMod && newTypMod != -1) ||
						(newTypMod > curTypMod && curTypMod == -1))
					{
						ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								errmsg("This ALTER TABLE command is not yet supported.")));
					}
					break;
				}

				default:
				{
					if (newTypId == curTypId && newTypMod == curTypMod)
					{
						/* Types are the same, no changes will occur. */
						break;
					}
					/* timestamp <-> timestamptz type change is allowed
						if no rewrite is needed */
					if (curTypId == TIMESTAMPOID && newTypId == TIMESTAMPTZOID &&
						!TimestampTimestampTzRequiresRewrite()) {
						break;
					}
					if (curTypId == TIMESTAMPTZOID && newTypId == TIMESTAMPOID &&
						!TimestampTimestampTzRequiresRewrite()) {
						break;
					}
					ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("This ALTER TABLE command is not yet supported.")));
				}
			}
			/*
			 * Do not allow collation update because that requires different collation
			 * encoding and therefore can cause on-disk changes.
			 */
			Oid cur_collation_id = attTup->attcollation;
			Oid new_collation_id = GetColumnDefCollation(NULL, colDef, newTypId);
			if (cur_collation_id != new_collation_id)
				ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("This ALTER TABLE command is not yet supported.")));
			break;
		}

		case AT_AddConstraint:
		case AT_AddConstraintRecurse:
		case AT_DropConstraint:
		case AT_DropConstraintRecurse:
		case AT_DropOids:
		case AT_EnableTrig:
		case AT_EnableAlwaysTrig:
		case AT_EnableReplicaTrig:
		case AT_EnableTrigAll:
		case AT_EnableTrigUser:
		case AT_DisableTrig:
		case AT_DisableTrigAll:
		case AT_DisableTrigUser:
		case AT_ChangeOwner:
		case AT_ColumnDefault:
		case AT_DropNotNull:
		case AT_SetNotNull:
		case AT_AddIdentity:
		case AT_SetIdentity:
		case AT_DropIdentity:
		case AT_EnableRowSecurity:
		case AT_DisableRowSecurity:
		case AT_ForceRowSecurity:
		case AT_NoForceRowSecurity:
		case AT_AttachPartition:
		case AT_DetachPartition:
		case AT_SetTableSpace:
			/* For these cases a YugaByte alter isn't required, so we do nothing. */
			break;

		default:
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("This ALTER TABLE command is not yet supported.")));
			break;
	}
}

YBCPgStatement
YBCPrepareAlterTable(List** subcmds,
					 int subcmds_size,
					 Oid relationId,
					 YBCPgStatement *rollbackHandle,
					 bool isPartitionOfAlteredTable)
{
	/* Appropriate lock was already taken */
	Relation rel = relation_open(relationId, NoLock);

	if (!IsYBRelation(rel))
	{
		relation_close(rel, NoLock);
		return NULL;
	}

	YBCPgStatement handle = NULL;
	HandleYBStatus(YBCPgNewAlterTable(YBCGetDatabaseOidByRelid(relationId),
									  relationId,
									  &handle));

	ListCell *lcmd;
	int col = 1;
	bool needsYBAlter = false;

	for (int cmd_idx = 0; cmd_idx < subcmds_size; ++cmd_idx)
	{
		foreach(lcmd, subcmds[cmd_idx])
		{
			YBCPrepareAlterTableCmd((AlterTableCmd *) lfirst(lcmd), rel, handle,
									 &col, &needsYBAlter, rollbackHandle,
									 isPartitionOfAlteredTable);
		}
	}
	relation_close(rel, NoLock);

	if (!needsYBAlter)
	{
		return NULL;
	}

	return handle;
}

void
YBCExecAlterTable(YBCPgStatement handle, Oid relationId)
{
	if (handle)
	{
		if (IsYBRelationById(relationId)) {
			HandleYBStatus(YBCPgExecAlterTable(handle));
		}
	}
}

void
YBCRename(RenameStmt *stmt, Oid relationId)
{
	YBCPgStatement	handle     = NULL;
	Oid				databaseId = YBCGetDatabaseOidByRelid(relationId);
	char		   *db_name	   = get_database_name(databaseId);

	switch (stmt->renameType)
	{
		case OBJECT_TABLE:
			HandleYBStatus(YBCPgNewAlterTable(databaseId,
											  relationId,
											  &handle));
			HandleYBStatus(YBCPgAlterTableRenameTable(handle, db_name, stmt->newname));
			break;

		case OBJECT_COLUMN:
		case OBJECT_ATTRIBUTE:

			HandleYBStatus(YBCPgNewAlterTable(databaseId,
											  relationId,
											  &handle));

			HandleYBStatus(YBCPgAlterTableRenameColumn(handle, stmt->subname, stmt->newname));
			break;

		default:
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("Renaming this object is not yet supported.")));

	}

	YBCExecAlterTable(handle, relationId);
}

void
YBCDropIndex(Oid relationId)
{
	YBCPgStatement	handle;
	bool			colocated  = false;
	Oid				databaseId = YBCGetDatabaseOidByRelid(relationId);

	/* Determine if table is colocated */
	if (MyDatabaseColocated)
	{
		bool not_found = false;
		HandleYBStatusIgnoreNotFound(YBCPgIsTableColocated(databaseId,
														   relationId,
														   &colocated),
									 &not_found);
	}

	/* Create table-level tombstone for colocated indexes / indexes in a tablegroup */
	Oid tablegroupId = InvalidOid;
	if (YbTablegroupCatalogExists)
		tablegroupId = get_tablegroup_oid_by_table_oid(relationId);
	if (colocated || tablegroupId != InvalidOid)
	{
		bool not_found = false;
		HandleYBStatusIgnoreNotFound(YBCPgNewTruncateColocated(databaseId,
															   relationId,
															   false,
															   &handle),
									 &not_found);
		const bool valid_handle = !not_found;
		if (valid_handle) {
			HandleYBStatusIgnoreNotFound(YBCPgDmlBindTable(handle), &not_found);
			int rows_affected_count = 0;
			HandleYBStatusIgnoreNotFound(YBCPgDmlExecWriteOp(handle, &rows_affected_count),
										 &not_found);
		}
	}

	/* Drop the index table */
	{
		bool not_found = false;
		HandleYBStatusIgnoreNotFound(YBCPgNewDropIndex(databaseId,
													   relationId,
													   false, /* if_exists */
													   &handle),
									 &not_found);
		const bool valid_handle = !not_found;
		if (valid_handle) {
			/*
			 * We cannot abort drop in DocDB so postpone the execution until
			 * the rest of the statement/txn is finished executing.
			 */
			YBSaveDdlHandle(handle);
		}
	}
}

bool
YBCIsTableColocated(Oid dboid, Oid relationId)
{
	bool colocated;
	HandleYBStatus(YBCPgIsTableColocated(dboid, relationId, &colocated));
	return colocated;
}

void
YbBackfillIndex(BackfillIndexStmt *stmt, DestReceiver *dest)
{
	IndexInfo  *indexInfo;
	ListCell   *cell;
	Oid			heapId;
	Oid			indexId;
	Relation	heapRel;
	Relation	indexRel;
	TupOutputState *tstate;
	YbPgExecOutParam *out_param;

	if (*YBCGetGFlags()->ysql_disable_index_backfill)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("backfill is not enabled")));

	/*
	 * Examine oid list.  Currently, we only allow it to be a single oid, but
	 * later it should handle multiple oids of indexes on the same indexed
	 * table.
	 * TODO(jason): fix from here downwards for issue #4785.
	 */
	if (list_length(stmt->oid_list) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("only a single oid is allowed in BACKFILL INDEX (see"
						" issue #4785)")));

	foreach(cell, stmt->oid_list)
	{
		indexId = lfirst_oid(cell);
	}

	heapId = IndexGetRelation(indexId, false);
	// TODO(jason): why ShareLock instead of ShareUpdateExclusiveLock?
	heapRel = heap_open(heapId, ShareLock);
	indexRel = index_open(indexId, ShareLock);

	indexInfo = BuildIndexInfo(indexRel);
	/*
	 * The index should be ready for writes because it should be on the
	 * BACKFILLING permission.
	 */
	Assert(indexInfo->ii_ReadyForInserts);
	indexInfo->ii_Concurrent = true;
	indexInfo->ii_BrokenHotChain = false;

	out_param = YbCreateExecOutParam();
	index_backfill(heapRel,
				   indexRel,
				   indexInfo,
				   false,
				   stmt->bfinfo,
				   out_param);

	index_close(indexRel, ShareLock);
	heap_close(heapRel, ShareLock);

	/* output tuples */
	tstate = begin_tup_output_tupdesc(dest, YbBackfillIndexResultDesc(stmt));
	do_text_output_oneline(tstate, out_param->bfoutput->data);
	end_tup_output(tstate);
}

TupleDesc YbBackfillIndexResultDesc(BackfillIndexStmt *stmt) {
	TupleDesc	tupdesc;
	Oid			result_type = TEXTOID;

	/* Need a tuple descriptor representing a single TEXT or XML column */
	tupdesc = CreateTemplateTupleDesc(1, false);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "BACKFILL SPEC",
					   result_type, -1, 0);
	return tupdesc;
}

void
YbDropAndRecreateIndex(Oid index_oid, Oid new_rel_id, Relation old_rel, AttrNumber *new_to_old_attmap) {
	Relation index_rel = index_open(index_oid, AccessExclusiveLock);

	/* Construct the new CREATE INDEX stmt */

	IndexStmt* index_stmt = generateClonedIndexStmt(NULL /* heapRel, we provide an oid instead */,
					new_rel_id,
					index_rel,
					new_to_old_attmap,
					RelationGetDescr(old_rel)->natts,
					NULL /* parent constraint OID pointer */);

	const char* index_name = RelationGetRelationName(index_rel);
	const char* index_namespace_name = get_namespace_name(index_rel->rd_rel->relnamespace);
	index_stmt->idxname = pstrdup(index_name);

	index_close(index_rel,  AccessExclusiveLock);

	/* Drop old index */

	DropStmt *stmt = makeNode(DropStmt);
	stmt->removeType = OBJECT_INDEX;
	stmt->missing_ok = false;
	stmt->objects = list_make1(list_make2(makeString(pstrdup(index_namespace_name)),
								makeString(pstrdup(index_name))));
	stmt->behavior = DROP_CASCADE;
	stmt->concurrent = false;

	RemoveRelations(stmt);

	/* Create the new index */

	DefineIndex(new_rel_id,
				index_stmt,
				InvalidOid, /* no predefined OID */
				InvalidOid, /* no parent index */
				InvalidOid, /* no parent constraint */
				false, /* is_alter_table */
				false, /* check_rights */
				false, /* check_not_in_use */
				false, /* skip_build */
				true /* quiet */);
}

/* ------------------------------------------------------------------------- */
/*  System validation. */
void
YBCValidatePlacement(const char *placement_info)
{
	HandleYBStatus(YBCPgValidatePlacement(placement_info));
}
