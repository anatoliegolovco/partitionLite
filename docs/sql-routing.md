# SQL routing

This document walks through how a single `SELECT` against the
virtual table becomes N `SELECT`s against the underlying partition
files. There are three pieces:

1. **xBestIndex** — the planner asks "which constraints can you
   use?". We answer with a bitmask describing what bounds we
   captured, and assign argv positions so xFilter gets them.
2. **xFilter** — we receive the bound values, parse them as dates,
   enumerate files in the matching directory window, and start the
   first child statement.
3. **xNext** — we step the active child statement, and on `DONE` we
   close it and open the next file in the list.

The end-to-end flow:

```mermaid
flowchart TB
    Q["parent SELECT:<br/>SELECT ts, host, value<br/>FROM metrics<br/>WHERE ts &gt;= 'X' AND ts &lt; 'Y'<br/>AND host = 'web-1'"]

    Q --> Plan["xBestIndex<br/>scan constraints"]
    Plan -->|"idxNum = HAS_LO | HAS_HI | HI_IS_LT<br/>argv[1]=X argv[2]=Y<br/>cost=1000"| Filter["xFilter(idxNum, [X,Y])"]

    Filter --> Parse["parse_date(X) -&gt; lo_tm<br/>parse_date(Y) -&gt; hi_tm"]
    Parse --> Enum["enumerate_range(lo_tm, hi_tm):<br/>for each day: stat(dir/strftime(pattern, day))"]
    Enum --> Files["files[] in chronological order"]

    Files --> Adv["cursor_advance"]
    Adv --> Open["open files[file_idx]<br/>(cache or fresh)"]
    Open --> Prep["prepare:<br/>SELECT ts,host,value FROM metrics<br/>WHERE ts &gt;= ? AND ts &lt; ?"]
    Prep --> Bind["bind X, Y"]
    Bind --> Step["step"]

    Step -->|"ROW"| Yield["xColumn passes through<br/>host re-checks 'host = web-1'"]
    Yield -->|"xNext"| Step
    Step -->|"DONE"| Close["finalize stmt,<br/>return db to cache<br/>(if historical)"]
    Close --> NextFile["file_idx++"]
    NextFile -->|"more files"| Open
    NextFile -->|"no files left"| EOF["xEof = 1"]
```

The host evaluates non-time predicates (`host = 'web-1'`) as a normal
post-filter on the rows we hand back. We don't push down arbitrary
expressions because the parent-side index machinery already does that
work; only `time_col` gets special treatment because it's the column
that drives **file selection**, not just row selection.

## xBestIndex: constraint scan and bitmask encoding

```mermaid
flowchart TB
    Start([xBestIndex called])
    Start --> Loop{for each<br/>constraint i}
    Loop -->|"usable AND<br/>iColumn == time_col_idx"| Op{op?}
    Loop -->|"otherwise"| Next1[next i]

    Op -->|"GT"| GT["if !HAS_LO:<br/>HAS_LO | LO_IS_GT<br/>lo_constraint = i"]
    Op -->|"GE"| GE["if !HAS_LO:<br/>HAS_LO<br/>lo_constraint = i"]
    Op -->|"LT"| LT["if !HAS_HI:<br/>HAS_HI | HI_IS_LT<br/>hi_constraint = i"]
    Op -->|"LE"| LE["if !HAS_HI:<br/>HAS_HI<br/>hi_constraint = i"]
    Op -->|"other"| Next1

    GT --> Next1
    GE --> Next1
    LT --> Next1
    LE --> Next1

    Next1 --> Loop
    Loop -->|done| Assign["assign argvIndex:<br/>lo_constraint → 1<br/>hi_constraint → 2"]
    Assign --> Cost["estimatedCost = HAS_*<br/>? 1000<br/>: 1e9"]
    Cost --> Out([return idxNum])
```

The bitmask is four bits wide:

| bit       | name      | meaning                                  |
| --------- | --------- | ---------------------------------------- |
| `1 << 0`  | `HAS_LO`  | a lower bound on `time_col` was captured |
| `1 << 1`  | `HAS_HI`  | an upper bound on `time_col` was captured |
| `1 << 2`  | `LO_IS_GT`| if set, lower bound is strict (`>`); else `>=` |
| `1 << 3`  | `HI_IS_LT`| if set, upper bound is strict (`<`); else `<=` |

A *strict* bound carries through to the child SELECT: if the parent
said `ts > 'X'`, the child SELECT becomes `... WHERE ts > ?` (not
`>=`), so per-file pruning preserves the semantics exactly.

The cost figures (1e9 unbounded vs 1000 bounded) are large enough that
the planner reliably prefers handing us the bounds; we leave
`omit = 0` so SQLite still re-checks each row in case the bound is
intra-day.

### Why omit = 0?

```mermaid
sequenceDiagram
    actor User
    participant Host
    participant Mod
    participant File as day file 2024-03-15

    User->>Host: SELECT ... WHERE ts < '2024-03-15T11:30:00'
    Host->>Mod: xBestIndex
    Mod-->>Host: HAS_HI | HI_IS_LT, omit=0
    Host->>Mod: xFilter('2024-03-15T11:30:00')
    Mod->>File: open, prepare WHERE ts < ?, bind, step
    File-->>Mod: row ts=2024-03-15T11:00:00 ✓ (already pre-filtered)
    Mod-->>Host: row
    Note over Host: With omit=0, host re-checks ts < '...11:30:00'.<br>This is harmless here, but is essential when the<br>parent bound is on a textual ISO format that<br>doesn't sort identically inside the child (e.g.<br>varying timezone suffixes across files).
    Host-->>User: row
```

Setting `omit = 1` would tell SQLite "trust the vtab — don't re-check
this row". We deliberately do *not* claim that, because:

- Some users mix bare dates (`'2024-03-01'`) and ISO 8601
  (`'2024-03-01T10:00:00Z'`); their lexical comparison varies.
- The child file's index on `time_col` is great for pruning but its
  collation could differ across day files.
- The cost of a double-check at the host is negligible.

## xFilter: from bounds to file list

```mermaid
flowchart TB
    Enter([xFilter idxNum, argv])
    Enter --> Reset["cursor_close_current<br/>cursor_release_files<br/>free lo_text / hi_text<br/>step_start = time(now)"]

    Reset --> ReadLo{idxNum & HAS_LO?}
    ReadLo -->|yes| StoreLo["lo_text = mprintf(argv[0])<br/>lo_is_gt = (idxNum & LO_IS_GT)"]
    ReadLo -->|no| SkipLo[ ]
    StoreLo --> ReadHi
    SkipLo --> ReadHi

    ReadHi{idxNum & HAS_HI?}
    ReadHi -->|yes| StoreHi["hi_text = mprintf(argv[idx])<br/>hi_is_lt = (idxNum & HI_IS_LT)"]
    ReadHi -->|no| SkipHi[ ]
    StoreHi --> Parse
    SkipHi --> Parse

    Parse["have_lo = parse_date(lo_text, lo_tm)<br/>have_hi = parse_date(hi_text, hi_tm)"]
    Parse --> Fallback{have_hi?}

    Fallback -->|no| HiToday["hi_tm = today_utc_midnight()"]
    Fallback -->|yes| HiKeep[ ]
    HiToday --> LoFallback
    HiKeep --> LoFallback

    LoFallback{have_lo?}
    LoFallback -->|no| LoBack["lo_tm = hi_tm - (lookback_days - 1) days"]
    LoFallback -->|yes| LoKeep[ ]
    LoBack --> Enumerate
    LoKeep --> Enumerate

    Enumerate["enumerate_range(lo_tm, hi_tm):<br/>cap span at lookback_days,<br/>walk day-by-day,<br/>stat each candidate"]
    Enumerate --> Build["files[] built<br/>file_idx = 0"]
    Build --> Advance([cursor_advance])
```

Two fallback rules let the vtab give a sensible answer even when the
parent didn't filter by `time_col` at all:

- Missing upper bound → use today.
- Missing lower bound → use `today - lookback_days + 1`.

`lookback_days` defaults to 1825 (5 years) and is configurable per
vtab.

### File enumeration in detail

```mermaid
flowchart TB
    EnterE([enumerate_range lo, hi])
    EnterE --> Empty{lo &gt; hi?}
    Empty -->|yes| Done0[n_files = 0]
    Empty -->|no| Span["span = (hi - lo)/86400 + 1<br/>if span &gt; lookback_days:<br/>  shift lo forward,<br/>  span = lookback_days"]

    Span --> AllocList["allocate files[] capacity = span"]
    AllocList --> DayLoop{for i in 0..span}
    DayLoop -->|"each i"| Stat["build candidate path:<br/>dir + '/' + strftime(pattern, lo + i days)"]
    Stat --> Exists{regular file?}
    Exists -->|yes| Push["files[n++] = path<br/>DLOG +file"]
    Exists -->|no| Free["sqlite3_free(path)<br/>DLOG -missing"]
    Push --> DayLoop
    Free --> DayLoop
    DayLoop -->|done| DoneN["n_files = n<br/>file_idx = 0"]
```

The directory is **re-scanned for every query**. There is no list
cache. This is deliberate: an ingest pipeline that drops a new day
file mid-session is picked up by the next panel refresh, no schema
reload required.

## Cursor advance: stepping through files

```mermaid
flowchart TB
    Enter([cursor_advance])
    Enter --> HasStmt{child_stmt?}

    HasStmt -->|yes| StepIt["rc = sqlite3_step(child_stmt)"]
    HasStmt -->|no| FilesLeft

    StepIt --> StepKind{rc?}
    StepKind -->|"SQLITE_ROW"| Yield["rowid++<br/>return ROW"]
    StepKind -->|"DONE or error"| CloseCur["cursor_close_current:<br/>finalize stmt;<br/>if historical: cache_put<br/>else: sqlite3_close;<br/>file_idx++"]
    CloseCur --> FilesLeft

    FilesLeft{file_idx &lt; n_files?}
    FilesLeft -->|no| EOF["eof = true<br/>return"]
    FilesLeft -->|yes| TryOpen["cursor_open_current"]
    TryOpen --> OpenOK{success?}
    OpenOK -->|no, silently| Bump["file_idx++"]
    Bump --> FilesLeft
    OpenOK -->|yes| StepIt
```

The loop is the heart of the routing logic. There are two exit
conditions:

- `SQLITE_ROW`: we hand control back to the host with a positioned
  row. `xColumn` and `xRowid` then read from `child_stmt`. The next
  `xNext` re-enters this function and steps again.
- All files exhausted: `eof = true`, the host stops calling `xNext`.

Open-or-prepare failures on a single file are **silent**: we
increment `file_idx` and continue. A corrupt or unreadable partition
file disappears from the result, never breaks the query.

## Push-down: building the per-file SELECT

```mermaid
flowchart TB
    Enter([cursor_open_current])
    Enter --> ClassifyToday["today_path = dir + strftime(pattern, today)<br/>is_today = (path == today_path)"]

    ClassifyToday --> TodayBranch{is_today?}
    TodayBranch -->|yes| FreshOpen["sqlite3_open_v2(<br/>  READONLY | NOMUTEX)"]
    TodayBranch -->|no| TryCache["db = cache_take(path)"]
    TryCache --> CacheHit{hit?}
    CacheHit -->|yes| GotDb[ ]
    CacheHit -->|no| FreshOpen2["sqlite3_open_v2(<br/>  READONLY | NOMUTEX)<br/>set busy_timeout"]
    FreshOpen --> SetTimeout["set busy_timeout"]
    FreshOpen2 --> SetTimeout
    SetTimeout --> GotDb
    GotDb --> Progress["sqlite3_progress_handler(<br/>  PROGRESS_OPS, progress_cb, cur)"]

    Progress --> BuildSql{lo_text? hi_text?}
    BuildSql -->|"both"| Both["sql = select_prefix +<br/>WHERE col &gt;|&gt;= ? AND col &lt;|&lt;= ?"]
    BuildSql -->|"lo only"| LoOnly["sql = select_prefix +<br/>WHERE col &gt;|&gt;= ?"]
    BuildSql -->|"hi only"| HiOnly["sql = select_prefix +<br/>WHERE col &lt;|&lt;= ?"]
    BuildSql -->|"neither"| NoWhere["sql = select_prefix<br/>(no WHERE)"]

    Both --> Prep
    LoOnly --> Prep
    HiOnly --> Prep
    NoWhere --> Prep

    Prep["sqlite3_prepare_v2"]
    Prep --> BindLo{lo_text?}
    BindLo -->|yes| BL["bind(1, lo_text)"]
    BindLo -->|no| BindHi
    BL --> BindHi
    BindHi{hi_text?}
    BindHi -->|yes| BH["bind(next, hi_text)"]
    BindHi -->|no| Ready
    BH --> Ready
    Ready([child_db + child_stmt set])
```

The child SELECT is rebuilt per-file because the same bound text is
re-bound. We could cache the compiled SQL string on the vtab, but it
costs essentially nothing to rebuild and saves us tracking when bounds
changed across queries.

### Strictness propagation

| parent bound on `time_col` | child SELECT clause             |
| -------------------------- | ------------------------------- |
| `ts >= 'X'`                | `WHERE "ts" >= ?`               |
| `ts >  'X'`                | `WHERE "ts" >  ?`               |
| `ts <= 'Y'`                | `WHERE "ts" <= ?`               |
| `ts <  'Y'`                | `WHERE "ts" <  ?`               |
| `ts >= 'X' AND ts < 'Y'`   | `WHERE "ts" >= ? AND "ts" < ?`  |

This is intentional: Grafana's `$__timeFilter` macro emits a half-open
interval (`>=` lower, `<` upper), which is exactly the pattern that
maps cleanly onto contiguous day partitions.

## LRU cache interactions

```mermaid
sequenceDiagram
    autonumber
    participant Cur as Cursor
    participant Cache as Vtab.cache[16]
    participant DB as sqlite3* (historical)

    Note over Cur,Cache: opening file_idx (historical)
    Cur->>Cache: cache_take(path)
    alt hit
        Cache-->>Cur: db (slot cleared)
        Note over Cache: ownership transfers OUT of cache
    else miss
        Cache-->>Cur: nullptr
        Cur->>DB: sqlite3_open_v2 READONLY
    end
    Cur->>DB: progress_handler(cur)
    Cur->>DB: prepare + bind + step

    Note over Cur,Cache: stepping yields rows...

    Note over Cur,Cache: SQLITE_DONE -> close current
    Cur->>DB: finalize stmt
    alt historical
        Cur->>Cache: cache_put(path, db)
        Note over Cache: clears progress_handler,<br>may evict the LRU victim
    else today's file
        Cur->>DB: sqlite3_close
    end
```

A handle is **either** in the cache or **in a cursor** — never both.
`cache_take` removes the entry; `cache_put` reinserts. This invariant
means eviction during use is impossible: cache_put only sees handles
nobody is currently iterating.

Today's file is excluded from the cache entirely so each query opens
it fresh and observes the writer's latest WAL state.

## Concrete walkthrough

Given the fixture `/data` with files for `2024-03-01`, `2024-03-02`,
`2024-03-04`, `2024-03-05` (note: 03-03 missing), and a parent query:

```sql
SELECT ts, host, value
  FROM metrics
 WHERE ts >= '2024-03-01' AND ts < '2024-03-06'
   AND host = 'web-1';
```

Step by step:

```mermaid
sequenceDiagram
    actor User
    participant Host
    participant Mod
    participant FS as filesystem
    participant F1 as 03-01.sqlite
    participant F2 as 03-02.sqlite
    participant F3 as 03-04.sqlite
    participant F4 as 03-05.sqlite

    User->>Host: SELECT ... WHERE ts >= '03-01' AND ts < '03-06' AND host='web-1'
    Host->>Mod: xBestIndex
    Mod-->>Host: HAS_LO | HAS_HI | HI_IS_LT, cost=1000
    Host->>Mod: xFilter('03-01', '03-06')
    Mod->>FS: stat 03-01.sqlite (✓)
    Mod->>FS: stat 03-02.sqlite (✓)
    Mod->>FS: stat 03-03.sqlite (✗ missing)
    Mod->>FS: stat 03-04.sqlite (✓)
    Mod->>FS: stat 03-05.sqlite (✓)
    Mod->>FS: stat 03-06.sqlite (✗ end of range, not present)
    Note over Mod: files = [03-01, 03-02, 03-04, 03-05]

    Mod->>F1: open READONLY, prepare, bind('03-01','03-06'), step
    F1-->>Mod: rows (web-1, db-1, web-2, ...)
    loop while ROW
        Host->>Mod: xColumn x3
        Note over Host: re-check host='web-1'
        Host->>Mod: xNext
        Mod->>F1: step
    end
    F1-->>Mod: DONE
    Mod->>Mod: cache_put F1, file_idx++

    Mod->>F2: open + prepare + step
    F2-->>Mod: rows
    Note over Mod: ...same loop...
    F2-->>Mod: DONE
    Mod->>Mod: cache_put F2

    Note over Mod,FS: 03-03 silently absent — no error, no row

    Mod->>F3: open + prepare + step
    F3-->>Mod: rows
    F3-->>Mod: DONE
    Mod->>Mod: cache_put F3

    Mod->>F4: open + prepare + step
    F4-->>Mod: rows
    F4-->>Mod: DONE
    Mod->>Mod: cache_put F4

    Mod->>Host: eof=1
    Host-->>User: result set
```

A re-run of the same query now hits the cache for all four files —
no `sqlite3_open_v2` calls, no schema parsing — and serves the result
out of pre-warm handles.
