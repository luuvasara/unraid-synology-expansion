#!/bin/bash
# build.sh [version] - build synology-expansion-<version>-x86_64-1.txz on the build host (NEVER on a NAS).
# Written: 2026-10-25. Version defaults to the .plg's &version; (and must match it); the .plg's
# &md5; is updated from the built txz. Every kernel build in ../out/<kver>/synology_dx.ko goes into
# the package as kmod/<kver>/ after its vermagic, sha256 and the write-guard object check pass.
# Dir modes are forced (build umask is not trusted) and the archive root is verified.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
PLG=$HERE/../synology-expansion.plg
PLGVER=$(sed -n 's/^ *<!ENTITY version *"\([^"]*\)">.*/\1/p' "$PLG")
VER=${1:-$PLGVER}
[ "$VER" = "$PLGVER" ] || { echo "FATAL: version $VER != synology-expansion.plg &version; $PLGVER (bump the .plg first)"; exit 1; }
OUT=$HERE/../packages; mkdir -p "$OUT"
PKG=$OUT/synology-expansion-$VER-x86_64-1.txz
S=usr/local/emhttp/plugins/synology-expansion
"$HERE/tests/run-tests.sh"
php "$HERE/tests/status_test.php" "$HERE/src/usr/local/emhttp/plugins/synology-expansion" || { echo "FATAL: status page tests failed"; exit 1; }
ST=$(mktemp -d); trap 'rm -rf "$ST"' EXIT
cp -a "$HERE/src/." "$ST/"
n=0
for d in "$HERE"/../out/*/; do
  kv=$(basename "$d"); ko=$d/synology_dx.ko
  [ -f "$ko" ] || continue
  (cd "$d" && sha256sum -c --status synology_dx.ko.sha256) || { echo "FATAL: $ko does not match its sha256"; exit 1; }
  vm=$(modinfo -F vermagic "$ko")
  case "$vm" in "$kv "*) ;; *) echo "FATAL: $ko vermagic '$vm' is not for $kv"; exit 1;; esac
  "$HERE/../tests/check-readonly.sh" ko "$ko" >/dev/null || { echo "FATAL: write-guard object check failed for $ko"; exit 1; }
  mkdir -p "$ST/$S/kmod/$kv"
  cp "$ko" "$d/synology_dx.ko.sha256" "$ST/$S/kmod/$kv/"
  echo "kmod $kv: $(modinfo -F version "$ko") $(cut -c1-16 "$d/synology_dx.ko.sha256")..."
  n=$((n+1))
done
[ $n -gt 0 ] || { echo "FATAL: no kernel builds in ../out"; exit 1; }
find "$ST" -type d -exec chmod 755 {} +
find "$ST" -type f -exec chmod 644 {} +
chmod 755 "$ST"/$S/scripts/sdx-load
bash -n "$ST"/$S/scripts/sdx-load
if command -v php >/dev/null; then
  for f in "$ST"/$S/SynologyExpansion.page "$ST"/$S/include/*.php; do
    LINT=$(php -l "$f" 2>&1) || { echo "FATAL: php -l failed: $LINT"; exit 1; }
  done
else echo "warning: php not found, page not linted"; fi
rm -f "$PKG" "$PKG.md5"
tar -C "$ST" --owner=root --group=root --numeric-owner --sort=name -cJf "$PKG" .
ROOT=$(tar -tJvf "$PKG" | sed -n 1p)   # sed reads all input: head -1 + pipefail = SIGPIPE (exit 141)
echo "archive root entry: $ROOT"
case "$(awk '{print $1}' <<<"$ROOT")" in drwxr-xr-x) ;; *) echo "FATAL: archive root is not drwxr-xr-x"; rm -f "$PKG"; exit 1 ;; esac
tar -tJvf "$PKG" | awk '$1 ~ /^d/ && $1 != "drwxr-xr-x" {bad=1; print "FATAL dir mode: " $0} END{exit bad}'
tar -tJvf "$PKG" | awk '$2 != "0/0" {bad=1; print "FATAL owner: " $0} END{exit bad}'
(cd "$OUT" && md5sum "$(basename "$PKG")" >"$(basename "$PKG").md5")
MD5=$(cut -d' ' -f1 "$PKG.md5")
sed -i "s|^\( *<!ENTITY md5 *\)\"[^\"]*\">|\1\"$MD5\">|" "$PLG"
grep -q "<!ENTITY md5 *\"$MD5\">" "$PLG" || { echo "FATAL: could not write md5 into $PLG"; exit 1; }
python3 -c 'import sys,xml.dom.minidom as m; m.parse(sys.argv[1])' "$PLG" || { echo "FATAL: $PLG is not well-formed XML"; exit 1; }
# every INLINE script, with the entities expanded as the plugin manager does, must parse as bash
python3 - "$PLG" "$ST" <<'PY' || { echo "FATAL: an INLINE block of $PLG does not parse"; exit 1; }
import sys, subprocess, xml.dom.minidom as m
doc = m.parse(sys.argv[1])
for i, el in enumerate(doc.getElementsByTagName("INLINE")):
    code = "".join(n.data for n in el.childNodes if n.nodeType in (n.TEXT_NODE, n.CDATA_SECTION_NODE))
    r = subprocess.run(["bash", "-n"], input=code, text=True, capture_output=True)
    if r.returncode:
        print("INLINE", i, r.stderr); sys.exit(1)
    if "&name;" in code or "&version;" in code or "&pkg;" in code:
        print("INLINE", i, "has unexpanded entities"); sys.exit(1)
print("plg INLINE blocks: bash -n OK (%d)" % len(doc.getElementsByTagName("INLINE")))
PY
echo "built $PKG"; cat "$PKG.md5"; echo "synology-expansion.plg: &md5; = $MD5"
