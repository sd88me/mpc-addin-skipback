#!/bin/sh
# Run ON the device after install.sh, with MPC playing something: triggers a save the way a remap would and checks the result.
#   sh device_test.sh [settle-seconds]
T=/tmp/mpc-addin-skipback.trigger D=/tmp/mpc-addin-skipback.done
LOG=/data/mpc-addins/skipback/skipback.log
echo "--- log so far"; tail -n 8 "$LOG" 2>/dev/null || echo "no log: the addin did not load"
sleep "${1:-5}"
rm -f "$D"; touch "$T"
i=0; while [ ! -f "$D" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
echo "--- trigger consumed: $([ -f "$T" ] && echo NO || echo yes), done after ~$((i * 100)) ms"
cat "$D" 2>/dev/null || { echo "no done file"; exit 1; }
f=$(sed -n 's/^ok //p' "$D")
[ -n "$f" ] && ls -l "$f" && echo "expect ~$((30 * 44100 * 6 + 44)) bytes for a full 30 s window at 44.1 kHz"
echo "--- log"; tail -n 4 "$LOG"
