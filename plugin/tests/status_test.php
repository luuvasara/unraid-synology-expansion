<?php
/* Offline tests for include/sdx-status.php (the synology-expansion status page). Written: 2026-10-25 (2026.10.25b).
 * usage: php tests/status_test.php <plugin-dir>   (run by build.sh)
 * Builds fake /sys, /var/run/synology-expansion and kmod trees for five states and checks the verdict,
 * checklist and findings. Any PHP notice/warning counts as a failure. */
$plug = $argv[1];
$fails = 0;
function ok($name, $cond, $detail = '') { global $fails; echo ($cond ? 'ok: ' : 'FAIL: ') . $name . (!$cond && $detail !== '' ? "\n   [" . substr(strip_tags($detail), 0, 600) . "]" : '') . "\n"; if (!$cond) $fails++; }
$warnings = [];
set_error_handler(function ($no, $str, $file, $line) use (&$warnings) { $warnings[] = "$str at " . basename($file) . ":$line"; return true; });
require "$plug/include/sdx-status.php";

$base = dirname(__DIR__, 2) . '/build/plugin-status-tests';
function rmr($d) { if (!is_dir($d)) return; foreach (new RecursiveIteratorIterator(new RecursiveDirectoryIterator($d, FilesystemIterator::SKIP_DOTS), RecursiveIteratorIterator::CHILD_FIRST) as $f) $f->isDir() ? rmdir($f) : unlink($f); rmdir($d); }
function put($f, $c) { if (!is_dir(dirname($f))) mkdir(dirname($f), 0755, true); file_put_contents($f, $c); }
const KV = '6.18.58-Unraid';
const UNIT = "host18 emid 0 unique 0x13\nhost19 emid 1 unique 0x13\nhost20 emid 2 unique 0x13\nhost21 emid 3 unique 0x13\nmaster host18\nunique 0x13\nmodel DX1215\nfan_level low\nfan_code 2\nfan_fail 0\nfan1_fail 0\nfan2_fail 0\npower_on 1\npsu1 0\npsu2 0\nswitch_mode 1\nload_code 2\nstop_test -\ncache_age_s 29\nlast_error -\n";
const CHECKS_OK = "started\tok\t2026-10-25 15:49:01\nbuild\tok\tkmod/6.18.58-Unraid/synology_dx.ko (version 0.4.0)\nsha256\tok\t04fa8ea333e8622c...\nvermagic\tok\t6.18.58-Unraid SMP preempt mod_unload \nhosts\tok\t18,19,20,21\nkallsyms\tok\t0xffffffff81ba8420\ninsmod\tok\tloaded, master host18\nresult\tloaded\tsynology_dx 0.4.0 loaded on hosts 18,19,20,21, master host18\n";

function state(string $name, array $o = []): array {
  global $base; $r = "$base/$name"; rmr($r);
  mkdir("$r/sys", 0755, true); mkdir("$r/run", 0755, true); mkdir("$r/kmod/" . KV, 0755, true);
  if (isset($o['checks'])) put("$r/run/checks", $o['checks']);
  if ($o['loaded'] ?? false) {
    put("$r/sys/module/synology_dx/version", "0.4.0\n");
    put("$r/sys/kernel/synology_dx/unit/summary", $o['unit'] ?? UNIT);
    foreach ([18, 19, 20, 21] as $i => $h) {
      $d = "$r/sys/kernel/synology_dx/host$h";
      put("$d/summary", "host $h\nata " . ($h + 1) . "\ncycles 1\nnot_quiet 0\nvendor 0x1b4b\ndevice 0x9705\nspec 1.2\nrev 160\nports 6\nfeat 0xd/0x1f\nlast_error -\n");
      put("$d/ata_port", "ata" . ($h + 1) . "\n"); put("$d/emid", "$i\n"); put("$d/unique", "0x13\n");
      put("$d/page_state", ($o['page_state'][$h] ?? 'ok') . "\n");
      $pages = $h == 18 ? ['0x13', '0x40', '0x3c', '0x09', '0x02'] : [sprintf('0x%02x', 0x13 | ($i << 5)), '0x40', '0x3f', '0x3f', '0x3f'];
      foreach ($pages as $p => $v) put("$d/page$p", "$v raw 0x0c000\n");
      put("$d/led_mask", ($o['led_mask'] ?? '0') . "\n"); put("$d/fault", ($o['dx_fault'][$h] ?? 0) . "\n");
    }
    put("$r/sys/kernel/synology_dx/unit/dx_leds_enabled", ($o['led_mask'] ?? '0') === '0' ? "1\n" : "0\n");
  }
  if ($o['leds'] ?? false) {   // 2026.10.26a: three 9215, PCH, two disks (DS bay 1 = 03:00.0 port 0 = ata7; DX bay 8 = host20/ata21 PMP port 1)
    $ds = "$r/sys/kernel/synology_dx/ds";
    foreach (['0000:03:00.0' => 7, '0000:05:00.0' => 11, '0000:06:00.0' => 15] as $pci => $a0) {
      put("$ds/$pci/leds_enabled", ($o['ds_enabled'] ?? '1') . "\n"); put("$ds/$pci/fault_mask", ($o['ds_fault'][$pci] ?? 0) . "\n");
      put("$ds/$pci/ports", implode('', array_map(fn($i) => "port$i ata" . ($a0 + $i) . "\n", range(0, 3))));
    }
    put("$ds/pch_led_enable", ($o['pch_on'] ?? '1') . "\n");
    put("$ds/pch", "state " . ($o['pch_state'] ?? 'ok') . "\ngpiobase 0x500\nuse_sel2 0x1fff0000\nio_sel2 0x00000000\nlvl2_b2_load 0x38\nlvl2_b2_now 0x3a\nusers 3\nlast_write 0\n");
    $dv = "$r/sys/devices/pci0000:00";
    mkdir("$dv/0000:00:02.0/0000:03:00.0/ata7/host6/target6:0:0/6:0:0:0", 0755, true);
    mkdir("$dv/0000:00:03.0/0000:07:00.0/ata21/link21.0", 0755, true);
    mkdir("$dv/0000:00:03.0/0000:07:00.0/ata21/host20/target20:1:0/20:1:0:0", 0755, true);
    mkdir("$dv/0000:00:16.0/usb1/1-1/1-1:1.0/host2/target2:0:0/2:0:0:0", 0755, true);
    foreach (['sdc' => "0000:00:02.0/0000:03:00.0/ata7/host6/target6:0:0/6:0:0:0", 'sdd' => "0000:00:03.0/0000:07:00.0/ata21/host20/target20:1:0/20:1:0:0",
              'sdb' => "0000:00:16.0/usb1/1-1/1-1:1.0/host2/target2:0:0/2:0:0:0"] as $b => $path) {
      mkdir("$r/sys/block/$b", 0755, true); symlink("$dv/$path", "$r/sys/block/$b/device");
    }
    if (isset($o['health'])) put("$r/hwrun/leds", $o['health']);
  }
  if (isset($o['cfg'])) put("$r/cfg", $o['cfg']);
  $GLOBALS['sdx_path_override'] = ['sys' => "$r/sys", 'run' => "$r/run", 'kmod' => "$r/kmod", 'kernel' => $o['kernel'] ?? KV, 'hwrun' => "$r/hwrun", 'cfg' => "$r/cfg"];
  $s = sdx_collect(sdx_paths());
  return [$s, sdx_render($s), sdx_verdict($s)];
}
$has = fn($h, $t) => strpos($h, $t) !== false;

// 1 loaded ok
[$s, $h, $v] = state('loaded-ok', ['loaded' => true, 'checks' => CHECKS_OK]);
ok('loaded ok: green verdict "Driver loaded: DX1215 found and responding"', $v[0] === 'green' && $v[1] === 'Driver loaded: DX1215 found and responding', json_encode($v));
ok('loaded ok: green orb + green-text headline', $has($h, 'green-orb') && $has($h, '<b class="green-text">Driver loaded: DX1215 found and responding</b>'), $h);
ok('loaded ok: no failed check in the list', !$has($h, 'fa-times'), $h);
ok('loaded ok: kernel build + version', $has($h, 'Driver build for the running kernel 6.18.58-Unraid') && $has($h, 'kmod/6.18.58-Unraid/synology_dx.ko (version 0.4.0)'), $h);
ok('loaded ok: 4 port multipliers ata19-ata22, Marvell 88SM9705 rev 160', $has($h, '4 (ata19&ndash;ata22, Marvell 88SM9705 rev 160, 6 ports each)'), $h);
ok('loaded ok: address confirmed by the driver', $has($h, '0xffffffff81ba8420, confirmed by the driver itself at load'), $h);
ok('loaded ok: master ata19 (host18), EMID 0', $has($h, 'ata19 (host18), EMID 0'), $h);
ok('loaded ok: unit identified DX1215 (ID 0x13)', $has($h, 'DX1215 (ID 0x13)'), $h);
ok('loaded ok: register reads/restore 4 of 4', $has($h, '4 of 4 port multipliers ok'), $h);
ok('loaded ok: fan level read low (page 4 = 0x02)', $has($h, 'low (page 4 = 0x02)'), $h);
ok('loaded ok: findings in words: low speed, fans OK, power on', $has($h, 'low speed (the unit') && substr_count($h, '<span class="green-text">OK</span>') === 2 && $has($h, '<td>Power</td><td class="whitespace-normal">on</td>'), $h);
ok('loaded ok: PSU raw and marked, switch bit 1 = DEFAULT (verified)', $has($h, 'meaning not yet confirmed') && $has($h, '<td>Rear DEFAULT/MANUAL switch</td><td class="whitespace-normal">DEFAULT <span class="grey-text">(bit 1)</span>'), $h);
ok('loaded ok: findings cite the research/experiment files', $has($h, 'research/2026-10-25-dx1215-gpio-spec.md') && $has($h, 'experiments/2026-10-25-synology-dx-phase2b/RESULT.md'), $h);
ok('loaded ok: raw details collapsed with the dump', $has($h, '<details class="sdx-raw"><summary>Raw details</summary>') && $has($h, 'master host18') && $has($h, 'page4 0x02 raw'), $h);
ok('loaded ok: no hex colour in the markup', !preg_match('/(?<!&)#[0-9a-fA-F]{3,6}\b|style=/', $h), $h);

// 2 no build for the running kernel
[$s, $h, $v] = state('no-build', ['kernel' => '6.18.59-Unraid', 'checks' => "started\tok\t2026-10-25 16:00:00\nbuild\tfail\tno build for 6.18.59-Unraid (builds: 6.18.58-Unraid )\nresult\tno_build\tno synology_dx build for kernel 6.18.59-Unraid\n"]);
ok('no build: red verdict naming the kernel', $v[0] === 'red' && $has($v[1], 'no build for kernel 6.18.59-Unraid'), json_encode($v));
ok('no build: lists the builds the plugin has', $has($h, 'builds in the plugin: 6.18.58-Unraid'), $h);
ok('no build: build check failed, later checks not run, no findings', $has($h, 'fa-times red-text') && $has($h, 'not checked') && !$has($h, '>Findings<'), $h);

// 3 no DX attached
[$s, $h, $v] = state('no-dx', ['checks' => str_replace("hosts\tok\t18,19,20,21\nkallsyms\tok\t0xffffffff81ba8420\ninsmod\tok\tloaded, master host18\nresult\tloaded\tsynology_dx 0.4.0 loaded on hosts 18,19,20,21, master host18\n", "hosts\tfail\tno ahci port with a port multiplier\nresult\tno_dx\tno expansion unit attached\n", CHECKS_OK)]);
ok('no DX: orange "No expansion unit found"', $v[0] === 'orange' && $v[1] === 'No expansion unit found', json_encode($v));
ok('no DX: hosts check shows the reason', $has($h, 'no ahci port with a port multiplier'), $h);

// 4 fan alarm
[$s, $h, $v] = state('alarm', ['loaded' => true, 'checks' => CHECKS_OK, 'unit' => str_replace(["fan_fail 0", "fan1_fail 0", "fan2_fail 0"], ["fan_fail 1", "fan1_fail 1", "fan2_fail 1"], UNIT)]);
ok('alarm: red verdict FAN ALARM, both fans', $v[0] === 'red' && $has($v[1], 'FAN ALARM') && $has($v[2], 'both fans'), json_encode($v));
ok('alarm: both fans shown as ALARM in red', substr_count($h, '<span class="red-text">ALARM: stopped or blocked</span>') === 2, $h);
[$s, $h, $v] = state('alarm-stoptest', ['loaded' => true, 'checks' => CHECKS_OK, 'unit' => str_replace(["fan2_fail 0", "stop_test -", "fan_level low"], ["fan2_fail 1", "stop_test active", "fan_level stop"], UNIT)]);
ok('alarm during stop test: fan 2 only, says the test causes it', $has($v[2], 'fan 2') && $has($v[2], 'stop test is running') && $has($h, 'STOPPED (fan stop test running)'), json_encode($v));

// 5 module missing (no loader record, not loaded)
[$s, $h, $v] = state('missing');
ok('module missing: red "Driver not loaded", loader has not run', $v[0] === 'red' && $v[1] === 'Driver not loaded' && $has($v[2], 'has not run'), json_encode($v));
ok('module missing: driver-loaded check failed', $has($h, '<td>Driver loaded</td><td class="whitespace-normal">not loaded</td>'), $h);

// 6 extra: one host's page read failed, and an escaped error text
[$s, $h, $v] = state('page-fail', ['loaded' => true, 'checks' => CHECKS_OK, 'page_state' => [20 => 'page 2 failed: err_mask 0x1'], 'unit' => str_replace('last_error -', 'last_error <script>x</script>', UNIT)]);
ok('page read failed on host20: orange, names the host', $v[0] === 'orange' && $has($v[2], 'host20'), json_encode($v));
ok('page read failed: 3 of 4 and a failed check', $has($h, '3 of 4 port multipliers ok') && $has($h, 'fa-times'), $h);
ok('sysfs text is escaped', !$has($h, '<script>x</script>') && $has($h, '&lt;script&gt;x&lt;/script&gt;'), $h);

[$s, $h, $v] = state('old-plugin', ['loaded' => true, 'checks' => "oldplugin\tfail\tthe old synology-dx plugin is still installed\n" . CHECKS_OK]);
ok('old synology-dx plugin present: orange verdict + failed check', $v[0] === 'orange' && $has($v[1], 'old synology-dx plugin') && $has($h, 'Old plugin (synology-dx) removed'), json_encode($v));

// 7 (2026.10.26a) bay LEDs on, health record from the Synology Hardware daemon
const CHECKS_LEDS = "ds_leds\tok\ton: leds_enabled=1 on 3 controllers (88SE9215)\npch\tok\tGPIO 49 high (orange enable), GPIOBASE 0x500\ndx_leds\tok\ton: dx_leds_enabled=1\n";
$HEALTH = "#at " . time() . "\n#health yes\nds\t1\tsdc\t0\thealthy: SMART attributes ok, unassigned\ndx\t8\tsdd\t0\thealthy: SMART attributes ok, unassigned\n";
$chk = str_replace("result\t", CHECKS_LEDS . "result\t", CHECKS_OK);
[$s, $h, $v] = state('leds-on', ['loaded' => true, 'leds' => true, 'checks' => $chk, 'health' => $HEALTH]);
ok('leds on: still green, three LED check lines ok with the live values', $v[0] === 'green' && $has($h, 'DS2415+ orange enable (PCH GPIO 49)') && $has($h, 'now: pch_led_enable 1, driver: ok') && $has($h, 'now: leds_enabled 1 1 1') && $has($h, 'now: dx_leds_enabled 1') && !$has($h, 'fa-times'), $h);
$B = sdx_bays($s);
ok('leds on: 12 DS bays and 12 DX bays', count($B['ds']) === 12 && count($B['dx']) === 12, json_encode(array_map('count', $B)));
ok('leds on: DS bay 1 = sdc (03:00.0 port 0), green on, orange off, health from the daemon', $B['ds'][1][2] === 'sdc' && $B['ds'][1][3] === 'on' && $B['ds'][1][4] === 'off' && $has($B['ds'][1][5], 'healthy'), json_encode($B['ds'][1]));
ok('leds on: DX bay 8 = sdd (host20 EMID 2, PMP port 1)', $B['dx'][8][2] === 'sdd' && $B['dx'][8][3] === 'on' && $B['dx'][8][4] === 'off', json_encode($B['dx'][8]));
ok('leds on: the USB stick is in no bay', !in_array('sdb', array_column(array_merge($B['ds'], $B['dx']), 2), true), json_encode($B));
ok('leds on: bay table rendered, empty bays marked', $has($h, '<td>DS2415+ internal bays</td>') && $has($h, '<td>DX1215 bays</td>') && $has($h, 'Bay 12') && $has($h, '<span class="grey-text">empty</span>') && $has($h, 'last evaluated'), $h);
// 8 fault on DS bay 1 and DX bay 8
[$s, $h, $v] = state('leds-fault', ['loaded' => true, 'leds' => true, 'checks' => $chk, 'ds_fault' => ['0000:03:00.0' => 1], 'dx_fault' => [20 => 2],
  'health' => str_replace(["0\thealthy: SMART attributes ok, unassigned\ndx", "sdd\t0\thealthy: SMART attributes ok, unassigned"], ["1\tSMART 5 Reallocated_Sector_Ct FAILING_NOW\ndx", "sdd\t1\tUnraid disk state DISK_DSBL"], $HEALTH)]);
$B = sdx_bays($s);
ok('fault: DS bay 1 and DX bay 8 orange on, others off', $B['ds'][1][4] === 'on' && $B['dx'][8][4] === 'on' && $B['ds'][2][4] === 'off' && $B['dx'][7][4] === 'off' && $B['dx'][9][4] === 'off', json_encode([$B['ds'][1], $B['dx'][8]]));
ok('fault: reasons shown as FAULT in orange', $has($h, '<span class="orange-text">FAULT: SMART 5 Reallocated_Sector_Ct FAILING_NOW</span>') && $has($h, 'FAULT: Unraid disk state DISK_DSBL') && $has($h, 'ON (fault)'), $h);
// 9 fault bit set but GPIO 49 not high (driver could not take the port): blocked + orange verdict from the pch check
[$s, $h, $v] = state('leds-pch-fail', ['loaded' => true, 'leds' => true, 'pch_on' => '0', 'pch_state' => 'GPIO I/O ports owned by another driver: not touched', 'ds_fault' => ['0000:03:00.0' => 1],
  'checks' => str_replace("pch\tok\tGPIO 49 high (orange enable), GPIOBASE 0x500", "pch\tfail\tpch_led_enable=0: GPIO I/O ports owned by another driver: not touched; orange cannot light", $chk)]);
$B = sdx_bays($s);
ok('pch fail: DS bay 1 orange "blocked", verdict orange naming the reason', $B['ds'][1][4] === 'blocked' && $v[0] === 'orange' && $has($v[2], 'owned by another driver') && $has($h, 'cannot light: PCH GPIO 49 is not high'), json_encode([$v, $B['ds'][1]]));
// 10 LEDs off by setting, no Synology Hardware daemon
[$s, $h, $v] = state('leds-off', ['loaded' => true, 'leds' => true, 'ds_enabled' => '0', 'led_mask' => '1', 'cfg' => "DS_LEDS=\"0\"\nDX_LEDS=\"0\"\n",
  'checks' => str_replace(CHECKS_LEDS, "ds_leds\tok\toff (DS_LEDS=\"0\"): leds_enabled=0 on 3 controllers\npch\tskip\tGPIO 49 as found at load (DS bay LEDs off): pch_led_enable=0\ndx_leds\tok\toff (DX_LEDS=\"0\"): dx_leds_enabled=0\n", $chk)]);
$B = sdx_bays($s);
ok('leds off: green off everywhere, settings read as 0, no-daemon note', $B['ds'][1][3] === 'off' && $B['dx'][8][3] === 'off' && $s['cfg'] === ['DS_LEDS' => '0', 'DX_LEDS' => '0'] && $has($h, 'no health record'), json_encode([$s['cfg'], $B['ds'][1]]));
[$s] = state('leds-cfg-default', ['loaded' => true, 'leds' => true, 'checks' => $chk]);
ok('no settings file: both LED settings default to on', $s['cfg'] === ['DS_LEDS' => '1', 'DX_LEDS' => '1'], json_encode($s['cfg']));

ok('no PHP notices or warnings', !$warnings, implode('; ', $warnings));
echo $fails ? "status_test: $fails failure(s)\n" : "status_test: OK\n";
exit($fails ? 1 : 0);
