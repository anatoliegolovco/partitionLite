# ts_partition

A read-only SQLite virtual-table extension that fans queries out across
date-partitioned sibling `.sqlite` files in a directory and exposes them
as a single logical table. It exists so a tool that speaks only "plain
SQLite" — most directly the
[Grafana SQLite datasource](https://github.com/fr-ser/grafana-sqlite-datasource)
plugin — can query 5+ years of daily time-series files plus a `today`
file under live append, without any client-side SQL generation. Missing
day files are silently skipped, the writer's WAL is observed via
read-only opens, and bounds on the time column are pushed down both for
file-list pruning and into each child SELECT so per-file indexes still
help.

Status: MVP. The smoke suite passes on Linux and macOS and the design is
sound, but this has not been beat on in production yet. Please file
issues with reproductions if you find rough edges.

## Build

Requires libsqlite3 development headers, a C23-capable C compiler
(GCC 13+ via `-std=c2x`, GCC 14+ or Clang 18+ via `-std=c23`), and
standard libc. No other dependencies.

### Ubuntu / Debian

```sh
sudo apt-get install -y build-essential libsqlite3-dev sqlite3
make
```

### macOS

```sh
brew install sqlite
make
```

The Makefile detects Darwin and produces `ts_partition.dylib`; on Linux
it produces `ts_partition.so`.

### Windows (MSYS2)

```sh
pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-sqlite3
make
```

The init-function symbol is exported with `__declspec(dllexport)` so the
resulting `ts_partition.dll` should load with `.load ./ts_partition`,
but Windows is not exercised in CI — treat it as best-effort.

## Use

```sql
.load ./ts_partition

CREATE VIRTUAL TABLE metrics USING ts_partition(
    dir       = '/data',
    pattern   = 'metrics_%Y-%m-%d.sqlite',
    table     = 'metrics',
    time_col  = 'ts'
);

SELECT ts, host, value
  FROM metrics
 WHERE ts >= '2024-03-01' AND ts < '2024-03-15'
   AND host = 'web-1';
```

Arguments:

| arg             | meaning                                                              |
| --------------- | -------------------------------------------------------------------- |
| `dir`           | Directory holding the partition files.                               |
| `pattern`       | A `strftime(3)` format string (e.g. `metrics_%Y-%m-%d.sqlite`).      |
| `table`         | The name of the child table inside each partition file.              |
| `time_col`      | The column the planner can range-scan on. Must exist in the schema.  |
| `lookback_days` | (optional) How far back to enumerate when the query has no lower bound. Default `1825` (5 years). |

The schema is sniffed at `CREATE VIRTUAL TABLE` time from the most
recent existing partition (walking back up to `lookback_days`) via
`PRAGMA table_info(<table>)`. The declared schema is the union of that
sniff, ordered as in the source file.

The file list is re-derived from the directory on every query, so
dropping a new `.sqlite` file in mid-session is picked up by the next
panel refresh — no `CREATE VIRTUAL TABLE` reload needed.

## Grafana

Point the
[`frser-sqlite-datasource`](https://github.com/fr-ser/grafana-sqlite-datasource)
plugin at a small "shim" SQLite database (it can even be `:memory:`-like
if you persist a `.sql` init script). In that database:

```sql
.load /opt/ts_partition/ts_partition
CREATE VIRTUAL TABLE metrics USING ts_partition(
    dir='/var/lib/metrics', pattern='metrics_%Y-%m-%d.sqlite',
    table='metrics', time_col='ts');
```

Then in a Grafana panel:

```sql
SELECT $__timeGroup(ts, '1m') AS time, host, avg(value) AS value
  FROM metrics
 WHERE $__timeFilter(ts)
 GROUP BY 1, 2
 ORDER BY 1
```

`$__timeFilter(ts)` generates a half-open `ts >= 'X' AND ts < 'Y'` clause
which is exactly what `ts_partition`'s `xBestIndex` is looking for.

## Compatibility note: which Grafana SQLite plugins can load this?

Loadable SQLite extensions are a C-runtime feature. Any Grafana SQLite
plugin that uses `mattn/go-sqlite3` (cgo, linked against libsqlite3) and
is built with the `sqlite_load_extension` tag — and exposes a hook to
call `LoadExtension` — can load `ts_partition`. A plugin that uses
`modernc.org/sqlite` (a pure-Go transpilation) **cannot**: there is no C
runtime to host the `.so`.

The widely-used [`frser-sqlite-datasource`](https://github.com/fr-ser/grafana-sqlite-datasource)
plugin switched from `mattn/go-sqlite3` to `modernc.org/sqlite` at v3.0,
so as-shipped it does not load this extension. To use `ts_partition`
with Grafana today you need either:

- A plugin build that uses cgo SQLite with `EnableLoadExtension`, or
- A fork of `frser-sqlite-datasource` that swaps the driver and exposes
  an extensions config option (a few-hundred-line patch).

The `bench/grafana_sim/` harness in this repo validates the same
runtime path a cgo Grafana plugin would take — see its README for what
it covers and how to run it.

## Caveats

- **WAL needs writable directory.** Even a read-only opener needs to
  create `-shm` and `-wal` files when the writer is in WAL mode. Make
  sure the Grafana process can write to the directory holding the
  partitions, even though it cannot write to the `.sqlite` files
  themselves.
- **Hot WAL recovery requires write intent.** A crashed writer leaves
  the WAL hot; the next process to open the file performs recovery,
  and recovery needs write access. A read-only Grafana cannot do this
  on its own. Run your writer under a supervisor that opens with write
  intent first, so any crash recovery happens before Grafana sees the
  file.
- **WAL snapshot is pinned for the cursor lifetime.** A long-running
  reader prevents WAL truncation while it's stepping. The extension
  opens fresh for "today" on every query and finishes quickly in
  practice; if you script your own long-running scan, break it into
  smaller queries.
- **No write path.** `xUpdate`, `xBegin`, etc. are not implemented.
  This is read-only by design; ingest is your concern.
- **One schema for the whole table.** All partition files must share
  the same `<table>` schema. Schema drift between days is not handled —
  the only thing the reader does on prepare failure is silently skip
  the file.
- **Date arithmetic is in UTC.** The day boundary used for file naming
  follows the calendar in UTC. "Today" for the cache classification is
  derived from local time, which matches what users expect when they
  drop a new daily file at local midnight.
- **Cache assumes serialized access.** The LRU is shared across cursors
  on the same vtab. SQLite's default serialized threading mode protects
  the parent connection from concurrent use; if you've explicitly opted
  out of that, serialize access yourself.

## License

MIT. See `LICENSE`.

## Deeper reading

Design and routing internals are documented under [`docs/`](./docs/):

- [`docs/architecture.md`](./docs/architecture.md) — component layout, class diagram, query lifecycle, cursor state machine.
- [`docs/sql-routing.md`](./docs/sql-routing.md) — how one `SELECT` becomes N child `SELECT`s; xBestIndex / xFilter / cursor-advance flows.
- [`docs/design-decisions.md`](./docs/design-decisions.md) — why each non-obvious choice was made.
