#!/bin/sh
# Offline x86 host tests under ASan + UBSan; nothing touches a device.
#   test_core: settings, formats, rolling buffer, WAV, paths, click
#   integration: the real libmpc_skipback.so preloaded in front of a fake libasound, in a binary named MPC (and one that isn't)
set -eu
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
B=build/host
mkdir -p "$B/bin"
CF="-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -g -O1 -fno-omit-frame-pointer"
SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined"
export ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1

$CC $CF $SAN tests/test_core.c src/core.c -lm -o "$B/t_core"
"$B/t_core"

$CC $CF $SAN -fPIC -shared tests/fake_lib.c -o "$B/libfake.so"
$CC $CF $SAN -fPIC -shared -fvisibility=hidden -pthread src/addin.c src/core.c -ldl -lm -o "$B/libmpc_skipback.so"
$CC $CF $SAN tests/fake_mpc.c -L"$B" -lfake -Wl,-rpath,"$PWD/$B" -o "$B/bin/MPC"
cp "$B/bin/MPC" "$B/bin/not-mpc"

ASANLIB=$($CC -print-file-name=libasan.so)
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
run() { # run <binary> <mode> <workdir>
  MPC_SKIPBACK_CONF="$3/skipback.conf" MPC_SKIPBACK_LEARNED="$3/button.learned" LD_PRELOAD="$ASANLIB:$PWD/$B/libmpc_skipback.so" "$1" "$3" "$2"
}
mk() { mkdir -p "$1"; cat > "$1/skipback.conf" <<CONF
window_sec=5
tap_card=0
output_dir=$1/out
trigger=$1/trigger
done=$1/done
log=$1/skipback.log
poll_ms=20
${2:-}
CONF
}
mk "$tmp/a"; run "$B/bin/MPC" mpc "$tmp/a" || { cat "$tmp/a/skipback.log"; exit 1; }
mk "$tmp/b" click=1; touch "$tmp/b/click-on"; run "$B/bin/MPC" mpc "$tmp/b" || { cat "$tmp/b/skipback.log"; exit 1; }
mk "$tmp/f" "led_ms=30
led_fast_ms=20";  touch "$tmp/f/btn-on"; run "$B/bin/MPC" mpc "$tmp/f" || { cat "$tmp/f/skipback.log"; exit 1; }
mk "$tmp/g" "led_ms=30
led_fast_ms=20"; touch "$tmp/g/mpc-on"; run "$B/bin/MPC" mpc "$tmp/g" || { cat "$tmp/g/skipback.log"; exit 1; }
mk "$tmp/h" "button=learn
led_ms=30
led_fast_ms=20"; touch "$tmp/h/learn-on"; run "$B/bin/MPC" mpc "$tmp/h" || { cat "$tmp/h/skipback.log"; exit 1; }
# a second start with button=learn uses what was learned and does not learn again
rm -f "$tmp/h/learn-on"; rm -f "$tmp/h/skipback.log"; run "$B/bin/MPC" mpc "$tmp/h" || { cat "$tmp/h/skipback.log"; exit 1; }
grep -q "button 60, as learned earlier" "$tmp/h/skipback.log" && echo "ok   learned button kept across starts" || { echo "FAIL: learned button not kept"; cat "$tmp/h/skipback.log"; exit 1; }
mk "$tmp/c"; run "$B/bin/not-mpc" inert "$tmp/c"
mk "$tmp/d" enabled=0; run "$B/bin/MPC" inert "$tmp/d"
mk "$tmp/e"; run /bin/true x "$tmp/e" 2>/dev/null || true
echo "--- addin log"; cat "$tmp/a/skipback.log"
# Installed layout: no MPC_SKIPBACK_CONF; skipback.conf and the log sit next to the .so.
mkdir -p "$tmp/inst"; cp "$B/libmpc_skipback.so" "$tmp/inst/"
sed "s#$tmp/a#$tmp/inst#g; /^log=/d" "$tmp/a/skipback.conf" > "$tmp/inst/skipback.conf"
LD_PRELOAD="$ASANLIB:$tmp/inst/libmpc_skipback.so" "$B/bin/MPC" "$tmp/inst" mpc || { cat "$tmp/inst/skipback.log"; exit 1; }
grep -q "active: codec card" "$tmp/inst/skipback.log" && echo "ok   settings and log next to the .so" || { echo "FAIL: no log next to the .so"; exit 1; }
echo "all host tests passed"
