/*
 * ts_partition: a SQLite virtual table that transparently fans queries out
 *               across date-partitioned sibling .sqlite files in a directory.
 *
 * This file is the skeleton commit: all xMethods are stubs. The module
 * registers and CREATE VIRTUAL TABLE succeeds with a trivial schema, so
 * the .so loads cleanly in the sqlite3 CLI. Real behaviour lands in
 * subsequent commits.
 */
#include "sqlite3ext.h"
SQLITE_EXTENSION_INIT1

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if __STDC_VERSION__ < 202311L
#  include <stdbool.h>
#  ifndef nullptr
#    define nullptr NULL
#  endif
#endif

typedef struct Vtab {
    sqlite3_vtab base;
} Vtab;

typedef struct Cursor {
    sqlite3_vtab_cursor base;
    bool eof;
} Cursor;

static int tsConnect(sqlite3 *db, [[maybe_unused]] void *aux,
                     [[maybe_unused]] int argc,
                     [[maybe_unused]] const char *const *argv,
                     sqlite3_vtab **ppVtab,
                     [[maybe_unused]] char **pzErr) {
    int rc = sqlite3_declare_vtab(db, "CREATE TABLE x(stub TEXT)");
    if (rc != SQLITE_OK) return rc;
    Vtab *v = sqlite3_malloc(sizeof *v);
    if (!v) return SQLITE_NOMEM;
    memset(v, 0, sizeof *v);
    *ppVtab = &v->base;
    return SQLITE_OK;
}

static int tsDisconnect(sqlite3_vtab *pVtab) {
    sqlite3_free(pVtab);
    return SQLITE_OK;
}

static int tsBestIndex([[maybe_unused]] sqlite3_vtab *pVtab,
                       sqlite3_index_info *info) {
    info->estimatedCost = 1e9;
    info->estimatedRows = 0;
    return SQLITE_OK;
}

static int tsOpen([[maybe_unused]] sqlite3_vtab *pVtab,
                  sqlite3_vtab_cursor **ppCur) {
    Cursor *cur = sqlite3_malloc(sizeof *cur);
    if (!cur) return SQLITE_NOMEM;
    memset(cur, 0, sizeof *cur);
    cur->eof = true;
    *ppCur = &cur->base;
    return SQLITE_OK;
}

static int tsClose(sqlite3_vtab_cursor *pCur) {
    sqlite3_free(pCur);
    return SQLITE_OK;
}

static int tsFilter(sqlite3_vtab_cursor *pCur,
                    [[maybe_unused]] int idxNum,
                    [[maybe_unused]] const char *idxStr,
                    [[maybe_unused]] int argc,
                    [[maybe_unused]] sqlite3_value **argv) {
    ((Cursor*)pCur)->eof = true;
    return SQLITE_OK;
}

static int tsNext(sqlite3_vtab_cursor *pCur) {
    ((Cursor*)pCur)->eof = true;
    return SQLITE_OK;
}

static int tsEof(sqlite3_vtab_cursor *pCur) {
    return ((Cursor*)pCur)->eof ? 1 : 0;
}

static int tsColumn([[maybe_unused]] sqlite3_vtab_cursor *pCur,
                    sqlite3_context *ctx,
                    [[maybe_unused]] int col) {
    sqlite3_result_null(ctx);
    return SQLITE_OK;
}

static int tsRowid([[maybe_unused]] sqlite3_vtab_cursor *pCur,
                   sqlite3_int64 *pRowid) {
    *pRowid = 0;
    return SQLITE_OK;
}

static sqlite3_module ts_module = {
    .iVersion    = 3,
    .xCreate     = tsConnect,
    .xConnect    = tsConnect,
    .xBestIndex  = tsBestIndex,
    .xDisconnect = tsDisconnect,
    .xDestroy    = tsDisconnect,
    .xOpen       = tsOpen,
    .xClose      = tsClose,
    .xFilter     = tsFilter,
    .xNext       = tsNext,
    .xEof        = tsEof,
    .xColumn     = tsColumn,
    .xRowid      = tsRowid,
};

#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
int sqlite3_tspartition_init(sqlite3 *db,
                             [[maybe_unused]] char **pzErrMsg,
                             const sqlite3_api_routines *pApi) {
    SQLITE_EXTENSION_INIT2(pApi);
    return sqlite3_create_module(db, "ts_partition", &ts_module, nullptr);
}
