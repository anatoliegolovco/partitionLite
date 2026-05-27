#!/usr/bin/env sh
# Smoke test for ts_partition. Builds queries against the fixture directory,
# compares output to expected values, and bails on the first mismatch.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DATA_DIR="$ROOT/test/data"
case "$(uname -s)" in
    Darwin) SO="$ROOT/ts_partition.dylib" ;;
    *)      SO="$ROOT/ts_partition.so" ;;
esac
[ -f "$SO" ] || { echo "extension not built at $SO"; exit 1; }
[ -d "$DATA_DIR" ] && [ -n "$(ls "$DATA_DIR"/metrics_*.sqlite 2>/dev/null)" ] \
    || { echo "no fixtures in $DATA_DIR (run 'make fixtures')"; exit 1; }

day_for_offset() {
    off=$1
    case "$(uname -s)" in
        Darwin) date -v "-${off}d" +%Y-%m-%d ;;
        *)      date -d "${off} days ago" +%Y-%m-%d ;;
    esac
}

CREATE_VT="CREATE VIRTUAL TABLE m USING ts_partition(\
dir='$DATA_DIR', pattern='metrics_%Y-%m-%d.sqlite',\
table='metrics', time_col='ts', lookback_days=10000);"

q() {
    sqlite3 :memory: ".load $SO" "$CREATE_VT" "$1"
}

pass=0
fail=0
check() {
    label=$1; expected=$2; actual=$3
    if [ "$actual" = "$expected" ]; then
        printf 'ok   %s\n' "$label"
        pass=$((pass + 1))
    else
        printf 'FAIL %s\n     expected: %s\n     got:      %s\n' \
            "$label" "$expected" "$actual"
        fail=$((fail + 1))
    fi
}

D10=$(day_for_offset 10)
D9=$(day_for_offset 9)
D7=$(day_for_offset 7)
D6=$(day_for_offset 6)
D11=$(day_for_offset 11)
D5=$(day_for_offset 5)

# 1. count rows across all available days
check "count across all days" 16 \
    "$(q 'SELECT count(*) FROM m;')"

# 2. range query crossing intentionally-missing day -8
#    (lo includes -11 .. hi excludes -5 → spans -10..-6 with -8 absent)
check "range crosses missing day" 16 \
    "$(q "SELECT count(*) FROM m WHERE ts >= '$D11' AND ts < '$D5';")"

# 3. single-day query (the file at offset -10, half-open)
check "single-day query" 4 \
    "$(q "SELECT count(*) FROM m WHERE ts >= '$D10' AND ts < '$D9';")"

# 4. WHERE on a non-time column also applied correctly
#    Each file has 2 rows with host='web-1' → 4 files × 2 = 8 rows.
check "non-time predicate" 8 \
    "$(q "SELECT count(*) FROM m WHERE host = 'web-1';")"

# 5. DISTINCT/aggregate across files
check "distinct hosts" "db-1
web-1
web-2" \
    "$(q "SELECT DISTINCT host FROM m ORDER BY host;")"

check "min/max value" "6.1|10.4" \
    "$(q 'SELECT min(value)||"|"||max(value) FROM m;')"

# 6. Range entirely outside any data → zero rows, no error
check "range outside data" 0 \
    "$(q "SELECT count(*) FROM m WHERE ts >= '1990-01-01' AND ts < '1990-01-10';")"

# 7. Verify the declared schema
check "declared schema" "ts|TEXT
host|TEXT
value|REAL" \
    "$(q 'SELECT name||"|"||type FROM pragma_table_info("m");')"

# 8. Intra-day bound returns the right slice within one day file
check "intra-day push-down" 2 \
    "$(q "SELECT count(*) FROM m WHERE ts >= '${D10}T11:00:00' AND ts < '${D10}T19:00:00';")"

# 9. Half-open across the missing day picks up adjacent files only
check "half-open across gap" 12 \
    "$(q "SELECT count(*) FROM m WHERE ts >= '$D9' AND ts < '$D5';")"

# 10. Repeat query exercises the LRU cache path (no error)
q "SELECT count(*) FROM m WHERE ts >= '$D11' AND ts < '$D5';" >/dev/null
check "cache hit on repeat" 16 \
    "$(q "SELECT count(*) FROM m WHERE ts >= '$D11' AND ts < '$D5';")"

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
