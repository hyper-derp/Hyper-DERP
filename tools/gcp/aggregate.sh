#!/bin/bash
# Aggregate per-instance results into single data points.
#
# Each rate point has 4 client JSON files (c0..c3). This script
# sums sent/recv counts and computes aggregate throughput and
# loss for each run.
#
# Usage: ./tools/gcp/aggregate.sh [results_dir]
#
# Reads:  {hd,ts}_{rate}_r{NN}_c{0..3}.json
# Writes: agg_{hd,ts}_{rate}_r{NN}.json

set -euo pipefail

RESULTS_DIR="${1:-.}"

if [ ! -d "$RESULTS_DIR" ]; then
  echo "Error: $RESULTS_DIR is not a directory"
  exit 1
fi

DURATION=15
MSG_SIZE=1400
COUNT=0

for f in "$RESULTS_DIR"/*_r*_c0.json; do
  [ -f "$f" ] || continue

  base=$(basename "$f" | sed 's/_c0\.json//')
  dir=$(dirname "$f")

  # Verify all 4 client files exist.
  ok=true
  for i in 0 1 2 3; do
    [ -f "$dir/${base}_c${i}.json" ] || ok=false
  done
  if ! $ok; then
    echo "WARN: incomplete set for $base, skipping"
    continue
  fi

  python3 -c "
import json, sys

base = '$base'
d = '$dir'
files = [f'{d}/{base}_c{i}.json' for i in range(4)]

total_sent = 0
total_recv = 0
total_errors = 0
connect_times = []
instance_data = []

for path in files:
    try:
        with open(path) as fh:
            data = json.load(fh)
        total_sent += data.get('messages_sent', 0)
        total_recv += data.get('messages_recv', 0)
        total_errors += data.get('send_errors', 0)
        ct = data.get('connect_time_ms', 0)
        if ct > 0:
            connect_times.append(ct)
        instance_data.append({
            'messages_sent': data.get('messages_sent', 0),
            'messages_recv': data.get('messages_recv', 0),
            'throughput_mbps': data.get('throughput_mbps', 0),
        })
    except (json.JSONDecodeError, FileNotFoundError) as e:
        print(f'WARN: {path}: {e}', file=sys.stderr)

if total_sent == 0:
    sys.exit(0)

loss = 100.0 * (1.0 - total_recv / max(total_sent, 1))
tp = total_recv * $MSG_SIZE * 8 / 1e6 / $DURATION

# Determine relay type from prefix.
relay = 'hyper-derp-hd' if base.startswith('hd') else 'hyper-derp'

# Extract rate from filename: {hd,ts}_{rate}_r{NN}
parts = base.split('_')
rate = int(parts[1])
run_num = parts[2]

agg = {
    'aggregated': True,
    'instance_count': 4,
    'run_id': base,
    'run_number': run_num,
    'relay': relay,
    'rate_mbps': rate,
    'duration_sec': $DURATION,
    'message_size': $MSG_SIZE,
    'messages_sent': total_sent,
    'messages_recv': total_recv,
    'message_loss_pct': round(loss, 4),
    'send_errors': total_errors,
    'throughput_mbps': round(tp, 1),
    'avg_connect_time_ms': round(
        sum(connect_times) / len(connect_times), 1
    ) if connect_times else 0,
    'per_instance': instance_data,
}

out_path = f'{d}/agg_{base}.json'
with open(out_path, 'w') as fh:
    json.dump(agg, fh, indent=2)
    fh.write('\n')
"
  COUNT=$((COUNT + 1))
done

echo "Aggregated $COUNT run(s)."
echo "Files:"
find "$RESULTS_DIR" -name "agg_*.json" 2>/dev/null | wc -l
echo " aggregated JSON files"
