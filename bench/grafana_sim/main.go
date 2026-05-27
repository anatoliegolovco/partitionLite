// grafana_sim is a headless validation harness for ts_partition. It uses
// mattn/go-sqlite3 with the sqlite_load_extension build tag — the same
// runtime path a cgo-based Grafana SQLite plugin would take — to load the
// extension, declare the virtual table, and run the kind of queries
// Grafana's $__timeFilter / $__timeGroup macros generate.
//
// What this is not: it is not Grafana itself. The shipping
// frser-sqlite-datasource plugin uses modernc.org/sqlite (a pure-Go
// transpilation) and cannot load C extensions, so a real Grafana panel
// test would require forking that plugin. This harness validates the
// integration shape one layer below; a cgo plugin behaves identically.
package main

import (
	"database/sql"
	"flag"
	"fmt"
	"log"
	"math"
	"os"
	"path/filepath"
	"runtime"
	"sort"
	"time"

	sqlite3 "github.com/mattn/go-sqlite3"
)

type config struct {
	extPath    string
	dataDir    string
	days       int
	hosts      int
	stepSec    int
	skipOffset int
	keepData   bool
}

func main() {
	c := config{}
	flag.StringVar(&c.extPath, "ext", findExtensionDefault(), "path to ts_partition.so")
	flag.StringVar(&c.dataDir, "data", "", "fixture dir (default: temp dir)")
	flag.IntVar(&c.days, "days", 30, "days of synthetic data")
	flag.IntVar(&c.hosts, "hosts", 5, "hosts per day")
	flag.IntVar(&c.stepSec, "step", 60, "seconds between samples")
	flag.IntVar(&c.skipOffset, "skip", 7, "offset of the deliberately-missing day (relative to today)")
	flag.BoolVar(&c.keepData, "keep", false, "keep fixture dir after run")
	flag.Parse()

	if c.dataDir == "" {
		tmp, err := os.MkdirTemp("", "ts_partition_sim_*")
		if err != nil {
			log.Fatalf("mkdtemp: %v", err)
		}
		c.dataDir = tmp
	}
	if !c.keepData {
		defer os.RemoveAll(c.dataDir)
	} else {
		fmt.Printf("keeping fixture dir: %s\n", c.dataDir)
	}

	if _, err := os.Stat(c.extPath); err != nil {
		log.Fatalf("extension not found at %s (use -ext): %v", c.extPath, err)
	}

	if err := generateFixtures(c); err != nil {
		log.Fatalf("fixtures: %v", err)
	}

	db, err := openWithExtension(c.extPath)
	if err != nil {
		log.Fatalf("open db: %v", err)
	}
	defer db.Close()

	if err := declareVtab(db, c.dataDir); err != nil {
		log.Fatalf("declare vtab: %v", err)
	}

	r := newRunner()
	runScenarios(r, db, c)
	r.report()
	if r.failed > 0 {
		os.Exit(1)
	}
}

func findExtensionDefault() string {
	_, here, _, _ := runtime.Caller(0)
	root := filepath.Join(filepath.Dir(here), "..", "..")
	if runtime.GOOS == "darwin" {
		return filepath.Join(root, "ts_partition.dylib")
	}
	return filepath.Join(root, "ts_partition.so")
}

// openWithExtension registers a custom driver that loads ts_partition into
// every new connection. This is the same approach a cgo Grafana plugin
// would use.
func openWithExtension(path string) (*sql.DB, error) {
	const driverName = "sqlite3_with_ts_partition"
	registered := false
	for _, d := range sql.Drivers() {
		if d == driverName {
			registered = true
			break
		}
	}
	if !registered {
		sql.Register(driverName, &sqlite3.SQLiteDriver{
			ConnectHook: func(conn *sqlite3.SQLiteConn) error {
				return conn.LoadExtension(path, "sqlite3_tspartition_init")
			},
		})
	}
	return sql.Open(driverName, ":memory:")
}

func declareVtab(db *sql.DB, dir string) error {
	stmt := fmt.Sprintf(`CREATE VIRTUAL TABLE metrics USING ts_partition(
        dir       = '%s',
        pattern   = 'metrics_%%Y-%%m-%%d.sqlite',
        table     = 'metrics',
        time_col  = 'ts'
    )`, dir)
	_, err := db.Exec(stmt)
	return err
}

// generateFixtures writes one .sqlite file per day for the last `days`,
// with `hosts` distinct hosts and a sample every `stepSec` seconds. The
// day at offset `skipOffset` is intentionally absent.
func generateFixtures(c config) error {
	if err := os.MkdirAll(c.dataDir, 0o755); err != nil {
		return err
	}
	today := todayMidnightUTC()
	totalRows := 0
	start := time.Now()
	for off := 0; off < c.days; off++ {
		if off == c.skipOffset {
			continue
		}
		day := today.AddDate(0, 0, -off)
		name := fmt.Sprintf("metrics_%s.sqlite", day.Format("2006-01-02"))
		path := filepath.Join(c.dataDir, name)
		rows, err := writeDayFile(path, day, c.hosts, c.stepSec)
		if err != nil {
			return fmt.Errorf("write %s: %w", path, err)
		}
		totalRows += rows
	}
	fmt.Printf("fixtures: %d files, %d rows total (skipped day -%d), built in %s\n",
		c.days-1, totalRows, c.skipOffset, time.Since(start).Round(time.Millisecond))
	return nil
}

func writeDayFile(path string, day time.Time, hosts, stepSec int) (int, error) {
	db, err := sql.Open("sqlite3", path)
	if err != nil {
		return 0, err
	}
	defer db.Close()
	if _, err := db.Exec(`PRAGMA journal_mode=WAL;
        CREATE TABLE metrics(ts TEXT, host TEXT, value REAL);
        CREATE INDEX metrics_ts ON metrics(ts);`); err != nil {
		return 0, err
	}
	tx, err := db.Begin()
	if err != nil {
		return 0, err
	}
	stmt, err := tx.Prepare("INSERT INTO metrics VALUES (?, ?, ?)")
	if err != nil {
		return 0, err
	}
	defer stmt.Close()
	rows := 0
	for sec := 0; sec < 86400; sec += stepSec {
		ts := day.Add(time.Duration(sec) * time.Second).Format("2006-01-02T15:04:05Z")
		for h := 0; h < hosts; h++ {
			host := fmt.Sprintf("host-%d", h)
			// Deterministic synthetic value: sinusoid + host offset.
			v := math.Sin(float64(sec)/3600.0) + float64(h)
			if _, err := stmt.Exec(ts, host, v); err != nil {
				return 0, err
			}
			rows++
		}
	}
	return rows, tx.Commit()
}

func todayMidnightUTC() time.Time {
	now := time.Now().UTC()
	return time.Date(now.Year(), now.Month(), now.Day(), 0, 0, 0, 0, time.UTC)
}

// --- scenario runner ---

type runner struct {
	passed int
	failed int
}

func newRunner() *runner { return &runner{} }

func (r *runner) run(name string, fn func() error) {
	start := time.Now()
	err := fn()
	dur := time.Since(start)
	if err != nil {
		fmt.Printf("FAIL %-48s  %8s  %s\n", name, dur.Round(time.Millisecond), err)
		r.failed++
	} else {
		fmt.Printf("ok   %-48s  %8s\n", name, dur.Round(time.Millisecond))
		r.passed++
	}
}

func (r *runner) report() {
	fmt.Printf("\n%d passed, %d failed\n", r.passed, r.failed)
}

func runScenarios(r *runner, db *sql.DB, c config) {
	today := todayMidnightUTC()

	// Time bounds Grafana-style: half-open, ISO 8601 with seconds.
	tf := func(o int) string {
		return today.AddDate(0, 0, -o).Format("2006-01-02T15:04:05Z")
	}

	expectInt := func(query, label string, want int64, budget time.Duration) func() error {
		return func() error {
			start := time.Now()
			var got int64
			if err := db.QueryRow(query).Scan(&got); err != nil {
				return err
			}
			elapsed := time.Since(start)
			if got != want {
				return fmt.Errorf("got %d, want %d (in %s)", got, want, elapsed)
			}
			if elapsed > budget {
				return fmt.Errorf("budget exceeded: %s > %s (result %d)",
					elapsed.Round(time.Millisecond), budget, got)
			}
			return nil
		}
	}

	samplesPerDay := int64(86400 / c.stepSec * c.hosts)
	presentDays := int64(c.days - 1)

	r.run("count(*) full table (no bounds, 30 days, gap)",
		expectInt(
			`SELECT count(*) FROM metrics`,
			"all", samplesPerDay*presentDays, 5*time.Second))

	r.run("range covers all days (incl. missing day silent skip)",
		expectInt(fmt.Sprintf(
			`SELECT count(*) FROM metrics WHERE ts >= '%s' AND ts < '%s'`,
			tf(c.days), tf(-1)),
			"range-all", samplesPerDay*presentDays, 3*time.Second))

	r.run("range entirely inside the missing day -> 0 rows",
		expectInt(fmt.Sprintf(
			`SELECT count(*) FROM metrics WHERE ts >= '%s' AND ts < '%s'`,
			tf(c.skipOffset), tf(c.skipOffset-1)),
			"gap", 0, 500*time.Millisecond))

	r.run("single recent day",
		expectInt(fmt.Sprintf(
			`SELECT count(*) FROM metrics WHERE ts >= '%s' AND ts < '%s'`,
			tf(1), tf(0)),
			"yesterday", samplesPerDay, 500*time.Millisecond))

	r.run("intra-day window (last 6 hours of day -1)",
		expectInt(fmt.Sprintf(
			`SELECT count(*) FROM metrics WHERE ts >= '%s' AND ts < '%s'`,
			today.AddDate(0, 0, -1).Add(18*time.Hour).Format("2006-01-02T15:04:05Z"),
			today.Format("2006-01-02T15:04:05Z")),
			"intra-day", int64(6*3600/c.stepSec*c.hosts), 500*time.Millisecond))

	r.run("$__timeGroup-shaped aggregate (1h buckets over 7 days)",
		func() error {
			lo := tf(7)
			hi := tf(0)
			q := fmt.Sprintf(`
                SELECT
                    strftime('%%Y-%%m-%%dT%%H:00:00Z', ts) AS bucket,
                    host,
                    avg(value) AS v
                FROM metrics
                WHERE ts >= '%s' AND ts < '%s'
                GROUP BY 1, 2
                ORDER BY 1, 2
            `, lo, hi)
			start := time.Now()
			rows, err := db.Query(q)
			if err != nil {
				return err
			}
			defer rows.Close()
			n := 0
			for rows.Next() {
				var bucket, host string
				var v float64
				if err := rows.Scan(&bucket, &host, &v); err != nil {
					return err
				}
				n++
			}
			elapsed := time.Since(start)
			// 7 days × 24 hours × 5 hosts = 840 buckets, minus the missing day.
			expected := (7 - 1) * 24 * c.hosts
			if n != expected {
				return fmt.Errorf("got %d rows, want %d (in %s)", n, expected, elapsed)
			}
			if elapsed > 3*time.Second {
				return fmt.Errorf("budget exceeded: %s (rows=%d)", elapsed, n)
			}
			return nil
		})

	r.run("non-time predicate (host-2 only, last 7d)",
		expectInt(fmt.Sprintf(
			`SELECT count(*) FROM metrics
              WHERE ts >= '%s' AND ts < '%s' AND host = 'host-2'`,
			tf(7), tf(0)),
			"host-filter", int64((7-1)*86400/c.stepSec), 500*time.Millisecond))

	r.run("DISTINCT host across all files",
		func() error {
			rows, err := db.Query(`SELECT DISTINCT host FROM metrics ORDER BY host`)
			if err != nil {
				return err
			}
			defer rows.Close()
			var hosts []string
			for rows.Next() {
				var h string
				if err := rows.Scan(&h); err != nil {
					return err
				}
				hosts = append(hosts, h)
			}
			if len(hosts) != c.hosts {
				return fmt.Errorf("got %d hosts, want %d", len(hosts), c.hosts)
			}
			if !sort.StringsAreSorted(hosts) {
				return fmt.Errorf("hosts not sorted: %v", hosts)
			}
			return nil
		})

	r.run("repeat query exercises LRU cache (3x same range)",
		func() error {
			q := fmt.Sprintf(
				`SELECT count(*) FROM metrics WHERE ts >= '%s' AND ts < '%s'`,
				tf(5), tf(0))
			var first, second, third time.Duration
			for i, p := range []*time.Duration{&first, &second, &third} {
				start := time.Now()
				var n int64
				if err := db.QueryRow(q).Scan(&n); err != nil {
					return err
				}
				*p = time.Since(start)
				if i == 0 && n != int64((5-1)*samplesPerDay) && i == 0 {
					// the gap day might or might not fall inside the 5-day window
					if n != 5*samplesPerDay {
						return fmt.Errorf("run %d: got %d", i, n)
					}
				}
			}
			fmt.Printf("       runs: 1st=%s  2nd=%s  3rd=%s",
				first.Round(time.Millisecond),
				second.Round(time.Millisecond),
				third.Round(time.Millisecond))
			// We don't fail on this — the cache effect on a small file set is
			// modest. We just report the numbers so a regression is visible.
			return nil
		})

	r.run("query range outside data -> 0 rows, no error",
		expectInt(
			`SELECT count(*) FROM metrics WHERE ts >= '1990-01-01' AND ts < '1990-01-10'`,
			"empty-range", 0, 200*time.Millisecond))
}
