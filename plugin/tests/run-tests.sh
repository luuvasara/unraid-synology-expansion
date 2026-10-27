#!/bin/bash
# Loader tests for the synology-expansion plugin: scripts/sdx-load against a fake sysfs, fake
# /proc/kallsyms and fake insmod/rmmod/logger/notify (real modinfo on the real module).
# Written: 2026-10-25. Usage: tests/run-tests.sh   (run by build.sh)
# Ends with a mutation check: loaders with the vermagic / sha256 / address guard removed must fail.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
LOADER0=$HERE/../src/usr/local/emhttp/plugins/synology-expansion/scripts/sdx-load
OUTKO=$(cd "$HERE/../.." && pwd)/out/6.18.58-Unraid
W=$(cd "$HERE/../.." && pwd)/build/plugin-tests
KV=6.18.58-Unraid
ADDR=ffffffff81ba8420
fails=0

fixture() {  # fresh fake system in $W
  rm -rf "$W"; mkdir -p "$W"/{bin,run,sys/class/scsi_host,sys/class/ata_link,proc,kmod/$KV}
  local h a
  mk_host() {  # hostno, devices path, proc_name
    mkdir -p "$W/sys/devices/$2/scsi_host/host$1"
    echo "$3" > "$W/sys/devices/$2/scsi_host/host$1/proc_name"
    ln -s "../../devices/$2/scsi_host/host$1" "$W/sys/class/scsi_host/host$1"
  }
  mk_host 0 pci0000:00/0000:00:13.0/ata1/host0 ahci
  mkdir -p "$W/sys/class/ata_link/link1"                     # no PMP on ata1
  for h in 18 19 20 21; do a=$((h+1))
    mk_host $h pci0000:00/0000:00:03.0/0000:05:00.0/ata$a/host$h ahci
    for l in 0 1 2 3 4 5; do mkdir -p "$W/sys/class/ata_link/link$a.$l"; done
  done
  mk_host 22 pci0000:00/0000:00:16.0/usb1/1-1/1-1:1.0/host22 usb-storage
  printf 'ffffffff81ba8400 t ata_eh_something\n%s T ata_exec_internal\nffffffff81bc3b10 T ahci_error_handler\n' "$ADDR" > "$W/proc/kallsyms"
  cp "$OUTKO/synology_dx.ko" "$OUTKO/synology_dx.ko.sha256" "$W/kmod/$KV/"
  cat > "$W/bin/insmod" <<'EOF'
#!/bin/bash
echo "$*" >> "$FAKE/insmod.calls"; [ "${FAKE_INSMOD_RC:-0}" = 0 ] || { echo "insmod: ERROR: could not insert module: Operation canceled"; exit 1; }
mkdir -p "$FAKE/sys/module/synology_dx" "$FAKE/sys/kernel/synology_dx/unit"
echo "${FAKE_SRCV:-X}" > "$FAKE/sys/module/synology_dx/srcversion"
printf 'host18 emid 0 unique 0x13\nmaster host18\nfan_level low\nfan_fail 0\n' > "$FAKE/sys/kernel/synology_dx/unit/summary"
echo 1 > "$FAKE/sys/kernel/synology_dx/unit/dx_leds_enabled"     # measured at power-on: all four led_mask = 1
case " $* " in *" ds_leds=1 "*)
  for c in 0000:03:00.0 0000:05:00.0 0000:06:00.0; do mkdir -p "$FAKE/sys/kernel/synology_dx/ds/$c"; echo 0 > "$FAKE/sys/kernel/synology_dx/ds/$c/leds_enabled"; done
  echo "${FAKE_PCH:-1}" > "$FAKE/sys/kernel/synology_dx/ds/pch_led_enable"
  printf 'state %s\ngpiobase 0x500\n' "${FAKE_PCH_STATE:-ok}" > "$FAKE/sys/kernel/synology_dx/ds/pch" ;;
esac
EOF
  cat > "$W/bin/rmmod" <<'EOF'
#!/bin/bash
echo "$*" >> "$FAKE/rmmod.calls"; rm -rf "$FAKE/sys/module/synology_dx" "$FAKE/sys/kernel/synology_dx"
EOF
  printf '#!/bin/bash\necho "$*" >> "$FAKE/logger.calls"\n' > "$W/bin/logger"
  printf '#!/bin/bash\necho "$*" >> "$FAKE/notify.calls"\n' > "$W/bin/notify"
  chmod +x "$W"/bin/*
}

run() {  # args -> runs the loader with the fake system; prints its stdout; sets RC
  FAKE=$W SDX_SYS=$W/sys SDX_PROC=$W/proc SDX_KV=${KVRUN:-$KV} SDX_KMOD=$W/kmod SDX_RUN=$W/run \
  SDX_INSMOD=$W/bin/insmod SDX_RMMOD=$W/bin/rmmod SDX_LOGGER=$W/bin/logger SDX_NOTIFY=$W/bin/notify \
  SDX_MODINFO=$(command -v modinfo) SDX_OLD=$W/oldplugin SDX_CFG=$W/cfg FAKE_INSMOD_RC=${FAKE_INSMOD_RC:-0} FAKE_SRCV=${FAKE_SRCV:-X} FAKE_PCH=${FAKE_PCH:-1} FAKE_PCH_STATE=${FAKE_PCH_STATE:-ok} \
    bash "$LOADER" "$@"; RC=$?
}
ok()  { echo "ok: $1"; }
bad() { echo "FAIL: $1"; fails=$((fails+1)); }
calls() { cat "$W/$1.calls" 2>/dev/null; }
expect_no_insmod() { [ -z "$(calls insmod)" ] && [ "$RC" = 0 ] && grep -q "$2" "$W/notify.calls" 2>/dev/null && ok "$1" || bad "$1 (rc=$RC insmod='$(calls insmod)' notify='$(calls notify)')"; }

suite() {
  local SRCV
  SRCV=$(modinfo -F srcversion "$OUTKO/synology_dx.ko")
  # 1 happy path
  fixture; run load
  [ "$(calls insmod)" = "$W/kmod/$KV/synology_dx.ko hosts=18,19,20,21 exec_internal=0x$ADDR ds_leds=1" ] && [ "$RC" = 0 ] && [ -z "$(calls notify)" ] \
    && grep -q 'master host18' "$W/run/status" && ok "loads with hosts=18,19,20,21 exec_internal=0x$ADDR ds_leds=1" \
    || bad "happy path: insmod='$(calls insmod)' rc=$RC notify='$(calls notify)' status='$(cat "$W/run/status" 2>/dev/null)'"
  # 1b what the page shows: the checks file
  C=$W/run/checks
  for want in "build	ok	kmod/$KV/synology_dx.ko (version 0.6.1)" "vermagic	ok	$KV SMP preempt mod_unload " "hosts	ok	18,19,20,21" "kallsyms	ok	0x$ADDR" "insmod	ok	loaded, master host18" "result	loaded	"; do
    grep -qF "$want" "$C" 2>/dev/null || { bad "checks file lacks '$want'"; break; }
  done
  grep -q '^sha256	ok	' "$C" && ok "checks file records every verified step" || bad "checks: no sha256 line"
  # 1c old synology-dx plugin still present: warned, still loaded once
  fixture; mkdir -p "$W/oldplugin"; run load
  [ "$(calls insmod | wc -l)" = 1 ] && grep -q 'old synology-dx plugin' "$W/notify.calls" && grep -q '^oldplugin	fail' "$W/run/checks" && ok "old synology-dx plugin present -> warned, loaded once" || bad "old plugin: insmod='$(calls insmod)' notify='$(calls notify)'"
  # 1d (2026.10.26a) bay LEDs on by default: every 9215 leds_enabled=1, DX dx_leds_enabled=1, check lines before result
  fixture; run load; D=$W/sys/kernel/synology_dx
  [ "$(cat "$D"/ds/0000:0[356]:00.0/leds_enabled | tr -d '\n')" = 111 ] && [ "$(cat "$D/unit/dx_leds_enabled")" = 1 ] \
    && grep -qF "ds_leds	ok	on: leds_enabled=1 on 3 controllers" "$W/run/checks" && grep -qF "pch	ok	GPIO 49 high (orange enable), GPIOBASE 0x500" "$W/run/checks" \
    && grep -qF "dx_leds	ok	on: dx_leds_enabled=1" "$W/run/checks" && [ "$(tail -1 "$W/run/checks" | cut -f1)" = result ] \
    && ok "no settings file -> DS and DX bay LEDs on, ds_leds/pch/dx_leds recorded" || bad "LEDs default: $(cat "$W/run/checks")"
  # 1e DS_LEDS="0" and DX_LEDS="0": nothing enabled, DX set to 0, pch skipped
  fixture; printf 'DS_LEDS="0"\nDX_LEDS="0"\n' > "$W/cfg"; run load
  [ "$(cat "$D"/ds/0000:0[356]:00.0/leds_enabled | tr -d '\n')" = 000 ] && [ "$(cat "$D/unit/dx_leds_enabled")" = 0 ] \
    && grep -q $'^pch\tskip\t' "$W/run/checks" && grep -qF 'ds_leds	ok	off (DS_LEDS="0")' "$W/run/checks" \
    && ok "DS_LEDS=0, DX_LEDS=0 -> LEDs left off, pch skipped" || bad "LEDs off: $(cat "$W/run/checks")"
  # 1f 'leds' after switching DS back on: lines rewritten in place, result kept, nothing reloaded
  printf 'DX_LEDS="0"\n' > "$W/cfg"; : > "$W/insmod.calls"; run leds
  [ "$(cat "$D"/ds/0000:0[356]:00.0/leds_enabled | tr -d '\n')" = 111 ] && [ -z "$(calls insmod)" ] \
    && [ "$(grep -c $'^ds_leds\t' "$W/run/checks")" = 1 ] && grep -qF "ds_leds	ok	on:" "$W/run/checks" \
    && [ "$(tail -1 "$W/run/checks" | cut -f1)" = result ] && grep -q $'^kallsyms\tok' "$W/run/checks" \
    && ok "leds -> settings applied to the running driver, check lines replaced" || bad "leds: $(cat "$W/run/checks")"
  rm -f "$W/cfg"
  # 1g the driver could not take GPIO 49: recorded as a failed pch check with its reason
  fixture; FAKE_PCH=0 FAKE_PCH_STATE="GPIO I/O ports owned by another driver: not touched" run load
  grep -qF "pch	fail	pch_led_enable=0: GPIO I/O ports owned by another driver" "$W/run/checks" && ok "pch not available -> failed pch check with the driver's reason" || bad "pch fail: $(cat "$W/run/checks")"
  # 2 no build for the running kernel
  fixture; KVRUN=6.18.59-Unraid run load; expect_no_insmod "no build for kernel 6.18.59-Unraid -> not loaded, notified" "no synology_dx build for kernel 6.18.59-Unraid"
  grep -q '^result	no_build	' "$W/run/checks" && grep -q '^build	fail	no build for 6.18.59-Unraid' "$W/run/checks" && ok "no build recorded in checks" || bad "no build: checks='$(cat "$W/run/checks" 2>/dev/null)'"
  # 3 vermagic mismatch (a 6.18.58 module filed under another kernel, sha256 valid)
  fixture; mkdir -p "$W/kmod/6.18.99-Unraid"; cp "$W/kmod/$KV/"* "$W/kmod/6.18.99-Unraid/"
  KVRUN=6.18.99-Unraid run load; expect_no_insmod "vermagic 6.18.58 vs running 6.18.99 -> refused" "vermagic"
  # 4 sha256 mismatch
  fixture; printf 'x' >> "$W/kmod/$KV/synology_dx.ko"; run load; expect_no_insmod "corrupt .ko (sha256) -> refused" "sha256"
  # 5 kallsyms hidden (kptr_restrict: zero addresses)
  fixture; sed -i "s/^$ADDR/0000000000000000/" "$W/proc/kallsyms"; run load; expect_no_insmod "kallsyms zeroed -> refused" "ata_exec_internal address"
  # 6 kallsyms without the symbol
  fixture; sed -i '/ata_exec_internal/d' "$W/proc/kallsyms"; run load; expect_no_insmod "symbol missing -> refused" "ata_exec_internal address"
  # 7 no DX attached: logged, no notification, no insmod
  fixture; rm -rf "$W"/sys/class/ata_link/link{19,20,21,22}.*; run load
  [ -z "$(calls insmod)" ] && [ "$RC" = 0 ] && [ -z "$(calls notify)" ] && grep -q 'no DX1215' "$W/logger.calls" && ok "no PMP links -> not loaded, logged only" || bad "no DX: insmod='$(calls insmod)' notify='$(calls notify)'"
  grep -q '^result	no_dx	' "$W/run/checks" && ok "no DX recorded in checks" || bad "no DX checks"
  # 8 insmod fails (module guard refused): notified, exit 0
  fixture; FAKE_INSMOD_RC=1 run load; [ -n "$(calls insmod)" ] && [ "$RC" = 0 ] && grep -q 'insmod synology_dx hosts=18,19,20,21 failed' "$W/notify.calls" && ok "insmod failure -> notified, exit 0" || bad "insmod failure: rc=$RC notify='$(calls notify)'"
  # 9 already loaded with the same srcversion: nothing done
  fixture; mkdir -p "$W/sys/module/synology_dx"; echo "$SRCV" > "$W/sys/module/synology_dx/srcversion"; run load
  [ -z "$(calls insmod)" ] && [ -z "$(calls rmmod)" ] && ok "same srcversion loaded -> no reload" || bad "same srcversion: insmod='$(calls insmod)' rmmod='$(calls rmmod)'"
  # 10 older module loaded: rmmod then insmod
  fixture; mkdir -p "$W/sys/module/synology_dx"; echo OLDSRCVERSION > "$W/sys/module/synology_dx/srcversion"; run load
  [ "$(calls rmmod)" = synology_dx ] && [ -n "$(calls insmod)" ] && ok "other srcversion loaded -> rmmod + insmod" || bad "upgrade: rmmod='$(calls rmmod)' insmod='$(calls insmod)'"
  # 11 unload
  fixture; run load; run unload; [ "$(calls rmmod)" = synology_dx ] && [ "$RC" = 0 ] && [ ! -d "$W/sys/module/synology_dx" ] && ok "unload -> rmmod synology_dx" || bad "unload: rmmod='$(calls rmmod)' rc=$RC"
  # 12 unload when not loaded: no rmmod, rc 0
  fixture; run unload; [ -z "$(calls rmmod)" ] && [ "$RC" = 0 ] && ok "unload when not loaded -> nothing" || bad "unload not loaded"
  # 13 the notify script missing must not break the loader
  fixture; rm -f "$W/bin/notify"; KVRUN=6.18.59-Unraid run load; [ "$RC" = 0 ] && grep -q 'no synology_dx build' "$W/logger.calls" && ok "notify missing -> still logged, exit 0" || bad "notify missing rc=$RC"
}

LOADER=$LOADER0; suite
CLEAN_FAILS=$fails

# mutation check: each guard removed must make the suite fail
MW=$W-mutants; rm -rf "$MW"; mkdir -p "$MW"
mutant() {  # name, sed expression
  sed -E "$2" "$LOADER0" > "$MW/$1"
  cmp -s "$LOADER0" "$MW/$1" && { echo "FAIL: mutant $1 did not change the loader"; fails=$((fails+1)); return; }
  bash -n "$MW/$1" 2>/dev/null || { echo "FAIL: mutant $1 does not parse (would be caught for the wrong reason)"; fails=$((fails+1)); return; }
  local before=$fails; LOADER=$MW/$1; suite >/dev/null; local got=$((fails-before)); fails=$before
  [ $got -gt 0 ] && echo "ok: mutant $1 caught ($got failing cases)" || { echo "FAIL: mutant $1 not caught"; fails=$((fails+1)); }
}
mutant no_vermagic 's/^  case "\$vm" in "\$KV "\*\) ;; \*\)$/  case "$vm" in *) ;; never-matches)/'
mutant no_sha256   's/! \(cd "\$KMOD\/\$KV" && sha256sum -c --status synology_dx.ko.sha256\)/false/'
mutant no_addr     's/^  if \[ "\$\(printf .%s\\n. "\$n" \| grep -c \.\)" != 1 \].*$/  if false; then/'
mutant no_pmp      's/^    ls -d "\$SYS\/class\/ata_link\/link\$\{a#ata\}\."\* >\/dev\/null 2>&1 \|\| continue$/    :/'
mutant leds_never 's/^  if cfg_on DS_LEDS; then want=1; else want=0; fi$/  want=0/'
mutant pch_unchecked 's/^  elif \[ \$want = 1 \] && \[ "\$pch" = 1 \]; then/  elif [ $want = 1 ]; then/'
mutant exit_rc     's/^  load\)   do_load; exit 0 ;;$/  load)   do_load; exit 1 ;;/'
LOADER=$LOADER0

# status page: the PHP tests must pass on the real library and fail on broken ones
PLUGDIR=$HERE/../src/usr/local/emhttp/plugins/synology-expansion
if command -v php >/dev/null; then
  php "$HERE/status_test.php" "$PLUGDIR" >/dev/null && echo "ok: status page tests pass" || { echo "FAIL: status page tests"; fails=$((fails+1)); }
  pmut() {  # name, sed expression on include/sdx-status.php
    local d=$MW/php-$1; rm -rf "$d"; mkdir -p "$d/include"
    sed -E "$2" "$PLUGDIR/include/sdx-status.php" > "$d/include/sdx-status.php"
    cmp -s "$PLUGDIR/include/sdx-status.php" "$d/include/sdx-status.php" && { echo "FAIL: php mutant $1 did not change the library"; fails=$((fails+1)); return; }
    php "$HERE/status_test.php" "$d" >/dev/null 2>&1 && { echo "FAIL: php mutant $1 not caught"; fails=$((fails+1)); } || echo "ok: php mutant $1 caught"
  }
  pmut no_alarm      "s/if \(\\\$f1 \|\| \\\$f2\)/if (false)/"
  pmut no_escape     "s/return htmlspecialchars\(trim\(\(string\)\\\$s\), ENT_QUOTES\);/return trim((string)\\\$s);/"
  pmut no_build_case "s/case 'no_build':/case 'no_build_x':/"
  pmut pch_ignored   "s/\(\\\$s\['pch_on'\] === '1' \? 'on' : 'blocked'\)/'on'/"
  pmut dx_bay_emid   "s/\\\$bay = 3 \* \(int\)\\\$h\['emid'\] \+ \\\$pm \+ 1;/\\\$bay = 3 * (int)\\\$h['emid'] + \\\$pm;/"
  pmut ds_led_lines  "s/foreach \(\['ds_leds' => /foreach (['x_ds_leds' => /"
  pmut page_ok_any   "s/\(\\\$h\['page_state'\] \?\? ''\) !== 'ok'/false/"
else echo "warning: php not found, status page tests skipped"; fi
[ $fails = 0 ] && echo "run-tests: OK" || { echo "run-tests: $fails failure(s) (clean suite: $CLEAN_FAILS)"; exit 1; }
