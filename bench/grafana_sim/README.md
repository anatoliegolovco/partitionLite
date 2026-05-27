# grafana_sim

A headless validation harness that exercises `ts_partition` through the
same code path a cgo-based Grafana SQLite plugin would use. It is not
Grafana itself.

## Why a separate harness?

The shipping `frser-sqlite-datasource` plugin uses `modernc.org/sqlite`,
a pure-Go transpilation of SQLite that has no `sqlite3_load_extension`
and cannot load a C `.so`. A real Grafana panel test against
`ts_partition` would require forking the plugin to swap the driver back
to `mattn/go-sqlite3` and adding a config option to call
`EnableLoadExtension`. That is a separate engineering project.

What this harness does instead: it links `mattn/go-sqlite3` directly
(with the `sqlite_load_extension` build tag), registers a custom driver
whose `ConnectHook` calls `LoadExtension("./ts_partition.so",
"sqlite3_tspartition_init")`, and runs the kinds of queries Grafana's
`$__timeFilter` / `$__timeGroup` macros generate. Same runtime path as
a cgo plugin; same answer the plugin would produce.

## What it covers

Generates 30 days of synthetic data (29 daily partition files; one
middle day intentionally absent), 5 hosts, one sample per minute —
about 210 000 rows total — then runs 10 scenarios:

- full-table count with no bounds
- 30-day range covering the silent-skip gap
- range entirely inside the missing day → 0 rows, no error
- single recent day
- intra-day window (last 6h of the previous day)
- `$__timeGroup`-shaped 1-hour buckets over 7 days
- non-time predicate (`host = 'host-2'`) combined with range
- `DISTINCT host` across every file
- repeat query (LRU cache exercise, runs 3x and reports each timing)
- range entirely outside the fixture window

Each scenario asserts an exact row count and a wall-clock budget. The
harness exits non-zero on any failure.

## Run

```sh
make            # build ts_partition.so at repo root
make bench-sim  # build fixtures + run the harness
```

Or manually:

```sh
go run -tags sqlite_load_extension ./bench/grafana_sim
```

Flags:

| flag    | default                       | meaning                                  |
| ------- | ----------------------------- | ---------------------------------------- |
| `-ext`  | `../../ts_partition.so`       | path to the built extension              |
| `-data` | random temp dir               | fixture directory (auto-cleaned)         |
| `-days` | `30`                          | number of days of synthetic data         |
| `-hosts`| `5`                           | distinct hosts per day                   |
| `-step` | `60`                          | seconds between samples                  |
| `-skip` | `7`                           | offset of the deliberately-absent day    |
| `-keep` | `false`                       | keep the fixture dir after the run       |
