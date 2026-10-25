#!/bin/bash
# Interoperability test with the official aoo library over UDP on localhost.
# usage: run-interop.sh <interop> <official_sender> <official_sink>
# The duration in seconds can be changed with AOO_INTEROP_SECONDS (default 3).
set -u
SECONDS_SEND=${AOO_INTEROP_SECONDS:-3}
SECONDS_RECV=$((SECONDS_SEND + 1))
INTEROP=$1
OFFICIAL_SENDER=$2
OFFICIAL_SINK=$3
rc=0

echo "=== official source -> arduino-aoo receiver"
AOO_MODE=receive AOO_PORT=19998 AOO_SECONDS=$SECONDS_RECV "$INTEROP" &
receiver=$!
sleep 0.5
"$OFFICIAL_SENDER" 19999 19998 1 $SECONDS_SEND || rc=1
wait $receiver || rc=1

echo "=== arduino-aoo sender -> official sink"
out=$(mktemp)
"$OFFICIAL_SINK" 19997 1 $SECONDS_RECV > "$out" &
sink=$!
sleep 0.5
AOO_MODE=send AOO_PORT=19996 AOO_REMOTE_PORT=19997 AOO_SECONDS=$SECONDS_SEND "$INTEROP" || rc=1
wait $sink || rc=1
cat "$out"
# a 441 Hz sine with amplitude 0.5: rms 0.354, 882 zero crossings/sec
python3 - "$out" "$SECONDS_SEND" <<'PY' || rc=1
import re, sys
text = open(sys.argv[1]).read()
m = re.search(r"starts=(\d+) stops=(\d+) samples=(\d+) rms=([\d.]+) crossings_per_sec=(\d+)", text)
ok = m and int(m[1]) == 1 and int(m[2]) == 1 and int(m[3]) > 44100 * (int(sys.argv[2]) - 1) * 0.97 \
    and abs(float(m[4]) - 0.354) < 0.03 and abs(int(m[5]) - 882) < 30
print("PASSED" if ok else "FAILED")
sys.exit(0 if ok else 1)
PY
rm -f "$out"
exit $rc
