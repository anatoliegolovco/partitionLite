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

/* Compile-time constants. Plain `constexpr int` would be the C23 idiom
 * but Clang 18 didn't land it; enums are the portable shape. */
enum : int {
    MAX_PATH_LEN          = 4096,
    MAX_COLS              = 256,
    DEFAULT_LOOKBACK_DAYS = 365 * 5,
    CHILD_BUSY_TIMEOUT_MS = 2'000,
    LRU_CAPACITY          = 16,
};

enum : int {
    HAS_LO   = 1 << 0,
    HAS_HI   = 1 << 1,
    LO_IS_GT = 1 << 2,
    HI_IS_LT = 1 << 3,
};

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

typedef struct CacheEntry {
    char    *path;       /* nullptr == empty slot */
    sqlite3 *db;
    int64_t  last_used;
} CacheEntry;

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
    /* LRU of historical-file handles. Today's file is never cached: each
     * query reopens it fresh so Grafana refreshes pick up new WAL pages. */
    CacheEntry cache[LRU_CAPACITY];
    int64_t    lru_clock;
} Vtab;

typedef struct Cursor {
    sqlite3_vtab_cursor base;
    Vtab           *vtab;
    char          **files;       /* malloc'd via sqlite3_mprintf */
    int             n_files;
    int             file_idx;
    sqlite3        *child_db;
    sqlite3_stmt   *child_stmt;
    bool            child_is_today;   /* if true: close on advance, don't cache */
    sqlite3_int64   rowid;
    char           *lo_text;
    char           *hi_text;
    bool            lo_is_gt;
    bool            hi_is_lt;
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

/* Parse "YYYY-MM-DD" or "YYYY-MM-DDTHH:MM:SS[Z]" — the first three ints are
 * enough since we only care about the calendar day. Returns 1 on success. */
static int parse_date(const char *s, struct tm *out) {
    if (!s) return 0;
    int y, m, d;
    if (sscanf(s, "%d-%d-%d", &y, &m, &d) != 3) return 0;
    memset(out, 0, sizeof *out);
    out->tm_year = y - 1900;
    out->tm_mon  = m - 1;
    out->tm_mday = d;
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

/* The path that today's partition would have, in v->dir. */
static char *today_partition_path(const Vtab *v) {
    struct tm today;
    today_utc_midnight(&today);
    return format_partition_path(v->dir, v->pattern, &today);
}

/* --- LRU cache for historical-file handles --- */

/* Take an entry out of the cache if present, transferring ownership of
 * the db handle to the caller. Returns nullptr on miss. */
static sqlite3 *cache_take(Vtab *v, const char *path) {
    for (int i = 0; i < LRU_CAPACITY; ++i) {
        if (v->cache[i].path && strcmp(v->cache[i].path, path) == 0) {
            sqlite3 *db = v->cache[i].db;
            sqlite3_free(v->cache[i].path);
            v->cache[i].path = nullptr;
            v->cache[i].db = nullptr;
            DLOG("cache hit: %s", path);
            return db;
        }
    }
    return nullptr;
}

/* Insert (path, db) into the cache, evicting the least-recently-used
 * entry if all slots are full. Ownership of db transfers to the cache. */
static void cache_put(Vtab *v, const char *path, sqlite3 *db) {
    int target = -1;
    int64_t oldest = INT64_MAX;
    for (int i = 0; i < LRU_CAPACITY; ++i) {
        if (!v->cache[i].path) { target = i; break; }
        if (v->cache[i].last_used < oldest) {
            oldest = v->cache[i].last_used;
            target = i;
        }
    }
    if (target < 0) {
        sqlite3_close(db);
        return;
    }
    if (v->cache[target].path) {
        DLOG("cache evict: %s", v->cache[target].path);
        sqlite3_close(v->cache[target].db);
        sqlite3_free(v->cache[target].path);
    }
    v->cache[target].path = sqlite3_mprintf("%s", path);
    v->cache[target].db = db;
    v->cache[target].last_used = ++v->lru_clock;
    DLOG("cache put: %s (slot %d)", path, target);
}

static void cache_release_all(Vtab *v) {
    for (int i = 0; i < LRU_CAPACITY; ++i) {
        if (v->cache[i].db) sqlite3_close(v->cache[i].db);
        sqlite3_free(v->cache[i].path);
        v->cache[i].db = nullptr;
        v->cache[i].path = nullptr;
        v->cache[i].last_used = 0;
    }
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
    cache_release_all(v);
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

static int tsBestIndex(sqlite3_vtab *pVtab, sqlite3_index_info *info) {
    Vtab *v = (Vtab*)pVtab;
    int idxNum = 0;
    int lo_constraint = -1;
    int hi_constraint = -1;

    for (int i = 0; i < info->nConstraint; ++i) {
        const struct sqlite3_index_constraint *c = &info->aConstraint[i];
        if (!c->usable) continue;
        if (c->iColumn != v->time_col_idx) continue;
        switch (c->op) {
            case SQLITE_INDEX_CONSTRAINT_GT:
                if (!(idxNum & HAS_LO)) {
                    idxNum |= HAS_LO | LO_IS_GT;
                    lo_constraint = i;
                }
                break;
            case SQLITE_INDEX_CONSTRAINT_GE:
                if (!(idxNum & HAS_LO)) {
                    idxNum |= HAS_LO;
                    lo_constraint = i;
                }
                break;
            case SQLITE_INDEX_CONSTRAINT_LT:
                if (!(idxNum & HAS_HI)) {
                    idxNum |= HAS_HI | HI_IS_LT;
                    hi_constraint = i;
                }
                break;
            case SQLITE_INDEX_CONSTRAINT_LE:
                if (!(idxNum & HAS_HI)) {
                    idxNum |= HAS_HI;
                    hi_constraint = i;
                }
                break;
            default: break;
        }
    }

    /* argv ordering: lo first, then hi. Leave omit=0 so SQLite re-checks
     * rows: a single day's file spans the whole day and the caller's
     * bound may be intra-day. */
    int argv_n = 1;
    if (lo_constraint >= 0) info->aConstraintUsage[lo_constraint].argvIndex = argv_n++;
    if (hi_constraint >= 0) info->aConstraintUsage[hi_constraint].argvIndex = argv_n++;

    info->idxNum = idxNum;
    info->estimatedCost = (idxNum & (HAS_LO | HAS_HI)) ? 1'000.0 : 1e9;
    info->estimatedRows = (idxNum & (HAS_LO | HAS_HI)) ? 1'000 : 1'000'000;
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
        if (cur->child_is_today) {
            sqlite3_close(cur->child_db);
        } else {
            /* Hand the historical handle back to the vtab cache. */
            cache_put(cur->vtab, cur->files[cur->file_idx], cur->child_db);
        }
        cur->child_db = nullptr;
    }
    cur->child_is_today = false;
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
    sqlite3_free(cur->lo_text);
    sqlite3_free(cur->hi_text);
    sqlite3_free(cur);
    return SQLITE_OK;
}

/* Open the file at file_idx and prepare its child statement. Returns true
 * on success (statement ready to step); false on any failure (caller
 * should silently advance to the next file). */
static bool cursor_open_current(Cursor *cur) {
    Vtab *v = cur->vtab;
    const char *path = cur->files[cur->file_idx];

    /* Classify today vs historical by comparing against today's formatted
     * path. today_path is cached on the cursor for the duration of the
     * query so we don't keep rebuilding it. */
    char *today_path = today_partition_path(v);
    bool is_today = (today_path && strcmp(path, today_path) == 0);
    sqlite3_free(today_path);

    sqlite3 *db = is_today ? nullptr : cache_take(v, path);
    if (!db) {
        DLOG("opening partition %s%s", path, is_today ? " (today)" : "");
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
    }
    cur->child_is_today = is_today;

    /* Append WHERE on time_col when bounds are present so the child's
     * index on time_col can help (and we read fewer rows per file). */
    char *sql = sqlite3_mprintf("%s", v->select_prefix);
    if (cur->lo_text && cur->hi_text) {
        sql = sqlite3_mprintf("%z WHERE \"%w\" %s ? AND \"%w\" %s ?",
                              sql,
                              v->time_col, cur->lo_is_gt ? ">" : ">=",
                              v->time_col, cur->hi_is_lt ? "<" : "<=");
    } else if (cur->lo_text) {
        sql = sqlite3_mprintf("%z WHERE \"%w\" %s ?",
                              sql, v->time_col,
                              cur->lo_is_gt ? ">" : ">=");
    } else if (cur->hi_text) {
        sql = sqlite3_mprintf("%z WHERE \"%w\" %s ?",
                              sql, v->time_col,
                              cur->hi_is_lt ? "<" : "<=");
    }
    if (!sql) { sqlite3_close(db); return false; }

    sqlite3_stmt *st = nullptr;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, nullptr);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) {
        DLOG("prepare(%s) failed: %s", path, sqlite3_errmsg(db));
        if (st) sqlite3_finalize(st);
        sqlite3_close(db);
        return false;
    }
    int bind_i = 1;
    if (cur->lo_text) sqlite3_bind_text(st, bind_i++, cur->lo_text, -1, SQLITE_TRANSIENT);
    if (cur->hi_text) sqlite3_bind_text(st, bind_i++, cur->hi_text, -1, SQLITE_TRANSIENT);
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

/* Enumerate files between lo and hi (inclusive), in chronological order.
 * The span is capped at lookback_days; if the caller's range is wider we
 * walk back from hi rather than open thousands of files. */
static int enumerate_range(Cursor *cur, struct tm lo, struct tm hi) {
    Vtab *v = cur->vtab;
    time_t t_lo = timegm(&lo);
    time_t t_hi = timegm(&hi);
    if (t_lo > t_hi) {
        cur->files = nullptr;
        cur->n_files = 0;
        cur->file_idx = 0;
        DLOG("xFilter: lo>hi, empty file list");
        return SQLITE_OK;
    }
    int span = (int)((t_hi - t_lo) / 86'400) + 1;
    if (span > v->lookback_days) {
        t_lo = t_hi - (time_t)(v->lookback_days - 1) * 86'400;
        span = v->lookback_days;
    }
    char **list = sqlite3_malloc((int)(sizeof(char*) * span));
    if (!list) return SQLITE_NOMEM;
    int n = 0;
    for (int i = 0; i < span; ++i) {
        time_t t = t_lo + (time_t)i * 86'400;
        struct tm probe;
        gmtime_r(&t, &probe);
        char *path = format_partition_path(v->dir, v->pattern, &probe);
        if (!path) continue;
        if (file_exists(path)) {
            DLOG("xFilter: +file %s", path);
            list[n++] = path;
        } else {
            DLOG("xFilter: -missing %s", path);
            sqlite3_free(path);
        }
    }
    cur->files = list;
    cur->n_files = n;
    cur->file_idx = 0;
    DLOG("xFilter: %d files of %d candidate day(s)", n, span);
    return SQLITE_OK;
}

static int tsFilter(sqlite3_vtab_cursor *pCur, int idxNum,
                    [[maybe_unused]] const char *idxStr,
                    [[maybe_unused]] int argc, sqlite3_value **argv) {
    Cursor *cur = (Cursor*)pCur;
    Vtab *v = cur->vtab;

    cursor_close_current(cur);
    cursor_release_files(cur);
    sqlite3_free(cur->lo_text); cur->lo_text = nullptr;
    sqlite3_free(cur->hi_text); cur->hi_text = nullptr;
    cur->lo_is_gt = false;
    cur->hi_is_lt = false;
    cur->rowid = 0;
    cur->eof = false;

    int ai = 0;
    if (idxNum & HAS_LO) {
        const unsigned char *t = sqlite3_value_text(argv[ai++]);
        if (t) cur->lo_text = sqlite3_mprintf("%s", (const char*)t);
        cur->lo_is_gt = (idxNum & LO_IS_GT) != 0;
    }
    if (idxNum & HAS_HI) {
        const unsigned char *t = sqlite3_value_text(argv[ai++]);
        if (t) cur->hi_text = sqlite3_mprintf("%s", (const char*)t);
        cur->hi_is_lt = (idxNum & HI_IS_LT) != 0;
    }

    /* Decide the day range to enumerate. If a bound is missing or
     * unparseable, the open side falls back to today / lookback_days. */
    struct tm lo_tm, hi_tm;
    bool have_lo = cur->lo_text && parse_date(cur->lo_text, &lo_tm);
    bool have_hi = cur->hi_text && parse_date(cur->hi_text, &hi_tm);
    if (!have_hi) {
        today_utc_midnight(&hi_tm);
    }
    if (!have_lo) {
        lo_tm = hi_tm;
        shift_days(&lo_tm, -(v->lookback_days - 1));
    }

    DLOG("xFilter: idxNum=0x%x lo=%s hi=%s",
         idxNum,
         cur->lo_text ? cur->lo_text : "<unbound>",
         cur->hi_text ? cur->hi_text : "<unbound>");

    int rc = enumerate_range(cur, lo_tm, hi_tm);
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
