#!/usr/bin/env bash
# Prepare the exact 6.18.58-Unraid kernel build tree for out-of-tree modules (on the build host, never
# on the NAS). Written: 2026-10-25.
#
# Inputs (all under synology-dx/build/, gitignored):
#   dl/linux-6.18.58.tar.xz        kernel.org tarball (sha256 + gregkh signature checked by hand)
#   recipe/src/linux-6.18.58-Unraid/  Limetech recipe (config, patches, drivers/md, System.map),
#                                  unsquashfs'd from the 7.3.4 bzmodules; identical (sha256) to
#                                  /usr/src/linux-6.18.58-Unraid on the box.
# Output: build/linux-6.18.58/ with .config, modules_prepare done, and (unless NO_VMLINUX=1)
#         vmlinux + Module.symvers, whose System.map is compared to Limetech's.
#
# Compiler: docker gcc:16.2.0 (the box kernel says "gcc (GCC) 16.2.0, GNU ld (GNU Binutils) 2.47").
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
B=$HERE/build
KV=6.18.58
RECIPE=$B/recipe/src/linux-$KV-Unraid
TREE=$B/linux-$KV
IMG=synology-dx-build:gcc16.2.0

[ -f "$B/dl/linux-$KV.tar.xz" ] || { echo "missing $B/dl/linux-$KV.tar.xz"; exit 1; }
[ -f "$RECIPE/config" ] || { echo "missing recipe $RECIPE"; exit 1; }

docker build -q -t "$IMG" - >/dev/null <<'EOF'
FROM gcc:16.2.0
RUN apt-get -qq update && DEBIAN_FRONTEND=noninteractive apt-get -qq install -y --no-install-recommends \
    flex bison bc libelf-dev libssl-dev cpio kmod xz-utils rsync dwarves patch python3 >/dev/null \
    && rm -rf /var/lib/apt/lists/*
EOF

if [ ! -f "$TREE/.prepared" ]; then
  rm -rf "$TREE"
  tar -C "$B" -xf "$B/dl/linux-$KV.tar.xz"
  cd "$TREE"
  for p in "$RECIPE"/*.patch; do
    echo "== $(basename "$p")"
    patch -p1 --forward --no-backup-if-mismatch < "$p" || echo "!! $(basename "$p") did not apply cleanly (see above)"
  done 2>&1 | tee "$B/patches.log"
  cp "$RECIPE"/drivers/md/* drivers/md/
  cp "$RECIPE/config" .config
  touch .prepared
fi

cd "$TREE"
run() { docker run --rm --user "$(id -u):$(id -g)" -v "$B:$B" -w "$TREE" "$IMG" "$@"; }
run gcc --version | head -1
run make -s olddefconfig
# Only the toolchain-probe lines may differ from Limetech's config.
diff <(grep -v '^#' "$RECIPE/config" | sort) <(grep -v '^#' "$TREE/.config" | sort) > "$B/config.diff" || true
echo "config diff vs Limetech (empty = identical):"; cat "$B/config.diff"
run make -s -j"$(nproc)" modules_prepare
echo "kernelrelease: $(run make -s kernelrelease)"

if [ "${NO_VMLINUX:-0}" != 1 ]; then
  # vmlinux gives Module.symvers (export list for modpost) and a System.map to compare with the
  # one Limetech ships: identical addresses = identical code layout = our tree is their kernel.
  run make -s -j"$(nproc)" vmlinux
  run make -s -j"$(nproc)" modules_prepare
  # Module.symvers for vmlinux only (no modules built): vmlinux.symvers is what modpost writes.
  [ -f Module.symvers ] || cp vmlinux.symvers Module.symvers 2>/dev/null || true
  if cmp -s System.map "$RECIPE/System.map"; then echo "System.map: IDENTICAL to Limetech's";
  else echo "System.map: DIFFERS"; diff "$RECIPE/System.map" System.map | head -20; fi
fi
