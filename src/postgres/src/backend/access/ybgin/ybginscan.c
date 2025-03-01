/*--------------------------------------------------------------------------
 *
 * ybginscan.c
 *	  routines to manage scans of Yugabyte inverted index relations
 *
 * Copyright (c) YugaByte, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License.  You may obtain a copy
 * of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 * IDENTIFICATION
 *			src/backend/access/ybgin/ybginscan.c
 *--------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/relscan.h"
#include "access/skey.h"
#include "access/ybgin.h"
#include "access/ybgin_private.h"
#include "commands/tablegroup.h"
#include "miscadmin.h"
#include "utils/palloc.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "pg_yb_utils.h"
#include "yb/yql/pggate/ybc_pggate.h"

/*
 * Parts copied from ginscan.c ginbeginscan.  Do the same thing except
 * - palloc size of YbginScanOpaqueData
 * - name memory contexts Ybgin
 */
IndexScanDesc
ybginbeginscan(Relation rel, int nkeys, int norderbys)
{
	IndexScanDesc scan;
	GinScanOpaque so;

	/* no order by operators allowed */
	Assert(norderbys == 0);

	scan = RelationGetIndexScan(rel, nkeys, norderbys);

	/* allocate private workspace */
	so = (GinScanOpaque) palloc(sizeof(YbginScanOpaqueData));
	so->keys = NULL;
	so->nkeys = 0;
	so->tempCtx = AllocSetContextCreate(GetCurrentMemoryContext(),
										"Ybgin scan temporary context",
										ALLOCSET_DEFAULT_SIZES);
	so->keyCtx = AllocSetContextCreate(GetCurrentMemoryContext(),
									   "Ybgin scan key context",
									   ALLOCSET_DEFAULT_SIZES);
	initGinState(&so->ginstate, scan->indexRelation);

	scan->opaque = so;

	return scan;
}

/*
 * Given pg_class oid, is relation colocated?
 */
static bool
isColocated(Oid relid)
{
	if (MyDatabaseColocated)
	{
		bool		colocated = false;
		bool		notfound;

		HandleYBStatusIgnoreNotFound(YBCPgIsTableColocated(MyDatabaseId,
														   relid,
														   &colocated),
									 &notfound);
		return colocated;
	}
	else
	{
		Oid			tablegroupId = InvalidOid;

		if (YbTablegroupCatalogExists)
			tablegroupId = get_tablegroup_oid_by_table_oid(relid);
		return tablegroupId != InvalidOid;
	}
}

void
ybginrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
			ScanKey orderbys, int norderbys)
{
	YbginScanOpaque ybso = (YbginScanOpaque) scan->opaque;

	/* Initialize non-yb gin scan opaque fields. */
	ginrescan(scan, scankey, nscankeys, orderbys, norderbys);

	/* Initialize ybgin scan opaque handle. */
	YBCPgPrepareParameters prepare_params = {
		.index_oid = RelationGetRelid(scan->indexRelation),
		.index_only_scan = scan->xs_want_itup,
		.use_secondary_index = true,	/* can't have ybgin primary index */
		.querying_colocated_table = isColocated(RelationGetRelid(scan->heapRelation)),
	};
	HandleYBStatus(YBCPgNewSelect(YBCGetDatabaseOid(scan->heapRelation),
								  YbGetStorageRelid(scan->heapRelation),
								  &prepare_params,
								  &ybso->handle));

	/* Initialize ybgin scan opaque is_exec_done. */
	ybso->is_exec_done = false;
}

void
ybginendscan(IndexScanDesc scan)
{
	/*
	 * This frees the scan opaque as if it were a GinScanOpaque rather than a
	 * YbginScanOpaque, but that's fine since the extra field HANDLE doesn't
	 * need special handling.
	 */
	ginendscan(scan);
}
