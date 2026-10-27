// SPDX-License-Identifier: GPL-2.0
/*
 * synology_dx - Synology DX1215 expansion unit (4x Marvell 88SM9705 port multiplier)
 *
 * PHASE 2b: GPIO PAGE READS + FAN LEVEL WRITES. Every PMP WRITE (0xE8) goes through
 * sdx_pmp_write_checked(), which refuses (without sending) anything but
 *   - 0x3a4 (GPIO output enable) = 0xfffff or 0xfc7c0 (Synology's two values),
 *   - 0x3a0 with W_CTL (R bit 14) high and only strobe/page bits (page select for reads), or
 *   - a page-4 data word (sdx_w4_ok, src/sdx_gpio.h) while sdx_fan_write() is armed, on the
 *     PMP whose page 0 read in the same EH round is 0x13 (EMID 0, DX1215 = unit master):
 *     data = the page 4 byte read in that round with only bits 1:0 replaced, bit 4 (power
 *     pulse) never set, code 3 (STOP) only during the time-limited stop test.
 * Page read = Synology's sequence (gpio-spec 2.3/3.2); page write = Synology's
 * SYNOEBox9705WriteGpio (W(V|0x300), W_CTL low pulse, W(V|0x300)), followed by restoring
 * 0x3a0/0x3a4 and reading page 4 back. tests/check-readonly.sh enforces the write path on
 * source and object.
 *
 * What it does, per DX host given in hosts=:
 *   - refuses to load unless the running kernel is the one it was built for, the address
 *     passed in exec_internal= is exactly ata_exec_internal+0x0 (sprint_symbol), and every
 *     host is an ahci port whose error_handler is ahci_error_handler with an enabled
 *     1b4b:9705 port multiplier attached;
 *   - points ap->ops at a module-owned copy of the original ops whose .error_handler is a
 *     wrapper; register reads run inside libata EH (the context in which libata itself
 *     talks to GSCRs), requested with ata_port_schedule_eh() + ata_port_wait_eh();
 *   - in a "quiet" EH round that this module scheduled (no error, no action, nothing in
 *     flight, not frozen) it reads GSCR 0,1,2,32,33,64,96 and vendor registers 0x3a0,
 *     0x3a4, 0x3b0, then GPIO pages 0..4, and does NOT call the original handler (which would hard-reset the
 *     disabled fan-out link 5 and re-check GSCR_ERROR). Any other EH round goes to the
 *     original handler unchanged. If a read times out (libata freezes the port), the
 *     original handler is called in the same round to recover, exactly as libata does
 *     after its own internal command times out;
 *   - exposes the values in /sys/kernel/synology_dx/hostN/ and dumps them to dmesg.
 *   - on unload restores the original ops pointer and waits for EH to finish.
 *
 * Design: research/2026-10-25-dx1215-kernel-design.md (option b).
 * Written: 2026-10-25.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kallsyms.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/utsname.h>
#include <linux/rcupdate.h>
#include <linux/libata.h>
#include <generated/utsrelease.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/hwmon.h>
#include <linux/err.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/spinlock.h>
#include <scsi/scsi_host.h>

#include "sdx_gpio.h"

/* drivers/ata/libata.h of the exact source tree: gives the real prototype of
 * ata_exec_internal, so a prototype change is a compile error (typeof below). */
#include "libata.h"
/* ahci_error_handler: exported by libahci; used to verify what we wrap. */
#include "ahci.h"

#define SDX_MAX_HOSTS	8
#define SDX_ATTEMPTS	3

/* GSCRs read (same set libata reads at attach, sata_pmp_read_gscr) and vendor registers. */
static const u16 sdx_regs[] = { 0, 1, 2, 32, 33, 64, 96,
				 SDX_REG_GPIO_OUT, SDX_REG_GPIO_EN, SDX_REG_GPIO_IN };
#define SDX_NREGS ARRAY_SIZE(sdx_regs)
#define SDX_IDX_OUT	7
#define SDX_IDX_EN	8

static int hosts[SDX_MAX_HOSTS];
static int nr_hosts;
module_param_array(hosts, int, &nr_hosts, 0444);
MODULE_PARM_DESC(hosts, "SCSI host numbers of the DX1215 PMP ports (e.g. 18,19,20,21)");

static unsigned long exec_internal;
module_param(exec_internal, ulong, 0444);
MODULE_PARM_DESC(exec_internal, "address of ata_exec_internal from /proc/kallsyms (0x...)");

static bool validate_only;
module_param(validate_only, bool, 0444);
MODULE_PARM_DESC(validate_only, "1 = run the load checks only: no ops swap, no ATA command");

static unsigned int refresh_s = 30;
module_param(refresh_s, uint, 0444);
MODULE_PARM_DESC(refresh_s, "max age in s of the unit values before a read re-reads the master (1..30)");

static bool ds_leds;
module_param(ds_leds, bool, 0444);
MODULE_PARM_DESC(ds_leds, "1 = DS2415+ internal bay LEDs: attach to the 1b4b:9215 controllers (read-only until leds_enabled/fault_mask is written)");

static typeof(ata_exec_internal) *sdx_exec_internal;

struct sdx_port {
	struct ata_port_operations ops;		/* must stay first: container_of from ap->ops */
	struct ata_port_operations *orig_ops;
	struct ata_port *ap;
	struct Scsi_Host *shost;
	int hostno;
	bool swapped;

	struct mutex lock;			/* one read cycle at a time; guards results */
	/* protected by ap->lock */
	bool want;				/* a read cycle is waiting for an EH round */
	bool served;				/* the handler ran the reads for this request */
	/* written by the EH handler only while a cycle holds ->lock */
	u32 val[SDX_NREGS];
	unsigned int err[SDX_NREGS];		/* AC_ERR_* mask, 0 = ok */
	bool valid;				/* at least one cycle completed */
	unsigned int cycles, not_quiet;
	char last_error[160];
	/* GPIO pages, written by the EH handler only while a cycle holds ->lock */
	u32 page_raw[SDX_NPAGES];		/* 0x3b0 word read for each page */
	bool pages_valid;			/* all five pages read in the last cycle */
	bool restored;				/* 0x3a0/0x3a4 read back equal to the saved values */
	char page_state[96];			/* "ok" or why pages were not read */
	/* phase 2b fan write, set by a cycle before scheduling, used by the EH handler */
	int fan_req;				/* -1 none, else SDX_FAN_* code */
	int fan_result;				/* 0 ok, 1 none requested, else error */
	char fan_error[128];
	bool wr_armed;				/* sdx_fan_write() is inside its write sequence */
	bool stop_armed;			/* STOP allowed until stop_until */
	unsigned long stop_until;
	bool wrote;				/* a page-4 write was sent since load */
	int load_code;				/* fan code read at load (restored at rmmod) */
	unsigned long stamp;			/* jiffies of the last complete cycle */
	/* phase 3 page-1 (disk LED) write, set by a cycle before scheduling */
	int led_chmask;				/* -1 none, else bits (within 0x47) to set from led_val */
	unsigned int led_val;
	int led_result;				/* 0 ok, 1 none requested, else error */
	char led_error[128];
	bool w1_armed;				/* sdx_led_write() is inside its write sequence */
	bool led_wrote;				/* a page-1 write was sent since load */
	int load_p1;				/* page 1 byte read at load (restored at rmmod), -1 unread */

	struct kobject kobj;
	bool kobj_added;
};

static const char *const sdx_fan_code_name[4] = { "full", "middle", "low", "stop" };

static struct sdx_port *sdx_ports[SDX_MAX_HOSTS];
static struct kobject *sdx_root;

/* ------------------------------------------------------------------ ATA side -- */

/* PMP READ of a 16-bit register on the control port (15). Mirrors sata_pmp_read()
 * (drivers/ata/libata-pmp.c) except that the register's high byte goes to
 * hob_feature (mainline truncates to 8 bits). ATA_CMD_PMP_READ is the only command
 * this module ever builds. */
static unsigned int sdx_pmp_read(struct ata_port *ap, u16 reg, u32 *r_val)
{
	struct ata_device *pmp_dev = ap->link.device;
	struct ata_taskfile tf;
	unsigned int err_mask;

	BUILD_BUG_ON(ATA_CMD_PMP_READ != 0xE4);

	ata_tf_init(pmp_dev, &tf);
	tf.command = ATA_CMD_PMP_READ;
	tf.protocol = ATA_PROT_NODATA;
	tf.flags |= ATA_TFLAG_ISADDR | ATA_TFLAG_DEVICE | ATA_TFLAG_LBA48;
	tf.feature = reg & 0xff;
	tf.hob_feature = reg >> 8;
	tf.device = SATA_PMP_CTRL_PORT;

	err_mask = sdx_exec_internal(pmp_dev, &tf, NULL, DMA_NONE, NULL, 0,
				     SATA_PMP_RW_TIMEOUT);
	if (err_mask)
		return err_mask;

	*r_val = tf.nsect | tf.lbal << 8 | tf.lbam << 16 | tf.lbah << 24;
	return 0;
}

/* The ONLY place a PMP WRITE is built. Refuses, without sending anything, every write
 * that is not an output-enable value Synology uses, a 0x3a0 word with W_CTL high and
 * only strobe/page bits (SDX_R_OK), or (phase 2b) a page-4 data word that sdx_w4_ok()
 * accepts against the page 4 read in this same cycle, while a fan write is armed, on the
 * PMP whose page 0 read in this cycle is exactly 0x13 (EMID 0, DX1215). STOP only while
 * the time-limited stop test is armed and not expired.
 * noipa: keep the run-time guard even though every current caller passes allowed values
 * (otherwise gcc's IPA drops the register compare), and keep it out of line so
 * tests/check-readonly.sh can find it in the object. */
static noinline __attribute__((__noipa__)) unsigned int sdx_pmp_write_checked(struct sdx_port *p, u16 reg, u32 val)
{
	struct ata_device *pmp_dev = p->ap->link.device;
	struct ata_taskfile tf;
	bool ok;

	if (reg == SDX_REG_GPIO_OUT)
		ok = SDX_R_OK(val) ||
		     (p->wr_armed && p->pages_valid &&
		      SDX_DEC(p->page_raw[0]) == SDX_DX1215_MASTER_P0 &&
		      sdx_w4_ok(val, SDX_DEC(p->page_raw[4]),
				p->stop_armed && time_before(jiffies, p->stop_until))) ||
		     (p->w1_armed && p->pages_valid && sdx_w1_ok(val, SDX_DEC(p->page_raw[1])));
	else if (reg == SDX_REG_GPIO_EN)
		ok = val == SDX_EN_ALL_OUT || val == SDX_EN_READ;
	else
		ok = false;
	if (!ok) {
		WARN_ONCE(1, "synology_dx: refused PMP write reg 0x%x val 0x%x\n", reg, val);
		return AC_ERR_INVALID;
	}

	ata_tf_init(pmp_dev, &tf);
	tf.command = ATA_CMD_PMP_WRITE;
	tf.protocol = ATA_PROT_NODATA;
	tf.flags |= ATA_TFLAG_ISADDR | ATA_TFLAG_DEVICE | ATA_TFLAG_LBA48;
	tf.feature = reg & 0xff;
	tf.hob_feature = reg >> 8;
	tf.device = SATA_PMP_CTRL_PORT;
	tf.nsect = val & 0xff;
	tf.lbal = (val >> 8) & 0xff;
	tf.lbam = (val >> 16) & 0xff;
	tf.lbah = (val >> 24) & 0xff;

	return sdx_exec_internal(pmp_dev, &tf, NULL, DMA_NONE, NULL, 0, SATA_PMP_RW_TIMEOUT);
}

/* Synology's syno_pm_gpio store (gpio-spec 3.2): output enable all-out, then 0x3a0. */
static unsigned int sdx_gpio_out(struct sdx_port *p, u32 r)
{
	unsigned int err;

	err = sdx_pmp_write_checked(p, SDX_REG_GPIO_EN, SDX_EN_ALL_OUT);
	if (!err)
		err = sdx_pmp_write_checked(p, SDX_REG_GPIO_OUT, r);
	usleep_range(1000, 2000);
	return err;
}

/* Restore 0x3a0/0x3a4 to the values read before the cycle and read them back. */
static unsigned int sdx_gpio_restore(struct sdx_port *p)
{
	struct ata_port *ap = p->ap;
	u32 out = p->val[SDX_IDX_OUT], en = p->val[SDX_IDX_EN], r_out, r_en;
	unsigned int err;

	err = sdx_gpio_out(p, out);
	if (!err)
		err = sdx_pmp_write_checked(p, SDX_REG_GPIO_EN, en);
	if (!err)
		err = sdx_pmp_read(ap, SDX_REG_GPIO_OUT, &r_out);
	if (!err)
		err = sdx_pmp_read(ap, SDX_REG_GPIO_EN, &r_en);
	p->restored = !err && r_out == out && r_en == en;
	if (!err && !p->restored)
		snprintf(p->page_state, sizeof(p->page_state),
			 "restore readback 0x3a0=0x%x 0x3a4=0x%x, saved 0x%x/0x%x", r_out, r_en,
			 out, en);
	return err;
}

/* One page read (gpio-spec 2.3): W(V|0x300), W(V|0x100), EN=0xfc7c0 + read 0x3b0,
 * W(V|0x300). No W_CTL pulse, no data bits. */
static unsigned int sdx_read_page(struct sdx_port *p, int pg, u32 *raw)
{
	unsigned int err;

	err = sdx_gpio_out(p, SDX_R_SEL(pg, 0));
	if (!err)
		err = sdx_gpio_out(p, SDX_R_SEL(pg, 1));
	if (!err)
		err = sdx_pmp_write_checked(p, SDX_REG_GPIO_EN, SDX_EN_READ);
	if (!err)
		err = sdx_pmp_read(p->ap, SDX_REG_GPIO_IN, raw);
	if (!err)
		err = sdx_gpio_out(p, SDX_R_SEL(pg, 0));
	return err;
}

/* Pages 0..4, then restore. Needs the GSCR/raw reads done. */
static void sdx_read_pages(struct sdx_port *p)
{
	struct ata_port *ap = p->ap;
	u32 out = p->val[SDX_IDX_OUT], en = p->val[SDX_IDX_EN];
	unsigned int err = 0;
	int pg;

	BUILD_BUG_ON(SDX_R_SEL(3, 0) != 0x6c000);	/* measured idle 0x3a0 */
	BUILD_BUG_ON(SDX_R_SEL(4, 0) != SDX_ENC(0x1300));
	BUILD_BUG_ON(SDX_R_SEL(4, 1) != SDX_ENC(0x1100));
	BUILD_BUG_ON(SDX_R_ALLOWED != 0xec000);

	p->pages_valid = false;
	p->restored = false;
	if (!SDX_R_OK(out) || (en != SDX_EN_ALL_OUT && en != SDX_EN_READ)) {
		snprintf(p->page_state, sizeof(p->page_state),
			 "skipped: idle 0x3a0=0x%x 0x3a4=0x%x not restorable", out, en);
		return;
	}

	for (pg = 0; pg < SDX_NPAGES && !err; pg++)
		err = sdx_read_page(p, pg, &p->page_raw[pg]);
	if (err) {
		snprintf(p->page_state, sizeof(p->page_state),
			 "page %d failed: err_mask 0x%x", pg - 1, err);
		if (!ata_port_is_frozen(ap))
			sdx_gpio_restore(p);
		return;
	}
	err = sdx_gpio_restore(p);
	if (err) {
		snprintf(p->page_state, sizeof(p->page_state), "restore failed: err_mask 0x%x", err);
		return;
	}
	p->pages_valid = true;
	if (p->restored)
		snprintf(p->page_state, sizeof(p->page_state), "ok");
}

/* Fan code write on page 4 (gpio-spec 1.2 + 2.3 SYNOEBox9705WriteGpio), then readback.
 * Only after a clean page read in this same EH round, only on the master. 0 = written and
 * read back equal; else an AC_ERR mask or -1 (refused before sending / mismatch). */
static int sdx_fan_write(struct sdx_port *p, unsigned int code)
{
	unsigned int cur, new, err, err2;
	u32 rb = 0;

	if (!p->pages_valid || !p->restored) {
		snprintf(p->fan_error, sizeof(p->fan_error), "no clean page read this cycle");
		return -1;
	}
	if (SDX_DEC(p->page_raw[0]) != SDX_DX1215_MASTER_P0) {
		snprintf(p->fan_error, sizeof(p->fan_error), "not the master (page0 0x%02x)",
			 SDX_DEC(p->page_raw[0]));
		return -1;
	}
	cur = SDX_DEC(p->page_raw[4]);
	if (cur & SDX_P4_POWER_PULSE) {
		snprintf(p->fan_error, sizeof(p->fan_error), "page4 0x%02x has bit 4 set: refusing", cur);
		return -1;
	}
	new = SDX_P4_NEW(cur, code);

	/* Synology: W(V|0x300), W((V|0x300) & ~0x100) = W_CTL pulse, W(V|0x300) */
	p->wr_armed = true;
	err = sdx_gpio_out(p, SDX_R_W4(new, 0));
	if (!err)
		err = sdx_gpio_out(p, SDX_R_W4(new, 1));
	if (!err)
		err = sdx_gpio_out(p, SDX_R_W4(new, 0));
	p->wr_armed = false;
	p->wrote = true;

	if (ata_port_is_frozen(p->ap)) {
		snprintf(p->fan_error, sizeof(p->fan_error), "write 0x%02x: err_mask 0x%x, port frozen", new, err);
		return err ? err : -1;
	}
	/* back to the idle strobes/page, then read page 4 back */
	err2 = sdx_gpio_restore(p);
	if (!err2)
		err2 = sdx_read_page(p, 4, &rb);
	if (!err2)
		err2 = sdx_gpio_restore(p);
	if (err || err2) {
		snprintf(p->fan_error, sizeof(p->fan_error), "write 0x%02x: err_mask 0x%x / readback 0x%x",
			 new, err, err2);
		return err ? err : err2;
	}
	p->page_raw[4] = rb;
	if (SDX_DEC(rb) != new) {
		snprintf(p->fan_error, sizeof(p->fan_error), "wrote 0x%02x, read back 0x%02x", new,
			 SDX_DEC(rb));
		return -1;
	}
	return 0;
}

/* Page-1 (disk LED) write, same sequence as the page-4 write, then readback. Only after a
 * clean page read in this same EH round. new = page 1 just read with only the bits of
 * chmask (within 0-2 and 6) replaced, bit 5 (buzzer pulse) always 0. 0 = written and read
 * back equal; else an AC_ERR mask or -1. Never touches page 4. */
static int sdx_led_write(struct sdx_port *p, unsigned int chmask, unsigned int v)
{
	unsigned int cur, new, err, err2;
	u32 rb = 0;

	if (!p->pages_valid || !p->restored) {
		snprintf(p->led_error, sizeof(p->led_error), "no clean page read this cycle");
		return -1;
	}
	cur = SDX_DEC(p->page_raw[1]);
	if (cur & SDX_P1_BUZZER) {
		snprintf(p->led_error, sizeof(p->led_error), "page1 0x%02x has bit 5 set: refusing", cur);
		return -1;
	}
	new = SDX_P1_NEW(cur, chmask, v);
	if (new == cur)
		return 0;

	p->w1_armed = true;
	err = sdx_gpio_out(p, SDX_R_W1(new, 0));
	if (!err)
		err = sdx_gpio_out(p, SDX_R_W1(new, 1));
	if (!err)
		err = sdx_gpio_out(p, SDX_R_W1(new, 0));
	p->w1_armed = false;
	p->led_wrote = true;

	if (ata_port_is_frozen(p->ap)) {
		snprintf(p->led_error, sizeof(p->led_error), "write 0x%02x: err_mask 0x%x, port frozen", new, err);
		return err ? err : -1;
	}
	err2 = sdx_gpio_restore(p);
	if (!err2)
		err2 = sdx_read_page(p, 1, &rb);
	if (!err2)
		err2 = sdx_gpio_restore(p);
	if (err || err2) {
		snprintf(p->led_error, sizeof(p->led_error), "write 0x%02x: err_mask 0x%x / readback 0x%x",
			 new, err, err2);
		return err ? err : err2;
	}
	p->page_raw[1] = rb;
	if (SDX_DEC(rb) != new) {
		snprintf(p->led_error, sizeof(p->led_error), "wrote 0x%02x, read back 0x%02x", new,
			 SDX_DEC(rb));
		return -1;
	}
	return 0;
}

/* Called with ap->lock held. True if this EH round has nothing for libata to do. */
static bool sdx_round_is_quiet(struct ata_port *ap)
{
	struct ata_device *pmp_dev = ap->link.device;
	struct ata_queued_cmd *qc;
	struct ata_link *link;
	unsigned int tag;
	int i;

	if (ata_port_is_frozen(ap) || ap->qc_active)
		return false;
	if (ap->pflags & (ATA_PFLAG_EH_PENDING | ATA_PFLAG_RESUMING | ATA_PFLAG_LOADING |
			  ATA_PFLAG_UNLOADING | ATA_PFLAG_UNLOADED | ATA_PFLAG_SUSPENDED |
			  ATA_PFLAG_INITIALIZING))
		return false;
	if (!sata_pmp_attached(ap) || pmp_dev->class != ATA_DEV_PMP ||
	    !ata_dev_enabled(pmp_dev))
		return false;

	ata_for_each_link(link, ap, HOST_FIRST) {
		struct ata_eh_info *ehi = &link->eh_context.i;

		if (ehi->err_mask || ehi->action || ehi->serror || ehi->flags)
			return false;
		for (i = 0; i < ATA_MAX_DEVICES; i++)
			if (ehi->dev_action[i])
				return false;
		/* errors that arrived after this round started */
		if (link->eh_info.err_mask || link->eh_info.action)
			return false;
	}

	ata_qc_for_each_raw(ap, qc, tag)
		if (qc->flags & (ATA_QCFLAG_EH | ATA_QCFLAG_ACTIVE))
			return false;

	return true;
}

static void sdx_error_handler(struct ata_port *ap)
{
	struct sdx_port *p = container_of(ap->ops, struct sdx_port, ops);
	unsigned long flags;
	bool serve = false, read_failed = false;
	unsigned int i;

	spin_lock_irqsave(ap->lock, flags);
	if (p->want) {
		if (sdx_round_is_quiet(ap)) {
			serve = true;
		} else {
			p->not_quiet++;
		}
	}
	spin_unlock_irqrestore(ap->lock, flags);

	if (!serve) {
		/* a real EH round (or ours but not quiet): libata's own handler, unchanged */
		p->orig_ops->error_handler(ap);
		return;
	}

	/* nothing from an earlier cycle may authorise anything in this one */
	p->pages_valid = false;
	p->restored = false;
	p->wr_armed = false;
	p->w1_armed = false;
	for (i = 0; i < SDX_NREGS; i++) {
		p->err[i] = sdx_pmp_read(ap, sdx_regs[i], &p->val[i]);
		if (p->err[i]) {
			/* stop at the first failure; mark the rest unread */
			read_failed = true;
			for (i++; i < SDX_NREGS; i++)
				p->err[i] = AC_ERR_OTHER;
			break;
		}
	}
	if (!read_failed)
		sdx_read_pages(p);
	else
		snprintf(p->page_state, sizeof(p->page_state), "skipped: register read failed");

	p->fan_result = 1;			/* 1 = no write requested */
	if (p->fan_req >= 0) {
		p->fan_error[0] = '\0';
		p->fan_result = sdx_fan_write(p, p->fan_req);
		/* any failure: try FULL once (only if the attempt got as far as the master
		 * check, i.e. this is the master with a clean read) */
		if (p->fan_result && p->fan_req != SDX_FAN_FULL &&
		    SDX_DEC(p->page_raw[0]) == SDX_DX1215_MASTER_P0 && !ata_port_is_frozen(ap)) {
			char first[64];
			int r2;

			strscpy(first, p->fan_error, sizeof(first));
			r2 = sdx_fan_write(p, SDX_FAN_FULL);
			snprintf(p->fan_error, sizeof(p->fan_error), "%s; fallback FULL %s", first,
				 r2 ? "FAILED" : "ok");
		}
	}

	p->led_result = 1;			/* 1 = no write requested */
	if (p->led_chmask >= 0 && !ata_port_is_frozen(ap)) {
		p->led_error[0] = '\0';
		p->led_result = sdx_led_write(p, p->led_chmask, p->led_val);
	}

	spin_lock_irqsave(ap->lock, flags);
	p->served = true;
	p->want = false;
	spin_unlock_irqrestore(ap->lock, flags);

	/* A timed-out internal command leaves the port frozen; recover it now, in this
	 * round, with libata's own handler (as after any libata internal-command timeout). */
	if (ata_port_is_frozen(ap))
		p->orig_ops->error_handler(ap);
}

/* Run one cycle (reads; plus a fan write if fan_req >= 0): schedule an EH round on the port
 * and wait for it. Returns 0 if the round was served and the reads (and the write, if any)
 * succeeded. */
static int sdx_cycle_req(struct sdx_port *p, int fan_req, int led_chmask, unsigned int led_val)
{
	struct ata_port *ap = p->ap;
	unsigned long flags;
	int attempt, rc = -EAGAIN;
	unsigned int i;

	mutex_lock(&p->lock);
	if (!p->swapped) {
		mutex_unlock(&p->lock);
		return -ENODEV;
	}
	for (attempt = 0; attempt < SDX_ATTEMPTS; attempt++) {
		spin_lock_irqsave(ap->lock, flags);
		p->want = true;
		p->served = false;
		p->fan_req = fan_req;
		p->led_chmask = led_chmask;
		p->led_val = led_val;
		ata_port_schedule_eh(ap);
		spin_unlock_irqrestore(ap->lock, flags);

		ata_port_wait_eh(ap);

		spin_lock_irqsave(ap->lock, flags);
		if (p->served)
			rc = 0;
		p->want = false;
		spin_unlock_irqrestore(ap->lock, flags);
		if (!rc)
			break;
	}

	if (rc) {
		snprintf(p->last_error, sizeof(p->last_error),
			 "no quiet EH round in %d attempts (not_quiet=%u)",
			 SDX_ATTEMPTS, p->not_quiet);
	} else {
		p->cycles++;
		p->valid = true;
		p->last_error[0] = '\0';
		for (i = 0; i < SDX_NREGS; i++) {
			if (p->err[i]) {
				snprintf(p->last_error, sizeof(p->last_error),
					 "read 0x%x failed: err_mask 0x%x", sdx_regs[i], p->err[i]);
				rc = -EIO;
				break;
			}
		}
		if (p->pages_valid && p->restored)
			p->stamp = jiffies;
		if (fan_req >= 0 && p->fan_result) {
			snprintf(p->last_error, sizeof(p->last_error), "fan write %s: %s",
				 sdx_fan_code_name[fan_req & 3], p->fan_error);
			rc = -EIO;
		}
		if (led_chmask >= 0 && p->led_result) {
			snprintf(p->last_error, sizeof(p->last_error), "led write: %s",
				 p->led_result == 1 ? "not attempted (port frozen)" : p->led_error);
			rc = -EIO;
		}
	}
	p->fan_req = -1;
	p->led_chmask = -1;
	mutex_unlock(&p->lock);
	return rc;
}

static int sdx_cycle(struct sdx_port *p, int fan_req)
{
	return sdx_cycle_req(p, fan_req, -1, 0);
}

/* --------------------------------------------------------------- reporting -- */

static int sdx_reg_index(u16 reg)
{
	unsigned int i;

	for (i = 0; i < SDX_NREGS; i++)
		if (sdx_regs[i] == reg)
			return i;
	return -1;
}

static const char *sdx_spec_rev(u32 gscr1)
{
	if (gscr1 & (1 << 3))
		return "1.2";
	if (gscr1 & (1 << 2))
		return "1.1";
	if (gscr1 & (1 << 1))
		return "1.0";
	return "<unknown>";
}

static const char *sdx_model(unsigned int unique)
{
	switch (unique) {	/* gpio-spec 2.2, kernel syno_pm_info_show */
	case 0x11: return "RX413";
	case 0x12: return "RX1214";
	case 0x13: return "DX1215";
	case 0x14: return "RX1217";
	case 0x15: return "DX517";
	case 0x16: return "RX418";
	case 0x1c: return "DX1215II";
	default:   return "unknown";
	}
}


/* page data byte (gpio-spec 3.1 decode); caller holds p->lock, pages_valid */
static unsigned int sdx_pd(struct sdx_port *p, int pg)
{
	return SDX_DEC(p->page_raw[pg]);
}

/* Caller holds p->lock. */
static void sdx_dump_pages(struct sdx_port *p)
{
	unsigned int d0, d2, d3, d4;
	int pg;

	pr_warn("host%d   pages: %s%s\n", p->hostno, p->page_state,
		p->restored ? ", 0x3a0/0x3a4 restored and read back" : "");
	if (!p->pages_valid)
		return;
	for (pg = 0; pg < SDX_NPAGES; pg++)
		pr_warn("host%d   page%d = 0x%02x (0x3b0 0x%05x)\n", p->hostno, pg, sdx_pd(p, pg),
			p->page_raw[pg]);
	d0 = sdx_pd(p, 0); d2 = sdx_pd(p, 2); d3 = sdx_pd(p, 3); d4 = sdx_pd(p, 4);
	pr_warn("host%d   => emid %u unique 0x%02x (%s), fan_code %u (%s), fan1_fail %u, fan2_fail %u, power_on %u, psu1 %u, psu2 %u, switch_mode %u\n",
		p->hostno, d0 >> 5, d0 & 0x1f, sdx_model(d0 & 0x1f), d4 & 3,
		sdx_fan_code_name[d4 & 3], d2 & 1, (d2 >> 1) & 1, (d2 >> 5) & 1, (d2 >> 6) & 1,
		(d2 >> 7) & 1, d3 & 1);
}

/* Caller holds p->lock. */
static void sdx_dump(struct sdx_port *p)
{
	u32 *cache = p->ap->link.device->gscr;	/* libata's copy from attach / FEAT_EN writes */
	unsigned int i;

	pr_warn("host%d (ata%u): dump, cycle %u%s%s\n", p->hostno,
		p->ap->print_id, p->cycles, p->last_error[0] ? ", error: " : "",
		p->last_error);
	for (i = 0; i < SDX_NREGS; i++) {
		u16 r = sdx_regs[i];

		if (p->err[i]) {
			pr_warn("host%d   %s[0x%03x] = <err_mask 0x%x>\n", p->hostno,
				r < 0x100 ? "GSCR" : "VREG", r, p->err[i]);
		} else if (r < 0x100) {
			pr_warn("host%d   GSCR[%3u] = 0x%08x   libata cache 0x%08x %s\n",
				p->hostno, r, p->val[i], cache[r],
				p->val[i] == cache[r] ? "same" : "DIFFERS");
		} else {
			pr_warn("host%d   VREG[0x%03x] = 0x%08x\n", p->hostno, r, p->val[i]);
		}
	}
	if (!p->err[0] && !p->err[1] && !p->err[2])
		pr_warn("host%d   => PMP %04x:%04x spec %s rev %u, %u ports\n", p->hostno,
			p->val[0] & 0xffff, p->val[0] >> 16, sdx_spec_rev(p->val[1]),
			(p->val[1] >> 8) & 0xff, p->val[2] & 0xf);
	if (!p->err[5] && !p->err[6])
		pr_warn("host%d   => feat 0x%x/0x%x (enabled/supported, libata prints this order)\n",
			p->hostno, p->val[6], p->val[5]);
	sdx_dump_pages(p);
}

struct sdx_attr {
	struct attribute attr;
	ssize_t (*show)(struct sdx_port *p, struct sdx_attr *a, char *buf);
	ssize_t (*store)(struct sdx_port *p, struct sdx_attr *a, const char *buf, size_t n);
	int reg;
};

static ssize_t sdx_show_reg(struct sdx_port *p, struct sdx_attr *a, char *buf)
{
	int i = sdx_reg_index(a->reg);
	ssize_t n;

	mutex_lock(&p->lock);
	if (i < 0 || !p->valid)
		n = sysfs_emit(buf, "unread\n");
	else if (p->err[i])
		n = sysfs_emit(buf, "error 0x%x\n", p->err[i]);
	else
		n = sysfs_emit(buf, "0x%08x\n", p->val[i]);
	mutex_unlock(&p->lock);
	return n;
}

static ssize_t sdx_show_summary(struct sdx_port *p, struct sdx_attr *a, char *buf)
{
	ssize_t n = 0;

	mutex_lock(&p->lock);
	n += sysfs_emit_at(buf, n, "host %d\nata %u\ncycles %u\nnot_quiet %u\n",
			   p->hostno, p->ap->print_id, p->cycles, p->not_quiet);
	if (p->valid && !p->err[0] && !p->err[1] && !p->err[2])
		n += sysfs_emit_at(buf, n, "vendor 0x%04x\ndevice 0x%04x\nspec %s\nrev %u\nports %u\n",
				   p->val[0] & 0xffff, p->val[0] >> 16, sdx_spec_rev(p->val[1]),
				   (p->val[1] >> 8) & 0xff, p->val[2] & 0xf);
	if (p->valid && !p->err[5] && !p->err[6])
		n += sysfs_emit_at(buf, n, "feat %#x/%#x\n", p->val[6], p->val[5]);
	n += sysfs_emit_at(buf, n, "last_error %s\n", p->last_error[0] ? p->last_error : "-");
	mutex_unlock(&p->lock);
	return n;
}

static ssize_t sdx_show_last_error(struct sdx_port *p, struct sdx_attr *a, char *buf)
{
	ssize_t n;

	mutex_lock(&p->lock);
	n = sysfs_emit(buf, "%s\n", p->last_error);
	mutex_unlock(&p->lock);
	return n;
}

static ssize_t sdx_show_ata_port(struct sdx_port *p, struct sdx_attr *a, char *buf)
{
	return sysfs_emit(buf, "ata%u\n", p->ap->print_id);
}

/* Writing 1 runs one more cycle (register + page reads) and dumps it to dmesg. */
static ssize_t sdx_store_refresh(struct sdx_port *p, struct sdx_attr *a, const char *buf,
				 size_t n)
{
	bool v;
	int rc;

	if (kstrtobool(buf, &v) || !v)
		return -EINVAL;
	rc = sdx_cycle(p, -1);
	mutex_lock(&p->lock);
	sdx_dump(p);
	mutex_unlock(&p->lock);
	return rc ? rc : n;
}

enum { F_EMID, F_UNIQUE, F_MODEL, F_FAN_CODE, F_FAN_FAIL, F_FAN1_FAIL, F_FAN2_FAIL, F_POWER_ON, F_PSU1, F_PSU2,
       F_SWITCH_MODE, F_PAGE0, F_PAGE4 = F_PAGE0 + 4, F_PAGE_STATE, F_LED_MASK, F_FAULT };

/* a->reg carries the field id for these */
static ssize_t sdx_show_field(struct sdx_port *p, struct sdx_attr *a, char *buf)
{
	unsigned int d0, d2, d3, d4;
	ssize_t n;

	mutex_lock(&p->lock);
	if (a->reg == F_PAGE_STATE) {
		n = sysfs_emit(buf, "%s\n", p->page_state[0] ? p->page_state : "unread");
		goto out;
	}
	if (!p->pages_valid) {
		n = sysfs_emit(buf, "unread\n");
		goto out;
	}
	d0 = sdx_pd(p, 0); d2 = sdx_pd(p, 2); d3 = sdx_pd(p, 3); d4 = sdx_pd(p, 4);
	switch (a->reg) {
	case F_EMID:		n = sysfs_emit(buf, "%u\n", d0 >> 5); break;
	case F_UNIQUE:		n = sysfs_emit(buf, "0x%02x\n", d0 & 0x1f); break;
	case F_MODEL:		n = sysfs_emit(buf, "%s\n", sdx_model(d0 & 0x1f)); break;
	case F_FAN_CODE:	n = sysfs_emit(buf, "%u\n", d4 & 3); break;
	case F_FAN_FAIL:	n = sysfs_emit(buf, "%u\n", !!(d2 & SDX_P2_FAN_ANY)); break;
	case F_FAN1_FAIL:	n = sysfs_emit(buf, "%u\n", d2 & 1); break;
	case F_FAN2_FAIL:	n = sysfs_emit(buf, "%u\n", (d2 >> 1) & 1); break;
	case F_POWER_ON:	n = sysfs_emit(buf, "%u\n", (d2 >> 5) & 1); break;
	case F_PSU1:		n = sysfs_emit(buf, "%u\n", (d2 >> 6) & 1); break;
	case F_PSU2:		n = sysfs_emit(buf, "%u\n", (d2 >> 7) & 1); break;
	case F_SWITCH_MODE:	n = sysfs_emit(buf, "%u\n", d3 & 1); break;
	case F_LED_MASK:	n = sysfs_emit(buf, "%u\n", !!(sdx_pd(p, 1) & SDX_P1_LED_MASK)); break;
	case F_FAULT:		n = sysfs_emit(buf, "%u\n", sdx_pd(p, 1) & SDX_P1_FAULT_MASK); break;
	default:
		if (a->reg >= F_PAGE0 && a->reg <= F_PAGE4)
			n = sysfs_emit(buf, "0x%02x raw 0x%05x\n", sdx_pd(p, a->reg - F_PAGE0),
				       p->page_raw[a->reg - F_PAGE0]);
		else
			n = -EIO;
	}
out:
	mutex_unlock(&p->lock);
	return n;
}

#define SDX_FIELD_ATTR(_name, _f) \
	static struct sdx_attr sdx_attr_##_name = { \
		.attr = { .name = __stringify(_name), .mode = 0444 }, \
		.show = sdx_show_field, .reg = (_f) }

SDX_FIELD_ATTR(emid, F_EMID);
SDX_FIELD_ATTR(unique, F_UNIQUE);
SDX_FIELD_ATTR(model, F_MODEL);
SDX_FIELD_ATTR(fan_code, F_FAN_CODE);
SDX_FIELD_ATTR(fan_fail, F_FAN_FAIL);
SDX_FIELD_ATTR(fan1_fail, F_FAN1_FAIL);
SDX_FIELD_ATTR(fan2_fail, F_FAN2_FAIL);
SDX_FIELD_ATTR(power_on, F_POWER_ON);
SDX_FIELD_ATTR(psu1, F_PSU1);
SDX_FIELD_ATTR(psu2, F_PSU2);
SDX_FIELD_ATTR(switch_mode, F_SWITCH_MODE);
SDX_FIELD_ATTR(page0, F_PAGE0);
SDX_FIELD_ATTR(page1, F_PAGE0 + 1);
SDX_FIELD_ATTR(page2, F_PAGE0 + 2);
SDX_FIELD_ATTR(page3, F_PAGE0 + 3);
SDX_FIELD_ATTR(page4, F_PAGE4);
SDX_FIELD_ATTR(page_state, F_PAGE_STATE);

/* led_mask: 1 = all disk LEDs of this PMP off (page 1 bit 6), 0 = on.
 * fault: 0..7, bit n = orange fault LED of this PMP's local disk n (page 1 bits 0-2).
 * A write is a page-1 read-modify-write in one EH round, read back. */
static ssize_t sdx_store_led(struct sdx_port *p, struct sdx_attr *a, const char *buf, size_t n)
{
	unsigned int v;
	int rc;

	if (kstrtouint(buf, 0, &v))
		return -EINVAL;
	if (a->reg == F_LED_MASK) {
		if (v > 1)
			return -EINVAL;
		rc = sdx_cycle_req(p, -1, SDX_P1_LED_MASK, v ? SDX_P1_LED_MASK : 0);
	} else {
		if (v > SDX_P1_FAULT_MASK)
			return -EINVAL;
		rc = sdx_cycle_req(p, -1, SDX_P1_FAULT_MASK, v);
	}
	mutex_lock(&p->lock);
	pr_warn("host%d %s=%u: page1 now 0x%02x (%s)\n", p->hostno, a->attr.name, v,
		p->pages_valid ? sdx_pd(p, 1) : 0xffu, rc ? p->last_error : "written and read back");
	mutex_unlock(&p->lock);
	return rc ? rc : n;
}

static struct sdx_attr sdx_attr_led_mask = {
	.attr = { .name = "led_mask", .mode = 0644 }, .show = sdx_show_field,
	.store = sdx_store_led, .reg = F_LED_MASK };
static struct sdx_attr sdx_attr_fault = {
	.attr = { .name = "fault", .mode = 0644 }, .show = sdx_show_field,
	.store = sdx_store_led, .reg = F_FAULT };

#define SDX_REG_ATTR(_name, _reg) \
	static struct sdx_attr sdx_attr_##_name = { \
		.attr = { .name = __stringify(_name), .mode = 0444 }, \
		.show = sdx_show_reg, .reg = (_reg) }

SDX_REG_ATTR(gscr_0, 0);
SDX_REG_ATTR(gscr_1, 1);
SDX_REG_ATTR(gscr_2, 2);
SDX_REG_ATTR(gscr_32, 32);
SDX_REG_ATTR(gscr_33, 33);
SDX_REG_ATTR(gscr_64, 64);
SDX_REG_ATTR(gscr_96, 96);
SDX_REG_ATTR(raw_3a0, 0x3a0);
SDX_REG_ATTR(raw_3a4, 0x3a4);
SDX_REG_ATTR(raw_3b0, 0x3b0);

static struct sdx_attr sdx_attr_summary = {
	.attr = { .name = "summary", .mode = 0444 }, .show = sdx_show_summary };
static struct sdx_attr sdx_attr_last_error = {
	.attr = { .name = "last_error", .mode = 0444 }, .show = sdx_show_last_error };
static struct sdx_attr sdx_attr_ata_port = {
	.attr = { .name = "ata_port", .mode = 0444 }, .show = sdx_show_ata_port };
static struct sdx_attr sdx_attr_refresh = {
	.attr = { .name = "refresh", .mode = 0200 }, .store = sdx_store_refresh };

static struct attribute *sdx_attrs[] = {
	&sdx_attr_gscr_0.attr, &sdx_attr_gscr_1.attr, &sdx_attr_gscr_2.attr,
	&sdx_attr_gscr_32.attr, &sdx_attr_gscr_33.attr, &sdx_attr_gscr_64.attr,
	&sdx_attr_gscr_96.attr, &sdx_attr_raw_3a0.attr, &sdx_attr_raw_3a4.attr,
	&sdx_attr_raw_3b0.attr, &sdx_attr_summary.attr, &sdx_attr_last_error.attr,
	&sdx_attr_ata_port.attr, &sdx_attr_refresh.attr,
	&sdx_attr_emid.attr, &sdx_attr_unique.attr, &sdx_attr_model.attr, &sdx_attr_fan_code.attr,
	&sdx_attr_fan_fail.attr, &sdx_attr_fan1_fail.attr, &sdx_attr_fan2_fail.attr,
	&sdx_attr_power_on.attr, &sdx_attr_psu1.attr, &sdx_attr_psu2.attr,
	&sdx_attr_switch_mode.attr, &sdx_attr_page0.attr, &sdx_attr_page1.attr,
	&sdx_attr_page2.attr, &sdx_attr_page3.attr, &sdx_attr_page4.attr,
	&sdx_attr_page_state.attr, &sdx_attr_led_mask.attr, &sdx_attr_fault.attr, NULL,
};
ATTRIBUTE_GROUPS(sdx);

static ssize_t sdx_attr_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
	struct sdx_attr *a = container_of(attr, struct sdx_attr, attr);

	return a->show ? a->show(container_of(kobj, struct sdx_port, kobj), a, buf) : -EIO;
}

static ssize_t sdx_attr_store(struct kobject *kobj, struct attribute *attr,
			      const char *buf, size_t n)
{
	struct sdx_attr *a = container_of(attr, struct sdx_attr, attr);

	return a->store ? a->store(container_of(kobj, struct sdx_port, kobj), a, buf, n) : -EIO;
}

static const struct sysfs_ops sdx_sysfs_ops = {
	.show = sdx_attr_show,
	.store = sdx_attr_store,
};

static void sdx_kobj_release(struct kobject *kobj)
{
	kfree(container_of(kobj, struct sdx_port, kobj));
}

static const struct kobj_type sdx_ktype = {
	.sysfs_ops = &sdx_sysfs_ops,
	.default_groups = sdx_groups,
	.release = sdx_kobj_release,
};

/* ------------------------------------------------------------ unit (master) -- */
/*
 * /sys/kernel/synology_dx/unit/: the EMID-0 PMP is the unit master (gpio-spec 4.2): only it
 * returns the real unique, and DSM reads/writes the unit's GPIO there. Every unit operation
 * runs under sdx_unit_lock. The fan write itself re-verifies the master (page 0 == 0x13) in
 * the same EH round, so a stale choice here cannot reach another PMP.
 */
static DEFINE_MUTEX(sdx_unit_lock);
static struct kobject *sdx_unit;
static bool sdx_closing;			/* unload in progress: no new stop test retries */
static bool sdx_stop_active;			/* STOP test running, revert pending */
static struct sdx_port *sdx_stop_port;
static int sdx_stop_tries;
static void sdx_stop_work_fn(struct work_struct *w);
static DECLARE_DELAYED_WORK(sdx_stop_work, sdx_stop_work_fn);

#define SDX_STOP_MAX_S	60
#define SDX_REVERT_TRIES 15

/* caller holds sdx_unit_lock */
static struct sdx_port *sdx_find_master(int *nmaster)
{
	struct sdx_port *m = NULL;
	int i, n = 0;

	for (i = 0; i < SDX_MAX_HOSTS; i++) {
		struct sdx_port *p = sdx_ports[i];

		if (!p)
			continue;
		mutex_lock(&p->lock);
		if (p->pages_valid && sdx_pd(p, 0) == SDX_DX1215_MASTER_P0) {
			n++;
			m = p;
		}
		mutex_unlock(&p->lock);
	}
	if (nmaster)
		*nmaster = n;
	return n == 1 ? m : NULL;
}

/* caller holds sdx_unit_lock: re-read the master if its cache is older than refresh_s */
static void sdx_unit_fresh(struct sdx_port *m)
{
	bool old;

	mutex_lock(&m->lock);
	old = !m->stamp || time_after(jiffies, m->stamp + refresh_s * HZ);
	mutex_unlock(&m->lock);
	if (old)
		sdx_cycle(m, -1);
}

/* caller holds sdx_unit_lock. End a STOP test: write LOW (the handler falls back to FULL on
 * any failure); retried from the delayed work until the fans are no longer at STOP. */
static void sdx_stop_revert_locked(const char *why)
{
	struct sdx_port *m = sdx_stop_port;
	unsigned int code;

	if (!sdx_stop_active || !m)
		return;
	mutex_lock(&m->lock);
	m->stop_armed = false;
	m->stop_until = 0;
	mutex_unlock(&m->lock);

	sdx_cycle(m, SDX_FAN_LOW);
	mutex_lock(&m->lock);
	code = sdx_pd(m, 4) & SDX_P4_FAN_MASK;
	mutex_unlock(&m->lock);
	if (code != SDX_FAN_STOP) {
		sdx_stop_active = false;
		pr_warn("fan STOP test ended (%s): fan code now %u (%s)\n", why, code,
			sdx_fan_code_name[code]);
		return;
	}
	if (++sdx_stop_tries < SDX_REVERT_TRIES && !sdx_closing) {
		pr_crit("fan STOP test: revert to LOW failed (try %d), retrying in 2 s\n",
			sdx_stop_tries);
		schedule_delayed_work(&sdx_stop_work, 2 * HZ);
		return;
	}
	sdx_stop_active = false;
	pr_crit("fan STOP test: could NOT revert the DX fans from STOP after %d tries: power-cycle the DX1215\n",
		sdx_stop_tries);
}

static void sdx_stop_work_fn(struct work_struct *w)
{
	mutex_lock(&sdx_unit_lock);
	sdx_stop_revert_locked("timer");
	mutex_unlock(&sdx_unit_lock);
}

/* caller holds sdx_unit_lock */
static void sdx_stop_cancel_locked(void)
{
	if (!sdx_stop_active)
		return;
	sdx_stop_active = false;
	if (sdx_stop_port) {
		mutex_lock(&sdx_stop_port->lock);
		sdx_stop_port->stop_armed = false;
		sdx_stop_port->stop_until = 0;
		mutex_unlock(&sdx_stop_port->lock);
	}
	cancel_delayed_work(&sdx_stop_work);	/* not _sync: the work takes sdx_unit_lock */
}

enum { U_SUMMARY, U_MASTER, U_UNIQUE, U_MODEL, U_FAN_CODE, U_FAN_LEVEL, U_FAN_FAIL, U_FAN1_FAIL,
       U_FAN2_FAIL, U_POWER_ON,
       U_PSU1, U_PSU2, U_SWITCH_MODE, U_LAST_ERROR, U_STOP, U_DX_LEDS };

struct sdx_uattr {
	struct kobj_attribute ka;
	int f;
};

static ssize_t sdx_unit_show(struct kobject *kobj, struct kobj_attribute *ka, char *buf)
{
	int f = container_of(ka, struct sdx_uattr, ka)->f;
	struct sdx_port *m;
	unsigned int d0, d2, d3, d4;
	int i, nmaster;
	ssize_t n = 0;

	mutex_lock(&sdx_unit_lock);
	m = sdx_find_master(&nmaster);
	if (m)
		sdx_unit_fresh(m);
	if (f == U_STOP) {
		long left = 0;

		if (sdx_stop_active && sdx_stop_port) {
			mutex_lock(&sdx_stop_port->lock);
			if (sdx_stop_port->stop_armed && time_before(jiffies, sdx_stop_port->stop_until))
				left = DIV_ROUND_UP(sdx_stop_port->stop_until - jiffies, HZ);
			mutex_unlock(&sdx_stop_port->lock);
		}
		n = sysfs_emit(buf, "%ld\n", left);
		goto out;
	}
	if (f == U_DX_LEDS) {
		/* 1 = page 1 bit 6 clear on every host, 0 = set on every host, else "mixed" */
		int on = 0, off = 0;

		for (i = 0; i < SDX_MAX_HOSTS; i++) {
			struct sdx_port *p = sdx_ports[i];

			if (!p)
				continue;
			mutex_lock(&p->lock);
			if (!p->pages_valid)
				off = on = -1000;
			else if (sdx_pd(p, 1) & SDX_P1_LED_MASK)
				off++;
			else
				on++;
			mutex_unlock(&p->lock);
		}
		n = sysfs_emit(buf, "%s\n", on < 0 ? "unread" : !off && on ? "1" : !on && off ? "0" : "mixed");
		goto out;
	}
	if (f == U_SUMMARY) {
		for (i = 0; i < SDX_MAX_HOSTS; i++) {
			struct sdx_port *p = sdx_ports[i];

			if (!p)
				continue;
			mutex_lock(&p->lock);
			if (p->pages_valid)
				n += sysfs_emit_at(buf, n, "host%d emid %u unique 0x%02x\n", p->hostno,
						   sdx_pd(p, 0) >> 5, sdx_pd(p, 0) & 0x1f);
			else
				n += sysfs_emit_at(buf, n, "host%d unread (%s)\n", p->hostno,
						   p->page_state[0] ? p->page_state : "-");
			mutex_unlock(&p->lock);
		}
	}
	if (!m) {
		if (f == U_SUMMARY)
			n += sysfs_emit_at(buf, n, "master %s\n", nmaster ? "ambiguous" : "none");
		else
			n = sysfs_emit(buf, "%s\n", f == U_MASTER ? (nmaster ? "ambiguous" : "none") :
				       "unavailable");
		goto out;
	}
	mutex_lock(&m->lock);
	d0 = sdx_pd(m, 0); d2 = sdx_pd(m, 2); d3 = sdx_pd(m, 3); d4 = sdx_pd(m, 4);
	switch (f) {
	case U_SUMMARY:
		n += sysfs_emit_at(buf, n,
				   "master host%d\nunique 0x%02x\nmodel %s\nfan_level %s\nfan_code %u\nfan_fail %u\nfan1_fail %u\nfan2_fail %u\npower_on %u\npsu1 %u\npsu2 %u\nswitch_mode %u\nload_code %d\nstop_test %s\ncache_age_s %lu\nlast_error %s\n",
				   m->hostno, d0 & 0x1f, sdx_model(d0 & 0x1f),
				   sdx_fan_code_name[d4 & 3], d4 & 3, !!(d2 & SDX_P2_FAN_ANY), d2 & 1,
				   (d2 >> 1) & 1, (d2 >> 5) & 1,
				   (d2 >> 6) & 1, (d2 >> 7) & 1, d3 & 1, m->load_code,
				   sdx_stop_active ? "active" : "-", (jiffies - m->stamp) / HZ,
				   m->last_error[0] ? m->last_error : "-");
		break;
	case U_MASTER:		n = sysfs_emit(buf, "host%d\n", m->hostno); break;
	case U_UNIQUE:		n = sysfs_emit(buf, "0x%02x\n", d0 & 0x1f); break;
	case U_MODEL:		n = sysfs_emit(buf, "%s\n", sdx_model(d0 & 0x1f)); break;
	case U_FAN_CODE:	n = sysfs_emit(buf, "%u\n", d4 & 3); break;
	case U_FAN_LEVEL:	n = sysfs_emit(buf, "%s\n", sdx_fan_code_name[d4 & 3]); break;
	case U_FAN_FAIL:	n = sysfs_emit(buf, "%u\n", !!(d2 & SDX_P2_FAN_ANY)); break;
	case U_FAN1_FAIL:	n = sysfs_emit(buf, "%u\n", d2 & 1); break;
	case U_FAN2_FAIL:	n = sysfs_emit(buf, "%u\n", (d2 >> 1) & 1); break;
	case U_POWER_ON:	n = sysfs_emit(buf, "%u\n", (d2 >> 5) & 1); break;
	case U_PSU1:		n = sysfs_emit(buf, "%u\n", (d2 >> 6) & 1); break;
	case U_PSU2:		n = sysfs_emit(buf, "%u\n", (d2 >> 7) & 1); break;
	case U_SWITCH_MODE:	n = sysfs_emit(buf, "%u\n", d3 & 1); break;
	case U_LAST_ERROR:	n = sysfs_emit(buf, "%s\n", m->last_error); break;
	default:		n = -EIO;
	}
	mutex_unlock(&m->lock);
out:
	mutex_unlock(&sdx_unit_lock);
	return n;
}

/* The one entry for a level write from sysfs/hwmon: full, middle or low, never STOP. */
static int sdx_unit_set_level(unsigned int code)
{
	struct sdx_port *m;
	int rc;

	if (code != SDX_FAN_FULL && code != SDX_FAN_MIDDLE && code != SDX_FAN_LOW)
		return -EINVAL;
	mutex_lock(&sdx_unit_lock);
	m = sdx_closing ? NULL : sdx_find_master(NULL);
	if (!m) {
		rc = -ENODEV;
		goto out;
	}
	sdx_stop_cancel_locked();
	rc = sdx_cycle(m, code);
	pr_warn("fan_level %s on host%d: %s\n", sdx_fan_code_name[code], m->hostno,
		rc ? m->last_error : "written and read back");
out:
	mutex_unlock(&sdx_unit_lock);
	return rc;
}

/* fan_level: "full", "middle" or "low" (never "stop": that is fan_stop_test_s only) */
static ssize_t sdx_unit_store_level(struct kobject *kobj, struct kobj_attribute *ka,
				    const char *buf, size_t len)
{
	unsigned int code;
	int rc;

	if (sysfs_streq(buf, "full"))
		code = SDX_FAN_FULL;
	else if (sysfs_streq(buf, "middle"))
		code = SDX_FAN_MIDDLE;
	else if (sysfs_streq(buf, "low"))
		code = SDX_FAN_LOW;
	else
		return -EINVAL;

	rc = sdx_unit_set_level(code);
	return rc ? rc : len;
}

/* fan_stop_test_s: N = 1..60 writes STOP and reverts to LOW after N s from a delayed work
 * (independent of userspace); 0 ends a running test now. */
static ssize_t sdx_unit_store_stop(struct kobject *kobj, struct kobj_attribute *ka,
				   const char *buf, size_t len)
{
	struct sdx_port *m;
	unsigned int secs;
	int rc;

	if (kstrtouint(buf, 0, &secs) || secs > SDX_STOP_MAX_S)
		return -EINVAL;

	mutex_lock(&sdx_unit_lock);
	if (!secs) {
		cancel_delayed_work(&sdx_stop_work);
		sdx_stop_tries = 0;
		sdx_stop_revert_locked("ended by user");
		rc = 0;
		goto out;
	}
	m = sdx_closing ? NULL : sdx_find_master(NULL);
	if (!m) {
		rc = -ENODEV;
		goto out;
	}
	if (sdx_stop_active) {
		rc = -EBUSY;
		goto out;
	}
	mutex_lock(&m->lock);
	m->stop_until = jiffies + secs * HZ;
	m->stop_armed = true;
	mutex_unlock(&m->lock);
	sdx_stop_port = m;
	sdx_stop_tries = 0;
	sdx_stop_active = true;
	/* the revert is scheduled before the STOP write, so it exists whatever happens next */
	schedule_delayed_work(&sdx_stop_work, secs * HZ);
	rc = sdx_cycle(m, SDX_FAN_STOP);
	if (rc) {
		/* the handler already fell back to FULL: end the test without writing LOW */
		pr_warn("fan STOP test on host%d failed: %s\n", m->hostno, m->last_error);
		sdx_stop_cancel_locked();
	} else {
		pr_warn("fan STOP test on host%d: fans at STOP for %u s, then LOW\n", m->hostno, secs);
	}
out:
	mutex_unlock(&sdx_unit_lock);
	return rc ? rc : len;
}

/* dx_leds_enabled: 1 clears the disk-LED mask (page 1 bit 6) on every host, 0 sets it */
static ssize_t sdx_unit_store_dx_leds(struct kobject *kobj, struct kobj_attribute *ka,
				      const char *buf, size_t len)
{
	bool v;
	int i, rc = 0, r;

	if (kstrtobool(buf, &v))
		return -EINVAL;
	mutex_lock(&sdx_unit_lock);
	for (i = 0; i < SDX_MAX_HOSTS && !sdx_closing; i++) {
		struct sdx_port *p = sdx_ports[i];

		if (!p)
			continue;
		r = sdx_cycle_req(p, -1, SDX_P1_LED_MASK, v ? 0 : SDX_P1_LED_MASK);
		pr_warn("dx_leds_enabled=%d on host%d: %s\n", v, p->hostno,
			r ? p->last_error : "written and read back");
		if (r && !rc)
			rc = r;
	}
	mutex_unlock(&sdx_unit_lock);
	return rc ? rc : len;
}

#define SDX_UATTR(_name, _f) \
	static struct sdx_uattr sdx_uattr_##_name = { \
		.ka = __ATTR(_name, 0444, sdx_unit_show, NULL), .f = (_f) }

SDX_UATTR(summary, U_SUMMARY);
SDX_UATTR(master_host, U_MASTER);
SDX_UATTR(unique, U_UNIQUE);
SDX_UATTR(model, U_MODEL);
SDX_UATTR(fan_code, U_FAN_CODE);
SDX_UATTR(fan_fail, U_FAN_FAIL);
SDX_UATTR(fan1_fail, U_FAN1_FAIL);
SDX_UATTR(fan2_fail, U_FAN2_FAIL);
SDX_UATTR(power_on, U_POWER_ON);
SDX_UATTR(psu1, U_PSU1);
SDX_UATTR(psu2, U_PSU2);
SDX_UATTR(switch_mode, U_SWITCH_MODE);
SDX_UATTR(last_error, U_LAST_ERROR);
static struct sdx_uattr sdx_uattr_fan_level = {
	.ka = __ATTR(fan_level, 0644, sdx_unit_show, sdx_unit_store_level), .f = U_FAN_LEVEL };
static struct sdx_uattr sdx_uattr_fan_stop_test_s = {
	.ka = __ATTR(fan_stop_test_s, 0644, sdx_unit_show, sdx_unit_store_stop), .f = U_STOP };
static struct sdx_uattr sdx_uattr_dx_leds_enabled = {
	.ka = __ATTR(dx_leds_enabled, 0644, sdx_unit_show, sdx_unit_store_dx_leds), .f = U_DX_LEDS };

static struct attribute *sdx_unit_attrs[] = {
	&sdx_uattr_summary.ka.attr, &sdx_uattr_master_host.ka.attr, &sdx_uattr_unique.ka.attr,
	&sdx_uattr_model.ka.attr, &sdx_uattr_fan_code.ka.attr, &sdx_uattr_fan_level.ka.attr,
	&sdx_uattr_fan_fail.ka.attr, &sdx_uattr_fan1_fail.ka.attr, &sdx_uattr_fan2_fail.ka.attr,
	&sdx_uattr_power_on.ka.attr, &sdx_uattr_psu1.ka.attr,
	&sdx_uattr_psu2.ka.attr, &sdx_uattr_switch_mode.ka.attr, &sdx_uattr_last_error.ka.attr,
	&sdx_uattr_fan_stop_test_s.ka.attr, &sdx_uattr_dx_leds_enabled.ka.attr, NULL,
};
static const struct attribute_group sdx_unit_group = { .attrs = sdx_unit_attrs };

/* ------------------------------------------------------------------- hwmon -- */
/* name synology_dx: fan1_alarm, fan2_alarm (page 2 bits 0, 1: measured 2026-10-25, a stopped
 * fan sets its bit), pwm1 (255 full, 128 middle, 64 low, 0 stop test; a write picks full above
 * 128, middle above 64, else low: never STOP), pwm1_enable (always 1 = manual). Parent: the
 * master's scsi host device (reference held until unload). */
static struct device *sdx_hwmon;

static const u8 sdx_code_pwm[4] = { 255, 128, 64, 0 };

static umode_t sdx_hwmon_visible(const void *data, enum hwmon_sensor_types type, u32 attr,
				 int channel)
{
	if (type == hwmon_fan && attr == hwmon_fan_alarm && channel < 2)
		return 0444;
	if (type == hwmon_pwm && channel == 0 &&
	    (attr == hwmon_pwm_input || attr == hwmon_pwm_enable))
		return 0644;
	return 0;
}

static int sdx_hwmon_read(struct device *dev, enum hwmon_sensor_types type, u32 attr,
			  int channel, long *val)
{
	struct sdx_port *m;
	int rc = 0;

	if (type == hwmon_pwm && attr == hwmon_pwm_enable) {
		*val = 1;
		return 0;
	}
	mutex_lock(&sdx_unit_lock);
	m = sdx_find_master(NULL);
	if (!m) {
		rc = -ENODATA;
		goto out;
	}
	sdx_unit_fresh(m);
	mutex_lock(&m->lock);
	if (!m->pages_valid)
		rc = -ENODATA;
	else if (type == hwmon_fan && attr == hwmon_fan_alarm)
		*val = (sdx_pd(m, 2) >> channel) & 1;
	else if (type == hwmon_pwm && attr == hwmon_pwm_input)
		*val = sdx_code_pwm[sdx_pd(m, 4) & SDX_P4_FAN_MASK];
	else
		rc = -EOPNOTSUPP;
	mutex_unlock(&m->lock);
out:
	mutex_unlock(&sdx_unit_lock);
	return rc;
}

static int sdx_hwmon_write(struct device *dev, enum hwmon_sensor_types type, u32 attr,
			   int channel, long val)
{
	if (type == hwmon_pwm && attr == hwmon_pwm_enable)
		return val == 1 ? 0 : -EINVAL;
	if (type != hwmon_pwm || attr != hwmon_pwm_input || val < 0 || val > 255)
		return -EINVAL;
	return sdx_unit_set_level(val > 128 ? SDX_FAN_FULL : val > 64 ? SDX_FAN_MIDDLE : SDX_FAN_LOW);
}

static const struct hwmon_ops sdx_hwmon_ops = {
	.is_visible = sdx_hwmon_visible,
	.read = sdx_hwmon_read,
	.write = sdx_hwmon_write,
};

static const struct hwmon_channel_info *const sdx_hwmon_info[] = {
	HWMON_CHANNEL_INFO(fan, HWMON_F_ALARM, HWMON_F_ALARM),
	HWMON_CHANNEL_INFO(pwm, HWMON_PWM_INPUT | HWMON_PWM_ENABLE),
	NULL
};

static const struct hwmon_chip_info sdx_hwmon_chip = {
	.ops = &sdx_hwmon_ops,
	.info = sdx_hwmon_info,
};

/* unload: end a STOP test and put the fan code back to what it was at load */
static void sdx_unit_shutdown(void)
{
	struct sdx_port *m;
	int code, rc, i;

	mutex_lock(&sdx_unit_lock);
	sdx_closing = true;
	mutex_unlock(&sdx_unit_lock);
	cancel_delayed_work_sync(&sdx_stop_work);	/* a running revert finishes first */

	mutex_lock(&sdx_unit_lock);
	sdx_stop_tries = 0;
	sdx_stop_revert_locked("unload");
	m = sdx_find_master(NULL);
	if (m && m->wrote) {
		code = (m->load_code >= 0 && m->load_code != SDX_FAN_STOP) ? m->load_code : SDX_FAN_LOW;
		rc = sdx_cycle(m, code);
		pr_warn("unload: fan level back to %s (level at load) on host%d: %s\n",
			sdx_fan_code_name[code], m->hostno, rc ? m->last_error : "ok");
	}
	/* page 1 (disk LEDs) back to what it was at load, on every host we wrote */
	for (i = 0; i < SDX_MAX_HOSTS; i++) {
		struct sdx_port *p = sdx_ports[i];

		if (!p || !p->led_wrote || p->load_p1 < 0)
			continue;
		rc = sdx_cycle_req(p, -1, SDX_P1_CHANGEABLE, p->load_p1);
		pr_warn("unload: host%d page 1 back to 0x%02x (at load): %s\n", p->hostno,
			p->load_p1, rc ? p->last_error : "ok");
	}
	mutex_unlock(&sdx_unit_lock);
}

/* ------------------------------------------------- DS2415+ internal bays -- */
/*
 * Phase 3, ds_leds=1 only. The DS2415+ main unit's bays hang off three 88SE9215 (stock ahci).
 * Their LEDs are the 9215's own GPIO block, reached through the vendor index/data window at
 * ABAR+0xA8/+0xAC (bay-leds research 1.x, DSM syno_mv_9235_gpio_*). This belongs to the main
 * unit, not the expansion; it lives in this module for now. Every access goes through
 * sdx_ds_reg(), which refuses any GPIO address but the three DSM uses and restores the index
 * register afterwards. At load: read only. leds_enabled=1 writes DSM's three init values (the
 * values found at load are kept), fault_mask changes DATA_OUT bits 4-7 only, leds_enabled=0
 * and unload write the load values back. Not re-applied after resume (Unraid does not suspend).
 */
#define SDX_DS_MAX	4
static const u32 sdx_ds_addr[3] = { SDX_DS_GPIO_OE, SDX_DS_GPIO_OUT, SDX_DS_GPIO_FUNC };

struct sdx_ds {
	struct pci_dev *pdev;			/* reference held until unload */
	void __iomem *mmio;			/* ahci's ABAR mapping (ahci_host_priv->mmio) */
	spinlock_t lock;			/* the 0xA8/0xAC window */
	struct mutex mlock;			/* one sysfs operation at a time */
	u32 saved[3];				/* OE, OUT, FUNC at load */
	bool enabled;				/* DSM init written */
	bool touched;				/* something written: restore at unload */
	unsigned int ports[4];			/* ata print_id per port_no, 0 = none */
	struct kobject kobj;
	bool kobj_added;
};

static struct sdx_ds *sdx_ds_ctl[SDX_DS_MAX];
static struct kobject *sdx_ds_root;

/*
 * 0.6.0: the orange enable. DSM's synobios drives Avoton PCH GPIO 49 high once, on the first
 * non-off disk-LED call, and never low again (research/2026-10-26-ds-orange-led.md 1.1-1.3);
 * without it the 9215 fault bits light nothing. Unraid has no gpio-ich (CONFIG_LPC_ICH is not
 * set), so the module drives the pin: high while at least one controller has leds_enabled = 1,
 * the byte found at load put back when the last one is disabled and at unload.
 * Only sdx_pch_write() writes, one byte at GPIOBASE+0x8a, and only a value that differs from
 * the byte read at load in bit 1 (= GPIO 49) at most; it re-reads USE_SEL2/IO_SEL2 first and
 * refuses unless GPIO 49 is still a GPIO output. The I/O range +0x80..+0x8b is taken with
 * request_region: it nests under the firmware's "pnp 00:01" reservation of 0x500-0x5fe (not
 * busy), and fails if any driver (gpio-ich in a later kernel) owns those ports: then the pin
 * is left alone and the reason is in ds/pch. The byte read at load is the shadow: the output
 * level cannot be read back on Avoton for certain (mainline gpio-ich caches it), but the read
 * at the box gave 0x38 and the tested write 0x3a lit orange with nothing else changing.
 */
static struct {
	u16 base;			/* GPIOBASE (0x500 on this unit) */
	bool region;			/* +0x80..+0x8b requested and GPIO 49 checked: writes allowed */
	u32 use_sel2, io_sel2;		/* read at attach */
	u8 load_b2;			/* LVL2 byte 2 (+0x8a) at load: what unload puts back */
	u8 cur_b2;			/* the byte last written (= load_b2 until the first write) */
	unsigned int users;		/* controllers with leds_enabled = 1 */
	bool on;			/* GPIO 49 high (written by us, or already high at load) */
	int err;			/* last write result */
	const char *state;		/* "ok" or why the pin is not driven */
} sdx_pch = { .state = "not attached (ds_leds=0)" };
static DEFINE_MUTEX(sdx_pch_lock);

/* The ONLY port write in the module. noipa + out of line: the guard stays in the object. */
static noinline __attribute__((__noipa__)) int sdx_pch_write(u8 v)
{
	u32 use, io;

	if (!sdx_pch.region || !sdx_pch_b2_ok(v, sdx_pch.load_b2)) {
		WARN_ONCE(1, "synology_dx: refused PCH GPIO write 0x%x\n", v);
		return -EPERM;
	}
	use = inl(sdx_pch.base + SDX_PCH_USE_SEL2);
	io = inl(sdx_pch.base + SDX_PCH_IO_SEL2);
	if (!sdx_pch_pin_ok(use, io)) {
		pr_warn("ds pch: GPIO 49 is no longer a GPIO output (USE_SEL2 0x%08x IO_SEL2 0x%08x): not written\n",
			use, io);
		return -EIO;
	}
	outb(v, sdx_pch.base + SDX_PCH_LVL2_B2);
	sdx_pch.cur_b2 = v;
	return 0;
}

/* a controller enabled its LEDs: GPIO 49 high if it is not already */
static void sdx_pch_get(void)
{
	mutex_lock(&sdx_pch_lock);
	sdx_pch.users++;
	if (sdx_pch.region && !sdx_pch.on) {
		u8 v = sdx_pch.load_b2 | SDX_PCH_B2_BIT;

		sdx_pch.err = v == sdx_pch.cur_b2 ? 0 : sdx_pch_write(v);
		sdx_pch.on = !sdx_pch.err;
		pr_warn("ds pch: GPIO 49 high (orange enable): byte +0x%x 0x%02x -> 0x%02x: %s\n",
			SDX_PCH_LVL2_B2, sdx_pch.load_b2, v, sdx_pch.err ? "FAILED" : "written");
	}
	mutex_unlock(&sdx_pch_lock);
}

/* a controller disabled its LEDs: the last one puts the load byte back */
static void sdx_pch_put(void)
{
	mutex_lock(&sdx_pch_lock);
	if (sdx_pch.users)
		sdx_pch.users--;
	if (!sdx_pch.users && sdx_pch.on && sdx_pch.region) {
		sdx_pch.err = sdx_pch.cur_b2 == sdx_pch.load_b2 ? 0 : sdx_pch_write(sdx_pch.load_b2);
		sdx_pch.on = !!sdx_pch.err;
		pr_warn("ds pch: GPIO 49 back to the value at load (byte 0x%02x): %s\n",
			sdx_pch.load_b2, sdx_pch.err ? "FAILED" : "written");
	}
	mutex_unlock(&sdx_pch_lock);
}

/* ds_leds=1, before the controllers: find GPIOBASE, check the pin, take the ports. Read only. */
static void sdx_pch_attach(void)
{
	struct pci_dev *lpc = pci_get_domain_bus_and_slot(0, 0, PCI_DEVFN(0x1f, 0));
	u32 cfg = 0;
	u16 base;

	mutex_lock(&sdx_pch_lock);
	if (!lpc || lpc->vendor != SDX_PCH_VENDOR || lpc->device != SDX_PCH_DEVICE) {
		sdx_pch.state = "no Avoton LPC (8086:1f38) at 00:1f.0";
		goto out;
	}
	pci_read_config_dword(lpc, SDX_PCH_CFG_GPIOBASE, &cfg);
	if (!sdx_pch_base_ok(cfg)) {
		sdx_pch.state = "GPIOBASE (config 0x48) not an I/O base in 0x400..0xff00";
		goto out;
	}
	base = cfg & SDX_PCH_BASE_MASK;
	sdx_pch.base = base;
	if (!request_region(base + SDX_PCH_USE_SEL2, SDX_PCH_REGION, "synology_dx")) {
		sdx_pch.state = "GPIO I/O ports owned by another driver: not touched";
		goto out;
	}
	sdx_pch.use_sel2 = inl(base + SDX_PCH_USE_SEL2);
	sdx_pch.io_sel2 = inl(base + SDX_PCH_IO_SEL2);
	if (!sdx_pch_pin_ok(sdx_pch.use_sel2, sdx_pch.io_sel2)) {
		release_region(base + SDX_PCH_USE_SEL2, SDX_PCH_REGION);
		sdx_pch.state = "GPIO 49 is not a GPIO output (USE_SEL2/IO_SEL2 bit 17): not touched";
		goto out;
	}
	sdx_pch.load_b2 = inb(base + SDX_PCH_LVL2_B2);
	sdx_pch.cur_b2 = sdx_pch.load_b2;
	sdx_pch.on = sdx_pch.load_b2 & SDX_PCH_B2_BIT;
	sdx_pch.region = true;
	sdx_pch.state = "ok";
out:
	pr_warn("ds pch: %s: config 0x48 0x%08x, GPIOBASE 0x%x, USE_SEL2 0x%08x IO_SEL2 0x%08x LVL2 byte 2 0x%02x (read only until leds_enabled=1)\n",
		sdx_pch.state, cfg, sdx_pch.base, sdx_pch.use_sel2, sdx_pch.io_sel2, sdx_pch.load_b2);
	mutex_unlock(&sdx_pch_lock);
	pci_dev_put(lpc);
}

/* unload, after the controllers: the load byte back (if still needed) and the ports released */
static void sdx_pch_detach(void)
{
	mutex_lock(&sdx_pch_lock);
	if (sdx_pch.region) {
		if (sdx_pch.cur_b2 != sdx_pch.load_b2) {
			sdx_pch.err = sdx_pch_write(sdx_pch.load_b2);
			pr_warn("ds pch: unload: GPIO 49 back to the value at load: %s\n",
				sdx_pch.err ? "FAILED" : "written");
		}
		release_region(sdx_pch.base + SDX_PCH_USE_SEL2, SDX_PCH_REGION);
		sdx_pch.region = false;
	}
	sdx_pch.on = false;
	sdx_pch.users = 0;
	sdx_pch.state = "detached";
	mutex_unlock(&sdx_pch_lock);
}

static ssize_t sdx_pch_show(struct kobject *kobj, struct kobj_attribute *ka, char *buf)
{
	ssize_t n;

	mutex_lock(&sdx_pch_lock);
	if (!strcmp(ka->attr.name, "pch_led_enable"))
		n = sysfs_emit(buf, "%d\n", sdx_pch.on);
	else
		/* lvl2_b2_read is what an inb returns now. On this unit it stays 0x38 after 0x3a was
		 * written and orange lights (measured 2026-10-26): the Avoton LVL register reads the
		 * pad, not the output latch (mainline gpio-ich: use_outlvl_cache). The written byte
		 * is the shadow, lvl2_b2_written. */
		n = sysfs_emit(buf, "state %s\ngpiobase 0x%x\nuse_sel2 0x%08x\nio_sel2 0x%08x\nlvl2_b2_load 0x%02x\nlvl2_b2_written 0x%02x\nlvl2_b2_read 0x%02x\nusers %u\nlast_write %d\n",
			       sdx_pch.state, sdx_pch.base, sdx_pch.use_sel2, sdx_pch.io_sel2,
			       sdx_pch.load_b2, sdx_pch.cur_b2, sdx_pch.region ? inb(sdx_pch.base + SDX_PCH_LVL2_B2) : 0,
			       sdx_pch.users, sdx_pch.err);
	mutex_unlock(&sdx_pch_lock);
	return n;
}

static struct kobj_attribute sdx_pch_attr_enable = __ATTR(pch_led_enable, 0444, sdx_pch_show, NULL);
static struct kobj_attribute sdx_pch_attr_info = __ATTR(pch, 0444, sdx_pch_show, NULL);
static struct attribute *sdx_pch_attrs[] = { &sdx_pch_attr_enable.attr, &sdx_pch_attr_info.attr, NULL };
static const struct attribute_group sdx_pch_group = { .attrs = sdx_pch_attrs };
static bool sdx_pch_group_added;

/* The ONLY place the 9215 vendor window is touched. noipa + out of line: the address check
 * stays in the object (tests/check-readonly.sh). */
static noinline __attribute__((__noipa__)) int sdx_ds_reg(struct sdx_ds *c, u32 addr, bool write, u32 *val)
{
	unsigned long flags;
	u32 idx, old;

	if (!sdx_ds_addr_ok(addr)) {
		WARN_ONCE(1, "synology_dx: refused 9215 GPIO access 0x%x\n", addr);
		return -EPERM;
	}
	spin_lock_irqsave(&c->lock, flags);
	idx = readl(c->mmio + SDX_DS_WIN_ADDR);
	writel(addr, c->mmio + SDX_DS_WIN_ADDR);
	old = readl(c->mmio + SDX_DS_WIN_DATA);
	if (write)
		writel(*val, c->mmio + SDX_DS_WIN_DATA);
	else
		*val = old;
	writel(idx, c->mmio + SDX_DS_WIN_ADDR);
	readl(c->mmio + SDX_DS_WIN_ADDR);	/* flush the posted writes */
	spin_unlock_irqrestore(&c->lock, flags);
	return 0;
}

/* caller holds c->mlock: put back what was found at load (FUNC first, then OUT, OE) */
static void sdx_ds_restore_locked(struct sdx_ds *c)
{
	int i;

	for (i = 2; i >= 0; i--)
		sdx_ds_reg(c, sdx_ds_addr[i], true, &c->saved[i]);
	if (c->enabled)
		sdx_pch_put();		/* the last controller off: GPIO 49 back */
	c->enabled = false;
	c->touched = false;
}

enum { DS_OE, DS_OUT, DS_FUNC, DS_ENABLED, DS_FAULT, DS_SAVED, DS_PORTS };

struct sdx_ds_attr {
	struct attribute attr;
	int f;
};

static ssize_t sdx_ds_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
	struct sdx_ds *c = container_of(kobj, struct sdx_ds, kobj);
	int f = container_of(attr, struct sdx_ds_attr, attr)->f, i;
	ssize_t n = 0;
	u32 v = 0;

	mutex_lock(&c->mlock);
	switch (f) {
	case DS_OE: case DS_OUT: case DS_FUNC:
		i = f;
		sdx_ds_reg(c, sdx_ds_addr[i], false, &v);
		n = sysfs_emit(buf, "0x%08x\n", v);
		break;
	case DS_ENABLED:
		n = sysfs_emit(buf, "%d\n", c->enabled);
		break;
	case DS_FAULT:
		sdx_ds_reg(c, SDX_DS_GPIO_OUT, false, &v);
		n = sysfs_emit(buf, "%u\n", (v & SDX_DS_FAULT_MASK) >> SDX_DS_FAULT_SHIFT);
		break;
	case DS_SAVED:
		n = sysfs_emit(buf, "gpio_oe 0x%08x\ngpio_out 0x%08x\ngpio_func 0x%08x\n",
			       c->saved[0], c->saved[1], c->saved[2]);
		break;
	case DS_PORTS:
		for (i = 0; i < 4; i++)
			n += sysfs_emit_at(buf, n, "port%d ata%u\n", i, c->ports[i]);
		break;
	default:
		n = -EIO;
	}
	mutex_unlock(&c->mlock);
	return n;
}

static ssize_t sdx_ds_store(struct kobject *kobj, struct attribute *attr, const char *buf,
			    size_t len)
{
	struct sdx_ds *c = container_of(kobj, struct sdx_ds, kobj);
	int f = container_of(attr, struct sdx_ds_attr, attr)->f;
	unsigned int v;
	u32 w, cur = 0;
	int rc = 0;

	if (kstrtouint(buf, 0, &v))
		return -EINVAL;
	mutex_lock(&c->mlock);
	if (f == DS_ENABLED) {
		if (v > 1) {
			rc = -EINVAL;
		} else if (v && !c->enabled) {
			/* DSM syno_mv_9235_gpio_active_init: OE = 0, OUT = 0, FUNC = 0x00B6D8D1 */
			c->touched = true;
			w = 0;
			sdx_ds_reg(c, SDX_DS_GPIO_OE, true, &w);
			w = 0;
			sdx_ds_reg(c, SDX_DS_GPIO_OUT, true, &w);
			w = SDX_DS_FUNC_DSM;
			sdx_ds_reg(c, SDX_DS_GPIO_FUNC, true, &w);
			c->enabled = true;
			sdx_pch_get();	/* DSM: GPIO 49 high with the first LED on */
		} else if (!v && c->touched) {
			sdx_ds_restore_locked(c);
		}
	} else if (f == DS_FAULT) {
		if (v > 0xf) {
			rc = -EINVAL;
		} else if (!c->enabled) {
			rc = -EPERM;		/* write leds_enabled=1 first */
		} else {
			/* DSM syno_mv_9235_disk_led_set: DATA_OUT read-modify-write, bits 4-7 */
			sdx_ds_reg(c, SDX_DS_GPIO_OUT, false, &cur);
			w = SDX_DS_OUT_NEW(cur, v);
			sdx_ds_reg(c, SDX_DS_GPIO_OUT, true, &w);
		}
	} else {
		rc = -EIO;
	}
	if (!rc) {
		u32 r[3];
		int i;

		for (i = 0; i < 3; i++)
			sdx_ds_reg(c, sdx_ds_addr[i], false, &r[i]);
		pr_warn("ds %s: %s=%u: gpio_oe 0x%08x gpio_out 0x%08x gpio_func 0x%08x\n",
			pci_name(c->pdev), attr->name, v, r[0], r[1], r[2]);
	}
	mutex_unlock(&c->mlock);
	return rc ? rc : len;
}

static void sdx_ds_release(struct kobject *kobj)
{
	kfree(container_of(kobj, struct sdx_ds, kobj));
}

#define SDX_DS_ATTR(_name, _mode, _f) \
	static struct sdx_ds_attr sdx_ds_attr_##_name = { \
		.attr = { .name = __stringify(_name), .mode = (_mode) }, .f = (_f) }
SDX_DS_ATTR(gpio_oe, 0444, DS_OE);
SDX_DS_ATTR(gpio_out, 0444, DS_OUT);
SDX_DS_ATTR(gpio_func, 0444, DS_FUNC);
SDX_DS_ATTR(leds_enabled, 0644, DS_ENABLED);
SDX_DS_ATTR(fault_mask, 0644, DS_FAULT);
SDX_DS_ATTR(saved, 0444, DS_SAVED);
SDX_DS_ATTR(ports, 0444, DS_PORTS);

static struct attribute *sdx_ds_attrs[] = {
	&sdx_ds_attr_gpio_oe.attr, &sdx_ds_attr_gpio_out.attr, &sdx_ds_attr_gpio_func.attr,
	&sdx_ds_attr_leds_enabled.attr, &sdx_ds_attr_fault_mask.attr, &sdx_ds_attr_saved.attr,
	&sdx_ds_attr_ports.attr, NULL,
};
ATTRIBUTE_GROUPS(sdx_ds);

static const struct sysfs_ops sdx_ds_sysfs_ops = { .show = sdx_ds_show, .store = sdx_ds_store };

static const struct kobj_type sdx_ds_ktype = {
	.sysfs_ops = &sdx_ds_sysfs_ops,
	.default_groups = sdx_ds_groups,
	.release = sdx_ds_release,
};

/* ds_leds=1: every 1b4b:9215 bound to ahci. Problems skip that controller, never the load. */
static void sdx_ds_attach(void)
{
	struct pci_dev *pdev = NULL;
	int n = 0, i;

	sdx_ds_root = kobject_create_and_add("ds", sdx_root);
	if (!sdx_ds_root) {
		pr_warn("ds: no sysfs dir: DS bay LEDs off\n");
		return;
	}
	sdx_pch_attach();
	if (sysfs_create_group(sdx_ds_root, &sdx_pch_group))
		pr_warn("ds: pch sysfs failed\n");
	else
		sdx_pch_group_added = true;
	while (n < SDX_DS_MAX && (pdev = pci_get_device(0x1b4b, 0x9215, pdev))) {
		struct ata_host *host = pci_get_drvdata(pdev);
		struct ahci_host_priv *hpriv;
		struct sdx_ds *c;

		if (!pdev->driver || strcmp(pdev->driver->name, "ahci") || !host ||
		    host->dev != &pdev->dev || !host->private_data) {
			pr_warn("ds %s: not bound to ahci: skipped\n", pci_name(pdev));
			continue;
		}
		hpriv = host->private_data;
		if (!hpriv->mmio) {
			pr_warn("ds %s: no ABAR mapping: skipped\n", pci_name(pdev));
			continue;
		}
		c = kzalloc(sizeof(*c), GFP_KERNEL);
		if (!c)
			break;
		c->pdev = pci_dev_get(pdev);
		c->mmio = hpriv->mmio;
		spin_lock_init(&c->lock);
		mutex_init(&c->mlock);
		for (i = 0; i < host->n_ports && i < 4; i++)
			if (host->ports[i])
				c->ports[host->ports[i]->port_no & 3] = host->ports[i]->print_id;
		for (i = 0; i < 3; i++)
			sdx_ds_reg(c, sdx_ds_addr[i], false, &c->saved[i]);
		sdx_ds_ctl[n++] = c;
		c->kobj_added = true;
		if (kobject_init_and_add(&c->kobj, &sdx_ds_ktype, sdx_ds_root, "%s", pci_name(pdev)))
			pr_warn("ds %s: sysfs failed\n", pci_name(pdev));
		pr_warn("ds %s: 88SE9215 at load: gpio_oe 0x%08x gpio_out 0x%08x gpio_func 0x%08x (read only; DSM init = 0/0/0x%08x)\n",
			pci_name(pdev), c->saved[0], c->saved[1], c->saved[2], SDX_DS_FUNC_DSM);
	}
	pci_dev_put(pdev);	/* NULL-safe: the iterator's reference when we stopped early */
	pr_warn("ds: %d controller(s) under /sys/kernel/synology_dx/ds/\n", n);
}

/* unload: write the load values back where we wrote, then drop sysfs and references */
static void sdx_ds_detach(void)
{
	int i;

	if (sdx_pch_group_added) {
		sysfs_remove_group(sdx_ds_root, &sdx_pch_group);	/* drains show */
		sdx_pch_group_added = false;
	}
	for (i = 0; i < SDX_DS_MAX; i++) {
		struct sdx_ds *c = sdx_ds_ctl[i];

		if (!c)
			continue;
		if (c->kobj.state_in_sysfs)
			kobject_del(&c->kobj);	/* drains show/store */
		mutex_lock(&c->mlock);
		if (c->touched) {
			sdx_ds_restore_locked(c);
			pr_warn("ds %s: unload: GPIO back to the values at load\n", pci_name(c->pdev));
		}
		mutex_unlock(&c->mlock);
		pci_dev_put(c->pdev);
		sdx_ds_ctl[i] = NULL;
		kobject_put(&c->kobj);		/* release frees it */
	}
	sdx_pch_detach();
	if (sdx_ds_root) {
		kobject_put(sdx_ds_root);
		sdx_ds_root = NULL;
	}
}

/* ---------------------------------------------------------- load / unload -- */

static int sdx_check_symbol(void)
{
	static const char want[] = "ata_exec_internal+0x0/";
	char sym[KSYM_SYMBOL_LEN];

	if (!exec_internal) {
		pr_err("exec_internal= not given (address of ata_exec_internal from /proc/kallsyms)\n");
		return -EINVAL;
	}
	sprint_symbol(sym, exec_internal);
	if (strncmp(sym, want, sizeof(want) - 1) || strchr(sym, '[')) {
		pr_err("exec_internal=0x%lx resolves to '%s', not '%s...': refusing\n",
		       exec_internal, sym, want);
		return -EINVAL;
	}
	pr_info("exec_internal=0x%lx is %s\n", exec_internal, sym);
	sdx_exec_internal = (typeof(sdx_exec_internal))exec_internal;
	return 0;
}

static int sdx_check_host(struct sdx_port *p)
{
	struct Scsi_Host *shost;
	struct ata_port *ap;
	struct ata_device *pmp_dev;
	u32 *g;
	int i;

	for (i = 0; i < SDX_MAX_HOSTS; i++)
		if (sdx_ports[i] && sdx_ports[i] != p && sdx_ports[i]->hostno == p->hostno) {
			pr_err("host%d given twice\n", p->hostno);
			return -EINVAL;
		}

	shost = scsi_host_lookup(p->hostno);
	if (!shost) {
		pr_err("host%d: no such scsi host\n", p->hostno);
		return -ENODEV;
	}
	p->shost = shost;		/* reference held until unload */

	if (!shost->hostt || !shost->hostt->proc_name ||
	    strcmp(shost->hostt->proc_name, "ahci")) {
		pr_err("host%d: driver '%s', not ahci: refusing\n", p->hostno,
		       shost->hostt && shost->hostt->proc_name ? shost->hostt->proc_name : "?");
		return -ENODEV;
	}
	ap = ata_shost_to_port(shost);
	if (!ap || ap->scsi_host != shost) {
		pr_err("host%d: no ata_port behind the scsi host: refusing\n", p->hostno);
		return -ENODEV;
	}
	if (ap->ops->error_handler != ahci_error_handler) {
		pr_err("host%d (ata%u): error_handler is %ps, not ahci_error_handler (already loaded?): refusing\n",
		       p->hostno, ap->print_id, ap->ops->error_handler);
		return -EBUSY;
	}
	pmp_dev = ap->link.device;
	if (!sata_pmp_attached(ap) || pmp_dev->class != ATA_DEV_PMP || !ata_dev_enabled(pmp_dev)) {
		pr_err("host%d (ata%u): no enabled port multiplier attached: refusing\n",
		       p->hostno, ap->print_id);
		return -ENODEV;
	}
	g = pmp_dev->gscr;
	if (sata_pmp_gscr_vendor(g) != 0x1b4b || sata_pmp_gscr_devid(g) != 0x9705) {
		pr_err("host%d (ata%u): PMP is %04x:%04x, not 1b4b:9705: refusing\n", p->hostno,
		       ap->print_id, sata_pmp_gscr_vendor(g), sata_pmp_gscr_devid(g));
		return -ENODEV;
	}
	p->ap = ap;
	pr_info("host%d (ata%u): ahci, PMP 1b4b:9705 rev %u, %u ports, %d fan-out links: ok\n",
		p->hostno, ap->print_id, sata_pmp_gscr_rev(g), sata_pmp_gscr_ports(g),
		ap->nr_pmp_links);
	return 0;
}

static void sdx_restore(struct sdx_port *p)
{
	unsigned long flags;

	if (!p->swapped)
		return;
	mutex_lock(&p->lock);		/* no cycle in progress */
	spin_lock_irqsave(p->ap->lock, flags);
	p->ap->ops = p->orig_ops;
	p->want = false;
	spin_unlock_irqrestore(p->ap->lock, flags);
	mutex_unlock(&p->lock);
	/* an EH round already inside sdx_error_handler finishes before we go on */
	ata_port_wait_eh(p->ap);
	p->swapped = false;
	pr_info("host%d (ata%u): original ops restored\n", p->hostno, p->ap->print_id);
}

static void sdx_free_all(void)
{
	int i;

	sdx_ds_detach();
	/* 0. hwmon and unit sysfs (stores drained), then STOP test ended and the load level
	 *    restored, while our handler is still in place */
	if (sdx_hwmon) {
		hwmon_device_unregister(sdx_hwmon);
		sdx_hwmon = NULL;
	}
	if (sdx_unit) {
		sysfs_remove_group(sdx_unit, &sdx_unit_group);
		kobject_put(sdx_unit);
		sdx_unit = NULL;
	}
	sdx_unit_shutdown();
	/* 1. sysfs first: kobject_del drains running show/store before it returns */
	for (i = 0; i < SDX_MAX_HOSTS; i++)
		if (sdx_ports[i] && sdx_ports[i]->kobj_added &&
		    sdx_ports[i]->kobj.state_in_sysfs)
			kobject_del(&sdx_ports[i]->kobj);
	if (sdx_root) {
		kobject_put(sdx_root);
		sdx_root = NULL;
	}
	/* 2. ops back, EH drained */
	for (i = 0; i < SDX_MAX_HOSTS; i++)
		if (sdx_ports[i])
			sdx_restore(sdx_ports[i]);
	/* 3. anyone who loaded ap->ops (our copy) under ap->lock or in irq context is done */
	synchronize_rcu();
	for (i = 0; i < SDX_MAX_HOSTS; i++) {
		if (!sdx_ports[i])
			continue;
		if (sdx_ports[i]->shost)
			scsi_host_put(sdx_ports[i]->shost);
		if (sdx_ports[i]->kobj_added)
			kobject_put(&sdx_ports[i]->kobj);	/* release frees it */
		else
			kfree(sdx_ports[i]);
		sdx_ports[i] = NULL;
	}
}

static int __init sdx_init(void)
{
	unsigned long flags;
	int i, rc;

	pr_warn("phase 3: GPIO page reads, fan code writes (page 4 bits 1:0, master only), disk LED writes (page 1 bits 0-2, 6), DS 9215 LEDs %s, built for %s\n",
		ds_leds ? "on (ds_leds=1)" : "off",
		UTS_RELEASE);
	if (refresh_s < 1 || refresh_s > 30) {
		pr_err("refresh_s=%u out of 1..30\n", refresh_s);
		return -EINVAL;
	}
	if (strcmp(utsname()->release, UTS_RELEASE)) {
		pr_err("running kernel %s, built for %s: refusing\n", utsname()->release,
		       UTS_RELEASE);
		return -EINVAL;
	}
	if (nr_hosts < 1) {
		pr_err("hosts= not given\n");
		return -EINVAL;
	}
	rc = sdx_check_symbol();
	if (rc)
		return rc;

	for (i = 0; i < nr_hosts; i++) {
		struct sdx_port *p = kzalloc(sizeof(*p), GFP_KERNEL);

		if (!p) {
			rc = -ENOMEM;
			goto fail;
		}
		p->hostno = hosts[i];
		p->fan_req = -1;
		p->load_code = -1;
		p->led_chmask = -1;
		p->load_p1 = -1;
		mutex_init(&p->lock);
		sdx_ports[i] = p;
		rc = sdx_check_host(p);
		if (rc)
			goto fail;
	}

	if (validate_only) {
		pr_warn("validate_only=1: all checks passed, no ops swapped, no ATA command sent; refusing to stay loaded (-ECANCELED is the expected result)\n");
		rc = -ECANCELED;
		goto fail;
	}

	sdx_root = kobject_create_and_add("synology_dx", kernel_kobj);
	if (!sdx_root) {
		rc = -ENOMEM;
		goto fail;
	}

	for (i = 0; i < nr_hosts; i++) {
		struct sdx_port *p = sdx_ports[i];
		struct ata_port *ap = p->ap;

		ata_port_wait_eh(ap);		/* start from a settled port */
		p->orig_ops = ap->ops;
		p->ops = *ap->ops;
		p->ops.error_handler = sdx_error_handler;
		spin_lock_irqsave(ap->lock, flags);
		if (ap->ops != p->orig_ops || ap->ops->error_handler != ahci_error_handler) {
			spin_unlock_irqrestore(ap->lock, flags);
			pr_err("host%d: ops changed under us: refusing\n", p->hostno);
			rc = -EBUSY;
			goto fail;
		}
		ap->ops = &p->ops;
		p->swapped = true;
		spin_unlock_irqrestore(ap->lock, flags);

		/* kobject_put() after a failed add frees p through the release */
		rc = kobject_init_and_add(&p->kobj, &sdx_ktype, sdx_root, "host%d", p->hostno);
		p->kobj_added = true;
		if (rc)
			goto fail;
	}

	sdx_unit = kobject_create_and_add("unit", sdx_root);
	if (!sdx_unit) {
		rc = -ENOMEM;
		goto fail;
	}
	rc = sysfs_create_group(sdx_unit, &sdx_unit_group);
	if (rc)
		goto fail;

	/* one host at a time, so only one DX port is in EH at any moment */
	for (i = 0; i < nr_hosts; i++) {
		struct sdx_port *p = sdx_ports[i];

		rc = sdx_cycle(p, -1);
		mutex_lock(&p->lock);
		sdx_dump(p);
		if (p->pages_valid)
			p->load_p1 = sdx_pd(p, 1);
		mutex_unlock(&p->lock);
		if (rc)
			pr_warn("host%d: read cycle failed (%d); staying loaded, see last_error\n",
				p->hostno, rc);
	}
	{
		struct sdx_port *m;
		int nmaster;

		mutex_lock(&sdx_unit_lock);
		m = sdx_find_master(&nmaster);
		if (m) {
			mutex_lock(&m->lock);
			m->load_code = sdx_pd(m, 4) & SDX_P4_FAN_MASK;
			mutex_unlock(&m->lock);
			pr_warn("unit master host%d (DX1215), fan code at load %d (%s); restored at rmmod\n",
				m->hostno, m->load_code, sdx_fan_code_name[m->load_code]);
			mutex_unlock(&sdx_unit_lock);	/* hwmon reads take it */
			sdx_hwmon = hwmon_device_register_with_info(&m->shost->shost_gendev,
								    "synology_dx", NULL,
								    &sdx_hwmon_chip, NULL);
			if (IS_ERR(sdx_hwmon)) {
				pr_warn("hwmon not registered (%ld); sysfs unit/ still works\n",
					PTR_ERR(sdx_hwmon));
				sdx_hwmon = NULL;
			}
			mutex_lock(&sdx_unit_lock);
		} else {
			pr_warn("no unique DX1215 master (%d candidates): fan writes disabled\n", nmaster);
		}
		mutex_unlock(&sdx_unit_lock);
	}
	if (ds_leds)
		sdx_ds_attach();
	pr_warn("loaded: /sys/kernel/synology_dx/{unit/,host*/%s}; rmmod restores the load fan level, page 1, %slibata ops\n",
		ds_leds ? ",ds/" : "", ds_leds ? "the 9215 GPIO, " : "");
	return 0;

fail:
	sdx_free_all();
	return rc;
}

static void __exit sdx_exit(void)
{
	sdx_free_all();
	pr_warn("unloaded\n");
}

module_init(sdx_init);
module_exit(sdx_exit);

MODULE_DESCRIPTION("Synology DX1215 (88SM9705 PMP) GPIO: fan level and disk LEDs; DS2415+ 88SE9215 bay LEDs + PCH GPIO 49 orange enable (phase 3)");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.6.1");
