# Synology Expansion Unit Driver: an Unraid plugin for the DX1215 (and DS2415+ bay LEDs)

This plugin exists because of **[synodeb](https://github.com/rylos/synodeb)**: it showed
that a Synology NAS can run a plain Linux with all its hardware working, and it is the way
to get another system booting on the box in the first place. Start there.

> Booting Unraid on a DS2415+ needs synodeb's stage-1 kexec boot chain, which is proposed
> upstream but not part of synodeb as released yet. Until (or unless) it lands, our fork has
> it: https://github.com/luuvasara/synodeb (worked example: branch
> `boot/06-docs-unraid-example`, `docs/stage1-unraid.md`).

It ships a small kernel module, `synology_dx`, built for each supported Unraid kernel. The
module talks to the DX1215's four Marvell 88SM9705 port multipliers the way DSM does. The
fan policy lives in the companion plugin,
[Synology Hardware](https://github.com/luuvasara/unraid-synology-hardware), which reads this
driver. Both are maintained and hosted here; they are not part of synodeb.

Status: experimental. Developed and tested on one DS2415+ with one DX1215, Unraid 7.3.4
(kernel 6.18.58-Unraid). The RX1214 and RX413 use the same chip and are untested.

## What it exposes

- `/sys/kernel/synology_dx/`: per port multiplier the fan-alarm bits and GPIO pages; `unit/`
  with `fan_level` (full, middle, low), `fan_fail`, a time-limited stop test;
- a hwmon device `synology_dx`: `fan1_alarm`, `fan2_alarm`, `pwm1`, `pwm1_enable`;
- bay LEDs: green presence/activity on both units, on by default (settings `DS_LEDS` /
  `DX_LEDS` on the page); the orange fault LEDs are driven from disk health by the Synology
  Hardware plugin;
- a page under Utilities that says in plain words whether the driver works and what was
  checked.

It never blocks the boot, checks the kernel build (sha256, vermagic) before loading, and
`rmmod` restores what it found at load.

## Roadmap

The aim is to become a backend for the FanCtrl Plus plugin: the DS2415+ and DX1215 fans as
standard hwmon `pwm` devices, with this project's safety floor kept, so fan curves live in
FanCtrl Plus instead of in yet another fan-control page. Not implemented yet; until then the
plugin's own fan modes are used.

## Install

Unraid webGui: **Plugins, Install Plugin**, paste

    https://raw.githubusercontent.com/luuvasara/unraid-synology-expansion/main/synology-expansion.plg

or on the console: `plugin install https://raw.githubusercontent.com/luuvasara/unraid-synology-expansion/main/synology-expansion.plg`.

The package (`packages/synology-expansion-<version>-x86_64-1.txz`, with the module build in
`kmod/<uname -r>/`) is downloaded from this repository and checked against the MD5 in the
`.plg`; updates show up in Unraid's plugin update check. On a kernel the package has no build
for, the loader does not load anything and says so (syslog and a notification).

## Repository layout

| path | what |
|---|---|
| `synology-expansion.plg` | the plugin manifest (version, pluginURL, package URL + MD5, install/remove scripts, CHANGES) |
| `packages/` | the built package of the version in the `.plg` and its `.md5` |
| `src/` | module source (`synology_dx.c`, `sdx_gpio.h`, `Kbuild`), GPL-2.0 |
| `tests/` | `test-encoding.c` (register encoding), `check-readonly.sh` (write-guard check on source and object) and its can-fail test |
| `build-tree.sh`, `build-module.sh` | prepare the exact Unraid kernel tree (kernel.org tarball + Lime Technology's published recipe, gcc in docker) and build the module into `out/<kernelrelease>/` |
| `out/<kernelrelease>/` | the module build that goes into the package, with its sha256 |
| `plugin/` | the plugin: `src/` (loader `scripts/sdx-load`, page, CSS), `tests/`, `build.sh` |
| `docs/MODULE.md` | design notes of the module (register protocol, phases, bay LEDs) |

Comments and notes cite `research/...`, `experiments/...` and `RUNBOOK-*` files: those are
the measurement notes of the development repository and are not published here. No
Synology software or firmware is included; the register protocol was measured on the
hardware.

## Build

On a build host, never on the NAS. `build-tree.sh` lists its inputs (kernel.org tarball,
the Unraid kernel recipe of the matching release) in its header.

    ./build-tree.sh && ./build-module.sh   # only if src/ changed or for a new kernel
    cd plugin && ./build.sh                # runs the tests; bump &version; in the .plg first

## How this repository is kept in sync

Development happens in a private repository (it holds the hardware captures the driver was
measured from, which are not publishable). This repository is an export of the module and
plugin tree only: `src/`, `tests/`, the build scripts, `out/`, `plugin/`, the `.plg`, this
README, `docs/MODULE.md` and the package of the current version are copied file for file by
an export script at each release, then committed here. Nothing is edited only here; a change
made here (for example from a pull request) is applied to the private tree first and comes
back with the next export. The history of this repository starts at its first export
(2026-10-26).

## License and credits

GPL-2.0 (see `LICENSE`), the same as synodeb and the Linux kernel the module is built for.

- [synodeb](https://github.com/rylos/synodeb), the inspiration and the boot path.
- Unraid is a trademark of Lime Technology. Synology, DiskStation, DSM and DX1215 are
  trademarks of Synology Inc. This project is not affiliated with either.
