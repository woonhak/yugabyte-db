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

// C wrappers around some YB utilities. Suitable for inclusion into C codebases such as our modified
// version of PostgreSQL.

#ifndef YB_COMMON_YBC_UTIL_H
#define YB_COMMON_YBC_UTIL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {

struct varlena;

#endif

/*
 * Guc variable to log the protobuf string for every outgoing (DocDB) read/write request.
 * See the "YB Debug utils" section in pg_yb_utils.h (as well as guc.c) for more information.
 */
extern bool yb_debug_log_docdb_requests;

/*
 * Toggles whether formatting functions exporting system catalog information
 * include DocDB metadata (such as tablet split information).
 */
extern bool yb_format_funcs_include_yb_metadata;

/*
 * Guc variable to enable the use of regular transactions for operating on system catalog tables
 * in case a DDL transaction has not been started.
 */
extern bool yb_non_ddl_txn_for_sys_tables_allowed;

/*
 * Toggles whether to force use of global transaction status table.
 */
extern bool yb_force_global_transaction;

/*
 * Guc variable to suppress non-Postgres logs from appearing in Postgres log file.
 */
extern bool suppress_nonpg_logs;

/*
 * Guc variable to enable binary restore from a binary backup of YSQL tables. When doing binary
 * restore, we copy the docdb SST files of those tables from the source database and reuse them
 * for a newly created target database to restore those tables.
 */
extern bool yb_binary_restore;

typedef struct YBCStatusStruct* YBCStatus;

extern YBCStatus YBCStatusOK;
bool YBCStatusIsOK(YBCStatus s);
bool YBCStatusIsNotFound(YBCStatus s);
bool YBCStatusIsDuplicateKey(YBCStatus s);
uint32_t YBCStatusPgsqlError(YBCStatus s);
uint16_t YBCStatusTransactionError(YBCStatus s);
void YBCFreeStatus(YBCStatus s);

size_t YBCStatusMessageLen(YBCStatus s);
const char* YBCStatusMessageBegin(YBCStatus s);
const char* YBCStatusCodeAsCString(YBCStatus s);
char* DupYBStatusMessage(YBCStatus status, bool message_only);

bool YBCIsRestartReadError(uint16_t txn_errcode);

bool YBCIsTxnConflictError(uint16_t txn_errcode);
bool YBCIsTxnSkipLockingError(uint16_t txn_errcode);
uint16_t YBCGetTxnConflictErrorCode();

void YBCResolveHostname();

#define CHECKED_YBCSTATUS __attribute__ ((warn_unused_result)) YBCStatus

typedef void* (*YBCPAllocFn)(size_t size);

typedef struct varlena* (*YBCCStringToTextWithLenFn)(const char* c, int size);

// Global initialization of the YugaByte subsystem.
CHECKED_YBCSTATUS YBCInit(
    const char* argv0,
    YBCPAllocFn palloc_fn,
    YBCCStringToTextWithLenFn cstring_to_text_with_len_fn);

CHECKED_YBCSTATUS YBCInitGFlags(const char* argv0);

// From glog's log_severity.h:
// const int GLOG_INFO = 0, GLOG_WARNING = 1, GLOG_ERROR = 2, GLOG_FATAL = 3;

// Logging macros with printf-like formatting capabilities.
#define YBC_LOG_INFO(...) \
    YBCLogImpl(/* severity */ 0, __FILE__, __LINE__, /* stack_trace */ false, __VA_ARGS__)
#define YBC_LOG_WARNING(...) \
    YBCLogImpl(/* severity */ 1, __FILE__, __LINE__, /* stack_trace */ false, __VA_ARGS__)
#define YBC_LOG_ERROR(...) \
    YBCLogImpl(/* severity */ 2, __FILE__, __LINE__, /* stack_trace */ false, __VA_ARGS__)
#define YBC_LOG_FATAL(...) \
    YBCLogImpl(/* severity */ 3, __FILE__, __LINE__, /* stack_trace */ false, __VA_ARGS__)

// Versions of these warnings that do nothing in debug mode. The fatal version logs a warning
// in release mode but does not crash.
#ifndef NDEBUG
// Logging macros with printf-like formatting capabilities.
#define YBC_DEBUG_LOG_INFO(...) YBC_LOG_INFO(__VA_ARGS__)
#define YBC_DEBUG_LOG_WARNING(...) YBC_LOG_WARNING(__VA_ARGS__)
#define YBC_DEBUG_LOG_ERROR(...) YBC_LOG_ERROR(__VA_ARGS__)
#define YBC_DEBUG_LOG_FATAL(...) YBC_LOG_FATAL(__VA_ARGS__)
#else
#define YBC_DEBUG_LOG_INFO(...)
#define YBC_DEBUG_LOG_WARNING(...)
#define YBC_DEBUG_LOG_ERROR(...)
#define YBC_DEBUG_LOG_FATAL(...) YBC_LOG_ERROR(__VA_ARGS__)
#endif

// The following functions log the given message formatted similarly to printf followed by a stack
// trace.

#define YBC_LOG_INFO_STACK_TRACE(...) \
    YBCLogImpl(/* severity */ 0, __FILE__, __LINE__, /* stack_trace */ true, __VA_ARGS__)
#define YBC_LOG_WARNING_STACK_TRACE(...) \
    YBCLogImpl(/* severity */ 1, __FILE__, __LINE__, /* stack_trace */ true, __VA_ARGS__)
#define YBC_LOG_ERROR_STACK_TRACE(...) \
    YBCLogImpl(/* severity */ 2, __FILE__, __LINE__, /* stack_trace */ true, __VA_ARGS__)

// 5 is the index of the format string, 6 is the index of the first printf argument to check.
void YBCLogImpl(int severity,
                const char* file_name,
                int line_number,
                bool stack_trace,
                const char* format,
                ...) __attribute__((format(printf, 5, 6)));

// Returns a string representation of the given block of binary data. The memory for the resulting
// string is allocated using palloc.
const char* YBCFormatBytesAsStr(const char* data, size_t size);

const char* YBCGetStackTrace();

// Initializes global state needed for thread management, including CDS library initialization.
void YBCInitThreading();

double YBCEvalHashValueSelectivity(int32_t hash_low, int32_t hash_high);

#ifdef __cplusplus
} // extern "C"
#endif

#endif  // YB_COMMON_YBC_UTIL_H
