/* SPDX-License-Identifier: GPL-2.0 */
/*
 * 88SM9705 GPIO register encoding for the DX1215, as used by Synology's kernel
 * (research/2026-10-25-dx1215-gpio-spec.md section 3). Pure macros: included by the module
 * and by tests/test-encoding.c (userspace), so the values are checked in both places.
 * Written: 2026-10-25.
 *
 * Register word R (20 bits) of 0x3a0 (output data) / 0x3b0 (input):
 *   R[4:0]   GPO1..5 / GPI1..5   data bits d[4:0]
 *   R[10:5]  LED pins            (never written by this module: always 0)
 *   R[13:11] GPO6..8 / GPI6..8   data bits d[7:5]
 *   R[14]    W_CTL  (write strobe, active LOW)   -> always 1 in every value we write
 *   R[15]    R_CTL  (read strobe, active LOW)
 *   R[16]    mask                (always 0)
 *   R[18:17] A1:A0, R[19] A2     page select
 * Synology's user value v maps to R as
 *   R = (v & 0x1f) | ((v << 6) & 0xf800) | ((v << 7) & 0x60000) | ((v << 7) & 0x80000)
 * with v[9] = R_CTL, v[8] = W_CTL, v[12:10] = page. Measured idle 0x3a0 on all four PMPs:
 * 0x0006c000 = SDX_ENC(0x0f00) = page 3, both strobes high.
 */
#ifndef SDX_GPIO_H
#define SDX_GPIO_H

#define SDX_REG_GPIO_OUT	0x3a0
#define SDX_REG_GPIO_EN		0x3a4
#define SDX_REG_GPIO_IN		0x3b0

#define SDX_EN_ALL_OUT		0xfffffu	/* Synology: before every 0x3a0 write */
#define SDX_EN_READ		0xfc7c0u	/* Synology: before reading 0x3b0 */

#define SDX_R_WCTL		(1u << 14)
#define SDX_R_RCTL		(1u << 15)
#define SDX_R_PAGE(p)		((((p) & 3u) << 17) | ((((p) >> 2) & 1u) << 19))
/* the only bits this module may ever set in 0x3a0: strobes + page select */
#define SDX_R_ALLOWED		(SDX_R_WCTL | SDX_R_RCTL | SDX_R_PAGE(7))	/* 0xec000 */
/* a value is writable to 0x3a0 iff W_CTL is set and nothing outside SDX_R_ALLOWED is */
#define SDX_R_OK(r)		((((r) & (SDX_R_WCTL | ~SDX_R_ALLOWED)) == SDX_R_WCTL))

/* Synology's encode of user value v (kernel 0x8136f2c0), for cross-checking */
#define SDX_ENC(v)		(((v) & 0x1fu) | (((v) << 6) & 0xf800u) | \
				 (((v) << 7) & 0x60000u) | (((v) << 7) & 0x80000u))
/* decode of a 0x3b0 word to the page data byte (kernel 0x8136f2a0) */
#define SDX_DEC(r)		(((r) & 0x1fu) | (((r) >> 6) & 0xe0u))

/* Page select word: W_CTL always high; R_CTL high (rctl_low = 0) or low (rctl_low = 1).
 * Data bits are always 0: this module never writes page data. */
#define SDX_R_SEL(p, rctl_low)	(SDX_R_WCTL | ((rctl_low) ? 0u : SDX_R_RCTL) | SDX_R_PAGE(p))

#define SDX_NPAGES		5	/* pages 0..4 */

/* ---- phase 2b: page-4 write (fan code), gpio-spec 1.2-1.4 and 2.3 ---- */
#define SDX_P4_FAN_MASK		0x03u	/* bits 1:0 fan code */
#define SDX_P4_POWER_PULSE	0x10u	/* bit 4: momentary power-switch line, never set */
#define SDX_FAN_FULL		0u
#define SDX_FAN_MIDDLE		1u
#define SDX_FAN_LOW		2u
#define SDX_FAN_STOP		3u	/* only via the time-limited stop test */
/* page 0 data byte of the unit master: EMID 0 (bits 7:5), unique 0x13 = DX1215 */
#define SDX_DX1215_MASTER_P0	0x13u
/* page 2: bit 0 fan 1 alarm, bit 1 fan 2 alarm (measured 2026-10-25: STOP set both) */
#define SDX_P2_FAN_ANY		0x03u

/* data byte d -> its R bits (inverse of SDX_DEC for d <= 0xff) */
#define SDX_ENC_DATA(d)		(((d) & 0x1fu) | (((d) << 6) & 0x3800u))
/* page-4 write word: R_CTL high, W_CTL high (wctl_low = 0) or low (the latch pulse) */
#define SDX_R_W4(d, wctl_low)	(SDX_R_RCTL | ((wctl_low) ? 0u : SDX_R_WCTL) | \
				 SDX_R_PAGE(4) | SDX_ENC_DATA(d))

/*
 * The page-4 write predicate. val is a 0x3a0 word, cur the page-4 data byte read in the same
 * cycle. True only if val is a page-4 data word (strobes as in SDX_R_W4, nothing else set),
 * its data byte d has bit 4 clear, cur has bit 4 clear, d differs from cur only in bits 1:0,
 * and the STOP code only when stop_ok.
 */
static inline int sdx_w4_ok(unsigned int val, unsigned int cur, int stop_ok)
{
	unsigned int d = SDX_DEC(val);

	if (cur > 0xffu)
		return 0;
	if (val != SDX_R_W4(d, 0) && val != SDX_R_W4(d, 1))
		return 0;
	if ((d | cur) & SDX_P4_POWER_PULSE)
		return 0;
	if ((d ^ cur) & ~SDX_P4_FAN_MASK)
		return 0;
	if ((d & SDX_P4_FAN_MASK) == SDX_FAN_STOP && !stop_ok)
		return 0;
	return 1;
}

/* the new page-4 byte for fan code c (gpio-spec 1.2 step 3) */
#define SDX_P4_NEW(cur, c)	((((cur) & ~SDX_P4_FAN_MASK) | ((c) & SDX_P4_FAN_MASK)) & ~SDX_P4_POWER_PULSE)

/* ---- phase 3: page-1 write (disk LEDs), bay-leds research section 3 ----
 * page 1 data byte, per PMP: bits 0..2 fault LED of local disk 0..2 (1 = orange), bit 5 the
 * buzzer-mute pulse line (never set), bit 6 disk-LED mask (1 = all disk LEDs off), bits 3, 4, 7
 * are not ours (bit 7 deep-sleep LED): preserved as read. Measured 0x40 on all four PMPs. */
#define SDX_P1_FAULT_MASK	0x07u
#define SDX_P1_BUZZER		0x20u
#define SDX_P1_LED_MASK		0x40u
#define SDX_P1_CHANGEABLE	0x47u	/* SDX_P1_FAULT_MASK | SDX_P1_LED_MASK */

/* page-1 write word: R_CTL high, W_CTL high (wctl_low = 0) or low (the latch pulse) */
#define SDX_R_W1(d, wctl_low)	(SDX_R_RCTL | ((wctl_low) ? 0u : SDX_R_WCTL) | \
				 SDX_R_PAGE(1) | SDX_ENC_DATA(d))

/*
 * The page-1 write predicate. val is a 0x3a0 word, cur the page-1 data byte read in the same
 * cycle. True only if val is a page-1 data word (strobes as in SDX_R_W1, nothing else set),
 * neither its data byte d nor cur has bit 5 (buzzer pulse) set, and d differs from cur only
 * in bits 0-2 and 6.
 */
static inline int sdx_w1_ok(unsigned int val, unsigned int cur)
{
	unsigned int d = SDX_DEC(val);

	if (cur > 0xffu)
		return 0;
	if (val != SDX_R_W1(d, 0) && val != SDX_R_W1(d, 1))
		return 0;
	if ((d | cur) & SDX_P1_BUZZER)
		return 0;
	if ((d ^ cur) & ~SDX_P1_CHANGEABLE)
		return 0;
	return 1;
}

/* new page-1 byte: the bits of chmask (within 0x47) taken from v, the rest from cur, bit 5 0 */
#define SDX_P1_NEW(cur, chmask, v) \
	((((cur) & ~((chmask) & SDX_P1_CHANGEABLE)) | ((v) & (chmask) & SDX_P1_CHANGEABLE)) & \
	 ~SDX_P1_BUZZER & 0xffu)

/* ---- phase 3: DS2415+ internal bays, 88SE9215 GPIO via the vendor window (bay-leds 1.x) ----
 * writel(addr, ABAR + 0xA8); readl/writel(ABAR + 0xAC). Only these three GPIO registers. */
#define SDX_DS_WIN_ADDR		0xA8
#define SDX_DS_WIN_DATA		0xAC
#define SDX_DS_GPIO_OUT		0x07071020u	/* DATA_OUT: bit 4+port = fault (orange) */
#define SDX_DS_GPIO_OE		0x07071024u	/* DATA_OUT_ENABLE, active low */
#define SDX_DS_GPIO_FUNC	0x07071058u	/* GPIO_ACTIVE: pin functions */
#define SDX_DS_FUNC_DSM		0x00B6D8D1u	/* DSM: GPIO0-3 link/activity, GPIO4-7 from DATA_OUT */
#define SDX_DS_FAULT_SHIFT	4
#define SDX_DS_FAULT_MASK	0xf0u

static inline int sdx_ds_addr_ok(unsigned int a)
{
	return a == SDX_DS_GPIO_OUT || a == SDX_DS_GPIO_OE || a == SDX_DS_GPIO_FUNC;
}

/* DATA_OUT with the four fault bits replaced by m (bit p = port p), every other bit kept */
#define SDX_DS_OUT_NEW(cur, m)	(((cur) & ~SDX_DS_FAULT_MASK) | \
				 (((m) << SDX_DS_FAULT_SHIFT) & SDX_DS_FAULT_MASK))

/* ---- 0.6.0: DS2415+ orange enable = Avoton PCH GPIO 49 (research/2026-10-26-ds-orange-led.md) ----
 * GPIOBASE = PCI 00:1f.0 (8086:1f38) config 0x48 & 0xff80 (0x500 on this unit). Second (SUS) bank:
 * USE_SEL2 +0x80 (1 = GPIO), IO_SEL2 +0x84 (0 = output), LVL2 +0x88; GPIO 49 = bit 17 of the bank.
 * The only write is one byte at +0x8a (LVL2 bits 16-23, the byte write tested at the box on
 * 2026-10-26: 0x38 -> 0x3a -> 0x38), and it may differ from the byte read at load only in bit 1. */
#define SDX_PCH_VENDOR		0x8086
#define SDX_PCH_DEVICE		0x1f38	/* Atom C2000 PCU (LPC), 00:1f.0 */
#define SDX_PCH_CFG_GPIOBASE	0x48
#define SDX_PCH_BASE_MASK	0xff80u
#define SDX_PCH_USE_SEL2	0x80
#define SDX_PCH_IO_SEL2		0x84
#define SDX_PCH_LVL2		0x88
#define SDX_PCH_LVL2_B2		0x8a	/* byte 2 of LVL2: bank bits 16-23 */
#define SDX_PCH_REGION		0x0c	/* +0x80..+0x8b: USE_SEL2, IO_SEL2, LVL2 */
#define SDX_PCH_GPIO49		(1u << 17)
#define SDX_PCH_B2_BIT		0x02u	/* GPIO 49 = bit 1 of the byte at +0x8a */

/* config 0x48: I/O space indicator (bit 0) set, base 0x80-aligned inside 0x400..0xff00 */
static inline int sdx_pch_base_ok(unsigned int cfg)
{
	unsigned int base = cfg & SDX_PCH_BASE_MASK;

	return (cfg & 1u) && base >= 0x400u && base <= 0xff00u;
}

/* GPIO 49 is a GPIO (USE_SEL2 bit 17 = 1) and an output (IO_SEL2 bit 17 = 0) */
static inline int sdx_pch_pin_ok(unsigned int use_sel2, unsigned int io_sel2)
{
	return (use_sel2 & SDX_PCH_GPIO49) && !(io_sel2 & SDX_PCH_GPIO49);
}

/* a byte for +0x8a: differs from the byte read at load in bit 1 (GPIO 49) at most */
static inline int sdx_pch_b2_ok(unsigned int v, unsigned int load)
{
	return v <= 0xffu && load <= 0xffu && !((v ^ load) & ~SDX_PCH_B2_BIT);
}

#endif
