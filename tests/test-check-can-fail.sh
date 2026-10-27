#!/usr/bin/env bash
# Mutation tests for the write guards (phase 2a, 2b, 3; 0.6.0 PCH GPIO 49). Every mutant must be REJECTED; the unmodified
# source must PASS. Written: 2026-10-25 (phase 1), extended 2026-10-25 (phase 2a).
#   tests/test-check-can-fail.sh        source + encoding mutants (seconds)
#   tests/test-check-can-fail.sh --ko   also builds object mutants in docker (needs build/linux-6.18.58)
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../src" && pwd)
B=$(cd "$HERE/../build" && pwd)
W=$B/mutants; rm -rf "$W"; mkdir -p "$W"
rc=0
"$HERE/check-readonly.sh" src "$SRC" >/dev/null || { echo "FAIL: clean source rejected"; rc=1; }

mk() { # name, file (synology_dx.c|sdx_gpio.h), perl substitution
  mkdir -p "$W/$1"; cp "$SRC"/synology_dx.c "$SRC"/sdx_gpio.h "$SRC"/Kbuild "$W/$1/"
  perl -pi -e "$3" "$W/$1/$2"
  cmp -s "$SRC/$2" "$W/$1/$2" && { echo "FAIL: mutant $1 did not change $2"; rc=1; return 1; }
  return 0
}
src_mut() {
  mk "$@" || return
  if "$HERE/check-readonly.sh" src "$W/$1" >/dev/null; then echo "FAIL: src mutant $1 accepted"; rc=1;
  else echo "ok: src mutant $1 rejected"; fi
}
enc_mut() {
  mk "$1" sdx_gpio.h "$2" || return
  cp "$HERE/test-encoding.c" "$W/$1/"
  sed -i 's|#include "../src/sdx_gpio.h"|#include "sdx_gpio.h"|' "$W/$1/test-encoding.c"
  if gcc -o "$W/$1/t" "$W/$1/test-encoding.c" 2>/dev/null && "$W/$1/t" >/dev/null; then
    echo "FAIL: encoding mutant $1 accepted"; rc=1; else echo "ok: encoding mutant $1 rejected"; fi
}

# phase-1 style: command byte
src_mut hex_cmd        synology_dx.c 's/tf\.command = ATA_CMD_PMP_WRITE;/tf.command = 0xE8;/'
src_mut second_write   synology_dx.c 's/(tf\.command = ATA_CMD_PMP_READ;)/tf.command = ATA_CMD_PMP_WRITE;/'
src_mut other_cmd      synology_dx.c 's/tf\.command = ATA_CMD_PMP_READ;/tf.command = ATA_CMD_SET_FEATURES;/'
src_mut extra_exec     synology_dx.c 's/(\terr_mask = sdx_exec_internal\()/\tsdx_exec_internal(pmp_dev, &tf, NULL, DMA_NONE, NULL, 0, 1);\n$1/'
src_mut scr_write      synology_dx.c 's/(\tBUILD_BUG_ON\(ATA_CMD_PMP_READ != 0xE4\);)/$1\n\tsata_scr_write(&ap->link, 0, 0);/'
# phase-2a: the write guard
src_mut guard_off      synology_dx.c 's/ok = SDX_R_OK\(val\) \|\|/ok = 1 ||/'
src_mut en_any         synology_dx.c 's/ok = val == SDX_EN_ALL_OUT \|\| val == SDX_EN_READ;/ok = true;/'
src_mut other_reg_ok   synology_dx.c 's/\t\tok = false;/\t\tok = true;/'
src_mut guard_late     synology_dx.c 's/\tif \(!ok\) \{/\tif (0) {/'
src_mut write_reg_3b0  synology_dx.c 's/sdx_pmp_write_checked\(p, SDX_REG_GPIO_EN, SDX_EN_READ\)/sdx_pmp_write_checked(p, SDX_REG_GPIO_IN, SDX_EN_READ)/'
src_mut en_value_0     synology_dx.c 's/sdx_pmp_write_checked\(p, SDX_REG_GPIO_EN, SDX_EN_READ\)/sdx_pmp_write_checked(p, SDX_REG_GPIO_EN, 0)/'
src_mut page_data      synology_dx.c 's/sdx_gpio_out\(p, SDX_R_SEL\(pg, 0\)\);/sdx_gpio_out(p, SDX_R_SEL(pg, 0) | 2);/'
src_mut wctl_pulse     synology_dx.c 's/sdx_gpio_out\(p, SDX_R_SEL\(pg, 1\)\);/sdx_gpio_out(p, SDX_R_SEL(pg, 1) & ~SDX_R_WCTL);/'
src_mut direct_out     synology_dx.c 's/(\t\terr = sdx_pmp_write_checked\(p, SDX_REG_GPIO_EN, SDX_EN_READ\);)/\t\terr = sdx_pmp_write_checked(p, SDX_REG_GPIO_OUT, 0x1302);\n$1/'
src_mut sel_no_wctl    sdx_gpio.h    's/#define SDX_R_SEL\(p, rctl_low\)\t\(SDX_R_WCTL \| /#define SDX_R_SEL(p, rctl_low)\t(/'
src_mut ok_always      sdx_gpio.h    's/#define SDX_R_OK\(r\).*/#define SDX_R_OK(r)\t\t(1)/'
src_mut wctl_bit13     sdx_gpio.h    's/#define SDX_R_WCTL\t\t\(1u << 14\)/#define SDX_R_WCTL\t\t(1u << 13)/'
# phase-2b: page-4 write path
src_mut w_no_master    synology_dx.c 's/SDX_DEC\(p->page_raw\[0\]\) == SDX_DX1215_MASTER_P0 &&/1 \&\&/'
src_mut w_no_armed     synology_dx.c 's/\(p->wr_armed && p->pages_valid &&/(1 \&\& p->pages_valid \&\&/'
src_mut w_stop_always  synology_dx.c 's/p->stop_armed && time_before\(jiffies, p->stop_until\)/1/'
src_mut fw_no_bit4     synology_dx.c 's/if \(cur & SDX_P4_POWER_PULSE\) \{/if (0) {/'
src_mut fw_new_raw     synology_dx.c 's/new = SDX_P4_NEW\(cur, code\);/new = code;/'
src_mut fw_no_master   synology_dx.c 's/if \(SDX_DEC\(p->page_raw\[0\]\) != SDX_DX1215_MASTER_P0\) \{/if (0) {/'
src_mut fw_cur_word    synology_dx.c 's/sdx_gpio_out\(p, SDX_R_W4\(new, 1\)\)/sdx_gpio_out(p, SDX_R_W4(cur, 1))/'
src_mut fw_cur_clobber synology_dx.c 's/^(\tp->wr_armed = true;)/\tp->page_raw[4] = 0;\n$1/'
src_mut armed_in_read  synology_dx.c 's/^(\terr = sdx_gpio_out\(p, SDX_R_SEL\(pg, 0\)\);)/\tp->wr_armed = true;\n$1/'
src_mut level_stop     synology_dx.c 's/^(\telse if \(sysfs_streq\(buf, "low"\)\))/\telse if (sysfs_streq(buf, "stop"))\n\t\tcode = SDX_FAN_STOP;\n$1/'
src_mut stop_3600      synology_dx.c 's/secs > SDX_STOP_MAX_S\)/secs > 3600)/'
src_mut stop_max_600   synology_dx.c 's/^#define SDX_STOP_MAX_S\t60/#define SDX_STOP_MAX_S\t600/'
src_mut sched_after    synology_dx.c 's/^\tschedule_delayed_work\(&sdx_stop_work, secs \* HZ\);\n//; s/^(\trc = sdx_cycle\(m, SDX_FAN_STOP\);)/$1\n\tschedule_delayed_work(\&sdx_stop_work, secs * HZ);/'
src_mut stop_elsewhere synology_dx.c 's/^(\tsdx_cycle\(m, SDX_FAN_LOW\);)/\tsdx_cycle(m, SDX_FAN_STOP);\n$1/'
src_mut level_guard    synology_dx.c 's/if \(code != SDX_FAN_FULL && code != SDX_FAN_MIDDLE && code != SDX_FAN_LOW\)/if (code > SDX_FAN_STOP)/'
src_mut hwmon_stop     synology_dx.c 's/: SDX_FAN_LOW\);$/: SDX_FAN_STOP);/'
src_mut h_bit4_off     sdx_gpio.h    's/if \(\(d \| cur\) & SDX_P4_POWER_PULSE\)/if (0)/'
src_mut h_mask_off     sdx_gpio.h    's/if \(\(d \^ cur\) & ~SDX_P4_FAN_MASK\)/if (0)/'
src_mut h_stop_ok      sdx_gpio.h    's/if \(\(d & SDX_P4_FAN_MASK\) == SDX_FAN_STOP && !stop_ok\)/if (0)/'
src_mut h_master_0x33  sdx_gpio.h    's/#define SDX_DX1215_MASTER_P0\t0x13u/#define SDX_DX1215_MASTER_P0\t0x33u/'
# phase 3: page-1 (disk LED) write path
src_mut w1_no_armed    synology_dx.c 's/\(p->w1_armed && p->pages_valid && sdx_w1_ok/(1 \&\& p->pages_valid \&\& sdx_w1_ok/'
src_mut w1_guard_off   synology_dx.c 's/sdx_w1_ok\(val, SDX_DEC\(p->page_raw\[1\]\)\)\);/1);/'
src_mut lw_no_bit5     synology_dx.c 's/if \(cur & SDX_P1_BUZZER\) \{/if (0) {/'
src_mut lw_new_raw     synology_dx.c 's/new = SDX_P1_NEW\(cur, chmask, v\);/new = v;/'
src_mut lw_page4_word  synology_dx.c 's/err = sdx_gpio_out\(p, SDX_R_W1\(new, 1\)\);/err = sdx_gpio_out(p, SDX_R_W4(new, 1));/'
src_mut lw_page4_rb    synology_dx.c 's/err2 = sdx_read_page\(p, 1, &rb\);/err2 = sdx_read_page(p, 4, \&rb);/'
src_mut lw_wr_armed    synology_dx.c 's/^(\tp->w1_armed = true;)/$1\n\tp->wr_armed = true;/'
src_mut lw_bit5_word   synology_dx.c 's/err = sdx_gpio_out\(p, SDX_R_W1\(new, 0\)\);/err = sdx_gpio_out(p, SDX_R_W1(new | SDX_P1_BUZZER, 0));/'
src_mut lw_cur_clobber synology_dx.c 's/^(\tp->w1_armed = true;)/\tp->page_raw[1] = 0x60;\n$1/'
src_mut fw_page1       synology_dx.c 's/^(\tnew = SDX_P4_NEW\(cur, code\);)/$1\n\tp->w1_armed = true;/'
src_mut h_w1_bit5      sdx_gpio.h    's/if \(\(d \| cur\) & SDX_P1_BUZZER\)/if (0)/'
src_mut h_w1_bits347   sdx_gpio.h    's/#define SDX_P1_CHANGEABLE\t0x47u/#define SDX_P1_CHANGEABLE\t0xdfu/'
src_mut h_w1_mask_off  sdx_gpio.h    's/if \(\(d \^ cur\) & ~SDX_P1_CHANGEABLE\)/if (0)/'
# phase 3: DS2415+ 9215 vendor window
src_mut ds_guard_off   synology_dx.c 's/if \(!sdx_ds_addr_ok\(addr\)\) \{/if (0) {/'
src_mut ds_guard_late  synology_dx.c 's/^(\tif \(!sdx_ds_addr_ok\(addr\)\) \{)/\twritel(addr, c->mmio + SDX_DS_WIN_ADDR);\n$1/'
src_mut ds_other_addr  synology_dx.c 's/sdx_ds_reg\(c, SDX_DS_GPIO_OE, true, &w\);/sdx_ds_reg(c, 0x07071000, true, \&w);/'
src_mut ds_writel_out  synology_dx.c 's/^(\t\tc->mmio = hpriv->mmio;)/$1\n\t\twritel(0, c->mmio + SDX_DS_WIN_DATA);/'
src_mut ds_table       synology_dx.c 's/SDX_DS_GPIO_OUT, SDX_DS_GPIO_FUNC \};/SDX_DS_GPIO_OUT, 0x07071000 };/'
src_mut ds_fault_raw   synology_dx.c 's/w = SDX_DS_OUT_NEW\(cur, v\);/w = v;/'
src_mut ds_no_idx_rest synology_dx.c 's/^\twritel\(idx, c->mmio \+ SDX_DS_WIN_ADDR\);\n//'
src_mut h_ds_addr      sdx_gpio.h    's/#define SDX_DS_GPIO_FUNC\t0x07071058u/#define SDX_DS_GPIO_FUNC\t0x07071050u/'
src_mut h_ds_extra     sdx_gpio.h    's/a == SDX_DS_GPIO_FUNC;/a == SDX_DS_GPIO_FUNC || a == 0x07071000u;/'
# 0.6.0: PCH GPIO 49 byte write
src_mut pch_guard_off  synology_dx.c 's/if \(!sdx_pch\.region \|\| !sdx_pch_b2_ok\(v, sdx_pch\.load_b2\)\) \{/if (!sdx_pch.region) {/'
src_mut pch_pin_skip   synology_dx.c 's/if \(!sdx_pch_pin_ok\(use, io\)\) \{/if (0) {/'
src_mut pch_outb_put   synology_dx.c 's/^(\tif \(sdx_pch\.users\)$)/\toutb(0, sdx_pch.base + SDX_PCH_LVL2_B2);\n$1/'
src_mut pch_outl       synology_dx.c 's/outb\(v, sdx_pch\.base \+ SDX_PCH_LVL2_B2\);/outl(v, sdx_pch.base + SDX_PCH_LVL2);/'
src_mut pch_set_value  synology_dx.c 's/u8 v = sdx_pch\.load_b2 \| SDX_PCH_B2_BIT;/u8 v = 0xff;/'
src_mut pch_call_zero  synology_dx.c 's/sdx_pch_write\(sdx_pch\.load_b2\)/sdx_pch_write(0)/'
src_mut pch_no_region  synology_dx.c 's/if \(!request_region\(base \+ SDX_PCH_USE_SEL2, SDX_PCH_REGION, "synology_dx"\)\) \{/if (0) {/'
src_mut pch_load_clob  synology_dx.c 's/^(\tsdx_pch\.cur_b2 = sdx_pch\.load_b2;)/$1\n\tsdx_pch.load_b2 = 0xff;/'
src_mut pch_no_reread  synology_dx.c 's/^\tio = inl\(sdx_pch\.base \+ SDX_PCH_IO_SEL2\);/\tio = 0;/'
src_mut h_pch_bit      sdx_gpio.h    's/#define SDX_PCH_B2_BIT(\s+)0x02u/#define SDX_PCH_B2_BIT${1}0x06u/'
src_mut h_pch_port     sdx_gpio.h    's/#define SDX_PCH_LVL2_B2(\s+)0x8a/#define SDX_PCH_LVL2_B2${1}0x8b/'
src_mut h_pch_b2_any   sdx_gpio.h    's/ && !\(\(v \^ load\) & ~SDX_PCH_B2_BIT\);/;/'
src_mut h_pch_pin      sdx_gpio.h    's/ && !\(io_sel2 & SDX_PCH_GPIO49\);/;/'
# encoding test must catch header mutants too (independent of the grep)
enc_mut enc_w1_bit5    's/if \(\(d \| cur\) & SDX_P1_BUZZER\)/if (d \& 0)/'
enc_mut enc_w1_bits347 's/#define SDX_P1_CHANGEABLE\t0x47u/#define SDX_P1_CHANGEABLE\t0xc7u/'
enc_mut enc_w1_page    's/SDX_R_PAGE\(1\) \| SDX_ENC_DATA\(d\)\)/SDX_R_PAGE(4) | SDX_ENC_DATA(d))/'
enc_mut enc_p1_bit5    's/^\t ~SDX_P1_BUZZER & 0xffu\)/\t 0xffu)/'
enc_mut enc_ds_extra   's/a == SDX_DS_GPIO_FUNC;/a == SDX_DS_GPIO_FUNC || a == 0x07071000u;/'
enc_mut enc_ds_addr    's/#define SDX_DS_GPIO_FUNC\t0x07071058u/#define SDX_DS_GPIO_FUNC\t0x07071050u/'
enc_mut enc_ds_outnew  's/\(\(\(m\) << SDX_DS_FAULT_SHIFT\) & SDX_DS_FAULT_MASK\)\)/((m) << SDX_DS_FAULT_SHIFT))/'
enc_mut enc_no_wctl    's/#define SDX_R_SEL\(p, rctl_low\)\t\(SDX_R_WCTL \| /#define SDX_R_SEL(p, rctl_low)\t(/'
enc_mut enc_rctl_swap  's/\(\(rctl_low\) \? 0u : SDX_R_RCTL\)/((rctl_low) ? SDX_R_RCTL : 0u)/'
enc_mut enc_page_shift 's/\(\(\(p\) & 3u\) << 17\)/(((p) \& 3u) << 16)/'
enc_mut enc_w4_bit4     's/if \(\(d \| cur\) & SDX_P4_POWER_PULSE\)/if (d \& 0)/'
enc_mut enc_w4_mask     's/if \(\(d \^ cur\) & ~SDX_P4_FAN_MASK\)/if ((d ^ cur) \& 0x80u)/'
enc_mut enc_w4_stop     's/ \&\& !stop_ok\)/ \&\& 0)/'
enc_mut enc_w4_any_word 's/if \(val != SDX_R_W4\(d, 0\) && val != SDX_R_W4\(d, 1\)\)/if (0)/'
enc_mut enc_new_bit4    's/ & ~SDX_P4_POWER_PULSE\)$/)/'
enc_mut enc_pch_b2_any  's/ && !\(\(v \^ load\) & ~SDX_PCH_B2_BIT\);/;/'
enc_mut enc_pch_bit     's/#define SDX_PCH_B2_BIT(\s+)0x02u/#define SDX_PCH_B2_BIT${1}0x06u/'
enc_mut enc_pch_pin     's/ && !\(io_sel2 & SDX_PCH_GPIO49\);/;/'
enc_mut enc_pch_base    's/base >= 0x400u/base >= 0x80u/'
enc_mut enc_pch_gpio48  's/#define SDX_PCH_GPIO49(\s+)\(1u << 17\)/#define SDX_PCH_GPIO49${1}(1u << 16)/'
enc_mut enc_allow_data 's/#define SDX_R_ALLOWED\t\t\(SDX_R_WCTL \| /#define SDX_R_ALLOWED\t\t(0x1fu | SDX_R_WCTL | /'

if [ "${1:-}" = --ko ]; then
  IMG=synology-dx-build:gcc16.2.0; TREE=$B/linux-6.18.58
  ko_mut() { # name, perl on synology_dx.c: must build, and the object check must reject it
    mk "$1" synology_dx.c "$2" || return
    if ! docker run --rm --user "$(id -u):$(id -g)" -v "$B:$B" -w "$W/$1" "$IMG" \
         make -s -C "$TREE" M="$W/$1" modules >"$W/$1/build.log" 2>&1; then
      echo "FAIL: ko mutant $1 did not build (see $W/$1/build.log)"; rc=1; return; fi
    if "$HERE/check-readonly.sh" ko "$W/$1/synology_dx.ko" >/dev/null; then echo "FAIL: ko mutant $1 accepted"; rc=1;
    else echo "ok: ko mutant $1 rejected"; fi
  }
  ko_mut ko_guard_off  's/ok = SDX_R_OK\(val\) \|\|/ok = (val \& 1) ||/'
  ko_mut ko_en_any     's/ok = val == SDX_EN_ALL_OUT \|\| val == SDX_EN_READ;/ok = true;/'
  ko_mut ko_inline     's/static noinline __attribute__\(\(__noipa__\)\) unsigned int sdx_pmp_write_checked/static inline unsigned int sdx_pmp_write_checked/'
  ko_mut ko_no_master  's/SDX_DEC\(p->page_raw\[0\]\) == SDX_DX1215_MASTER_P0 &&/(p->hostno >= 0) \&\&/'
  ko_mut ko_read_is_write 's/tf\.command = ATA_CMD_PMP_READ;/tf.command = ATA_CMD_PMP_WRITE;/; s/BUILD_BUG_ON\(ATA_CMD_PMP_READ != 0xE4\);//'
  ko_mut ko_ds_guard_off 's/if \(!sdx_ds_addr_ok\(addr\)\) \{/if (addr == 1) {/'
  ko_mut ko_ds_extra     's/if \(!sdx_ds_addr_ok\(addr\)\) \{/if (!sdx_ds_addr_ok(addr) \&\& addr != 0x07071000u) {/'
  ko_mut ko_w1_guard_off 's/sdx_w1_ok\(val, SDX_DEC\(p->page_raw\[1\]\)\)\);/(val >> 16) == 2);/'
  ko_mut ko_pch_guard_off 's/!sdx_pch_b2_ok\(v, sdx_pch\.load_b2\)/(v == 1)/'
  ko_mut ko_pch_pin_off   's/if \(!sdx_pch_pin_ok\(use, io\)\) \{/if (use == 1 \&\& io == 2) {/'
  ko_mut ko_pch_outb_put  's/^(\tif \(sdx_pch\.users\)$)/\toutb(0, sdx_pch.base + SDX_PCH_LVL2_B2);\n$1/'
  ko_mut ko_pch_outl      's/outb\(v, sdx_pch\.base \+ SDX_PCH_LVL2_B2\);/outl(v, sdx_pch.base + SDX_PCH_LVL2);/'
fi
exit $rc
