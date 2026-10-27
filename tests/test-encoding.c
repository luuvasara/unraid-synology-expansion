/* Userspace check of src/sdx_gpio.h against the gpio-spec and the phase-1 measurement.
 * Written: 2026-10-25. Build+run: tests/run-tests.sh */
#include <stdio.h>
#include "../src/sdx_gpio.h"

static int fails;
#define EXPECT(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
	unsigned int p, r;

	for (p = 0; p < 8; p++) {
		unsigned int hi = SDX_R_SEL(p, 0), lo = SDX_R_SEL(p, 1);

		EXPECT(hi & SDX_R_WCTL, "page %u strobes-high word 0x%x has W_CTL low", p, hi);
		EXPECT(lo & SDX_R_WCTL, "page %u R_CTL-low word 0x%x has W_CTL low", p, lo);
		EXPECT(hi & SDX_R_RCTL, "page %u idle word 0x%x has R_CTL low", p, hi);
		EXPECT(!(lo & SDX_R_RCTL), "page %u read word 0x%x has R_CTL high", p, lo);
		EXPECT(SDX_R_OK(hi) && SDX_R_OK(lo), "page %u words not SDX_R_OK", p);
		EXPECT(hi == SDX_ENC((p << 10) | 0x300), "page %u: 0x%x != enc(V|0x300) 0x%x", p, hi, SDX_ENC((p << 10) | 0x300));
		EXPECT(lo == SDX_ENC((p << 10) | 0x100), "page %u: 0x%x != enc(V|0x100) 0x%x", p, lo, SDX_ENC((p << 10) | 0x100));
		EXPECT(((hi >> 17) & 3) == (p & 3) && ((hi >> 19) & 1) == (p >> 2), "page %u bits", p);
	}
	/* measured idle 0x3a0 on all four PMPs (experiments/2026-10-25-synology-dx-phase1) */
	EXPECT(SDX_R_SEL(3, 0) == 0x6c000, "SEL(3,0) = 0x%x, measured idle 0x6c000", SDX_R_SEL(3, 0));
	EXPECT(SDX_R_OK(0x6c000), "measured idle 0x6c000 not restorable");
	/* spec worked values */
	EXPECT(SDX_ENC(0x1300) == 0x8c000 && SDX_ENC(0x1100) == 0x84000 && SDX_ENC(0x0300) == 0xc000, "spec enc values");
	EXPECT(SDX_R_ALLOWED == 0xec000, "ALLOWED 0x%x", SDX_R_ALLOWED);
	/* everything a page WRITE needs must be refused */
	EXPECT(!SDX_R_OK(SDX_ENC(0x1302) & ~SDX_R_WCTL), "W_CTL pulse word accepted");
	EXPECT(!SDX_R_OK(SDX_ENC(0x1302)), "page-4 data word 0x%x accepted", SDX_ENC(0x1302));
	EXPECT(!SDX_R_OK(SDX_ENC(0x1301)), "page-4 data word accepted");
	EXPECT(!SDX_R_OK(SDX_ENC(0x1310)), "power-pulse data word accepted");
	EXPECT(!SDX_R_OK(0x68000), "W_CTL low (page 3, 0x68000) accepted");
	EXPECT(!SDX_R_OK(0x6c020), "LED pin accepted");
	EXPECT(!SDX_R_OK(0x7c000), "mask bit 16 accepted");
	EXPECT(!SDX_R_OK(0x16c000), "bit 20 accepted");
	EXPECT(!SDX_R_OK(0x0), "zero accepted");
	for (r = 0; r < (1u << 20); r++)	/* exhaustive: OK <=> W_CTL set and only allowed bits */
		if (SDX_R_OK(r) != ((r & SDX_R_WCTL) && !(r & ~SDX_R_ALLOWED))) { EXPECT(0, "SDX_R_OK(0x%x) wrong", r); break; }
	/* decode of measured 0x3b0 values: data byte 0 (bit 5/7 are LED pins, not data) */
	EXPECT(SDX_DEC(0x7c020) == 0 && SDX_DEC(0x7c0a0) == 0, "decode of measured idle 0x3b0");
	EXPECT(SDX_DEC(0x381f) == 0xff && SDX_DEC(0x7e0) == 0, "decode all data bits: 0x%x", SDX_DEC(0x381f));
	/* ---- phase 2b: page-4 write words ---- */
	{
		unsigned int d, cur, val, c;
		unsigned long accepted = 0;
		const unsigned int keep = SDX_R_RCTL | SDX_R_WCTL | SDX_R_PAGE(4) | SDX_ENC_DATA(0xffu);

		EXPECT(SDX_R_W4(0x02, 0) == 0x8c002 && SDX_R_W4(0x02, 0) == SDX_ENC(0x1302), "W4(LOW) = 0x%x, spec enc(0x1302) 0x8c002", SDX_R_W4(0x02, 0));
		for (d = 0; d < 256; d++) {
			EXPECT(SDX_R_W4(d, 0) == SDX_ENC(0x1000 | d | 0x300), "W4(0x%x,0) != Synology W(V|0x300)", d);
			EXPECT(SDX_R_W4(d, 1) == SDX_ENC((0x1000 | d | 0x300) & ~0x100u), "W4(0x%x,1) != Synology W_CTL pulse word", d);
			EXPECT(SDX_DEC(SDX_R_W4(d, 0)) == d && SDX_DEC(SDX_R_W4(d, 1)) == d, "DEC(W4(0x%x)) round trip", d);
			EXPECT(!SDX_R_OK(SDX_R_W4(d, 1)), "W_CTL pulse word for 0x%x passes the select-word check", d);
			EXPECT(SDX_R_OK(SDX_R_W4(d, 0)) == (d == 0), "W4(0x%x,0) vs SDX_R_OK", d);
		}
		/* measured master page 4 = 0x02 (experiments/2026-10-25-synology-dx-phase2a) */
		for (c = 0; c < 4; c++) {
			unsigned int n = SDX_P4_NEW(0x02u, c);

			EXPECT(n == c, "P4_NEW(0x02, %u) = 0x%x", c, n);
			EXPECT(sdx_w4_ok(SDX_R_W4(n, 0), 0x02, 0) == (c != 3), "code %u without stop_ok", c);
			EXPECT(sdx_w4_ok(SDX_R_W4(n, 1), 0x02, 1), "code %u pulse with stop_ok", c);
		}
		for (cur = 0; cur < 256; cur++)		/* the new byte never has bit 4, keeps bits 7:2 except 4 */
			for (c = 0; c < 4; c++) {
				unsigned int n = SDX_P4_NEW(cur, c);

				EXPECT(!(n & SDX_P4_POWER_PULSE) && (n & 3) == c &&
				       (n & ~3u & ~SDX_P4_POWER_PULSE) == (cur & ~3u & ~SDX_P4_POWER_PULSE),
				       "P4_NEW(0x%x, %u) = 0x%x", cur, c, n);
			}
		for (cur = 0; cur < 256; cur++)		/* bit 4 already set: refuse every write */
			if (cur & SDX_P4_POWER_PULSE)
				for (c = 0; c < 4; c++)
					EXPECT(!sdx_w4_ok(SDX_R_W4(SDX_P4_NEW(cur, c), 0), cur, 1), "cur 0x%x code %u accepted", cur, c);
		EXPECT(!sdx_w4_ok(SDX_R_W4(0x40, 0), 0x02, 1), "alarm-LED change accepted");
		EXPECT(!sdx_w4_ok(SDX_R_W4(0x12, 1), 0x02, 1), "power pulse accepted");
		EXPECT(!sdx_w4_ok(SDX_R_W4(0x02, 0) & ~SDX_R_RCTL, 0x02, 1), "R_CTL low accepted");
		EXPECT(!sdx_w4_ok(SDX_R_W4(0x02, 0) ^ SDX_R_PAGE(4) ^ SDX_R_PAGE(3), 0x02, 1), "page 3 data word accepted");
		EXPECT(!sdx_w4_ok(SDX_R_W4(0x02, 0) | (1u << 6), 0x02, 1), "LED pin accepted");
		EXPECT(!sdx_w4_ok(SDX_R_W4(0x02, 0), 0x102, 1), "cur > 0xff accepted");
		/* exhaustive: every accepted word has the properties, for every cur and stop_ok */
		for (val = 0; val < (1u << 20); val++)
			for (cur = 0; cur < 256; cur++)
				for (c = 0; c < 2; c++) {
					if (!sdx_w4_ok(val, cur, (int)c))
						continue;
					accepted++;
					d = SDX_DEC(val);
					if ((val & ~keep) || (val & SDX_R_PAGE(7)) != SDX_R_PAGE(4) || !(val & SDX_R_RCTL) ||
					    (d & SDX_P4_POWER_PULSE) || ((d ^ cur) & ~SDX_P4_FAN_MASK) ||
					    (!c && (d & 3) == SDX_FAN_STOP)) {
						EXPECT(0, "sdx_w4_ok(0x%x, 0x%x, %u) accepted a bad word", val, cur, c);
						val = 1u << 20; cur = 256; break;
					}
				}
		/* 128 curs (bit 4 clear) x (4 codes x 2 strobes) with stop, 3 x 2 without = 128 x 14 */
		EXPECT(accepted == 128ul * 14, "accepted %lu words, expected %lu", accepted, 128ul * 14);
	}
	/* ---- phase 3: page-1 write words (disk LEDs) ---- */
	{
		unsigned int d, cur, val, cm, v;
		unsigned long accepted = 0;
		const unsigned int keep = SDX_R_RCTL | SDX_R_WCTL | SDX_R_PAGE(1) | SDX_ENC_DATA(0xffu);

		for (d = 0; d < 256; d++) {
			EXPECT(SDX_R_W1(d, 0) == SDX_ENC(0x400 | d | 0x300), "W1(0x%x,0) != Synology W(V|0x300)", d);
			EXPECT(SDX_R_W1(d, 1) == SDX_ENC((0x400 | d | 0x300) & ~0x100u), "W1(0x%x,1) != W_CTL pulse word", d);
			EXPECT(SDX_DEC(SDX_R_W1(d, 0)) == d && SDX_DEC(SDX_R_W1(d, 1)) == d, "DEC(W1(0x%x))", d);
			EXPECT(!sdx_w4_ok(SDX_R_W1(d, 0), d, 1) && !sdx_w4_ok(SDX_R_W1(d, 1), d, 1), "page-1 word 0x%x passes the page-4 guard", d);
			EXPECT(!sdx_w1_ok(SDX_R_W4(d, 0), d) && !sdx_w1_ok(SDX_R_W4(d, 1), d), "page-4 word 0x%x passes the page-1 guard", d);
		}
		/* measured page 1 = 0x40 on all four PMPs: enable = 0x00, fault on disk 1 = 0x02 */
		EXPECT(SDX_P1_NEW(0x40u, SDX_P1_LED_MASK, 0u) == 0x00, "P1_NEW enable");
		EXPECT(SDX_P1_NEW(0x00u, SDX_P1_FAULT_MASK, 2u) == 0x02, "P1_NEW fault 1");
		EXPECT(SDX_P1_NEW(0x02u, SDX_P1_CHANGEABLE, 0x40u) == 0x40, "P1_NEW restore");
		EXPECT(sdx_w1_ok(SDX_R_W1(0x00, 0), 0x40) && sdx_w1_ok(SDX_R_W1(0x00, 1), 0x40), "enable from 0x40 refused");
		EXPECT(!sdx_w1_ok(SDX_R_W1(0x60, 1), 0x40), "buzzer pulse accepted");
		EXPECT(!sdx_w1_ok(SDX_R_W1(0x48, 1), 0x40), "bit 3 change accepted");
		EXPECT(!sdx_w1_ok(SDX_R_W1(0x50, 1), 0x40), "bit 4 change accepted");
		EXPECT(!sdx_w1_ok(SDX_R_W1(0xc0, 1), 0x40), "bit 7 change accepted");
		EXPECT(!sdx_w1_ok(SDX_R_W1(0x00, 0), 0x60), "write with cur bit 5 set accepted");
		EXPECT(!sdx_w1_ok(SDX_R_W1(0x00, 0) & ~SDX_R_RCTL, 0x40), "R_CTL low accepted");
		EXPECT(!sdx_w1_ok(SDX_R_W1(0x00, 0) | (1u << 6), 0x40), "LED pin accepted");
		EXPECT(!sdx_w1_ok(SDX_R_W1(0x00, 0), 0x140), "cur > 0xff accepted");
		for (cur = 0; cur < 256; cur++)		/* the new byte: only chmask & 0x47 from v, bit 5 0 */
			for (cm = 0; cm < 256; cm++)
				for (v = 0; v < 256; v += 5) {
					unsigned int n = SDX_P1_NEW(cur, cm, v), ch = cm & SDX_P1_CHANGEABLE;

					if ((n & SDX_P1_BUZZER) || n > 0xff || (n & ~ch & ~SDX_P1_BUZZER) != (cur & ~ch & ~SDX_P1_BUZZER) ||
					    (n & ch) != (v & ch) || (!(cur & SDX_P1_BUZZER) && !sdx_w1_ok(SDX_R_W1(n, 1), cur))) {
						EXPECT(0, "P1_NEW(0x%x, 0x%x, 0x%x) = 0x%x", cur, cm, v, n);
						cur = 256; cm = 256; break;
					}
				}
		/* exhaustive: every accepted word is a page-1 word changing only bits 0-2, 6 */
		for (val = 0; val < (1u << 20); val++)
			for (cur = 0; cur < 256; cur++) {
				if (!sdx_w1_ok(val, cur))
					continue;
				accepted++;
				d = SDX_DEC(val);
				if ((val & ~keep) || (val & SDX_R_PAGE(7)) != SDX_R_PAGE(1) || !(val & SDX_R_RCTL) ||
				    ((d | cur) & SDX_P1_BUZZER) || ((d ^ cur) & 0x98u)) {
					EXPECT(0, "sdx_w1_ok(0x%x, 0x%x) accepted a bad word", val, cur);
					val = 1u << 20; break;
				}
			}
		/* 128 curs (bit 5 clear) x 16 data bytes (bits 0-2, 6 free) x 2 strobes */
		EXPECT(accepted == 128ul * 16 * 2, "page 1: accepted %lu words, expected %lu", accepted, 128ul * 32);
	}
	/* ---- phase 3: DS2415+ 9215 GPIO allowlist ---- */
	{
		unsigned long a, n = 0;
		unsigned int cur, m;

		EXPECT(sdx_ds_addr_ok(0x07071020u) && sdx_ds_addr_ok(0x07071024u) && sdx_ds_addr_ok(0x07071058u), "DSM addresses refused");
		EXPECT(SDX_DS_GPIO_OUT == 0x07071020u && SDX_DS_GPIO_OE == 0x07071024u && SDX_DS_GPIO_FUNC == 0x07071058u, "address constants");
		EXPECT(SDX_DS_FUNC_DSM == 0x00B6D8D1u && SDX_DS_WIN_ADDR == 0xA8 && SDX_DS_WIN_DATA == 0xAC, "DSM init / window");
		for (a = 0; a < (1ul << 32); a += (a >> 24) == 0x07 ? 1 : 0x101)	/* every address in 0x07xxxxxx, a grid elsewhere */
			n += sdx_ds_addr_ok((unsigned int)a) ? 1 : 0;
		EXPECT(n == 3, "allowlist accepts %lu addresses, expected 3", n);
		EXPECT(!sdx_ds_addr_ok(0) && !sdx_ds_addr_ok(0xffffffffu) && !sdx_ds_addr_ok(0x07071028u), "neighbour accepted");
		for (cur = 0; cur < (1u << 12); cur++)	/* fault RMW: bits 4-7 = m, the rest kept */
			for (m = 0; m < 32; m++) {
				unsigned int w = SDX_DS_OUT_NEW(cur | 0xabc00000u, m);

				if ((w & ~0xf0u) != ((cur | 0xabc00000u) & ~0xf0u) || ((w >> 4) & 0xf) != (m & 0xf)) {
					EXPECT(0, "DS_OUT_NEW(0x%x, %u) = 0x%x", cur, m, w);
					cur = 1u << 12; break;
				}
			}
	}
	/* ---- 0.6.0: PCH GPIO 49 (orange enable) ---- */
	{
		unsigned long n = 0, a;
		unsigned int v, l;

		EXPECT(SDX_PCH_LVL2_B2 == 0x8a && SDX_PCH_LVL2 == 0x88 && SDX_PCH_USE_SEL2 == 0x80 && SDX_PCH_IO_SEL2 == 0x84,
		       "PCH register offsets");
		EXPECT(SDX_PCH_GPIO49 == (1u << 17) && SDX_PCH_B2_BIT == (SDX_PCH_GPIO49 >> 8 * (SDX_PCH_LVL2_B2 - SDX_PCH_LVL2)),
		       "GPIO 49 bit in the byte at +0x8a");
		EXPECT(SDX_PCH_USE_SEL2 + SDX_PCH_REGION == SDX_PCH_LVL2 + 4, "region covers USE_SEL2..LVL2 exactly");
		/* exhaustive: a byte is accepted iff it equals the load byte or differs in bit 1 only */
		for (l = 0; l < 0x200u; l++)
			for (v = 0; v < 0x200u; v++) {
				int want = v <= 0xff && l <= 0xff && (v == l || v == (l ^ 0x02u));

				if (sdx_pch_b2_ok(v, l) != want) {
					EXPECT(0, "sdx_pch_b2_ok(0x%x, 0x%x) = %d", v, l, !want);
					l = 0x200u; break;
				}
				n += sdx_pch_b2_ok(v, l) ? 1 : 0;
			}
		EXPECT(n == 256ul * 2, "PCH byte guard accepts %lu pairs, expected 512", n);
		EXPECT(sdx_pch_b2_ok(0x3a, 0x38) && sdx_pch_b2_ok(0x38, 0x38) && !sdx_pch_b2_ok(0x3b, 0x38) && !sdx_pch_b2_ok(0x78, 0x38),
		       "the measured 0x38/0x3a pair");
		EXPECT(sdx_pch_pin_ok(1u << 17, 0) && !sdx_pch_pin_ok(0, 0) && !sdx_pch_pin_ok(1u << 17, 1u << 17) &&
		       sdx_pch_pin_ok(0xffffffffu, ~(1u << 17)) && !sdx_pch_pin_ok(~(1u << 17), 0), "GPIO 49 mode check");
		EXPECT(sdx_pch_base_ok(0x503) && !sdx_pch_base_ok(0x502) && !sdx_pch_base_ok(0x81) && !sdx_pch_base_ok(0x381) &&
		       sdx_pch_base_ok(0xff01) && !sdx_pch_base_ok(0xff81) && !sdx_pch_base_ok(1), "GPIOBASE range");
		for (n = 0, a = 0; a < 0x10000ul; a++)
			n += sdx_pch_base_ok((unsigned int)a) ? 1 : 0;
		/* bases 0x400..0xff00 in 0x80 steps (0x1f7 values) x 64 low-bit patterns with bit 0 set */
		EXPECT(n == 0x1f7ul * 64, "GPIOBASE accepts %lu config values, expected %lu", n, 0x1f7ul * 64);
	}
	if (!fails)
		printf("test-encoding: OK\n");
	return fails != 0;
}
