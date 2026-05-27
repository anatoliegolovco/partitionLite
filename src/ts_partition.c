/*
 * ts_partition: a SQLite virtual table that transparently fans queries out
 *               across date-partitioned sibling .sqlite files in a directory.
 *
 * Read-only. xFilter enumerates partition files in the active date range,
 * opens each READONLY, and iterates row-by-row across the list. Range
 * push-down on time_col lands in a later commit; for now every query
 * scans the last lookback_days. Missing partition files are silently
 * skipped.
 */
#include "sqlite3ext.h"
SQLITE_EXTENSION_INIT1

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#if __STDC_VERSION__ < 202311L
#  include <stdbool.h>
#  ifndef nullptr
#    define nullptr NULL
#  endif
#endif

#ifndef TS_DEBUG_ENV
#  define TS_DEBUG_ENV "TS_PARTITION_DEBUG"
#endif

constexpr int  MAX_PATH_LEN          = 4096;
constexpr int  MAX_COLS              = 256;
constexpr int  DEFAULT_LOOKBACK_DAYS = 365 * 5;
constexpr int  CHILD_BUSY_TIMEOUT_MS = 2'000;

static bool ts_debug_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv(TS_DEBUG_ENV);
        cached = (e && *e && *e != '0') ? 1 : 0;
    }
    return cached != 0;
}

#define DLOG(...) do { \
    if (ts_debug_enabled()) { \
        fprintf(stderr, "[ts_partition] " __VA_ARGS__); \
        fputc('\n', stderr); \
    } \
} while (0)

typedef struct ColInfo {
    char *name;
    char *type;
} ColInfo;

typedef struct Vtab {
    sqlite3_vtab base;
    char    *dir;
    char    *pattern;
    char    *child_table;
    char    *time_col;
    int      n_cols;
    int      time_col_idx;
    int      lookback_days;
    char    *select_prefix;     /* SELECT cols FROM "child_table" */
    ColInfo  cols[MAX_COLS];
} Vtab;

typedef struct Cursor {
    sqlite3_vtab_cursor base;
    Vtab           *vtab;
    char          **files;       /* malloc'd via sqlite3_mprintf */
    int             n_files;
    int             file_idx;
    sqlite3        *child_db;
    sqlite3_stmt   *child_stmt;
    sqlite3_int64   rowid;
    bool            eof;
} Cursor;

/* --- small string helpers --- */

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t'
                       || end[-1] == '\n' || end[-1] == '\r')) end--;
    *end = '\0';
    return s;
}

static char *unquote(char *s) {
    size_t n = strlen(s);
    if (n >= 2 && ((s[0] == '\'' && s[n-1] == '\'')
                   || (s[0] == '"' && s[n-1] == '"'))) {
        s[n-1] = '\0';
        return s + 1;
    }
    return s;
}

[[nodiscard]]
static int parse_kv(const char *arg, char **out_key, char **out_val) {
    const char *eq = strchr(arg, '=');
    if (!eq) return 0;
    size_t klen = (size_t)(eq - arg);
    char *k = sqlite3_malloc((int)(klen + 1));
    char *v = sqlite3_mprintf("%s", eq + 1);
    if (!k || !v) { sqlite3_free(k); sqlite3_free(v); return 0; }
    memcpy(k, arg, klen);
    k[klen] = '\0';
    char *kt = trim(k);
    char *vt = unquote(trim(v));
    if (kt != k) memmove(k, kt, strlen(kt) + 1);
    if (vt != v) memmove(v, vt, strlen(vt) + 1);
    *out_key = k;
    *out_val = v;
    return 1;
}

/* --- date / path helpers --- */

static bool file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static char *format_partition_path(const char *dir, const char *pattern,
                                   const struct tm *t) {
    char name[MAX_PATH_LEN];
    if (strftime(name, sizeof name, pattern, t) == 0) return nullptr;
    return sqlite3_mprintf("%s/%s", dir, name);
}

static void today_utc_midnight(struct tm *out) {
    /* Use local calendar's "today" as the day boundary, but render in
     * struct tm fields directly. The strftime pattern reads Y/M/D fields
     * regardless of tz interpretation. */
    time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    memset(out, 0, sizeof *out);
    out->tm_year = lt.tm_year;
    out->tm_mon  = lt.tm_mon;
    out->tm_mday = lt.tm_mday;
}

/* Advance a date by ndays (negative for backward) using time_t arithmetic
 * against timegm. */
static void shift_days(struct tm *t, int ndays) {
    time_t base = timegm(t);
    base += (time_t)ndays * 86'400;
    struct tm gt;
    gmtime_r(&base, &gt);
    *t = gt;
}

/* --- schema sniffing --- */

static char *find_first_existing_partition(const Vtab *v) {
    struct tm cursor;
    today_utc_midnight(&cursor);
    for (int i = 0; i < v->lookback_days; ++i) {
        struct tm probe = cursor;
        shift_days(&probe, -i);
        char *path = format_partition_path(v->dir, v->pattern, &probe);
        if (!path) continue;
        if (file_exists(path)) return path;
        sqlite3_free(path);
    }
    return nullptr;
}

static int sniff_schema(Vtab *v, const char *path, char **pzErr) {
    sqlite3 *db = nullptr;
    int rc = sqlite3_open_v2(path, &db,
                             SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX,
                             nullptr);
    if (rc != SQLITE_OK) {
        *pzErr = sqlite3_mprintf("cannot open partition '%s': %s",
                                 path, sqlite3_errmsg(db));
        sqlite3_close(db);
        return rc;
    }
    sqlite3_busy_timeout(db, CHILD_BUSY_TIMEOUT_MS);

    char *sql = sqlite3_mprintf("PRAGMA table_info(\"%w\")", v->child_table);
    sqlite3_stmt *st = nullptr;
    rc = sqlite3_prepare_v2(db, sql, -1, &st, nullptr);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) {
        *pzErr = sqlite3_mprintf("PRAGMA table_info failed in '%s': %s",
                                 path, sqlite3_errmsg(db));
        sqlite3_close(db);
        return rc;
    }
    v->n_cols = 0;
    v->time_col_idx = -1;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (v->n_cols >= MAX_COLS) {
            *pzErr = sqlite3_mprintf("too many columns (>%d) in '%s'",
                                     MAX_COLS, v->child_table);
            sqlite3_finalize(st);
            sqlite3_close(db);
            return SQLITE_ERROR;
        }
        const unsigned char *cname = sqlite3_column_text(st, 1);
        const unsigned char *ctype = sqlite3_column_text(st, 2);
        v->cols[v->n_cols].name = sqlite3_mprintf("%s",
            cname ? (const char*)cname : "");
        v->cols[v->n_cols].type = sqlite3_mprintf("%s",
            ctype ? (const char*)ctype : "");
        if (strcmp(v->cols[v->n_cols].name, v->time_col) == 0) {
            v->time_col_idx = v->n_cols;
        }
        v->n_cols++;
    }
    sqlite3_finalize(st);
    sqlite3_close(db);

    if (v->n_cols == 0) {
        *pzErr = sqlite3_mprintf(
            "child table '%s' has no columns (does it exist in '%s'?)",
            v->child_table, path);
        return SQLITE_ERROR;
    }
    if (v->time_col_idx < 0) {
        *pzErr = sqlite3_mprintf(
            "time_col '%s' not found in child table '%s'",
            v->time_col, v->child_table);
        return SQLITE_ERROR;
    }
    return SQLITE_OK;
}

/* SELECT col1, col2, ... FROM "child_table" — bounds are appended at xFilter
 * time so we can adapt to the constraints we received. */
static char *build_select_prefix(const Vtab *v) {
    char *sql = sqlite3_mprintf("SELECT ");
    if (!sql) return nullptr;
    for (int i = 0; i < v->n_cols; ++i) {
        const char *sep = (i + 1 < v->n_cols) ? ", " : "";
        char *next = sqlite3_mprintf("%z\"%w\"%s", sql, v->cols[i].name, sep);
        if (!next) return nullptr;
        sql = next;
    }
    return sqlite3_mprintf("%z FROM \"%w\"", sql, v->child_table);
}

static char *build_create_table_sql(const Vtab *v) {
    char *sql = sqlite3_mprintf("CREATE TABLE x(");
    if (!sql) return nullptr;
    for (int i = 0; i < v->n_cols; ++i) {
        const char *sep = (i + 1 < v->n_cols) ? ", " : "";
        char *next;
        if (v->cols[i].type && *v->cols[i].type) {
            next = sqlite3_mprintf("%z\"%w\" %s%s",
                                   sql, v->cols[i].name,
                                   v->cols[i].type, sep);
        } else {
            next = sqlite3_mprintf("%z\"%w\"%s",
                                   sql, v->cols[i].name, sep);
        }
        if (!next) return nullptr;
        sql = next;
    }
    return sqlite3_mprintf("%z)", sql);
}

/* --- module callbacks --- */

static int tsDisconnect(sqlite3_vtab *pVtab) {
    Vtab *v = (Vtab*)pVtab;
    if (!v) return SQLITE_OK;
    sqlite3_free(v->dir);
    sqlite3_free(v->pattern);
    sqlite3_free(v->child_table);
    sqlite3_free(v->time_col);
    sqlite3_free(v->select_prefix);
    for (int i = 0; i < v->n_cols; ++i) {
        sqlite3_free(v->cols[i].name);
        sqlite3_free(v->cols[i].type);
    }
    sqlite3_free(v);
    return SQLITE_OK;
}

static int tsConnect(sqlite3 *db, [[maybe_unused]] void *aux,
                     int argc, const char *const *argv,
                     sqlite3_vtab **ppVtab, char **pzErr) {
    Vtab *v = sqlite3_malloc(sizeof *v);
    if (!v) return SQLITE_NOMEM;
    memset(v, 0, sizeof *v);
    v->lookback_days = DEFAULT_LOOKBACK_DAYS;
    v->time_col_idx = -1;

    /* argv[0]=module, [1]=db schema name, [2]=table name, [3..]=user args. */
    for (int i = 3; i < argc; ++i) {
        char *k = nullptr;
        char *val = nullptr;
        if (!parse_kv(argv[i], &k, &val)) {
            *pzErr = sqlite3_mprintf("malformed argument: %s", argv[i]);
            sqlite3_free(k); sqlite3_free(val);
            tsDisconnect(&v->base);
            return SQLITE_ERROR;
        }
        if      (strcmp(k, "dir")       == 0) { sqlite3_free(v->dir);         v->dir = val;         val = nullptr; }
        else if (strcmp(k, "pattern")   == 0) { sqlite3_free(v->pattern);     v->pattern = val;     val = nullptr; }
        else if (strcmp(k, "table")     == 0) { sqlite3_free(v->child_table); v->child_table = val; val = nullptr; }
        else if (strcmp(k, "time_col")  == 0) { sqlite3_free(v->time_col);    v->time_col = val;    val = nullptr; }
        else if (strcmp(k, "lookback_days") == 0) {
            int n = atoi(val);
            if (n > 0) v->lookback_days = n;
            sqlite3_free(val); val = nullptr;
        } else {
            *pzErr = sqlite3_mprintf("unknown argument: %s", k);
            sqlite3_free(k); sqlite3_free(val);
            tsDisconnect(&v->base);
            return SQLITE_ERROR;
        }
        sqlite3_free(k);
        sqlite3_free(val);
    }

    if (!v->dir || !v->pattern || !v->child_table || !v->time_col) {
        *pzErr = sqlite3_mprintf(
            "ts_partition requires: dir=..., pattern=..., table=..., time_col=...");
        tsDisconnect(&v->base);
        return SQLITE_ERROR;
    }

    char *first = find_first_existing_partition(v);
    if (!first) {
        *pzErr = sqlite3_mprintf(
            "no partition files found in '%s' matching '%s' (looked back %d days)",
            v->dir, v->pattern, v->lookback_days);
        tsDisconnect(&v->base);
        return SQLITE_ERROR;
    }
    int rc = sniff_schema(v, first, pzErr);
    DLOG("sniffed schema from %s: n_cols=%d time_col_idx=%d",
         first, v->n_cols, v->time_col_idx);
    sqlite3_free(first);
    if (rc != SQLITE_OK) {
        tsDisconnect(&v->base);
        return rc;
    }

    char *ct = build_create_table_sql(v);
    if (!ct) {
        *pzErr = sqlite3_mprintf("OOM building schema");
        tsDisconnect(&v->base);
        return SQLITE_NOMEM;
    }
    DLOG("declare_vtab: %s", ct);
    rc = sqlite3_declare_vtab(db, ct);
    sqlite3_free(ct);
    if (rc != SQLITE_OK) {
        *pzErr = sqlite3_mprintf("declare_vtab failed: %s", sqlite3_errmsg(db));
        tsDisconnect(&v->base);
        return rc;
    }

    v->select_prefix = build_select_prefix(v);
    if (!v->select_prefix) {
        *pzErr = sqlite3_mprintf("OOM building select");
        tsDisconnect(&v->base);
        return SQLITE_NOMEM;
    }

    *ppVtab = &v->base;
    return SQLITE_OK;
}

static int tsBestIndex([[maybe_unused]] sqlite3_vtab *pVtab,
                       sqlite3_index_info *info) {
    info->estimatedCost = 1e9;
    info->estimatedRows = 0;
    return SQLITE_OK;
}

static int tsOpen(sqlite3_vtab *pVtab, sqlite3_vtab_cursor **ppCur) {
    Cursor *cur = sqlite3_malloc(sizeof *cur);
    if (!cur) return SQLITE_NOMEM;
    memset(cur, 0, sizeof *cur);
    cur->vtab = (Vtab*)pVtab;
    cur->eof = true;
    *ppCur = &cur->base;
    return SQLITE_OK;
}

static void cursor_close_current(Cursor *cur) {
    if (cur->child_stmt) {
        sqlite3_finalize(cur->child_stmt);
        cur->child_stmt = nullptr;
    }
    if (cur->child_db) {
        sqlite3_close(cur->child_db);
        cur->child_db = nullptr;
    }
}

static void cursor_release_files(Cursor *cur) {
    if (cur->files) {
        for (int i = 0; i < cur->n_files; ++i) sqlite3_free(cur->files[i]);
        sqlite3_free(cur->files);
        cur->files = nullptr;
    }
    cur->n_files = 0;
    cur->file_idx = 0;
}

static int tsClose(sqlite3_vtab_cursor *pCur) {
    Cursor *cur = (Cursor*)pCur;
    cursor_close_current(cur);
    cursor_release_files(cur);
    sqlite3_free(cur);
    return SQLITE_OK;
}

/* Open the file at file_idx and prepare its child statement. Returns true
 * on success (statement ready to step); false on any failure (caller
 * should silently advance to the next file). */
static bool cursor_open_current(Cursor *cur) {
    Vtab *v = cur->vtab;
    const char *path = cur->files[cur->file_idx];
    DLOG("opening partition %s", path);

    sqlite3 *db = nullptr;
    int rc = sqlite3_open_v2(path, &db,
                             SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX,
                             nullptr);
    if (rc != SQLITE_OK) {
        DLOG("sqlite3_open_v2(%s) failed: %s",
             path, db ? sqlite3_errmsg(db) : "(no handle)");
        if (db) sqlite3_close(db);
        return false;
    }
    sqlite3_busy_timeout(db, CHILD_BUSY_TIMEOUT_MS);

    sqlite3_stmt *st = nullptr;
    rc = sqlite3_prepare_v2(db, v->select_prefix, -1, &st, nullptr);
    if (rc != SQLITE_OK) {
        DLOG("prepare(%s) failed: %s", path, sqlite3_errmsg(db));
        if (st) sqlite3_finalize(st);
        sqlite3_close(db);
        return false;
    }
    cur->child_db = db;
    cur->child_stmt = st;
    return true;
}

/* Step forward in the file list and the active statement until we land on
 * a row or exhaust all files. */
static int cursor_advance(Cursor *cur) {
    while (true) {
        if (cur->child_stmt) {
            int rc = sqlite3_step(cur->child_stmt);
            if (rc == SQLITE_ROW) {
                cur->rowid++;
                cur->eof = false;
                return SQLITE_OK;
            }
            /* SQLITE_DONE or error -> close and move on. Errors are
             * deliberately swallowed: partition reads must never propagate. */
            cursor_close_current(cur);
            cur->file_idx++;
        }
        if (cur->file_idx >= cur->n_files) {
            cur->eof = true;
            return SQLITE_OK;
        }
        if (!cursor_open_current(cur)) {
            cur->file_idx++;
            continue;
        }
    }
}

/* Enumerate files for the last lookback_days, in chronological order. */
static int enumerate_recent_files(Cursor *cur) {
    Vtab *v = cur->vtab;
    int cap = v->lookback_days;
    char **list = sqlite3_malloc((int)(sizeof(char*) * cap));
    if (!list) return SQLITE_NOMEM;
    int n = 0;

    struct tm today;
    today_utc_midnight(&today);
    for (int i = cap - 1; i >= 0; --i) {
        struct tm probe = today;
        shift_days(&probe, -i);
        char *path = format_partition_path(v->dir, v->pattern, &probe);
        if (!path) continue;
        if (file_exists(path)) {
            list[n++] = path;
        } else {
            sqlite3_free(path);
        }
    }
    cur->files = list;
    cur->n_files = n;
    cur->file_idx = 0;
    DLOG("xFilter: discovered %d files (no bounds, lookback=%d)",
         n, cap);
    return SQLITE_OK;
}

static int tsFilter(sqlite3_vtab_cursor *pCur,
                    [[maybe_unused]] int idxNum,
                    [[maybe_unused]] const char *idxStr,
                    [[maybe_unused]] int argc,
                    [[maybe_unused]] sqlite3_value **argv) {
    Cursor *cur = (Cursor*)pCur;
    cursor_close_current(cur);
    cursor_release_files(cur);
    cur->rowid = 0;
    cur->eof = false;
    int rc = enumerate_recent_files(cur);
    if (rc != SQLITE_OK) return rc;
    return cursor_advance(cur);
}

static int tsNext(sqlite3_vtab_cursor *pCur) {
    return cursor_advance((Cursor*)pCur);
}

static int tsEof(sqlite3_vtab_cursor *pCur) {
    return ((Cursor*)pCur)->eof ? 1 : 0;
}

static int tsColumn(sqlite3_vtab_cursor *pCur, sqlite3_context *ctx, int col) {
    Cursor *cur = (Cursor*)pCur;
    if (!cur->child_stmt || col < 0 || col >= cur->vtab->n_cols) {
        sqlite3_result_null(ctx);
        return SQLITE_OK;
    }
    sqlite3_result_value(ctx, sqlite3_column_value(cur->child_stmt, col));
    return SQLITE_OK;
}

static int tsRowid(sqlite3_vtab_cursor *pCur, sqlite3_int64 *pRowid) {
    *pRowid = ((Cursor*)pCur)->rowid;
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
