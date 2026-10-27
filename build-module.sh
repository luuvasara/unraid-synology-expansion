#!/usr/bin/env bash
# Build synology_dx.ko against the prepared 6.18.58-Unraid tree (run build-tree.sh first).
# Runs tests/check-readonly.sh before and after. Output: out/<kernelrelease>/synology_dx.ko
# Written: 2026-10-25.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
B=$HERE/build
TREE=$B/linux-6.18.58
IMG=synology-dx-build:gcc16.2.0
KR=6.18.58-Unraid
WANT_VERMAGIC="6.18.58-Unraid SMP preempt mod_unload "

"$HERE/tests/check-readonly.sh" src
rm -rf "$B/mod" && mkdir -p "$B/mod" && cp "$HERE"/src/{synology_dx.c,sdx_gpio.h,Kbuild} "$B/mod/"
gcc -Wall -Wextra -o "$B/test-encoding" "$HERE/tests/test-encoding.c" && "$B/test-encoding"
docker run --rm --user "$(id -u):$(id -g)" -v "$B:$B" -w "$B/mod" "$IMG" \
  make -C "$TREE" M="$B/mod" W=1 modules 2>&1 | tee "$B/mod/build.log"
grep -E 'warning:|error:' "$B/mod/build.log" && { echo "FAIL: compiler/modpost warnings"; exit 1; } || true

KO=$B/mod/synology_dx.ko
VM=$(modinfo -F vermagic "$KO")
[ "$VM" = "$WANT_VERMAGIC" ] || { echo "FAIL: vermagic '$VM' != '$WANT_VERMAGIC'"; exit 1; }
"$HERE/tests/check-readonly.sh" ko "$KO"
mkdir -p "$HERE/out/$KR"
cp "$KO" "$HERE/out/$KR/synology_dx.ko"
(cd "$HERE/out/$KR" && sha256sum synology_dx.ko > synology_dx.ko.sha256 && cat synology_dx.ko.sha256)
modinfo "$HERE/out/$KR/synology_dx.ko"
