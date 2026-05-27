#!/usr/bin/env sh
# Generate four daily metric partition files at offsets -10, -9, -7, -6
# (the middle day -8 is intentionally skipped so smoke tests exercise the
# missing-file path).
set -eu

DATA_DIR="${1:-test/data}"
mkdir -p "$DATA_DIR"
rm -f "$DATA_DIR"/metrics_*.sqlite

day_for_offset() {
    off=$1
    case "$(uname -s)" in
        Darwin) date -v "-${off}d" +%Y-%m-%d ;;
        *)      date -d "${off} days ago" +%Y-%m-%d ;;
    esac
}

for off in 10 9 7 6; do
    DAY=$(day_for_offset "$off")
    F="$DATA_DIR/metrics_${DAY}.sqlite"
    sqlite3 "$F" <<EOF
CREATE TABLE metrics(ts TEXT, host TEXT, value REAL);
CREATE INDEX metrics_ts ON metrics(ts);
INSERT INTO metrics VALUES
  ('${DAY}T00:00:00', 'web-1', ${off}.1),
  ('${DAY}T06:00:00', 'web-2', ${off}.2),
  ('${DAY}T12:00:00', 'db-1',  ${off}.3),
  ('${DAY}T18:00:00', 'web-1', ${off}.4);
EOF
    echo "  wrote $F"
done

echo "fixtures: 4 files in $DATA_DIR (day -8 deliberately missing)"
