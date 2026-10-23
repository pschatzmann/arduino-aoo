#!/bin/bash
# Interoperability test with the official aoo library over UDP on localhost.
# usage: run-interop.sh <interop> <official_sender> <official_sink>
set -u
INTEROP=$1
OFFICIAL_SENDER=$2
OFFICIAL_SINK=$3
rc=0

echo "=== official source -> arduino-aoo receiver"
AOO_MODE=receive AOO_PORT=19998 AOO_SECONDS=4 "$INTEROP" &
receiver=$!
sleep 0.5
"$OFFICIAL_SENDER" 19999 19998 1 3 || rc=1
wait $receiver || rc=1

echo "=== arduino-aoo sender -> official sink"
out=$(mktemp)
"$OFFICIAL_SINK" 19997 1 4 > "$out" &
sink=$!
sleep 0.5
AOO_MODE=send AOO_PORT=19996 AOO_REMOTE_PORT=19997 AOO_SECONDS=3 "$INTEROP" || rc=1
wait $sink || rc=1
cat "$out"
# a 441 Hz sine with amplitude 0.5: rms 0.354, 882 zero crossings/sec
python3 - "$out" <<'PY' || rc=1
import re, sys
text = open(sys.argv[1]).read()
m = re.search(r"starts=(\d+) stops=(\d+) samples=(\d+) rms=([\d.]+) crossings_per_sec=(\d+)", text)
ok = m and int(m[1]) == 1 and int(m[2]) == 1 and int(m[3]) > 44100 * 2 \
    and abs(float(m[4]) - 0.354) < 0.03 and abs(int(m[5]) - 882) < 30
print("PASSED" if ok else "FAILED")
sys.exit(0 if ok else 1)
PY
rm -f "$out"
exit $rc
