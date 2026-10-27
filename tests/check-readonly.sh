#!/usr/bin/env bash
# Write guard for synology_dx: the only PMP WRITE (0xE8) path is sdx_pmp_write_checked(), it
# targets only 0x3a0/0x3a4, and it refuses (before building a taskfile) every value that is not
# 0xfffff/0xfc7c0 for 0x3a4, a W_CTL-high strobe/page-only word for 0x3a0, or (phase 2b) a
# page-4 data word accepted by sdx_w4_ok() (bit 4 never set, only bits 1:0 differ from the page 4
# just read, STOP only in the armed stop test) while armed, on the PMP whose page 0 is 0x13.
# No other write or reset primitive exists. Written: 2026-10-25 (phase 1), extended 2026-10-25
# (phase 2a, 2b).
#   check-readonly.sh src [dir]     static check of the C source (default dir: ../src)
#   check-readonly.sh ko  <file.ko> check of the built object
# Exit 0 = clean, 1 = violation.
set -u   # no pipefail: "printf | grep -q" would race into SIGPIPE (141) and fake a failure
HERE=$(cd "$(dirname "$0")" && pwd)
mode=${1:-src}; fail=0
bad() { echo "check-readonly: FAIL: $*"; fail=1; }
cnt() { grep -oE "$1" | wc -l; }

if [ "$mode" = src ]; then
  d=${2:-$HERE/../src}; [ "$d" = src ] && d=$HERE/../src
  # strip comments; keep code
  code=$(cat "$d"/synology_dx.c | sed -e 's://.*$::' | perl -0pe 's:/\*.*?\*/::gs')
  hdr=$(cat "$d"/sdx_gpio.h | sed -e 's://.*$::' | perl -0pe 's:/\*.*?\*/::gs')
  all="$code"$'\n'"$hdr"
  printf '%s\n' "$all" | grep -nEi '0x0*e8\b'                 && bad "literal 0xE8 in code"
  printf '%s\n' "$all" | grep -nE 'sata_pmp_write|PMP_WRITE_DMA' && bad "other PMP write symbol"
  printf '%s\n' "$all" | grep -nE 'ATA_CMD_[A-Z_]+' | grep -vE 'ATA_CMD_PMP_(READ|WRITE)\b' && bad "ATA command other than PMP READ/WRITE"
  printf '%s\n' "$all" | grep -nE '\.command\s*=' | grep -vE '\.command\s*=\s*ATA_CMD_PMP_(READ|WRITE);' && bad "taskfile command not a PMP READ/WRITE constant"
  [ "$(printf '%s\n' "$all" | cnt 'ATA_CMD_PMP_READ;')" = 1 ]  || bad "expected exactly one PMP READ taskfile"
  [ "$(printf '%s\n' "$all" | cnt 'ATA_CMD_PMP_WRITE')" = 1 ]  || bad "expected exactly one ATA_CMD_PMP_WRITE use"
  [ "$(printf '%s\n' "$all" | cnt 'sdx_exec_internal\(')" = 2 ] || bad "expected exactly two sdx_exec_internal() call sites"

  # the write function: guard before the taskfile, both inside the one function
  wf=$(printf '%s\n' "$code" | awk '/^static noinline __attribute__\(\(__noipa__\)\) unsigned int sdx_pmp_write_checked\(/,/^}/')
  [ -n "$wf" ] || bad "sdx_pmp_write_checked() (noinline noipa) not found"
  printf '%s\n' "$wf" | grep -q 'ATA_CMD_PMP_WRITE' || bad "ATA_CMD_PMP_WRITE outside sdx_pmp_write_checked()"
  [ "$(printf '%s\n' "$wf" | cnt 'sdx_exec_internal\(')" = 1 ] || bad "write function must call sdx_exec_internal exactly once"
  for g in 'if \(reg == SDX_REG_GPIO_OUT\)' 'ok = SDX_R_OK\(val\) \|\|' \
           '\(p->wr_armed && p->pages_valid &&' 'SDX_DEC\(p->page_raw\[0\]\) == SDX_DX1215_MASTER_P0 &&' \
           'sdx_w4_ok\(val, SDX_DEC\(p->page_raw\[4\]\),' 'p->stop_armed && time_before\(jiffies, p->stop_until\)\)\) \|\|' \
           '\(p->w1_armed && p->pages_valid && sdx_w1_ok\(val, SDX_DEC\(p->page_raw\[1\]\)\)\);' \
           'else if \(reg == SDX_REG_GPIO_EN\)' \
           'ok = val == SDX_EN_ALL_OUT \|\| val == SDX_EN_READ;' 'ok = false;' 'if \(!ok\) \{' 'return AC_ERR_INVALID;'; do
    printf '%s\n' "$wf" | grep -qE "^\s*(\} )?$g" || bad "write guard line missing: $g"
  done
  ln_ret=$(printf '%s\n' "$wf" | grep -nE 'return AC_ERR_INVALID;' | head -1 | cut -d: -f1)
  ln_cmd=$(printf '%s\n' "$wf" | grep -nE 'ATA_CMD_PMP_WRITE' | head -1 | cut -d: -f1)
  [ -n "$ln_ret" ] && [ -n "$ln_cmd" ] && [ "$ln_ret" -lt "$ln_cmd" ] || bad "guard does not precede the taskfile"
  # call sites: register is one of the two GPIO registers, values are the named ones
  printf '%s\n' "$code" | grep -nE 'sdx_pmp_write_checked\(' | grep -vE 'noipa' \
    | grep -vE 'sdx_pmp_write_checked\(p, SDX_REG_GPIO_EN, (SDX_EN_ALL_OUT|SDX_EN_READ|en)\)' \
    | grep -vE 'sdx_pmp_write_checked\(p, SDX_REG_GPIO_OUT, r\)' && bad "write call site with an unexpected register/value"
  printf '%s\n' "$code" | grep -nE 'sdx_gpio_out\(' | grep -vE '^[0-9]+:static unsigned int sdx_gpio_out' \
    | grep -vE 'sdx_gpio_out\(p, (SDX_R_SEL\(pg, [01]\)|out|SDX_R_W4\(new, [01]\)|SDX_R_W1\(new, [01]\))\)' && bad "sdx_gpio_out() with a value that is not a page select, the saved word or a page-4/page-1 word of new"
  # phase 2b: the page-4 write
  ff=$(printf '%s\n' "$code" | awk '/^static int sdx_fan_write\(/,/^}/')
  [ -n "$ff" ] || bad "sdx_fan_write() not found"
  for g in 'if \(SDX_DEC\(p->page_raw\[0\]\) != SDX_DX1215_MASTER_P0\) \{' 'if \(cur & SDX_P4_POWER_PULSE\) \{' \
           'new = SDX_P4_NEW\(cur, code\);' 'p->wr_armed = true;' 'p->wr_armed = false;'; do
    printf '%s\n' "$ff" | grep -qE "^\s*$g" || bad "fan write line missing: $g"
  done
  printf '%s\n' "$ff" | grep -qE 'SDX_R_W4\(new, 1\)' || bad "no W_CTL pulse word in sdx_fan_write (sequence changed?)"
  [ "$(printf '%s\n' "$code" | cnt 'wr_armed = true')" = 1 ] || bad "wr_armed set outside sdx_fan_write"
  printf '%s\n' "$code" | grep -nE 'SDX_R_W4\(' | grep -vE 'SDX_R_W4\(new, [01]\)' && bad "SDX_R_W4 used with something other than new"
  printf '%s\n' "$code" | grep -nE 'page_raw\[4\] =' | grep -vE 'p->page_raw\[4\] = rb;' && bad "page_raw[4] (the guard's cur) written outside the readback"
  # phase 3: the page-1 (disk LED) write never touches page 4, and vice versa
  lf=$(printf '%s\n' "$code" | awk '/^static int sdx_led_write\(/,/^}/')
  [ -n "$lf" ] || bad "sdx_led_write() not found"
  for g in 'if \(cur & SDX_P1_BUZZER\) \{' 'new = SDX_P1_NEW\(cur, chmask, v\);' 'p->w1_armed = true;' 'p->w1_armed = false;' \
           'err = sdx_gpio_out\(p, SDX_R_W1\(new, 0\)\);' 'err = sdx_gpio_out\(p, SDX_R_W1\(new, 1\)\);' \
           'err2 = sdx_read_page\(p, 1, &rb\);'; do
    printf '%s\n' "$lf" | grep -qE "^\s*$g" || bad "led write line missing: $g"
  done
  printf '%s\n' "$lf" | grep -nE 'SDX_R_W4|SDX_P4_|page_raw\[4\]|wr_armed|stop_|SDX_FAN_|sdx_read_page\(p, [^1]' && bad "page-1 LED write touches page-4 state"
  printf '%s\n' "$ff" | grep -nE 'SDX_R_W1|SDX_P1_|page_raw\[1\]|w1_armed' && bad "fan write touches page-1 state"
  [ "$(printf '%s\n' "$code" | cnt 'w1_armed = true')" = 1 ] || bad "w1_armed set outside sdx_led_write"
  printf '%s\n' "$code" | grep -nE 'SDX_R_W1\(' | grep -vE 'SDX_R_W1\(new, [01]\)' && bad "SDX_R_W1 used with something other than new"
  printf '%s\n' "$code" | grep -nE 'page_raw\[1\] =' | grep -vE 'p->page_raw\[1\] = rb;' && bad "page_raw[1] (the guard's cur) written outside the readback"
  printf '%s\n' "$code" | grep -nE 'sdx_cycle_req\([^)]*SDX_FAN' && bad "LED request combined with a fan request"
  # phase 3: DS2415+ 9215 vendor window, one checked accessor
  df=$(printf '%s\n' "$code" | awk '/^static noinline __attribute__\(\(__noipa__\)\) int sdx_ds_reg\(/,/^}/')
  [ -n "$df" ] || bad "sdx_ds_reg() (noinline noipa) not found"
  ln_g=$(printf '%s\n' "$df" | grep -nE '^\s*if \(!sdx_ds_addr_ok\(addr\)\) \{$' | head -1 | cut -d: -f1)
  ln_r=$(printf '%s\n' "$df" | grep -nE '^\s*return -EPERM;$' | head -1 | cut -d: -f1)
  ln_l=$(printf '%s\n' "$df" | grep -nE 'spin_lock_irqsave' | head -1 | cut -d: -f1)
  [ -n "$ln_g" ] && [ -n "$ln_r" ] && [ -n "$ln_l" ] && [ "$ln_g" -lt "$ln_r" ] && [ "$ln_r" -lt "$ln_l" ] || bad "9215 address guard does not precede the window access"
  [ "$(printf '%s\n' "$df" | cnt 'writel\(')" = 3 ] || bad "sdx_ds_reg: expected exactly three writel (index, data, index restore)"
  for g in 'idx = readl\(c->mmio \+ SDX_DS_WIN_ADDR\);' 'writel\(addr, c->mmio \+ SDX_DS_WIN_ADDR\);' \
           'writel\(\*val, c->mmio \+ SDX_DS_WIN_DATA\);' 'writel\(idx, c->mmio \+ SDX_DS_WIN_ADDR\);'; do
    printf '%s\n' "$df" | grep -qE "^\s*$g" || bad "9215 window line missing: $g"
  done
  nods=$(printf '%s\n' "$code" | awk '/^static noinline __attribute__\(\(__noipa__\)\) int sdx_(ds_reg|pch_write)\(/{s=1} !s{print} s&&/^}/{s=0}')
  printf '%s\n' "$nods" | grep -nE '\b(readl|writel|readw|readb|ioread(8|16|32)|pci_iomap|pcim_iomap|ioremap|pci_write_config_[a-z]+)\b' && bad "MMIO/config access outside sdx_ds_reg"
  printf '%s\n' "$code" | grep -nE 'sdx_ds_reg\(' | grep -vE 'noipa' \
    | grep -vE 'sdx_ds_reg\(c, (SDX_DS_GPIO_(OE|OUT|FUNC)|sdx_ds_addr\[i\]), (true|false), ' && bad "sdx_ds_reg call with an address that is not a named GPIO register"
  printf '%s\n' "$code" | grep -qE '^static const u32 sdx_ds_addr\[3\] = \{ SDX_DS_GPIO_OE, SDX_DS_GPIO_OUT, SDX_DS_GPIO_FUNC \};$' || bad "sdx_ds_addr table changed"
  printf '%s\n' "$code" | grep -qE '^\s*w = SDX_DS_OUT_NEW\(cur, v\);$' || bad "fault_mask write is not SDX_DS_OUT_NEW (bits 4-7 only)"
  for g in '#define SDX_DS_GPIO_OUT\s+0x07071020u' '#define SDX_DS_GPIO_OE\s+0x07071024u' '#define SDX_DS_GPIO_FUNC\s+0x07071058u' \
           '#define SDX_DS_WIN_ADDR\s+0xA8$' '#define SDX_DS_WIN_DATA\s+0xAC$' '#define SDX_DS_FUNC_DSM\s+0x00B6D8D1u' \
           'return a == SDX_DS_GPIO_OUT \|\| a == SDX_DS_GPIO_OE \|\| a == SDX_DS_GPIO_FUNC;$' \
           '#define SDX_P1_BUZZER\s+0x20u' '#define SDX_P1_CHANGEABLE\s+0x47u' \
           'if \(val != SDX_R_W1\(d, 0\) && val != SDX_R_W1\(d, 1\)\)' 'if \(\(d \| cur\) & SDX_P1_BUZZER\)' \
           'if \(\(d \^ cur\) & ~SDX_P1_CHANGEABLE\)'; do
    printf '%s\n' "$hdr" | grep -qE "^\s*$g" || bad "header phase-3 line missing: $g"
  done
  # 0.6.0: PCH GPIO 49 (orange enable), one checked byte write at GPIOBASE+0x8a
  pf=$(printf '%s\n' "$code" | awk '/^static noinline __attribute__\(\(__noipa__\)\) int sdx_pch_write\(/,/^}/')
  [ -n "$pf" ] || bad "sdx_pch_write() (noinline noipa) not found"
  ln_g=$(printf '%s\n' "$pf" | grep -nE '^\s*if \(!sdx_pch\.region \|\| !sdx_pch_b2_ok\(v, sdx_pch\.load_b2\)\) \{$' | head -1 | cut -d: -f1)
  ln_r=$(printf '%s\n' "$pf" | grep -nE '^\s*return -EPERM;$' | head -1 | cut -d: -f1)
  ln_p=$(printf '%s\n' "$pf" | grep -nE '^\s*if \(!sdx_pch_pin_ok\(use, io\)\) \{$' | head -1 | cut -d: -f1)
  ln_o=$(printf '%s\n' "$pf" | grep -nE '^\s*outb\(v, sdx_pch\.base \+ SDX_PCH_LVL2_B2\);$' | head -1 | cut -d: -f1)
  [ -n "$ln_g" ] && [ -n "$ln_r" ] && [ -n "$ln_p" ] && [ -n "$ln_o" ] && [ "$ln_g" -lt "$ln_r" ] && [ "$ln_r" -lt "$ln_p" ] && [ "$ln_p" -lt "$ln_o" ] \
    || bad "PCH write: guard, pin re-check and the one outb not in that order"
  [ "$(printf '%s\n' "$pf" | cnt '\bout[bwl]\(')" = 1 ] || bad "sdx_pch_write: expected exactly one outb"
  printf '%s\n' "$pf" | grep -nE '\b(outw|outl|iowrite(8|16|32)|writel)\b' && bad "sdx_pch_write: other write primitive"
  printf '%s\n' "$pf" | grep -qE '^\s*use = inl\(sdx_pch\.base \+ SDX_PCH_USE_SEL2\);$' || bad "PCH write does not re-read USE_SEL2"
  printf '%s\n' "$pf" | grep -qE '^\s*io = inl\(sdx_pch\.base \+ SDX_PCH_IO_SEL2\);$' || bad "PCH write does not re-read IO_SEL2"
  printf '%s\n' "$code" | grep -nE 'sdx_pch_write\(' | grep -vE 'noipa' | grep -vE 'sdx_pch_write\((v|sdx_pch\.load_b2)\)' && bad "sdx_pch_write call with a value that is not v or the load byte"
  printf '%s\n' "$code" | grep -qE '^\s*u8 v = sdx_pch\.load_b2 \| SDX_PCH_B2_BIT;$' || bad "PCH set value is not load byte | GPIO 49 bit"
  [ "$(printf '%s\n' "$code" | cnt 'sdx_pch\.load_b2 = inb\(base \+ SDX_PCH_LVL2_B2\);')" = 1 ] || bad "PCH load byte not read once from +0x8a"
  printf '%s\n' "$code" | grep -nE 'sdx_pch\.load_b2 =' | grep -vE 'sdx_pch\.load_b2 = inb\(base \+ SDX_PCH_LVL2_B2\);' && bad "PCH load byte (the guard's reference) written elsewhere"
  printf '%s\n' "$code" | grep -nE 'sdx_pch\.region = true;' | wc -l | grep -qx 1 || bad "PCH region set in more than one place"
  ln_rq=$(printf '%s\n' "$code" | grep -nE 'if \(!request_region\(base \+ SDX_PCH_USE_SEL2, SDX_PCH_REGION, "synology_dx"\)\) \{' | head -1 | cut -d: -f1)
  ln_rt=$(printf '%s\n' "$code" | grep -nE 'sdx_pch\.region = true;' | head -1 | cut -d: -f1)
  [ -n "$ln_rq" ] && [ -n "$ln_rt" ] && [ "$ln_rq" -lt "$ln_rt" ] || bad "PCH writes allowed without request_region first"
  for g in '#define SDX_PCH_LVL2_B2\s+0x8a\s' '#define SDX_PCH_B2_BIT\s+0x02u' '#define SDX_PCH_GPIO49\s+\(1u << 17\)' \
           '#define SDX_PCH_USE_SEL2\s+0x80$' '#define SDX_PCH_IO_SEL2\s+0x84$' '#define SDX_PCH_REGION\s+0x0c\s' \
           '#define SDX_PCH_BASE_MASK\s+0xff80u' '#define SDX_PCH_DEVICE\s+0x1f38\s' \
           'return v <= 0xffu && load <= 0xffu && !\(\(v \^ load\) & ~SDX_PCH_B2_BIT\);$' \
           'return \(use_sel2 & SDX_PCH_GPIO49\) && !\(io_sel2 & SDX_PCH_GPIO49\);$' \
           'return \(cfg & 1u\) && base >= 0x400u && base <= 0xff00u;$'; do
    printf '%s\n' "$hdr" | grep -qE "^\s*$g" || bad "header PCH line missing: $g"
  done
  # STOP: only from the stop test, bounded, with the revert scheduled before the write
  [ "$(printf '%s\n' "$code" | cnt 'sdx_cycle\(m, SDX_FAN_STOP\)')" = 1 ] || bad "STOP written from more than one place"
  [ "$(printf '%s\n' "$code" | cnt 'stop_armed = true')" = 1 ] || bad "stop_armed set in more than one place"
  sf=$(printf '%s\n' "$code" | awk '/^static ssize_t sdx_unit_store_stop\(/,/^}/')
  printf '%s\n' "$sf" | grep -qE 'secs > SDX_STOP_MAX_S\)' || bad "stop test length not bounded"
  printf '%s\n' "$sf" | grep -qE 'stop_until = jiffies \+ secs \* HZ;' || bad "stop deadline not secs"
  printf '%s\n' "$code" | grep -qE '^#define SDX_STOP_MAX_S\s+60$' || bad "SDX_STOP_MAX_S is not 60"
  l_sched=$(printf '%s\n' "$sf" | grep -nE 'schedule_delayed_work\(&sdx_stop_work, secs \* HZ\);' | head -1 | cut -d: -f1)
  l_stop=$(printf '%s\n' "$sf" | grep -nE 'sdx_cycle\(m, SDX_FAN_STOP\)' | head -1 | cut -d: -f1)
  [ -n "$l_sched" ] && [ -n "$l_stop" ] && [ "$l_sched" -lt "$l_stop" ] || bad "revert work not scheduled before the STOP write"
  printf '%s\n' "$code" | awk '/^static ssize_t sdx_unit_store_level\(/,/^}/' | grep -qE 'SDX_FAN_STOP|"stop"' && bad "fan_level accepts STOP"
  printf '%s\n' "$code" | awk '/^static int sdx_unit_set_level\(/,/^}/' | grep -qE '^\s*if \(code != SDX_FAN_FULL && code != SDX_FAN_MIDDLE && code != SDX_FAN_LOW\)$' || bad "sdx_unit_set_level (fan_level, hwmon pwm1) does not refuse STOP"
  printf '%s\n' "$code" | awk '/^static int sdx_hwmon_write\(/,/^}/' | grep -qE 'SDX_FAN_STOP' && bad "hwmon pwm1 can request STOP"
  # header: SDX_R_SEL always ORs W_CTL; SDX_R_OK requires it
  printf '%s\n' "$hdr" | grep -qE '#define SDX_R_SEL\(p, rctl_low\)\s+\(SDX_R_WCTL \| ' || bad "SDX_R_SEL does not start with SDX_R_WCTL"
  printf '%s\n' "$hdr" | grep -qE '#define SDX_R_WCTL\s+\(1u << 14\)' || bad "SDX_R_WCTL is not R bit 14"
  for g in '#define SDX_P4_FAN_MASK\s+0x03u' '#define SDX_P4_POWER_PULSE\s+0x10u' '#define SDX_FAN_STOP\s+3u' \
           '#define SDX_DX1215_MASTER_P0\s+0x13u' 'if \(val != SDX_R_W4\(d, 0\) && val != SDX_R_W4\(d, 1\)\)' \
           'if \(\(d \| cur\) & SDX_P4_POWER_PULSE\)' 'if \(\(d \^ cur\) & ~SDX_P4_FAN_MASK\)' \
           'if \(\(d & SDX_P4_FAN_MASK\) == SDX_FAN_STOP && !stop_ok\)' 'if \(cur > 0xffu\)'; do
    printf '%s\n' "$hdr" | grep -qE "^\s*$g" || bad "header guard line missing: $g"
  done
  printf '%s\n' "$hdr" | grep -qE '#define SDX_R_OK\(r\)\s+\(\(\(\(r\) & \(SDX_R_WCTL \| ~SDX_R_ALLOWED\)\) == SDX_R_WCTL\)\)' || bad "SDX_R_OK changed"
  printf '%s\n' "$nods"$'\n'"$hdr" | grep -nE '\b(sata_scr_write|sata_scr_write_flush|writel|writew|writeb|iowrite(8|16|32)|outb|outw|outl|ata_dev_set_feature|ata_tf_to_fis|sata_link_hardreset|ata_link_abort|ata_port_freeze)\b' && bad "write/reset primitive in code"
  [ $fail = 0 ] && echo "check-readonly: src OK ($d)"
  exit $fail
fi

if [ "$mode" = ko ]; then
  ko=$2
  allow='__fentry__ __x86_return_thunk __x86_indirect_thunk_rax __stack_chk_fail __ref_stack_chk_guard _printk sprint_symbol strncmp strcmp strchr snprintf sysfs_emit sysfs_emit_at strcpy sysfs_create_file_ns sysfs_remove_file_ns kstrtobool kobject_create_and_add kobject_init_and_add kobject_del kobject_put kernel_kobj kfree kmalloc_caches __kmalloc_cache_noprof __mutex_init mutex_lock mutex_unlock _raw_spin_lock_irqsave _raw_spin_unlock_irqrestore scsi_host_lookup scsi_host_put ata_port_schedule_eh ata_port_wait_eh ata_link_next pci_get_domain_bus_and_slot pci_read_config_dword __request_region __release_region ioport_resource const_current_task ahci_error_handler synchronize_rcu usleep_range_state hwmon_device_register_with_info hwmon_device_unregister cancel_delayed_work cancel_delayed_work_sync delayed_work_timer_fn queue_delayed_work_on system_percpu_wq jiffies kstrtouint param_ops_uint sized_strscpy sysfs_create_group sysfs_remove_group sysfs_streq pci_get_device pci_dev_get pci_dev_put param_ops_int param_ops_ulong param_ops_bool param_array_ops __warn_printk'
  for s in $(nm -u "$ko" | awk '{print $2}'); do
    case " $allow " in *" $s "*) ;; *) bad "unexpected import: $s";; esac
  done
  nm -u "$ko" | grep -qE ' (ata_exec_internal|sata_pmp_write|sata_pmp_read|ata_qc_issue)$' && bad "direct libata internal import"
  dis=$(objdump -d --no-show-raw-insn "$ko")
  # gcc merges tf.device (0x0f) and tf.command into one store, e.g. 'movw $0xe40f,0x14(%rsp)'
  fn_of() { printf '%s\n' "$dis" | awk -v pat="$1" '/^[0-9a-f]+ <.*>:$/{f=$2} $0 ~ pat {print f}' | sort -u; }
  wfn=$(fn_of 'mov[bw] +\$0xe8([0-9a-f]{2})?,')
  rfn=$(fn_of 'mov[bw] +\$0xe4([0-9a-f]{2})?,')
  [ "$wfn" = "<sdx_pmp_write_checked>:" ] || bad "0xe8 command store must be in sdx_pmp_write_checked only, found in: ${wfn:-nothing}"
  [ -n "$rfn" ] || bad "positive control failed: no 0xe4 command store found (object check blind)"
  body=$(printf '%s\n' "$dis" | awk '/^[0-9a-f]+ <sdx_pmp_write_checked>:$/{p=1;next} /^[0-9a-f]+ <.*>:$/{p=0} p')
  # 0x13 = master page 0, 0x8c000/0x88000 = page-4 word with W_CTL high/low, 0x10 = power pulse bit,
  # 0x2c000/0x28000 = page-1 word W_CTL high/low, 0xffffffb8 = ~0x47 (page-1 changeable), 0x20 = buzzer
  for imm in '\$0x3a0,' '\$0x3a4,' '\$0xfffff,' '\$0xfc7c0,' '\$0xfff17fff,' '\$0x4000,' \
             '\$0x13,' '\$0x8c000,' '\$0x88000,' '\$0x10,' '\$0x2c000,' '\$0x28000,' '\$0xffffffb8,' '\$0x20,'; do
    printf '%s\n' "$body" | grep -qE "$imm" || bad "write guard constant ${imm%,} missing in sdx_pmp_write_checked"
  done
  # phase 3: the 9215 accessor keeps its address allowlist in the object
  dbody=$(printf '%s\n' "$dis" | awk '/^[0-9a-f]+ <sdx_ds_reg>:$/{p=1;next} /^[0-9a-f]+ <.*>:$/{p=0} p')
  [ -n "$dbody" ] || bad "sdx_ds_reg not found in the object"
  # gcc 16.2 turns the 3-address compare into: lea -0x7071020(addr); cmp $0x38; bt with mask
  # bits {0, 4, 0x38} = 0x100000000000011 (= 0x07071020, 0x07071024, 0x07071058 exactly)
  for imm in '-0x7071020\(' 'cmp +\$0x38,' '\$0x100000000000011,' '0xa8\(' '0xac\('; do
    printf '%s\n' "$dbody" | grep -qE -e "$imm" || bad "9215 window/allowlist constant ${imm%,} missing in sdx_ds_reg"
  done
  # ... and no other address compare (an extra allowed address shows up as one more cmp/constant)
  cmps=$(printf '%s\n' "$dbody" | grep -oE 'cmp[bwlq]? +\$0x[0-9a-f]+' | awk '{print $2}' | sort -u | tr '\n' ' ')
  [ "$cmps" = '$0x0 $0x38 ' ] || bad "sdx_ds_reg compares against more than the allowlist: $cmps"
  k707=$(printf '%s\n' "$dbody" | grep -oE '0x707[0-9a-f]{4}' | sort -u | tr '\n' ' ')
  [ "$k707" = '0x7071020 ' ] || bad "sdx_ds_reg has other 0x707xxxx constants: $k707"
  # 0.6.0: the only port write (out) is in sdx_pch_write, which keeps its byte guard and pin re-check
  ofn=$(fn_of '	out +%(al|ax|eax),')
  [ "$ofn" = "<sdx_pch_write>:" ] || bad "port write (out) must be in sdx_pch_write only, found in: ${ofn:-nothing}"
  printf '%s\n' "$dis" | grep -qE '	out +%(ax|eax),' && bad "16/32-bit port write in the object"
  pbody=$(printf '%s\n' "$dis" | awk '/^[0-9a-f]+ <sdx_pch_write>:$/{p=1;next} /^[0-9a-f]+ <.*>:$/{p=0} p')
  # gcc 16.2: (v ^ load) & ~2 -> 'test $0xfd,%al'; pin check -> 'bt $0x11' on USE_SEL2 and IO_SEL2; port +0x8a
  for imm in 'test +\$0xfd,%al' '0x8a\(%r' '0x80\(%r' '0x84\(%r'; do
    printf '%s\n' "$pbody" | grep -qE -e "$imm" || bad "PCH guard/port constant ${imm} missing in sdx_pch_write"
  done
  [ "$(printf '%s\n' "$pbody" | grep -cE 'bt +\$0x11,')" = 2 ] || bad "sdx_pch_write: GPIO 49 (bit 17) not checked in both USE_SEL2 and IO_SEL2"
  [ "$(printf '%s\n' "$pbody" | grep -cE '	in +\(%dx\)')" = 2 ] || bad "sdx_pch_write: expected two port reads (USE_SEL2, IO_SEL2) before the write"
  [ $fail = 0 ] && echo "check-readonly: ko OK ($ko): 0xe8 only in sdx_pmp_write_checked ($(printf '%s\n' "$body" | wc -l) insns, guard constants present); 0xe4 in $rfn"
  exit $fail
fi
echo "usage: $0 src [dir] | ko <file.ko>"; exit 2
