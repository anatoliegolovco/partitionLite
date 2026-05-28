# Architecture

`ts_partition` is a SQLite **loadable extension** that registers a
single **virtual-table module** named `ts_partition`. The host process
(any SQLite3 — CLI, your app, a cgo-based Grafana plugin) loads the
`.so`, the module's init function registers an `sqlite3_module`
function table, and from then on `CREATE VIRTUAL TABLE foo USING
ts_partition(...)` creates an instance that fans queries out across
sibling `.sqlite` files.

This document covers the runtime topology, the in-memory types, the
query lifecycle, and the cursor state machine. SQL routing — how a
single `SELECT` becomes N child `SELECT`s — lives in
[sql-routing.md](./sql-routing.md).

## Runtime topology

```mermaid
flowchart LR
    subgraph HostProcess["host process (sqlite3 CLI, app, Grafana plugin, ...)"]
        Host[("host sqlite3<br/>connection")]
        Mod["ts_partition module<br/>(.so loaded via .load)"]
        VT["vtab instance:<br/>metrics"]
        Cur["cursor"]
        LRU["LRU cache<br/>16 historical handles"]
    end

    subgraph FS["filesystem: dir/"]
        F1[("metrics_2024-03-01.sqlite")]
        F2[("metrics_2024-03-02.sqlite")]
        F3[("metrics_today.sqlite<br/>WAL writer attached")]
        Fx[("...")]
    end

    Host -->|"CREATE VIRTUAL TABLE,<br/>SELECT, xBestIndex,<br/>xFilter, xNext, ..."| Mod
    Mod --> VT
    VT --> Cur
    VT -. caches .-> LRU
    LRU -. sqlite3* .-> F1
    LRU -. sqlite3* .-> F2
    Cur -. fresh open per query .-> F3
    Cur -. via cache .-> F1
    Cur -. via cache .-> F2
```

Every parent SQL statement runs inside the host connection. The vtab
module receives callbacks and, internally, opens **additional**
`sqlite3*` handles — one per partition file actively being read. These
are private; the host never sees them. Today's file is always opened
fresh per query so a Grafana refresh sees the latest WAL state from
the writer. Historical files are pooled in a 16-slot LRU on the vtab
to amortize open cost across panel refreshes.

## In-memory types

```mermaid
classDiagram
    direction LR

    class Vtab {
        +sqlite3_vtab base
        +char* dir
        +char* pattern
        +char* child_table
        +char* time_col
        +int n_cols
        +int time_col_idx
        +int lookback_days
        +char* select_prefix
        +ColInfo cols[MAX_COLS]
        +CacheEntry cache[LRU_CAPACITY]
        +int64_t lru_clock
    }

    class Cursor {
        +sqlite3_vtab_cursor base
        +Vtab* vtab
        +char** files
        +int n_files
        +int file_idx
        +sqlite3* child_db
        +sqlite3_stmt* child_stmt
        +bool child_is_today
        +time_t step_start
        +sqlite3_int64 rowid
        +char* lo_text
        +char* hi_text
        +bool lo_is_gt
        +bool hi_is_lt
        +bool eof
    }

    class CacheEntry {
        +char* path
        +sqlite3* db
        +int64_t last_used
    }

    class ColInfo {
        +char* name
        +char* type
    }

    class sqlite3_module {
        +xCreate / xConnect
        +xBestIndex
        +xOpen / xClose
        +xFilter / xNext / xEof
        +xColumn / xRowid
        +xDisconnect / xDestroy
    }

    Vtab "1" *-- "MAX_COLS" ColInfo : sniffed schema
    Vtab "1" *-- "LRU_CAPACITY" CacheEntry : historical handles
    Cursor "*" --> "1" Vtab : reads schema + cache from
    Cursor ..> sqlite3_module : dispatched via
    Vtab ..> sqlite3_module : dispatched via
```

Two structs carry per-vtab and per-cursor state. Both inline a SQLite
base struct (`sqlite3_vtab` / `sqlite3_vtab_cursor`) as their first
field so the cast between SQLite's pointer and ours is free. The Vtab
owns the LRU; cursors borrow from it. A cursor's `child_db` /
`child_stmt` pair is the active partition file at any given moment —
the cursor walks `files[]` left-to-right, opening one file at a time.

## Query lifecycle

```mermaid
sequenceDiagram
    autonumber
    actor User
    participant Host as host sqlite3
    participant Mod as ts_partition module
    participant Vtab
    participant Cur as Cursor
    participant Child as child sqlite3* + stmt

    User->>Host: CREATE VIRTUAL TABLE metrics USING ts_partition(...)
    Host->>Mod: xConnect(argv)
    Mod->>Vtab: parse args, find newest existing partition
    Vtab->>Child: open READONLY + PRAGMA table_info
    Child-->>Vtab: column rows
    Vtab->>Host: sqlite3_declare_vtab("CREATE TABLE x(ts TEXT, ...)")
    Host-->>User: ok

    Note over User,Host: ... later, a SELECT comes in ...

    User->>Host: SELECT ts,host,value FROM metrics WHERE ts >= 'X' AND ts < 'Y'
    Host->>Mod: xBestIndex(constraints)
    Mod-->>Host: idxNum bitmask + cost + argv layout
    Host->>Mod: xOpen
    Mod->>Cur: alloc
    Host->>Mod: xFilter(idxNum, argv=[X,Y])
    Cur->>Cur: parse dates, enumerate files in [X..Y]
    loop while not EOF
        Cur->>Vtab: cache_take(path) or fresh open
        Cur->>Child: prepare SELECT ... WHERE ts >= ? AND ts < ?
        Cur->>Child: bind(X,Y) + step
        Child-->>Cur: SQLITE_ROW
        Host->>Mod: xColumn(0), xColumn(1), xColumn(2)
        Mod-->>Host: passthrough values
        Host->>Mod: xNext
        Cur->>Child: step
        alt SQLITE_DONE
            Cur->>Child: finalize stmt
            Cur->>Vtab: cache_put(path, db) [if historical]
            Cur->>Cur: file_idx++
        end
    end
    Host->>Mod: xClose
    Mod->>Cur: free files[], free bounds, free cursor
```

The four interesting phases:

1. **Connect (once per `CREATE VIRTUAL TABLE`).** Parse args, walk
   back from today to find the first existing partition, run `PRAGMA
   table_info` on it, declare the matching schema to the host.

2. **Plan (once per query).** xBestIndex inspects `ts_col`
   constraints, encodes which were captured into `idxNum`, and
   reports a low cost only when bounds were found — so the planner
   reliably hands us the bounds.

3. **Filter (once per query).** Cursor receives bound values from
   `argv`, parses them as dates, enumerates files to open, then steps
   the first one.

4. **Iterate (many times per query).** xNext steps the active child
   statement; on `DONE`, it closes (or returns to cache), increments
   `file_idx`, and prepares the next file.

## Cursor state machine

```mermaid
stateDiagram-v2
    direction LR

    [*] --> Open : xOpen alloc

    Open --> Filtered : xFilter\nbounds parsed\nfiles[] built
    Filtered --> Opening : cursor_advance\nopen file_idx

    Opening --> Stepping : open OK + prepare OK
    Opening --> SkippingFile : open or prepare failed\n(silent)
    SkippingFile --> Opening : file_idx++

    Stepping --> RowReady : step -> SQLITE_ROW
    Stepping --> AdvancingFile : step -> SQLITE_DONE or error

    RowReady --> Stepping : xNext\nstep again

    AdvancingFile --> Opening : file_idx++\nmore files left
    AdvancingFile --> EOF : file_idx == n_files

    EOF --> [*] : xClose
    RowReady --> [*] : xClose (early)
    Stepping --> [*] : xClose (early)
```

Every state transition out of `Stepping` either yields a row to the
host or releases the active child and moves to the next file. The
`SkippingFile` state is the silent-skip path — open or prepare
failures on a single partition never propagate out of the module.

## Module function table

```mermaid
flowchart LR
    subgraph Vtab["sqlite3_module ts_module"]
        direction TB
        x1[xCreate / xConnect → tsConnect]
        x2[xBestIndex → tsBestIndex]
        x3[xDisconnect / xDestroy → tsDisconnect]
        x4[xOpen → tsOpen]
        x5[xClose → tsClose]
        x6[xFilter → tsFilter]
        x7[xNext → tsNext]
        x8[xEof → tsEof]
        x9[xColumn → tsColumn]
        x10[xRowid → tsRowid]
        x11["xUpdate, xBegin,<br/>xRename, xFindFunction,<br/>... → nullptr"]
    end

    init[sqlite3_tspartition_init] --> reg[sqlite3_create_module<br/>name='ts_partition']
    reg --> Vtab
```

Only the methods needed for read-only iteration are filled in.
Everything else is `nullptr`, which SQLite interprets as "not
supported" — the host raises a normal `cannot modify` error if a user
ever tries to write through the vtab.

## File layout

```mermaid
flowchart TB
    Root["partitionLite/"]
    Root --> SRC["src/ts_partition.c<br/>~870 LoC, the whole module"]
    Root --> MK["Makefile"]
    Root --> RD["README.md"]
    Root --> LIC["LICENSE (MIT)"]

    Root --> Test["test/"]
    Test --> Gen["gen_fixtures.sh<br/>4-day fixture, 1 missing"]
    Test --> Smoke["smoke_test.sh<br/>11 assertions"]
    Test --> Data["data/  (gitignored, generated)"]

    Root --> Bench["bench/grafana_sim/"]
    Bench --> Main["main.go<br/>mattn/go-sqlite3 + LoadExtension<br/>30 days × 5 hosts, 209k rows"]
    Bench --> Mod["go.mod / go.sum"]

    Root --> Docs["docs/"]
    Docs --> DR["README.md  (index)"]
    Docs --> DA["architecture.md  (this file)"]
    Docs --> DS["sql-routing.md"]
    Docs --> DD["design-decisions.md"]

    Root --> CI[".github/workflows/ci.yml<br/>ubuntu + macOS matrix"]
```

The entire shipping extension is one C file plus a Makefile; the
tests and the benchmarking harness live alongside but are independent
builds.
