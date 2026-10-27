<?php
/* synology-expansion status page library: collect (read-only) + render. Written: 2026-10-25 (2026.10.25b).
 * Sources: the loader's record of what it verified (/var/run/synology-expansion/checks, written by
 * scripts/sdx-load at every load), the module's sysfs (/sys/kernel/synology_dx/{unit,hostN}/),
 * /sys/module/synology_dx. Nothing here writes anything. Reading unit/summary may make the
 * module re-read the master port multiplier if its 30 s cache is old (one short libata EH round).
 * Colours: dynamix classes only (green-text, orange-text, red-text, grey-text, *-orb).
 * 2026.10.26a: bay LEDs. Per bay (DS2415+ 1-12: 4 x controller index + port + 1; DX1215 1-12: 3 x EMID + port
 * multiplier port + 1, both confirmed at the box by blink count, experiments/2026-10-26-bay-map/RESULT.md) the disk
 * in it (/sys/block/sdX/device -> ataN [+ target channel]), green on/off (ds/<pci>/leds_enabled, hostN/led_mask),
 * orange (ds/<pci>/fault_mask bit port, needs ds/pch_led_enable = 1; hostN/fault bit port) and the health verdict
 * of the Synology Hardware plugin's daemon, which owns the orange policy (/var/run/synology-hardware/leds).
 * Tests: tests/status_test.php (fixture states). */

/* DS2415+ bay of port 0 per 88SE9215 (4 x controller index; confirmed by blink count 2026-10-26) */
const SDX_DS_BAY0 = ['0000:03:00.0' => 0, '0000:05:00.0' => 4, '0000:06:00.0' => 8];

function sdx_paths(): array {
  $p = ['sys' => '/sys', 'run' => '/var/run/synology-expansion', 'hwrun' => '/var/run/synology-hardware',
        'cfg' => '/boot/config/plugins/synology-expansion/synology-expansion.cfg',
        'kmod' => '/usr/local/emhttp/plugins/synology-expansion/kmod', 'kernel' => php_uname('r')];
  return array_merge($p, $GLOBALS['sdx_path_override'] ?? []);
}

function sdx_h($s): string { return htmlspecialchars(trim((string)$s), ENT_QUOTES); }
function sdx_read(string $f): ?string { if (!is_file($f) || !is_readable($f)) return null; $v = @file_get_contents($f); return $v === false ? null : $v; }
/* "key value" lines -> [key => value] */
function sdx_kv(?string $t): array {
  $o = [];
  foreach (explode("\n", (string)$t) as $l) {
    $l = trim($l); if ($l === '') continue;
    $sp = strpos($l, ' ');
    if ($sp === false) $o[$l] = ''; else $o[substr($l, 0, $sp)] = trim(substr($l, $sp + 1));
  }
  return $o;
}

function sdx_collect(array $p): array {
  $sys = $p['sys']; $s = ['kernel' => $p['kernel']];
  $s['loaded'] = is_dir("$sys/module/synology_dx");
  $s['version'] = trim((string)sdx_read("$sys/module/synology_dx/version"));
  $s['status'] = trim((string)sdx_read("$p[run]/status"));
  $s['checks_raw'] = sdx_read("$p[run]/checks");
  $s['checks'] = []; $s['result'] = null;
  foreach (explode("\n", (string)$s['checks_raw']) as $l) {
    $f = explode("\t", $l, 3); if (count($f) < 3) continue;
    if ($f[0] === 'result') $s['result'] = ['code' => $f[1], 'text' => $f[2]];
    else $s['checks'][$f[0]] = ['state' => $f[1], 'value' => $f[2]];
  }
  $s['builds'] = array_values(array_map('basename', glob("$p[kmod]/*", GLOB_ONLYDIR) ?: []));
  $s['hosts'] = [];
  foreach (glob("$sys/kernel/synology_dx/host*", GLOB_ONLYDIR) ?: [] as $d) {
    $h = sdx_kv(sdx_read("$d/summary"));
    $h['dir'] = basename($d);
    foreach (['ata_port', 'emid', 'unique', 'page_state', 'page0', 'page1', 'page2', 'page3', 'page4', 'led_mask', 'fault'] as $k)
      $h[$k] = trim((string)sdx_read("$d/$k"));
    $h['raw'] = (string)sdx_read("$d/summary");
    $s['hosts'][$h['dir']] = $h;
  }
  uksort($s['hosts'], fn($a, $b) => (int)substr($a, 4) <=> (int)substr($b, 4));
  $s['unit_raw'] = $s['loaded'] ? sdx_read("$sys/kernel/synology_dx/unit/summary") : null;
  $s['unit'] = sdx_kv($s['unit_raw']);
  // 2026.10.26a: bay LEDs
  $ds = "$sys/kernel/synology_dx/ds";
  $s['ds'] = [];
  foreach (glob("$ds/0000:*", GLOB_ONLYDIR) ?: [] as $d) {
    $ports = [];
    foreach (explode("\n", (string)sdx_read("$d/ports")) as $l)
      if (preg_match('/^port([0-3]) ata(\d+)$/', trim($l), $m)) $ports[(int)$m[1]] = (int)$m[2];
    $s['ds'][basename($d)] = ['enabled' => trim((string)sdx_read("$d/leds_enabled")), 'fault' => trim((string)sdx_read("$d/fault_mask")), 'ports' => $ports];
  }
  ksort($s['ds']);
  $s['pch_on'] = trim((string)sdx_read("$ds/pch_led_enable"));
  $s['pch'] = sdx_kv(sdx_read("$ds/pch"));
  $s['dx_leds'] = $s['loaded'] ? trim((string)sdx_read("$sys/kernel/synology_dx/unit/dx_leds_enabled")) : '';
  $s['disks'] = [];                                   // "ata<N>" or "ata<N>.<pmp port>" -> sdX
  foreach (glob("$sys/block/sd*") ?: [] as $b) {
    $r = realpath("$b/device");
    if (!is_string($r) || strpos($r, '/usb') !== false || !preg_match_all('#/ata(\d+)/#', $r, $m)) continue;
    $ata = (int)end($m[1]);
    $key = "ata$ata";
    if (is_dir(substr($r, 0, strpos($r, "/ata$ata/") + strlen("/ata$ata")) . "/link$ata.0") && preg_match('#/target\d+:(\d+):\d+/#', $r, $t)) $key .= '.' . (int)$t[1];
    $s['disks'][$key] = basename($b);
  }
  $s['health'] = []; $s['health_at'] = null; $s['health_policy'] = '';
  foreach (explode("\n", (string)sdx_read("$p[hwrun]/leds")) as $l) {
    if (preg_match('/^#at (\d+)$/', trim($l), $m)) { $s['health_at'] = (int)$m[1]; continue; }
    if (preg_match('/^#health (\S+)$/', trim($l), $m)) { $s['health_policy'] = $m[1]; continue; }
    $f = explode("\t", $l);
    if (count($f) >= 5 && preg_match('/^[a-z0-9]{1,16}$/', $f[2])) $s['health'][$f[2]] = ['fault' => $f[3] === '1', 'why' => $f[4]];
  }
  $cfg = is_file($p['cfg']) ? (@parse_ini_file($p['cfg']) ?: []) : [];
  $s['cfg'] = ['DS_LEDS' => ($cfg['DS_LEDS'] ?? '1') === '0' ? '0' : '1', 'DX_LEDS' => ($cfg['DX_LEDS'] ?? '1') === '0' ? '0' : '1'];
  return $s;
}

/* one row per bay: [box ds|dx, bay, device|'', green on|off|unknown, orange on|off|blocked|unknown, health text] */
function sdx_bays(array $s): array {
  $B = ['ds' => [], 'dx' => []];
  $hw = function ($dev) use ($s) {
    if ($dev === '') return '';
    $h = $s['health'][$dev] ?? null;
    return $h ? ($h['fault'] ? 'FAULT: ' : '') . $h['why'] : '';
  };
  foreach ($s['ds'] as $pci => $c) {
    if (!isset(SDX_DS_BAY0[$pci])) continue;
    $en = $c['enabled'] === '1'; $fm = ctype_digit($c['fault']) ? (int)$c['fault'] : -1;
    for ($p = 0; $p < 4; $p++) {
      $dev = isset($c['ports'][$p]) ? ($s['disks']['ata' . $c['ports'][$p]] ?? '') : '';
      $bit = $fm >= 0 && ($fm >> $p) & 1;
      $or = !$en ? 'off' : ($fm < 0 ? 'unknown' : ($bit ? ($s['pch_on'] === '1' ? 'on' : 'blocked') : 'off'));
      $B['ds'][SDX_DS_BAY0[$pci] + $p + 1] = ['ds', SDX_DS_BAY0[$pci] + $p + 1, $dev, $c['enabled'] === '' ? 'unknown' : ($en ? 'on' : 'off'), $or, $hw($dev)];
    }
  }
  foreach ($s['hosts'] as $h) {
    if (!preg_match('/^[0-3]$/', $h['emid'] ?? '') || !preg_match('/^ata(\d+)$/', $h['ata_port'] ?? '', $m)) continue;
    $mask = $h['led_mask'] ?? ''; $f = ctype_digit($h['fault'] ?? '') ? (int)$h['fault'] : -1;
    for ($pm = 0; $pm < 3; $pm++) {
      $bay = 3 * (int)$h['emid'] + $pm + 1;
      $dev = $s['disks']["ata{$m[1]}.$pm"] ?? '';
      $gr = $mask === '0' ? 'on' : ($mask === '1' ? 'off' : 'unknown');
      $or = $gr !== 'on' ? ($gr === 'off' ? 'off' : 'unknown') : ($f < 0 ? 'unknown' : (($f >> $pm) & 1 ? 'on' : 'off'));
      $B['dx'][$bay] = ['dx', $bay, $dev, $gr, $or, $hw($dev)];
    }
  }
  ksort($B['ds']); ksort($B['dx']);
  return $B;
}

/* [level green|orange|red, headline, reason] */
function sdx_verdict(array $s): array {
  $u = $s['unit']; $r = $s['result'];
  if (!$s['loaded']) {
    if (!$r) return ['red', 'Driver not loaded', 'The loader has not run since this boot (or the plugin is not installed correctly). It runs at boot and at plugin install; see syslog: grep synology-expansion /var/log/syslog'];
    switch ($r['code']) {
      case 'no_build': return ['red', 'Driver not loaded: no build for kernel ' . $s['kernel'], 'This plugin version has no driver build for the running Unraid kernel (builds in the plugin: ' . (implode(', ', $s['builds']) ?: 'none') . '). The expansion unit keeps its current fan level. A plugin update with a build for this kernel is needed.'];
      case 'no_dx': return ['orange', 'No expansion unit found', 'No SATA port with a port multiplier was found, so there is nothing for the driver to do. Is the DX1215 connected and powered on?'];
      default: return ['red', 'Driver not loaded: the loader refused', $r['text']];
    }
  }
  $master = $u['master'] ?? '';
  if ($master === '' || $master === 'none' || $master === 'ambiguous')
    return ['orange', 'Driver loaded, but the unit was not identified', 'No single port multiplier answered as the unit master (EMID 0, ID 0x13). Fan control is disabled.'];
  $f1 = ($u['fan1_fail'] ?? '0') === '1'; $f2 = ($u['fan2_fail'] ?? '0') === '1';
  if ($f1 || $f2)
    return ['red', 'Driver loaded: FAN ALARM on the ' . ($u['model'] ?? 'unit'), 'The unit reports ' . ($f1 && $f2 ? 'both fans' : ($f1 ? 'fan 1' : 'fan 2')) . ' as failed (stopped or blocked). Check the fans at the back of the unit.' . (($u['stop_test'] ?? '-') === 'active' ? ' A fan stop test is running right now, which causes this on purpose.' : '')];
  if (isset($s['checks']['oldplugin']))
    return ['orange', 'Driver loaded, but the old synology-dx plugin is still installed', 'Remove it (Plugins, synology-dx, Remove): both plugins load the same driver, and removing the old one later unloads it.'];
  if (($s['checks']['pch']['state'] ?? '') === 'fail' || ($s['checks']['ds_leds']['state'] ?? '') === 'fail' || ($s['checks']['dx_leds']['state'] ?? '') === 'fail')
    return ['orange', 'Driver loaded, but the bay LEDs are not fully working', 'See the LED lines in the list below: ' . implode('; ', array_map(fn($k) => $s['checks'][$k]['value'], array_filter(['ds_leds', 'pch', 'dx_leds'], fn($k) => ($s['checks'][$k]['state'] ?? '') === 'fail')))];
  $bad = array_filter($s['hosts'], fn($h) => ($h['page_state'] ?? '') !== 'ok');
  $err = $u['last_error'] ?? '-';
  if ($bad || ($err !== '' && $err !== '-'))
    return ['orange', 'Driver loaded, with a problem', $bad ? 'Reading the unit\'s registers did not complete on ' . implode(', ', array_keys($bad)) . '.' : 'Last error: ' . $err];
  return ['green', 'Driver loaded: ' . ($u['model'] ?? 'unit') . ' found and responding', 'All checks below passed.'];
}

function sdx_ata_range(array $hosts): string {
  $n = array_values(array_filter(array_map(fn($h) => (int)preg_replace('/\D/', '', $h['ata_port'] ?? ''), $hosts)));
  if (!$n) return '';
  sort($n);
  return count($n) > 1 ? "ata{$n[0]}&ndash;ata" . end($n) : "ata{$n[0]}";
}

/* the checklist: [state ok|fail|skip, what, measured value] */
function sdx_checklist(array $s): array {
  $c = $s['checks']; $u = $s['unit']; $hosts = $s['hosts']; $L = [];
  $ck = function ($k) use ($c) { return $c[$k] ?? null; };
  $st = fn($x) => $x ? $x['state'] : 'skip';
  if ($x = $ck('oldplugin')) $L[] = ['fail', 'Old plugin (synology-dx) removed', sdx_h($x['value'])];
  $b = $ck('build');
  $L[] = [$b ? $st($b) : (in_array($s['kernel'], $s['builds']) ? 'ok' : 'fail'), 'Driver build for the running kernel ' . sdx_h($s['kernel']),
          $b ? sdx_h($b['value']) : 'builds in the plugin: ' . sdx_h(implode(', ', $s['builds']) ?: 'none')];
  $x = $ck('sha256');   $L[] = [$st($x), 'Driver file checksum (sha256)', $x ? sdx_h($x['value']) : 'not checked'];
  $x = $ck('vermagic'); $L[] = [$st($x), 'Driver made for this kernel (vermagic)', $x ? sdx_h($x['value']) : 'not checked'];
  $x = $ck('hosts');
  if ($hosts) {
    $h0 = reset($hosts);
    $chip = (($h0['vendor'] ?? '') === '0x1b4b' && ($h0['device'] ?? '') === '0x9705') ? 'Marvell 88SM9705' : sdx_h(($h0['vendor'] ?? '?') . ':' . ($h0['device'] ?? '?'));
    $L[] = ['ok', 'Port multipliers of the expansion unit found', count($hosts) . ' (' . sdx_ata_range($hosts) . ', ' . $chip . ' rev ' . sdx_h($h0['rev'] ?? '?') . ', ' . sdx_h($h0['ports'] ?? '?') . ' ports each)'];
  } else {
    $L[] = [$st($x), 'Port multipliers of the expansion unit found', $x ? ($x['state'] === 'ok' ? 'SCSI hosts ' . sdx_h($x['value']) : sdx_h($x['value'])) : 'not checked'];
  }
  $x = $ck('kallsyms');
  $L[] = [$st($x), 'Kernel function address (ata_exec_internal)', $x ? sdx_h($x['value']) . ($s['loaded'] && $x['state'] === 'ok' ? ', confirmed by the driver itself at load' : '') : 'not checked'];
  $x = $ck('insmod');
  $L[] = [$s['loaded'] ? 'ok' : ($x ? $x['state'] : 'fail'), 'Driver loaded', $s['loaded'] ? 'synology_dx ' . sdx_h($s['version']) : ($x ? sdx_h($x['value']) : 'not loaded')];
  if (!$s['loaded']) return $L;
  $m = $u['master'] ?? ''; $mh = $hosts[$m] ?? null;
  $okm = $mh !== null;
  $L[] = [$okm ? 'ok' : 'fail', 'Unit master found (the port multiplier that carries the fan and status lines)', $okm ? sdx_h($mh['ata_port']) . ' (' . sdx_h($m) . '), EMID ' . sdx_h($mh['emid']) : sdx_h($m ?: 'none')];
  $L[] = [($u['model'] ?? 'unknown') !== 'unknown' && isset($u['model']) ? 'ok' : 'fail', 'Unit identified', sdx_h($u['model'] ?? 'unknown') . ' (ID ' . sdx_h($u['unique'] ?? '?') . ')'];
  $okp = array_filter($hosts, fn($h) => ($h['page_state'] ?? '') === 'ok');
  $L[] = [count($okp) === count($hosts) && $hosts ? 'ok' : 'fail', 'Status lines read, registers put back as found', count($okp) . ' of ' . count($hosts) . ' port multipliers ok'];
  $lv = $u['fan_level'] ?? ''; $p4 = $mh ? preg_replace('/ raw .*/', '', $mh['page4']) : '';
  $err = $u['last_error'] ?? '-';
  $L[] = [$lv !== '' && ($err === '-' || $err === '') ? 'ok' : 'fail', 'Fan level read from the unit', $lv !== '' ? sdx_h($lv) . ' (page 4 = ' . sdx_h($p4) . ')' . ($err !== '-' && $err !== '' ? ', last error: ' . sdx_h($err) : '') : 'no value'];
  // 2026.10.26a: the loader's LED lines, with the live value next to them
  $live = ['ds_leds' => $s['ds'] ? 'now: leds_enabled ' . implode(' ', array_map(fn($c) => $c['enabled'] !== '' ? $c['enabled'] : '?', $s['ds'])) : 'now: no 88SE9215 attached',
           'pch' => 'now: pch_led_enable ' . ($s['pch_on'] !== '' ? $s['pch_on'] : '?') . (($s['pch']['state'] ?? '') !== '' ? ', driver: ' . $s['pch']['state'] : ''),
           'dx_leds' => 'now: dx_leds_enabled ' . ($s['dx_leds'] !== '' ? $s['dx_leds'] : '?')];
  foreach (['ds_leds' => 'DS2415+ bay LEDs (green: 88SE9215 link/activity)', 'pch' => 'DS2415+ orange enable (PCH GPIO 49)', 'dx_leds' => 'DX1215 bay LEDs (port multiplier page 1)'] as $k => $what) {
    $x = $ck($k);
    $L[] = [$x ? $x['state'] : 'skip', $what, ($x ? sdx_h($x['value']) : 'not recorded by the loader') . ' <span class="grey-text">(' . sdx_h($live[$k]) . ')</span>'];
  }
  return $L;
}

function sdx_findings(array $s): array {
  $u = $s['unit']; $F = [];
  if (!$s['loaded'] || !isset($u['model'])) return $F;
  $spec = 'research/2026-10-25-dx1215-gpio-spec.md';
  $lvl = ['full' => 'full speed', 'middle' => 'middle speed', 'low' => 'low speed (the unit\'s power-on default; the lowest level at which the fans run)', 'stop' => 'STOPPED (fan stop test running)'];
  $al = fn($v) => $v === '1' ? '<span class="red-text">ALARM: stopped or blocked</span>' : '<span class="green-text">OK</span>';
  $F[] = ['Unit model', sdx_h($u['model']) . ' (ID ' . sdx_h($u['unique']) . ')', 'Synology\'s ID table in its kernel; ' . $spec . ' section 2.2'];
  $F[] = ['Fan level', sdx_h($lvl[$u['fan_level']] ?? $u['fan_level']), 'Bits 1:0 of status page 4; written and read back on this unit (experiments/2026-10-25-synology-dx-phase2b/RESULT.md). Set by the Synology Hardware plugin, or by hand in /sys/kernel/synology_dx/unit/fan_level'];
  $F[] = ['Fan 1', $al($u['fan1_fail'] ?? '0'), 'Page 2 bit 0; a stopped fan set it within 5 s (phase2b RESULT.md)'];
  $F[] = ['Fan 2', $al($u['fan2_fail'] ?? '0'), 'Page 2 bit 1; same test'];
  $F[] = ['Power', ($u['power_on'] ?? '') === '1' ? 'on' : sdx_h('bit = ' . ($u['power_on'] ?? '?')), 'Page 2 bit 5 (Synology: IsPowerOn); ' . $spec];
  $F[] = ['PSU 1 / PSU 2 bits', sdx_h(($u['psu1'] ?? '?') . ' / ' . ($u['psu2'] ?? '?')) . ' <span class="grey-text">meaning not yet confirmed</span>', 'Page 2 bits 6 and 7 (Synology: RedundantPowerStatGet). The DX1215 has one PSU and both read 0, so which value means "OK" is not known yet (experiments/2026-10-25-synology-dx-phase2a/RESULT.md)'];
  $sw = $u['switch_mode'] ?? '?';
  $F[] = ['Rear DEFAULT/MANUAL switch', ($sw === '1' ? 'DEFAULT' : ($sw === '0' ? 'MANUAL' : 'unknown')) . ' <span class="grey-text">(bit ' . sdx_h($sw) . ')</span>', 'Page 3 bit 0 (Synology: SwitchModeGet). Verified at the box on 2026-10-26: the switch on DEFAULT reads 1 (MANUAL = 0 follows; not toggled)'];
  return $F;
}

function sdx_render(array $s): string {
  [$lvl, $head, $why] = sdx_verdict($s);
  $icon = ['ok' => '<i class="fa fa-check green-text"></i>', 'fail' => '<i class="fa fa-times red-text"></i>', 'skip' => '<i class="fa fa-minus grey-text"></i>'];
  $o = '<p class="sdx-verdict"><i class="fa fa-circle orb ' . $lvl . '-orb middle"></i> <b class="' . $lvl . '-text">' . sdx_h($head) . '</b></p>';
  $o .= '<p class="whitespace-normal">' . sdx_h($why) . '</p>';
  if ($s['result']) $o .= '<p class="grey-text">Loader result: ' . sdx_h($s['result']['text']) . ' (' . sdx_h($s['checks']['started']['value'] ?? '') . ')</p>';
  $o .= '<div class="title"><span class="left"><i class="title fa fa-list-ul"></i>What was checked</span></div>';
  $o .= '<table class="unraid sdx-table"><thead><tr><td></td><td>Check</td><td>Result</td></tr></thead><tbody>';
  foreach (sdx_checklist($s) as [$st, $what, $val])
    $o .= '<tr><td>' . $icon[$st] . '</td><td>' . $what . '</td><td class="whitespace-normal">' . $val . '</td></tr>';
  $o .= '</tbody></table>';
  $F = sdx_findings($s);
  if ($F) {
    $o .= '<div class="title"><span class="left"><i class="title fa fa-server"></i>Findings</span></div>';
    $o .= '<table class="unraid sdx-table sdx-findings"><thead><tr><td>Item</td><td>Value</td><td>What it is, where it comes from</td></tr></thead><tbody>';
    foreach ($F as [$k, $v, $src]) $o .= '<tr><td>' . $k . '</td><td class="whitespace-normal">' . $v . '</td><td class="whitespace-normal grey-text">' . sdx_h($src) . '</td></tr>';
    $o .= '</tbody></table>';
  }
  if ($s['loaded']) $o .= sdx_bays_html($s);
  $raw = "Loader status: " . $s['status'] . "\n\nLoader checks (/var/run/synology-expansion/checks):\n" . ($s['checks_raw'] ?? "(none)\n")
       . "\nUnit (/sys/kernel/synology_dx/unit/summary):\n" . ($s['unit_raw'] ?? "(module not loaded)\n")
       . "\nds/pch:\n" . ($s['pch'] ? implode("\n", array_map(fn($k, $v) => "$k $v", array_keys($s['pch']), $s['pch'])) . "\n" : "(none)\n");
  foreach ($s['hosts'] as $n => $h)
    $raw .= "\n$n:\n" . $h['raw'] . "page_state " . $h['page_state'] . "\n" . implode("\n", array_map(fn($i) => "page$i " . $h["page$i"], range(0, 4))) . "\n";
  $o .= '<details class="sdx-raw"><summary>Raw details</summary><pre>' . htmlspecialchars($raw, ENT_QUOTES) . '</pre></details>';
  return $o;
}

/* the bay LED section (2026.10.26a) */
function sdx_bays_html(array $s): string {
  $B = sdx_bays($s);
  $led = function (string $st, string $colour, string $on_txt): string {
    switch ($st) {
      case 'on': return '<i class="fa fa-circle ' . $colour . '-text"></i> ' . $on_txt;
      case 'blocked': return '<i class="fa fa-circle-o orange-text"></i> fault bit set, but cannot light: PCH GPIO 49 is not high';
      case 'unknown': return '<i class="fa fa-question grey-text"></i> unknown';
      default: return '<i class="fa fa-circle-o grey-text"></i> off';
    }
  };
  $age = $s['health_at'] !== null ? max(0, time() - $s['health_at']) : null;
  $o = '<div class="title"><span class="left"><i class="title fa fa-lightbulb-o"></i>Bay LEDs</span></div>';
  $o .= '<p class="whitespace-normal">Green = presence/activity (the controller drives it once the LEDs are on). Orange = fault, set by the Synology Hardware plugin from disk health '
      . ($s['health_at'] !== null ? '(policy ' . sdx_h($s['health_policy'] ?: '?') . ', last evaluated ' . $age . ' s ago)' : '<span class="orange-text">(no health record: the Synology Hardware plugin is not running or not installed, so orange is never set)</span>')
      . '. Bay numbers as Synology prints them, confirmed at the box by blink count.</p>';
  foreach (['ds' => 'DS2415+ internal bays', 'dx' => 'DX1215 bays'] as $box => $title) {
    if (!$B[$box]) { $o .= '<p class="grey-text">' . $title . ': ' . ($box === 'ds' ? 'no 88SE9215 attached by the driver' : 'no expansion unit') . '.</p>'; continue; }
    $o .= '<table class="unraid sdx-table sdx-bays"><thead><tr><td>' . $title . '</td><td>Disk</td><td>Green</td><td>Orange</td><td>Health (Synology Hardware)</td></tr></thead><tbody>';
    foreach ($B[$box] as [, $bay, $dev, $gr, $or, $why])
      $o .= '<tr><td>Bay ' . (int)$bay . '</td><td>' . ($dev !== '' ? sdx_h($dev) : '<span class="grey-text">empty</span>') . '</td><td>' . $led($gr, 'green', 'on')
          . '</td><td>' . $led($or, 'orange', 'ON (fault)') . '</td><td class="whitespace-normal">' . ($why !== '' ? (strpos($why, 'FAULT: ') === 0 ? '<span class="orange-text">' . sdx_h($why) . '</span>' : sdx_h($why)) : '<span class="grey-text">' . ($dev !== '' ? 'no record' : '') . '</span>') . '</td></tr>';
    $o .= '</tbody></table>';
  }
  return $o;
}
