#!/bin/sh
# Build libmpc_skipback.so for MPC OS (armv7 hard-float; glibc <= 2.31, the oldest supported MPC OS) in a Debian bullseye
# container and check the result: no symbol newer than GLIBC_2.31, no DT_NEEDED on libasound, only the expected exports.
# Needs Docker with arm/v7 emulation. Output: build/armhf/, and build/package/ (the .so, the default skipback.conf and
# addin.manifest) for the release.
set -eu
cd "$(dirname "$0")/.."
IMAGE=${IMAGE:-arm32v7/gcc:11-bullseye}
OUT=build/armhf
mkdir -p "$OUT"
docker run --rm --platform linux/arm/v7 -u "$(id -u):$(id -g)" -v "$PWD:/w" -w /w "$IMAGE" sh -c "
  set -e
  gcc -std=c11 -D_GNU_SOURCE -O2 -g0 -fPIC -shared -fvisibility=hidden \
      -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard \
      -Wall -Wextra -Werror -Wl,--as-needed -Wl,-z,relro,-z,now -Wl,--no-undefined \
      src/addin.c src/core.c -ldl -lpthread -lm -o $OUT/libmpc_skipback.so
  strip --strip-unneeded $OUT/libmpc_skipback.so
  readelf -d $OUT/libmpc_skipback.so | grep NEEDED > $OUT/needed.txt
  objdump -T $OUT/libmpc_skipback.so | grep -o 'GLIBC_[0-9.]*' | sort -uV > $OUT/glibc.txt
  objdump -T $OUT/libmpc_skipback.so | awk '\$4 == \".text\" {print \$NF}' | sort > $OUT/exports.txt
"
echo "NEEDED:"; sed 's/^/  /' "$OUT/needed.txt"
if grep -q libasound "$OUT/needed.txt"; then echo "FAIL: links libasound" >&2; exit 1; fi
max=$(tail -n1 "$OUT/glibc.txt")
echo "highest symbol version: $max"
if [ "$(printf '%s\nGLIBC_2.31\n' "$max" | sort -V | tail -n1)" != GLIBC_2.31 ]; then
  echo "FAIL: needs $max; the oldest supported MPC OS has glibc 2.31" >&2; exit 1
fi
echo "exports:"; sed 's/^/  /' "$OUT/exports.txt"
unexpected=$(grep -Ev '^(snd_(pcm_(open|close|hw_params|writei|writen)|rawmidi_(open|close|read|write)))$' "$OUT/exports.txt" || true)
if [ -n "$unexpected" ]; then echo "FAIL: unexpected exports: $unexpected" >&2; exit 1; fi
P=build/package
rm -rf "$P"; mkdir -p "$P"
cp "$OUT/libmpc_skipback.so" addin.manifest "$P/"
cp etc/skipback.conf.example "$P/skipback.conf"
echo "package: $P/"; ls "$P"
