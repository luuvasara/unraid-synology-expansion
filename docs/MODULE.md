# synology_dx: DX1215 kernel module (0.6.1)

Written: 2026-10-25. Out-of-tree Linux module for the Synology DX1215 expansion unit (four
Marvell 88SM9705 port multipliers behind the DS2415+'s 88SE9235) on Unraid 7.3.4,
kernel `6.18.58-Unraid`. It will ship in its own Unraid plugin (not inside synology-hw);
synology-hw will later read its sysfs. Not meant for mainline.

Design: `research/2026-10-25-dx1215-kernel-design.md` (option b). Register protocol:
`research/2026-10-25-dx1215-gpio-spec.md`.

## Phase 1 scope

READ-ONLY. The only ATA command the module can build is PMP READ (0xE4) to the PMP control
port 15. It reads, on every host given:

| register | meaning | expected on the DX1215 |
|---|---|---|
| GSCR 0 | product id (device<<16 \| vendor) | `0x97051b4b` |
| GSCR 1 | revision: bits 3/2/1 = spec 1.2/1.1/1.0, bits 15:8 = rev | bit 3 set, rev 0xa0 (160) |
| GSCR 2 | port info, bits 3:0 = fan-out ports | 6 |
| GSCR 32 / 33 | error summary (per port) / error-info enable | unknown; bit 5 (link 5) may be set |
| GSCR 64 / 96 | features supported / enabled | 0x1f / 0x5 (ata19: 0x1) |
| VREG 0x3a0 | GPIO output data (20 bits) | unknown (power-on state) |
| VREG 0x3a4 | GPIO output enable | unknown (power-on state) |
| VREG 0x3b0 | GPIO input | unknown |

GSCR 0, 1, 2, 32, 33, 64, 96 are the registers libata itself reads at attach; the module prints
libata's cached copy next to each value ("same"/"DIFFERS") as a cross-check that the path, the
port number 15 and the register encoding are right. The vendor registers use the high byte of
the 16-bit register number in HOB_FEATURE (mainline `sata_pmp_read` truncates it to 8 bits).

Phase 1 ran on the box 2026-10-25 (`experiments/2026-10-25-synology-dx-phase1/RESULT.md`): all
as expected, idle 0x3a0 = 0x0006c000, 0x3a4 = 0x000fffff on all four PMPs.

## 0.6.0 / 0.6.1 (current, 2026-10-26): DS2415+ orange enable (PCH GPIO 49), deployed and verified

Evidence: `research/2026-10-26-ds-orange-led.md` (DSM's synobios drives Avoton PCH GPIO 49 high before its first disk LED),
`experiments/2026-10-26-bay-map/RESULT.md` (orange on all 12 DS bays once the pin is high).

- With `ds_leds=1` the module finds GPIOBASE from 00:1f.0 (must be 8086:1f38) config 0x48 & 0xff80 (0x500 here; accepted only
  with the I/O bit set and inside 0x400..0xff00), takes +0x80..+0x8b with `request_region` (nests under the firmware's
  `pnp 00:01` 0x500-0x5fe, which is not busy; fails if a driver such as gpio-ich owns the ports: then the pin is not touched),
  and requires USE_SEL2 bit 17 = 1 (GPIO) and IO_SEL2 bit 17 = 0 (output). Read only at load.
- `sdx_pch_write()` is the only port write in the module: one byte at GPIOBASE+0x8a (LVL2 bits 16-23), accepted only if it
  differs from the byte read at load in bit 1 (= GPIO 49) at most (`sdx_pch_b2_ok`), after re-reading USE_SEL2/IO_SEL2.
  The pin goes high when the first 88SE9215 gets `leds_enabled=1` (as synobios does) and back to the load byte when the
  last one is disabled and at unload.
- sysfs: `ds/pch_led_enable` (1 = high), `ds/pch` (state or refusal reason, gpiobase, use_sel2, io_sel2, lvl2_b2_load,
  lvl2_b2_written, lvl2_b2_read, users, last_write). **Measured**: after writing 0x3a a read gives 0x38 again while orange
  lights: the Avoton level register reads the pin input, not the latch (mainline gpio-ich caches output levels for the same
  reason). 0.6.1 only adds `lvl2_b2_written` (0.6.0 showed the read value as `lvl2_b2_now`).
- Tests: `test-encoding.c` exhaustive over the byte guard (exactly 512 accepted pairs), GPIOBASE range and pin check; source
  checks (guard, pin re-check and the one outb in order, call sites, request_region before writes, header constants);
  object checks (`out` only in `sdx_pch_write`, `test $0xfd`, two `bt $0x11`, ports 0x80/0x84/0x8a); 22 new mutants, 105 in all.
- Verified on the box 2026-10-26: `pch_led_enable` 1, `/proc/ioports` `0580-058b : synology_dx`, DS bay 1 orange for 10 s by
  `fault_mask=1` (10:14:08.535-10:14:18.550 UTC), no libata resets/errors after the load.
- Policy is outside the module: the synology-expansion loader writes `leds_enabled=1` / `dx_leds_enabled=1` at boot
  (default on); the Synology Hardware daemon sets `fault_mask` / `hostN/fault` from disk health.

## 0.5.0: bay LEDs, DX1215 and DS2415+ (hardware-verified 2026-10-26, see 0.6.0)

Spec and evidence: `research/2026-10-25-bay-leds.md`. Hardware steps: `RUNBOOK-phase3.md`.
The driver lights nothing on its own: LEDs change only when one of the files below is written.

- **DX1215** (always available): page 1 of each 88SM9705's GPIO. `hostN/led_mask` (1 = all disk
  LEDs of that PMP off, page 1 bit 6; measured 1 on all four), `hostN/fault` (0..7, bit n =
  orange fault of the PMP's local disk n, page 1 bits 0-2), `unit/dx_leds_enabled` (1 clears bit 6
  on all four, 0 sets it; reads 1, 0 or "mixed"). Each write is one EH round: page reads, then
  page 1 read-modify-write with Synology's three-word latch sequence, restore, page 1 read back.
  `sdx_pmp_write_checked()` accepts a page-1 data word only while `sdx_led_write()` is armed and
  only if it differs from the page 1 read in that round in bits 0-2 and 6: bit 5 (buzzer-mute
  pulse) is never set, and bits 3, 4, 7 are kept. The LED path never builds a page-4 word.
  rmmod writes back the page 1 value found at load on every PMP that was written.
- **DS2415+ internal bays** (module parameter `ds_leds=1`, default off; plugin: `DS_LEDS="1"`
  in `/boot/config/plugins/synology-expansion/synology-expansion.cfg`). This belongs to the main
  unit, not the expansion unit; it lives in this module for now. The three 88SE9215 (03:00.0,
  05:00.0, 06:00.0, stock ahci) have their own GPIO block behind the vendor index/data window
  ABAR+0xA8/+0xAC, reached through ahci's own mapping (`pci_get_drvdata` -> `ata_host` ->
  `ahci_host_priv->mmio`; nothing is mapped by this module). `sdx_ds_reg()` is the only code that
  touches the window: it refuses every GPIO address except 0x07071024 (output enable, active
  low), 0x07071020 (data out) and 0x07071058 (pin functions), holds its own spinlock and puts
  the index register back after each access. `/sys/kernel/synology_dx/ds/0000:BB:DD.F/`:
  `gpio_oe`, `gpio_out`, `gpio_func` (live reads), `saved` (values at load), `ports` (port_no ->
  ataN), `leds_enabled` (1 = DSM's init: OE 0, OUT 0, FUNC 0x00B6D8D1, so the chip drives green
  link/activity itself; 0 = back to the values at load), `fault_mask` (4 bits, bit p = orange of
  port p = DATA_OUT bit 4+p, only after leds_enabled=1). At load: reads only. rmmod writes the
  load values back if anything was written. Not re-applied after resume (Unraid does not
  suspend). If ahci were unbound from a 9215 while this module is loaded the mapping would go
  away under it: do not unbind ahci with `ds_leds=1`.
- Tests: exhaustive page-1 predicate (2^20 words x 256 page values: exactly 128 x 16 x 2
  accepted, none changing bits 3/4/5/7), the 9215 allowlist (every address 0x07000000-0x07ffffff
  plus a grid over the rest: exactly three accepted), the fault read-modify-write; source checks
  (one accessor, guard before the lock, three `writel` only there, named addresses only, LED code
  free of page-4 state and vice versa); object checks (page-1 constants in the write guard; the
  allowlist compare in `sdx_ds_reg` and no other address compare); 32 new mutants, 83 in all.

## 0.4.0: fan1/fan2 alarms, hwmon, Unraid plugin

Phase 2b ran on the box (`experiments/2026-10-25-synology-dx-phase2b/RESULT.md`): writes read
back exactly; STOP set page 2 bits 0 AND 1 within 5 s (two fan alarms), the timer revert worked.
- `fan1_fail` (page 2 bit 0) and `fan2_fail` (bit 1) per host and in `unit/`; `fan_fail` = either.
- hwmon device `synology_dx` (parent: the master's scsi host): `fan1_alarm`, `fan2_alarm`,
  `pwm1` (255 full, 128 middle, 64 low, 0 during a stop test; writes: > 128 full, > 64 middle,
  else low, never STOP), `pwm1_enable` (always 1). Reads go through the same 30 s cache; writes
  through the same `sdx_unit_set_level()` as `unit/fan_level`.
- Packaged as its own Unraid plugin, "Synology Expansion Unit Driver", ID `synology-expansion`
  (2026.10.25a was ID `synology-dx`): `plugin/` (README there). The module name `synology_dx`, its
  sysfs `/sys/kernel/synology_dx/` and hwmon `synology_dx` are the stable interface synology-hw
  consumes; they stay as they are whatever the plugin is called.

## Phase 2b (0.3.0-phase2b): fan level writes on the master

Phase 2a ran on the box (`experiments/2026-10-25-synology-dx-phase2a/RESULT.md`): master
host18/ata19 (EMID 0, unique 0x13), page 4 = 0x02 (LOW), slaves' pages 2-4 read 0x3f.

`/sys/kernel/synology_dx/unit/`: `summary`, `master_host`, `unique`, `model`, `fan_level` (rw:
full|middle|low), `fan_code`, `fan_fail`, `power_on`, `psu1`, `psu2`, `switch_mode`,
`last_error`, `fan_stop_test_s` (w: 1..60 = STOP for N s then LOW from a kernel delayed work,
0 = end now; r: seconds left). Reading re-reads the master when its cache is older than
`refresh_s` (module parameter, default and max 30).

A write (one EH round on the master): the 2a reads, then refuse unless page 0 read in that round
is 0x13 and page 4 bit 4 is 0; `new = ((cur & ~3) | code) & ~0x10`; Synology's page write
(W_CTL-high word, W_CTL-low latch word, W_CTL-high word); restore 0x3a0/0x3a4; read page 4 back;
any error or mismatch: one FULL write and `last_error`. The guard in `sdx_pmp_write_checked()`
accepts a page-4 word only via `sdx_w4_ok()` (`src/sdx_gpio.h`): data bit 4 clear, equal to the
page 4 just read except bits 1:0, STOP only while the stop test is armed and before its deadline,
and only while `sdx_fan_write()` is armed on the PMP whose fresh page 0 is 0x13. Code 11 (STOP)
is reachable only through `fan_stop_test_s`; its revert work is scheduled before the STOP write
and retries every 2 s (15 tries, then pr_crit "power-cycle the DX1215").

**Choice at rmmod: the module writes back the fan level read at load (LOW on this unit, the
CPLD's power-on default), not FULL.** DSM forces FULL on its own failures (fan fail, write
error: gpio-spec 5); here the policy (temperatures, fan-fail -> FULL) will live in synology-hw's
daemon, and an unloaded module should leave the DX as it found it. The module itself forces FULL
only when a write fails or does not read back. It does not react to `fan_fail` by itself.

Hardware steps: `RUNBOOK-phase2b.md`.

## Phase 2a (0.2.0-phase2a): GPIO page reads, never a page write

Synology's page "read" writes 0x3a4 and 0x3a0 before reading 0x3b0. The module now does exactly
that for pages 0..4 on every host, then restores 0x3a0/0x3a4 to the values read at the start of
the cycle and reads them back. Every PMP WRITE goes through `sdx_pmp_write_checked()`, which
refuses (nothing sent, WARN_ONCE) anything but 0x3a4 = 0xfffff/0xfc7c0 or a 0x3a0 word with
W_CTL (R bit 14) high and only strobe/page bits (`SDX_R_OK`, `src/sdx_gpio.h`). Encoding checked
against the spec's `enc()` and the measured idle 0x6c000 (= page 3, both strobes high) by
`BUILD_BUG_ON`s and `tests/test-encoding.c` (exhaustive over all 2^20 words for `SDX_R_OK`).

New sysfs per host: `page0..page4` (`0x<data> raw 0x<0x3b0 word>`), `page_state`, `emid`,
`unique`, `model`, `fan_code` (0 full, 1 middle, 2 low, 3 stop), `fan_fail`, `power_on`, `psu1`,
`psu2`, `switch_mode` (raw bits; polarity of psu/switch not yet known). `/sys/kernel/synology_dx/unit`
lists each host's emid/unique and picks the EMID-0 host as master (or says none/ambiguous).
Hardware steps: `RUNBOOK-phase2a.md`.

## Files

| path | what |
|---|---|
| `src/synology_dx.c`, `src/Kbuild` | the module |
| `build-tree.sh` | prepares the exact kernel tree in `build/` (docker `gcc:16.2.0`) |
| `build-module.sh` | builds the module, runs the read-only checks, writes `out/<kver>/` |
| `src/sdx_gpio.h` | GPIO register encoding (macros, shared with the userspace test) |
| `tests/check-readonly.sh` | write guard. Source: one PMP READ and one PMP WRITE taskfile, the write only in `sdx_pmp_write_checked()` with its guard before the taskfile, call sites only with the GPIO registers and named values, `SDX_R_SEL` always ORs W_CTL, no other write/reset primitive. Object: allowlisted imports, the 0xe8 command store only in `sdx_pmp_write_checked`, which must contain the guard constants 0x3a0, 0x3a4, 0xfffff, 0xfc7c0, 0xfff17fff, 0x4000; positive control on the 0xe4 store |
| `tests/test-encoding.c` | userspace check of the encoding (run by build-module.sh) |
| `tests/test-check-can-fail.sh [--ko]` | 37 source + 9 encoding mutants (+ 5 object mutants built in docker with `--ko`) must all be rejected |
| `RUNBOOK-phase1.md`, `RUNBOOK-phase2a.md`, `RUNBOOK-phase2b.md` | hardware steps for the box |

## Build (build host only, never on the NAS)

1. `build/dl/linux-6.18.58.tar.xz` from kernel.org (sha256 in the signed `sha256sums.asc`, and
   gregkh's signature on the tar, both checked).
2. Limetech recipe: `unsquashfs` of the 7.3.4 `bzmodules` (`src/linux-6.18.58-Unraid`) into
   `build/recipe/`; sha256 of every file equals `/usr/src/linux-6.18.58-Unraid` on the box.
3. `./build-tree.sh`: applies the patches, `config` -> `.config`, `olddefconfig`,
   `modules_prepare`, then `vmlinux` for `Module.symvers` and a System.map that is compared with
   Limetech's.
4. `./build-module.sh`; `tests/test-check-can-fail.sh`.

Result 2026-10-25 [measured]: config differs from Limetech's only in toolchain probes
(`AS/LD_VERSION` 24400 vs 24700, `PAHOLE_VERSION`, `RUSTC_*`); our System.map has the same
203608 lines and **every text symbol at the same address** as Limetech's (66 differences, all
kallsyms tables/CSWTCH data); `ata_exec_internal` = `0xffffffff81ba8420` in both and in the box's
/proc/kallsyms. Module: gcc 16.2.0, W=1 clean, vermagic `6.18.58-Unraid SMP preempt mod_unload `,
`out/6.18.58-Unraid/synology_dx.ko` (35 KB, sha256 in the file next to it). The object check was
also run against a mutant built with `ATA_CMD_PMP_WRITE`: it fails on `movw $0xe80f` (gcc merges
device 0x0f and the command byte into one store) and on the missing 0xe4 positive control.

## Safety design (why each step is safe)

- **Load checks, all before anything changes**: running release == `UTS_RELEASE` (and the
  kernel's own vermagic check); `exec_internal` resolves via `sprint_symbol` to exactly
  `ata_exec_internal+0x0/...` in vmlinux (not a module); each host: `proc_name == "ahci"`,
  `ata_shost_to_port` gives a port whose `scsi_host` points back, `ops->error_handler ==
  ahci_error_handler` (also catches a second load), PMP attached, class `ATA_DEV_PMP`, enabled,
  libata's GSCR copy says 1b4b:9705. `validate_only=1` runs exactly these checks, sends nothing and
  refuses to stay loaded.
- **Prototype**: the function pointer is `typeof(ata_exec_internal)` from the tree's own
  `drivers/ata/libata.h`; a prototype change in a future kernel is a compile error.
- **Context**: register reads run only inside libata EH, on the EH thread that owns the port
  (`ata_port_schedule_eh` + `ata_port_wait_eh`): SCSI has drained the host, nothing is in flight,
  `ata_exec_internal` is used exactly as libata's `sata_pmp_read` uses it.
- **Quiet rounds only**: the wrapper serves a request only if the EH round has nothing else to do
  (not frozen, no qc active or failed, no err_mask/action/serror/flags on any link, no pending
  EH, not loading/unloading/suspended). It then does NOT call `ahci_error_handler`: an idle libata
  EH round would hard-reset the disabled link 5 and re-check GSCR_ERROR, which on this unit can
  start the link-5 reset storm. Every other EH round (all real errors) goes to the original
  handler untouched. Not quiet three times in a row: the request fails with `last_error`.
- **Timeout**: a read that times out makes libata freeze the port (inside `ata_exec_internal`);
  the wrapper then calls the original handler in the same round, which recovers the port the way it
  does after libata's own internal-command timeouts. Reads stop at the first error.
- **One host at a time** at load, so at most one DX port is in EH.
- **Unload**: sysfs removed first (kernfs drains running show/store), then per port under
  `ap->lock` the original `ops` pointer is restored, `ata_port_wait_eh` lets a round already in
  the wrapper finish, `synchronize_rcu` covers irq/locked readers that still hold our ops copy,
  then the scsi host references are dropped. Nothing is left behind (no flash, no persistent state).
- **Single checked write path** (phase 2a): see above; `noinline __noipa__` keeps the run-time
  guard in the object (without it gcc's IPA proved the register always 0x3a0/0x3a4 and dropped the
  compare) and keeps the function findable by the object check.
- **Restore**: pages are read only if the idle 0x3a0/0x3a4 are themselves writable by the guard;
  after the pages (and after a failure, unless the port froze) both are restored and read back;
  `page_state` says whether the readback matched.

Residual risks: the page read drives the strobe and page-select pins like DSM does; while R_CTL is
low and output enable is still 0xfffff (1-2 ms, same order as DSM) the data pins are driven 0 by the
9705 and possibly by the CPLD. Reading vendor registers 0x3a0/0x3a4/0x3b0 is assumed side-effect free (it is a
register read, but the 9705's vendor space is undocumented); a bug in the wrapper runs in the EH
path of the four DX ports (worst case an oops there: reboot). The module uses an unexported
symbol by address and must be rebuilt and re-verified for every kernel.
