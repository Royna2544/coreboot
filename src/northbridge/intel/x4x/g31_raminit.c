/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Intel G31 (Bearlake) DRAM initialisation.
 *
 * This follows the step order of the OEM BIOS' BLMRC220.006 reference code.
 * reference/re/memory-training.md in the project root documents every register
 * and table, and says which claims were reproduced from a capture of the stock
 * firmware's runtime state.
 *
 * The per-bytelane phase computation the reference code performs after the DLL
 * sweep is in g31_phase.c, and its edge-walk pre-pass is in g31_prepass.c. Both
 * are pure functions so they can be checked offline against the OEM binary;
 * reference/mrc/g31_phase_test.c and g31_prepass_test.c do that.
 *
 * The cold path initialises DRAM on the Foxconn G31MX after the mainboard runs
 * the vendor boot block's pre-MRC chipset program. Warm reset is untested.
 */

#include <arch/io.h>
#include <arch/hpet.h>
#include <cbmem.h>
#include <cf9_reset.h>
#include <commonlib/helpers.h>
#include <lib.h>
#include <types.h>
#include <console/console.h>
#include <delay.h>
#include <device/pci_ops.h>
#include <device/device.h>
#include <device/pci_def.h>
#include <device/mmio.h>
#include <device/smbus_host.h>
#include <southbridge/intel/common/rcba.h>
#include <southbridge/intel/i82801gx/i82801gx.h>
#include <spd.h>
#include <string.h>

#include "g31.h"
#include "g31_warm.h"
#include "g31_mrc_services.h"
#include "g31_cold.h"
#include "g31_limits.h"
#include "g31_phase.h"
#include "raminit.h"
#include "x4x.h"


/* Minimum clocks for each timing parameter; see section 5 of the write-up. */
static const u8 g31_timing_min[8] = { 9, 3, 3, 0, 15, 0, 0, 0 };
/* Largest value each timing field can hold. */
static const u8 g31_timing_max[8] = { 0x0f, 0x07, 0x07, 0x0f, 0x3f, 0x0f, 0x0f, 0x0f };
/* SPD byte each parameter comes from. */
static const u8 g31_timing_spd[8] = { 30, 27, 29, 36, 42, 37, 28, 38 };
/* Whether the SPD byte is in quarter-ns units. */
static const u8 g31_timing_quarter[8] = { 0, 1, 1, 1, 0, 1, 1, 1 };

/* SPD byte 40 extension adder in ps, indexed by its low nibble. */
static const u32 g31_ext_adder[12] = {
	0, 256000, 250, 256250, 333, 256333, 500, 256500, 667, 256667, 750, 256750,
};

/* Which SPD (tCK, tAC) byte pair describes a given CAS, by CLmax - CL. */
static const u8 g31_cas_spd[3][2] = { { 9, 10 }, { 23, 24 }, { 25, 26 } };

enum {
	T_RAS, T_RP, T_RCD, T_WR, T_RFC, T_WTR, T_RRD, T_RTP,
};

struct g31_dimm {
	bool present;
	u8 spd[64];
};

static void g31_read_spd(struct g31_dimm dimms[4], const u8 *spd_map)
{
	int i, j, value;
	u8 checksum;

	for (i = 0; i < 4; i++) {
		if (!spd_map[i])
			continue;
		value = smbus_read_byte(spd_map[i], SPD_MEMORY_TYPE);
		if (value < 0)
			continue;
		if (value != DDR2SPD)
			die("G31: unsupported SPD memory type\n");
		dimms[i].spd[SPD_MEMORY_TYPE] = value;
		checksum = 0;
		for (j = 0; j <= SPD_CHECKSUM_FOR_BYTES_0_TO_62; j++) {
			if (j != SPD_MEMORY_TYPE) {
				value = smbus_read_byte(spd_map[i], j);
				if (value < 0)
					die("G31: SPD byte read failed\n");
				dimms[i].spd[j] = value;
			}
			if (j < SPD_CHECKSUM_FOR_BYTES_0_TO_62)
				checksum += dimms[i].spd[j];
		}
		if (checksum != dimms[i].spd[SPD_CHECKSUM_FOR_BYTES_0_TO_62])
			die("G31: SPD checksum mismatch\n");

		/*
		 * The reference code clamps the column count and repairs a
		 * known-bad primary width byte before using either.
		 */
		dimms[i].spd[4] = MAX(9, MIN(10, dimms[i].spd[4]));
		if (dimms[i].spd[13] == 0xa5)
			dimms[i].spd[13] = 8;
		if (dimms[i].spd[3] < 12 || dimms[i].spd[3] > 15 ||
		    (dimms[i].spd[13] != 8 && dimms[i].spd[13] != 16) ||
		    (dimms[i].spd[17] != 4 && dimms[i].spd[17] != 8) ||
		    (dimms[i].spd[5] & 7) > 1 ||
		    (dimms[i].spd[13] == 16 && (dimms[i].spd[5] & 7)) ||
		    (dimms[i].spd[40] & 0x0f) >= ARRAY_SIZE(g31_ext_adder))
			die("G31: unsupported SPD geometry or timing encoding\n");

		dimms[i].present = true;
	}
}

/* Fill in the x4x sysinfo view of the geometry. */
static void g31_decode_geometry(struct sysinfo *s, struct g31_dimm dimms[4])
{
	int i;

	for (i = 0; i < 4; i++) {
		s->dimms[i].card_type = RAW_CARD_UNPOPULATED;
		if (!dimms[i].present)
			continue;

		s->dimms[i].card_type = RAW_CARD_POPULATED;
		s->dimms[i].ranks = (dimms[i].spd[5] & 7) + 1;
		s->dimms[i].width = (dimms[i].spd[13] == 16) ? CHIP_WIDTH_x16 : CHIP_WIDTH_x8;
		s->dimms[i].n_banks = (dimms[i].spd[17] == 8) ? N_BANKS_8 : N_BANKS_4;
		s->dimms[i].rows = dimms[i].spd[3];
		s->dimms[i].cols = dimms[i].spd[4];
		s->dimms[i].page_size = (1 << dimms[i].spd[4]) * dimms[i].spd[13] * 8;

		if (s->dimms[i].ranks > 2)
			die("G31: more than two ranks per DIMM is not supported\n");
	}
}

/* The packed 2-bit-per-DIMM rank/type code the tables are indexed by. */
static void g31_dimm_config(struct sysinfo *s)
{
	int ch, i, code;

	for (ch = 0; ch < 2; ch++) {
		s->dimm_config[ch] = 0;
		for (i = ch * 2; i < ch * 2 + 2; i++) {
			if (!DIMM_IS_POPULATED(s->dimms, i))
				continue;
			if (s->dimms[i].width == CHIP_WIDTH_x16)
				code = 3;		/* single rank x16 */
			else if (s->dimms[i].ranks == 2)
				code = 2;		/* dual rank x8 */
			else
				code = 1;		/* single rank x8 */
			s->dimm_config[ch] |= code << ((i & 1) * 2);
		}
	}
}

/*
 * Pick the fastest mem_clock and then the lowest CAS the modules and the FSB
 * can all sustain. See section 4 of the write-up.
 */
static enum cb_err g31_pick_timings(struct sysinfo *s, struct g31_dimm dimms[4])
{
	u8 cas_mask = 0x78;
	int i, clk, cl, max_clk;

	for (i = 0; i < 4; i++)
		if (dimms[i].present)
			cas_mask &= dimms[i].spd[18];
	if (!cas_mask) {
		printk(BIOS_ERR, "G31: no CAS latency common to all DIMMs\n");
		return CB_ERR;
	}

	/*
	 * The reference code derives its ceiling from D0:F0 0xe3 bit 7, but that
	 * expression is min(8 - bit, 3), which is 3 either way, so the register
	 * does not affect the result and is not read here.
	 */
	max_clk = MEM_CLOCK_800MHz;
	switch (s->max_fsb) {
	case FSB_CLOCK_800MHz:
		max_clk = MIN(max_clk, MEM_CLOCK_800MHz);
		break;
	case FSB_CLOCK_1066MHz:
		max_clk = MIN(max_clk, MEM_CLOCK_1066MHz);
		break;
	default:
		break;			/* FSB1333 imposes no ceiling */
	}

	for (clk = max_clk; clk >= MEM_CLOCK_533MHz; clk--) {
		u8 work = cas_mask;

		while (work) {
			bool ok = true;

			cl = __ffs(work);
			for (i = 0; i < 4 && ok; i++) {
				int clmax, tck_b, tac_b;

				if (!dimms[i].present)
					continue;
				clmax = log2(dimms[i].spd[18]);
				if (clmax - cl > 2 || clmax < cl) {
					ok = false;
					break;
				}
				tck_b = g31_cas_spd[clmax - cl][0];
				tac_b = g31_cas_spd[clmax - cl][1];
				if (!dimms[i].spd[tck_b] || !dimms[i].spd[tac_b])
					ok = false;
				else if (g31_jedec_tck[clk][0] < dimms[i].spd[tck_b] ||
					 g31_jedec_tck[clk][1] < dimms[i].spd[tac_b])
					ok = false;
			}
			if (ok) {
				s->selected_timings.mem_clk = clk;
				s->selected_timings.CAS = cl;
				s->selected_timings.tclk = g31_tck_ps[clk];
				return CB_SUCCESS;
			}
			work &= ~(1 << cl);
		}
	}
	printk(BIOS_ERR, "G31: no usable frequency and CAS combination\n");
	return CB_ERR;
}

/* SPD timings to DRAM clocks. See section 5 of the write-up. */
static enum cb_err g31_pick_dram_timings(struct sysinfo *s, struct g31_dimm dimms[4])
{
	unsigned int *out[8] = {
		&s->selected_timings.tRAS, &s->selected_timings.tRP,
		&s->selected_timings.tRCD, &s->selected_timings.tWR,
		&s->selected_timings.tRFC, &s->selected_timings.tWTR,
		&s->selected_timings.tRRD, &s->selected_timings.tRTP,
	};
	const u16 tck = g31_tck_ps[s->selected_timings.mem_clk];
	int p, i;

	for (p = 0; p < 8; p++) {
		u32 need = 0;
		u8 field = 0;

		for (i = 0; i < 4; i++) {
			u32 ps;

			if (!dimms[i].present)
				continue;
			ps = dimms[i].spd[g31_timing_spd[p]] * 1000;
			if (g31_timing_quarter[p])
				ps >>= 2;
			if (p == T_RFC)
				ps += g31_ext_adder[dimms[i].spd[40] & 0x0f];
			need = MAX(need, ps);
		}

		while ((u32)(g31_timing_min[p] + field) * tck < need) {
			if (++field > g31_timing_max[p]) {
				printk(BIOS_ERR, "G31: timing %d does not fit\n", p);
				return CB_ERR;
			}
		}
		if (p == T_RFC)
			field = ((field + 0x10) & 0xfe) - 0x0f;

		*out[p] = g31_timing_min[p] + field;
	}

	return CB_SUCCESS;
}

/* Step POST 0x24: program CLKCFG and read back what the hardware accepted. */
static enum cb_err g31_clkcfg(struct sysinfo *s)
{
	mchbar_clrsetbits32(CLKCFG_MCHBAR, CLKCFG_MEMCLK_MASK,
			    (s->selected_timings.mem_clk << CLKCFG_MEMCLK_SHIFT)
			    | CLKCFG_UPDATE);

	s->selected_timings.mem_clk = (mchbar_read32(CLKCFG_MCHBAR)
				       & CLKCFG_MEMCLK_MASK) >> CLKCFG_MEMCLK_SHIFT;

	if (s->selected_timings.mem_clk < MEM_CLOCK_533MHz ||
	    s->selected_timings.mem_clk > MEM_CLOCK_800MHz)
		return CB_ERR;

	if (g31_mem_rate[s->selected_timings.mem_clk] >
	    g31_fsb_rate[g31_fsb_mode(s->selected_timings.fsb_clk)]) {
		printk(BIOS_ERR, "G31: DDR2-%d exceeds the %d MT/s FSB\n",
		       g31_mem_rate[s->selected_timings.mem_clk],
		       g31_fsb_rate[g31_fsb_mode(s->selected_timings.fsb_clk)]);
		return CB_ERR;
	}
	return CB_SUCCESS;
}

/* Step POST 0x27: per (FSB, mem_clock) clock and DLL programming. */
static void g31_clock_dll(struct sysinfo *s)
{
	static const u16 dll_reg[10] = {
		0x6d8, 0x6dc, 0x6e8, 0x6ec, 0x6f8, 0x6fc, 0x700, 0x704, 0x708, 0x70c,
	};
	/* The first four entries are written to a second register as well. */
	static const u16 dll_alias[4] = { 0x6e0, 0x6e4, 0x6f0, 0x6f4 };
	const int idx = g31_fsb_mode(s->selected_timings.fsb_clk) * 4
			+ s->selected_timings.mem_clk - 1;
	int i;

	mchbar_write32(0xc04, g31_clk_c04[idx]);
	mchbar_write32(0xc50, g31_clk_c50[idx]);
	mchbar_write32(0xc54, g31_clk_c54[idx]);
	mchbar_setbits8(0xc08, 0x80);

	for (i = 0; i < 10; i++)
		mchbar_write32(dll_reg[i], g31_clk_dll[idx][i]);
	for (i = 0; i < 4; i++)
		mchbar_write32(dll_alias[i], g31_clk_dll[idx][i]);
}

/* Step POST 0x28: pattern-test setup, before the DRAM timing registers. */
static void g31_stage28(struct sysinfo *s)
{
	const int mem_clk = s->selected_timings.mem_clk;
	const int idx = g31_fsb_mode(s->selected_timings.fsb_clk) * 4 + mem_clk - 1;
	int ch;

	mchbar_clrsetbits16(0x1fa, 0x0ff0, g31_stage28_ctrl[idx][0] << 4);
	mchbar_clrsetbits16(0x1f8, 0x3f00, g31_stage28_ctrl[idx][1] << 8);
	mchbar_setbits8(0x1f8, 1 << 5);
	mchbar_clrbits8(0x1f8, 1);

	for (ch = 0; ch < 2; ch++) {
		const u32 o = ch * G31_CH1;

		mchbar_clrsetbits8(o + 0x5f8, 1 << 1, g31_stage28_5f8[mem_clk - 1] << 1);
		mchbar_clrsetbits32(o + 0x5d0, 1U << 31,
				    (u32)g31_stage28_5d0[mem_clk] << 31);
	}
}

/* Step POST 0x29: write the three-record per-channel script. */
static void g31_stage29(struct sysinfo *s)
{
	const int mem_clk = s->selected_timings.mem_clk;
	const int cas = s->selected_timings.CAS;
	const int idx = 3 * mem_clk + cas - 3;
	int ch;

	if (cas < 3 || cas > 6 || idx >= ARRAY_SIZE(g31_stage29_224))
		die("G31: unsupported CAS for POST 0x29\n");
	for (ch = 0; ch < 2; ch++) {
		const u32 o = ch * G31_CH1;

		if (!s->dimm_config[ch])
			continue;
		mchbar_write32(o + 0x220, 0x58001117);
		mchbar_write32(o + 0x224, g31_stage29_224[idx]);
		mchbar_setbits32(o + 0x248, 1 << 23);
	}
}

/* Step POST 0x33: signal group drive strength, then arm the calibration. */
static void g31_signal_groups(struct sysinfo *s)
{
	const int mem_clk = s->selected_timings.mem_clk;
	int ch, g, cls, rt, val;

	for (ch = 0; ch < 2; ch++) {
		if (!s->dimm_config[ch])
			continue;
		rt = s->dimm_config[ch];

		for (g = 0; g < G31_SIGGROUPS; g++) {
			const u32 base = ch * G31_CH1 + g31_siggroup_off[g];
			int idx;

			if (g == 1) {
				cls = (rt == 6 || (rt >= 9 && rt <= 10)) ? 2 : 1;
				val = g31_siggroup_val_g1[rt];
			} else {
				cls = (g >= 2 && g <= 3) ? 1 : 0;
				val = g31_siggroup_val[g];
			}
			idx = mem_clk * 3 + cls;

			mchbar_write8(base + 0x21, val);
			mchbar_clrsetbits16(base + 0x1a, 0x03fc, val << 2);
			mchbar_write16(base + 0x28, g31_siggroup_28[idx]);
			mchbar_clrsetbits8(base + 0x29, 0x80,
					   (g31_siggroup_flag[g] & 1) << 7);
			mchbar_write8(base + 0x27, g31_siggroup_27[idx]);
			mchbar_setbits8(base + 0x25, 0x80);
			mchbar_setbits8(base + 0x1f, 0x02);
		}
		mchbar_clrsetbits8(ch * G31_CH1 + 0x450, 0x3f, g31_odt_drive[mem_clk]);
		mchbar_clrsetbits8(ch * G31_CH1 + 0x454, 0x3f, g31_odt_drive[mem_clk]);
	}

	rt = s->dimm_config[0] ? s->dimm_config[0] : s->dimm_config[1];
	mchbar_clrsetbits8(0x164, 0x3f, g31_global_164[rt]);
	mchbar_clrsetbits8(0x168, 0x3f, g31_global_164[rt]);
	mchbar_clrsetbits8(0x16c, 0x3f, g31_global_164[rt]);
	mchbar_clrsetbits8(0x170, 0x3f, g31_global_164[rt]);

	mchbar_write32(0x138, g31_global_138[mem_clk]);
	mchbar_write32(0x13c, g31_global_138[mem_clk]);
	mchbar_write32(0x134, g31_global_134[mem_clk - 1]);
	mchbar_write16(0x174, 0x01ff);
	mchbar_write16(0x176, 0x0134);
	mchbar_write32(G31_CAL_CTRL, g31_global_130[mem_clk - 1]);

	if (!s->dimm_config[0])
		mchbar_clrbits32(G31_CAL_CTRL, 1 << 27);
	if (!s->dimm_config[1])
		mchbar_clrbits32(G31_CAL_CTRL, 1 << 28);

	/* Bit 0 starts the calibration and the hardware clears it when done. */
	mchbar_setbits8(G31_CAL_CTRL, 1);
}

/* Step POST 0x35. */
static void g31_wait_calibration(void)
{
	int timeout = 100000;

	while ((mchbar_read8(G31_CAL_CTRL) & 1) && --timeout)
		udelay(1);
	if (!timeout)
		die("G31: DRAM calibration did not complete\n");
}

static u8 g31_timing_12d_read8(void *ctx)
{
	(void)ctx;
	return mchbar_read8(0x12d);
}

static u32 g31_timing_12d_read32(void *ctx, unsigned int offset)
{
	(void)ctx;
	return mchbar_read32(offset);
}

static void g31_timing_12d_write8(void *ctx, u8 value)
{
	(void)ctx;
	mchbar_write8(0x12d, value);
}

/* Step POST 0x31: DRAM timing registers. */
static void g31_program_timings(struct sysinfo *s)
{
	static const struct g31_timing_12d_ops timing_12d_ops = {
		.read8 = g31_timing_12d_read8,
		.read32 = g31_timing_12d_read32,
		.write8 = g31_timing_12d_write8,
	};
	const struct timings *t = &s->selected_timings;
	const u8 tCL = t->CAS;
	const bool eight_bank = (s->dimms[0].n_banks == N_BANKS_8) ||
				(s->dimms[1].n_banks == N_BANKS_8) ||
				(s->dimms[2].n_banks == N_BANKS_8) ||
				(s->dimms[3].n_banks == N_BANKS_8);
	const bool four_bank = !eight_bank;
	const bool x16 = (s->dimms[0].width == CHIP_WIDTH_x16) ||
			 (s->dimms[2].width == CHIP_WIDTH_x16);
	int ch;

	for (ch = 0; ch < 2; ch++) {
		const u32 o = ch * G31_CH1;
		u32 v;
		u8 idx;
		int initial_latency;

		if (!s->dimm_config[ch])
			continue;

		mchbar_setbits8(o + 0x26f, 0x03);

		mchbar_write16(o + 0x250,
			       ((t->tWR + tCL + 3) << 6)
			       | (MAX(t->tRTP, 2) * 4 + 8)
			       | (t->tRAS << 11) | 1);

		v = ((((four_bank << 4) | t->tRRD) << 4 | t->tRP) << 13)
		    | ((eight_bank + t->tRP) << 9) | t->tRFC;
		if (!four_bank) {
			idx = (x16 ? 1 : 0)
			      + (((mchbar_read8(o + 0x26f) >> 1) & 1)
				 + (t->mem_clk - 1) * 2) * 2;
			v |= (u32)g31_timing_252[idx] << 22;
		}
		mchbar_write32(o + 0x252, v);

		mchbar_write16(o + 0x256, (t->tRCD << 12) | 0x468);
		mchbar_write32(o + 0x258,
			       ((t->tWTR + tCL + 3) << 12) | (t->tRCD << 17) | 0x546);
		mchbar_write16(o + 0x25b, ((eight_bank + t->tRP) << 9) | t->tRFC);
		mchbar_clrsetbits16(o + 0x260, 0x026e, 0x0190);
		mchbar_clrsetbits8(o + 0x25d, 0x3f, t->tRAS);
		mchbar_write16(o + 0x244, 0x2310);
		mchbar_clrsetbits8(o + 0x246, 0x1e,
				   (g31_timing_246[t->mem_clk] << 2) | 1);

		/* The OEM seeds read latency here before the first JEDEC DRAM read. */
		initial_latency = g31_initial_read_latency(g31_timing_246[t->mem_clk],
							tCL, g31_tck_ps[t->mem_clk],
							g31_fsb_ps[g31_fsb_mode(t->fsb_clk)]);
		if (initial_latency < 0)
			die_with_post_code(G31_POST_INITIAL_LATENCY_BAD,
					   "G31: initial read latency outside register range\n");
		mchbar_clrsetbits16(o + 0x248, 0x1f00, initial_latency << 8);
		/* POST 0x84 replaces the provisional value after receive training. */

		v = tCL - 2 - (tCL - 1 > 2 ? 1 : 0);
		mchbar_clrsetbits16(o + 0x24d, 0x01ff,
				    ((tCL - 1 > 2) << 8) | (v << 4) | v);
		mchbar_write16(o + 0x25e, 0x15a5);
		mchbar_clrbits32(o + 0x265, 0x1f);
		mchbar_clrsetbits16(o + 0x265, 0x3f00, (tCL + 9) << 8);
		mchbar_write32(o + 0x269,
			       g31_initial_refresh_control(mchbar_read32(o + 0x269),
						   g31_trefi[t->mem_clk]));
		mchbar_setbits8(o + 0x274, 1);
		mchbar_clrbits8(o + 0x24c, 0x03);
		mchbar_clrsetbits16(o + 0x24d, 0x7c00, (tCL - 1 + 10) << 10);
		mchbar_clrsetbits8(o + 0x267, 0x2c, 0x13);
		mchbar_clrsetbits16(o + 0x26d, 0x0140, 0x02bf);
		mchbar_clrsetbits32(o + 0x269, 0x00300000, 0x02000000);
		mchbar_clrbits8(o + 0x271, 0x80);
		mchbar_clrbits8(o + 0x274, 0x06);
	}

	mchbar_setbits16(0x125, 0x1fe0);
	mchbar_clrsetbits16(0x127, 0x02bf, 0x0540);
	mchbar_setbits8(0x129, 0x1f);
	mchbar_clrbits8(0x12f, 1);
	mchbar_clrsetbits32(0x241, ~0xfffe0011U, 0x11);
	mchbar_clrsetbits32(0x641, ~0xfffe0011U, 0x11);
	mchbar_write32(0x120, 0x508d7f5f);
	g31_timing_12d_program(&timing_12d_ops, NULL);
}

/* Step POST 0x30, both of its register scripts. */
static void g31_script30(struct sysinfo *s)
{
	const int idx = g31_script30_index(s->selected_timings.CAS,
			s->selected_timings.mem_clk, ARRAY_SIZE(g31_script30_115));
	int ch;

	if (idx < 0)
		die("G31: unsupported CL/memory clock for script 30\n");
	mchbar_clrbits8(0xf18, 1);
	mchbar_clrbits8(0xfaf, 0x80);
	mchbar_clrbits8(0xffb, 0x80);
	mchbar_setbits32(G31_CAL_CTRL, 0x00020000);
	mchbar_write8(0x18d, 0x0f);
	mchbar_clrsetbits32(0x2c0, ~0xfff0000fU, 0x000cc5f0);
	mchbar_clrsetbits32(0x044, ~0xff10ffffU, 0x00ef0000);
	mchbar_clrbits8(0x1ac, 0x40);
	mchbar_clrsetbits8(0x1f0, 0x80, 0x80);
	mchbar_clrsetbits16(0x18c, 0x0300, 0x0300);
	mchbar_write16(0x115, g31_script30_115[idx]);
	mchbar_clrsetbits32(0x117, 0x00ffffff, g31_script30_117[idx]);
	mchbar_write8(0x124, 0x07);
	mchbar_clrsetbits32(0x12a, 0x01ffffff, 0x00100080);
	mchbar_write16(0xf6e, 0xfffe);

	for (ch = 0; ch < 2; ch++) {
		const u32 o = ch * G31_CH1;

		if (!s->dimm_config[ch])
			continue;
		mchbar_setbits8(o + 0x262, 1);
		mchbar_clrsetbits32(o + 0x248, ~0xf08fffffU, 0x0e400000);
		mchbar_write8(o + 0x264, g31_script30_264[idx]);
		mchbar_write16(o + 0x23c, g31_script30_23c[idx]);
	}
}

/* Step POST 0x34. */
static void g31_script34(struct sysinfo *s)
{
	int ch;

	for (ch = 0; ch < 2; ch++) {
		const u32 o = ch * G31_CH1;

		if (!s->dimm_config[ch])
			continue;
		mchbar_write16(o + 0x298, g31_script34_298[s->dimm_config[ch] & 3]);
		mchbar_write16(o + 0x294, 0x0000);
		mchbar_clrsetbits16(o + 0x29c, 0x0fff, 0x0668);
		mchbar_clrsetbits32(o + 0x260, ~0xf8f1c3ffU, 0x03063c00);
	}
}

/* Step POST 0x36: temporary rank decode, valid only until step POST 0x43. */
static void g31_temp_decode(void)
{
	mchbar_clrsetbits32(0x260, 1, 0x00f00000);
	mchbar_clrsetbits32(0x660, 1, 0x00f00000);
	mchbar_write32(0x208, 0x01010101);
	mchbar_write32(0x608, 0x01010101);
	mchbar_write32(0x200, 0x00040002);
	mchbar_write32(0x204, 0x00080006);
	mchbar_write32(0x600, 0x00040002);
	mchbar_write32(0x604, 0x00100006);
	mchbar_setbits8(G31_CHDECMISC, STACKED_MEM);
	mchbar_write32(0x104, 0);
	mchbar_write16(0x102, 0x0400);
	mchbar_write8(0x110, 0x58);
	mchbar_write16(0x10e, 0);
	mchbar_write32(0x108, 0);

	/* The same step sets a provisional host memory map. */
	pci_write_config16(HOST_BRIDGE, D0F0_TOLUD, 0x4000);
	pci_write_config16(HOST_BRIDGE, D0F0_TOM, 0x0010);
	pci_write_config16(HOST_BRIDGE, D0F0_TOUUD, 0x0400);
	pci_write_config32(HOST_BRIDGE, D0F0_GBSM, 0x40000000);
	pci_write_config32(HOST_BRIDGE, D0F0_BGSM, 0x40000000);
	pci_write_config32(HOST_BRIDGE, D0F0_TSEG, 0x40000000);
}

/* Step POST 0x38. */
static void g31_pre_jedec(struct sysinfo *s)
{
	int ch;

	mchbar_setbits8(0x40, 1 << 1);
	for (ch = 0; ch < 2; ch++)
		if (s->dimm_config[ch])
			mchbar_setbits32(G31_CKECTRL(ch), 1 << 27);
}

/*
 * Step POST 0x39: the JEDEC DDR2 power-up sequence. The command codes, the
 * sequence and the ODT payloads are the same ones x4x uses; the reference code
 * stores them pre-shifted by three. Unlike x4x, it writes the selected channel
 * only and does not restore NORMALOP between commands.
 */
static void g31_jedec_init(struct sysinfo *s, const struct g31_cold_ops *ops)
{
	static const struct { u8 cmd; u16 addr; } jedec[12] = {
		{ NOP_CMD,	0x000 },
		{ PRECHARGE_CMD, 0x000 },
		{ EMRS2_CMD,	0x000 },
		{ EMRS3_CMD,	0x000 },
		{ EMRS1_CMD,	0x000 },	/* ODT, enables the DLL */
		{ MRS_CMD,	0x100 },	/* DLL reset */
		{ PRECHARGE_CMD, 0x000 },
		{ CBR_CMD,	0x000 },
		{ CBR_CMD,	0x000 },
		{ MRS_CMD,	0x000 },	/* DLL out of reset */
		{ EMRS1_CMD,	0x380 },	/* OCD calibration default */
		{ EMRS1_CMD,	0x000 },	/* OCD exit */
	};
	const u16 mrsval = (s->selected_timings.CAS << 4)
			   | ((s->selected_timings.tWR - 1) << 9) | 0xb;
	int ch, r, i;

	udelay(200);

	FOR_EACH_POPULATED_RANK(s->dimms, ch, r) {
		for (i = 0; i < 12; i++) {
			u32 v = jedec[i].addr;

			if (jedec[i].cmd == EMRS1_CMD)
				v |= g31_jedec_odt[s->dimm_config[ch]][r] >> 3;
			else if (jedec[i].cmd == MRS_CMD)
				v |= mrsval;

			g31_jedec_command(ch, r, jedec[i].cmd, v, ops, NULL);
			udelay(1);
		}
	}
}

/* Step POST 0x41. */
static void g31_refresh_config(struct sysinfo *s)
{
	const int idx = (s->selected_timings.mem_clk - 1)
			+ g31_fsb_mode(s->selected_timings.fsb_clk) * 4;
	int ch;

	mchbar_clrbits8(0x40, 1 << 1);

	for (ch = 0; ch < 2; ch++) {
		const u32 o = ch * G31_CH1;

		if (!s->dimm_config[ch])
			continue;
		mchbar_clrsetbits32(o + 0x274, 0x00ffff00,
				    (((u32)(g31_refresh_274[idx][0] | 0x40)) << 13)
				    | ((u32)g31_refresh_274[idx][1] << 8));
		mchbar_clrbits8(o + 0x274, 0x80);
		mchbar_setbits8(o + 0x26c, 1);
		mchbar_setbits32(o + 0x27c, 1 << 8);
		mchbar_write8(o + 0x292, 0xf2);
		mchbar_setbits8(o + 0x271, 0x0e);
	}
}

/* Step POST 0x42: set INITDONE and REFEN on both channels. */
static void g31_refresh_enable(void)
{
	mchbar_setbits32(0x268, 0xc0000000);
	mchbar_setbits32(0x668, 0xc0000000);
}

/* Step POST 0x43: final rank attributes, CKE masks and rank boundaries. */
static void g31_rank_decode(struct sysinfo *s)
{
	u32 dra[2] = { 0, 0 };
	u8 rank_mask = 0;
	int ch, r, i;

	for (i = 0; i < 8; i++) {
		const int dimm = i >> 1;
		int code;

		if (!RANK_IS_POPULATED(s->dimms, i >> 2, i & 3))
			continue;
		rank_mask |= 1 << i;

		code = g31_dra_code[(s->dimms[dimm].rows - 12)
				    + 4 * ((s->dimms[dimm].cols - 9)
					   + 2 * ((s->dimms[dimm].width == CHIP_WIDTH_x16)
						  + 2 * (s->dimms[dimm].n_banks == N_BANKS_8)))];
		if (code == 0xff)
			die("G31: unsupported DRAM geometry\n");
		if (s->dimms[dimm].n_banks == N_BANKS_8)
			code |= 0x80;
		dra[i >> 2] |= (u32)code << ((i & 3) * 8);
	}

	mchbar_write32(G31_DRA(0), dra[0]);
	mchbar_write32(G31_DRA(1), dra[1]);

	mchbar_clrsetbits8(0x262, 0xf0, (rank_mask & 0x0f) << 4);
	mchbar_clrsetbits8(0x662, 0xf0, rank_mask & 0xf0);

	for (ch = 0; ch < 2; ch++) {
		const bool both = DIMM_IS_POPULATED(s->dimms, ch * 2) &&
				  DIMM_IS_POPULATED(s->dimms, ch * 2 + 1);
		if (!both)
			mchbar_setbits8(G31_CKECTRL(ch), 1);
	}

	for (ch = 0; ch < 2; ch++) {
		u32 run = 0;

		for (r = 0; r < 4; r++) {
			if (rank_mask & (1 << (ch * 4 + r)))
				run += g31_rank_size[(dra[ch] >> (r * 8)) & 0x7f];
			mchbar_write16(G31_DRB(ch, r), run);
		}
		s->channel_capacity[ch] = run << 6;	/* MiB */
	}

	/* Asymmetric channels get the stacked channel-1 mapping. */
	s->stacked_mode = 0;
	if (s->channel_capacity[0] != s->channel_capacity[1] &&
	    s->channel_capacity[0] && s->channel_capacity[1]) {
		const u32 top = (s->channel_capacity[0] + s->channel_capacity[1]) >> 6;
		int highest = -1;

		s->stacked_mode = 1;
		for (r = 0; r < 4; r++)
			if (rank_mask & (1 << (4 + r)))
				highest = r;
		for (r = highest; r >= 0 && r < 4; r++)
			mchbar_write16(G31_DRB(1, r), top);
	}
}

/* Step POST 0x44: channel interleave decode. */
static void g31_channel_decode(struct sysinfo *s)
{
	const int c0 = s->channel_capacity[0];
	const int c1 = s->channel_capacity[1];
	const int inter = s->stacked_mode ? 0 : 2 * MIN(c0, c1);
	u8 mode;

	if (c0 + c1 < 256)
		printk(BIOS_ERR, "G31: less than 256 MiB of memory\n");

	mchbar_clrsetbits8(G31_CHDECMISC, STACKED_MEM,
			   s->stacked_mode ? STACKED_MEM : 0);
	mchbar_write16(0x104, inter);
	mchbar_write16(0x102, (c0 + c1) - inter);

	mode = !c0 ? 0 : (c1 ? 0x40 : 0x20);
	if (!inter)
		mode |= 0x18;
	if (!(s->stacked_mode && c0 && c1))
		mode |= (c0 <= c1) ? 0x05 : 0x04;
	mchbar_write8(0x110, mode);
	mchbar_write16(0x10e, 0);

	if (s->stacked_mode && c1)
		mchbar_write16(0x108, 0);
	else if (c0 > c1)
		mchbar_write16(0x108, (inter >> 1) + MIN(c0, c1));
	else
		mchbar_write16(0x108, inter >> 1);
	mchbar_write16(0x10a, inter >> 1);

	s->nmode = (c0 == c1) ? 2 : 1;
}

/* Step POST 0x45: host memory map. */
static bool g31_host_map(struct sysinfo *s)
{
	static const u8 gms_mib[8] = { 0, 1, 4, 8, 16, 0, 0, 0 };
	static const u8 ggms_mib[4] = { 0, 1, 2, 0 };
	const u16 ggc = pci_read_config16(HOST_BRIDGE, D0F0_GGC);
	const u32 gms = gms_mib[(ggc >> 4) & 7];
	const u32 gtt = ggms_mib[(ggc >> 8) & 3];
	/* 2 MiB, as native x4x: TSEG also holds the SMM stage cache. */
	const u32 tseg = 2;
	/*
	 * The reference code takes the size of the hole below 4 GiB from its
	 * caller. The vendor BIOS passes 768 MiB (stock TOLUD 0xd0000000),
	 * which also keeps the ECAM window above TOLUD.
	 */
	const u32 ceiling = MIN(4096 - 768, CONFIG_ECAM_MMCONF_BASE_ADDRESS >> 20);
	u32 top = s->channel_capacity[0] + s->channel_capacity[1];
	u32 tolud = MIN(ceiling, top);
	u32 touud, gbsm, bgsm, tsegbase;
	bool remap;

	remap = (top - tolud) > 0x40;
	if (remap) {
		u32 rbase, rlimit;

		top &= ~0x3f;
		tolud &= ~0x3f;
		rbase = MAX(top, 0x1000);
		rlimit = (MIN(top, 0x1000) - tolud) + rbase - 0x40;
		touud = rlimit + 0x40;
		pci_write_config16(HOST_BRIDGE, D0F0_REMAPBASE, rbase >> 6);
		pci_write_config16(HOST_BRIDGE, D0F0_REMAPLIMIT, rlimit >> 6);
	} else {
		touud = top;
		/* Leave the remap window disabled. */
		pci_write_config16(HOST_BRIDGE, D0F0_REMAPBASE, 0x03ff);
		pci_write_config16(HOST_BRIDGE, D0F0_REMAPLIMIT, 0);
	}

	gbsm = tolud - gms;
	bgsm = gbsm - gtt;
	tsegbase = bgsm - tseg;

	pci_write_config16(HOST_BRIDGE, D0F0_TOLUD, tolud << 4);
	pci_write_config16(HOST_BRIDGE, D0F0_TOM, (s->channel_capacity[0]
						  + s->channel_capacity[1]) >> 6);
	pci_write_config16(HOST_BRIDGE, D0F0_TOUUD, touud);
	pci_write_config32(HOST_BRIDGE, D0F0_GBSM, gbsm << 20);
	pci_write_config32(HOST_BRIDGE, D0F0_BGSM, bgsm << 20);
	pci_write_config32(HOST_BRIDGE, D0F0_TSEG, tsegbase << 20);
	/* Enable TSEG with a 2 MiB size. */
	pci_update_config8(HOST_BRIDGE, D0F0_ESMRAMC, ~0x07, (1 << 1) | (1 << 0));

	return remap;
}

/* Step POST 0x46: final decode. */
static void g31_final_decode(struct sysinfo *s, bool remap)
{
	int ch, r, n, distinct;
	u8 code, first_code = 0;
	u32 dra;
	u32 mch40 = 0x0a038000;
	u32 mch20 = 0x00013001;
	u16 fsb_bits;

	mchbar_write32(0xfa8, g31_fa8[g31_fsb_mode(s->selected_timings.fsb_clk)]);

	for (ch = 0; ch < 2; ch++) {
		const u32 o = ch * G31_CH1;

		if (!s->dimm_config[ch])
			continue;
		mchbar_setbits8(o + 0x26c, 1);
		mchbar_write32(o + 0x278, 0x88141881);
		mchbar_write16(o + 0x27c, 0x0041);
		mchbar_setbits16(o + 0x272, 0x0100);
		mchbar_clrsetbits8(o + 0x243, 0x02, 0x01);
		mchbar_write32(o + 0x288, 0x08040200);
		mchbar_write32(o + 0x28c, 0xff402010);
		mchbar_write32(o + 0x290, 0x04f2091c);
	}

	/* The global block the reference code runs as a register script. */
	pci_or_config8(HOST_BRIDGE, 0xf0, 1);
	mchbar_clrsetbits32(0xfa0, ~0xfffdfffdU,
			    s->selected_timings.fsb_clk == FSB_CLOCK_1333MHz ?
			    0x00020002 : 0x00000002);
	mchbar_clrsetbits32(0xfa4, ~0xffeffffdU,
			    s->selected_timings.fsb_clk == FSB_CLOCK_1333MHz ?
			    0x00100003 : 0x00100002);
	mchbar_write32(0x030, 0x001f5a06);
	mchbar_write32(0x034, 0x01902810);
	mchbar_write32(0x038, 0x37000000);
	mchbar_write32(0x03c, ((u32)(remap ? 0x65 : 0x23) << 24) | 0x00034417);
	switch (s->selected_timings.fsb_clk) {
	case FSB_CLOCK_800MHz:
		mch40 |= 0x04000000;
		mch20 |= 0x02000000;
		fsb_bits = 0x1068;
		break;
	case FSB_CLOCK_1066MHz:
		fsb_bits = 0x15e0;
		break;
	case FSB_CLOCK_1333MHz:
		fsb_bits = 0x1b58;
		break;
	default:
		die("G31: unsupported FSB for final decode\n");
	}
	mchbar_clrsetbits32(0x040, ~0xf0fc7fffU, mch40);
	mchbar_write32(0x044, 0x000f0000);
	mchbar_write32(0x010, 0x3d0a0000 | fsb_bits);
	mchbar_write32(0x020, mch20);
	pci_and_config8(HOST_BRIDGE, 0xf0, (u8)~1);

	if (s->stacked_mode)
		return;

	/* Rank interleave code, only when both channels look identical. */
	ch = s->channel_capacity[0] ? 0 : 1;
	if (s->channel_capacity[0] && s->channel_capacity[1]) {
		if (mchbar_read32(0x200) != mchbar_read32(0x600) ||
		    mchbar_read32(0x204) != mchbar_read32(0x604))
			return;
	}

	dra = mchbar_read32(G31_DRA(ch));
	n = 0;
	distinct = 0;
	for (r = 0; r < 4; r++) {
		if (!RANK_IS_POPULATED(s->dimms, ch, r))
			continue;
		code = (dra >> (r * 8)) & 0x7f;
		if (!n)
			first_code = code;
		else if (code != first_code)
			distinct = 1;
		n++;
	}

	switch (n) {
	case 1:
		code = 0x6c;
		break;
	case 2:
		code = 0x2c;
		break;
	case 4:
		code = distinct ? 0x2c : 0xac;
		break;
	default:
		return;
	}
	mchbar_clrsetbits8(G31_CHDECMISC, 0xfc, code & 0xfc);
}

static void g31_cold_set_bit0(void *ctx, unsigned int ch)
{
	(void)ctx;
	mchbar_setbits8(G31_RCVEN_TRIG(ch), 1);
}

static void g31_cold_read32(void *ctx, uint32_t address)
{
	(void)ctx;
	(void)read32p(address);
}

static void g31_cold_write_command(void *ctx, unsigned int ch, u8 command)
{
	(void)ctx;
	mchbar_write8(G31_JEDEC_CMD(ch), command);
}





static u8 g31_warm_read8(void *ctx, u32 offset)
{
	(void)ctx;
	return mchbar_read8(offset);
}

static u16 g31_warm_read16(void *ctx, u32 offset)
{
	(void)ctx;
	return mchbar_read16(offset);
}

static u32 g31_warm_read32(void *ctx, u32 offset)
{
	(void)ctx;
	return mchbar_read32(offset);
}

static void g31_warm_write32(void *ctx, u32 offset, u32 value)
{
	(void)ctx;
	mchbar_write32(offset, value);
}

static u8 g31_mrc_read_pmcon2(void *ctx)
{
	(void)ctx;
	return pci_read_config8(PCI_DEV(0, 0x1f, 0), GEN_PMCON_2);
}

static void g31_mrc_write_pmcon2(void *ctx, u8 value)
{
	(void)ctx;
	pci_write_config8(PCI_DEV(0, 0x1f, 0), GEN_PMCON_2, value);
}

static u32 g31_mrc_read_hptc(void *ctx)
{
	(void)ctx;
	return RCBA32(HPTC);
}

static void g31_mrc_write_hptc(void *ctx, u32 value)
{
	(void)ctx;
	RCBA32(HPTC) = value;
}

static u32 g31_mrc_read_hpet_conf(void *ctx)
{
	(void)ctx;
	return read32p(HPET_BASE_ADDRESS + 0x10);
}

static void g31_mrc_write_hpet_conf(void *ctx, u32 value)
{
	(void)ctx;
	write32p(HPET_BASE_ADDRESS + 0x10, value);
}

static void g31_mrc_full_reset(void *ctx)
{
	(void)ctx;
	/* The OEM callback issues one CF9=0x0e write, without touching CAR. */
	outb(FULL_RST | RST_CPU | SYS_RST, RST_CNT);
}

/*
 * MCHBAR 0xc8c programming the Award boot block performs before it enters the
 * memory reference code (773F1P14 raw 0x75eee). The stock pre-JEDEC register
 * dump has 0xc8c = 0x00030012.
 */
static void g31_award_chipset_preinit(void)
{
	u32 v;

	v = mchbar_read32(0xc8c);
	mchbar_setbits32(0xc8c, 1 << 9);
	v = (v & 0xfff0ff80) | 0x00030012;
	mchbar_clrbits32(0xc8c, 1 << 9);
	mchbar_write32(0xc8c, v);
	if (!(pci_read_config32(HOST_BRIDGE, 0xe4) & (1 << 14))) {
		v = mchbar_read32(0xc8c);
		mchbar_setbits32(0xc8c, 1 << 9);
		mchbar_write32(0xc8c, (v & 0xfff0fd80) | 0x00030012);
	}
}

void g31_sdram_initialize(int boot_path, const u8 *spd_map)
{
	static const struct g31_cold_ops ops = {
		.set_bit0 = g31_cold_set_bit0,
		.read32 = g31_cold_read32,
		.write_command = g31_cold_write_command,
	};
	static const struct g31_warm_ops warm_ops = {
		.read8 = g31_warm_read8,
		.read16 = g31_warm_read16,
		.read32 = g31_warm_read32,
		.write32 = g31_warm_write32,
	};
	static const struct g31_mrc_service_ops service_ops = {
		.read_pmcon2 = g31_mrc_read_pmcon2,
		.write_pmcon2 = g31_mrc_write_pmcon2,
		.read_hptc = g31_mrc_read_hptc,
		.write_hptc = g31_mrc_write_hptc,
		.read_hpet_conf = g31_mrc_read_hpet_conf,
		.write_hpet_conf = g31_mrc_write_hpet_conf,
		.full_reset = g31_mrc_full_reset,
	};
	struct g31_warm_result recovered;
	struct g31_mrc_timer_state timer;
	struct g31_dimm dimms[4] = {};
	struct sysinfo s = {};
	u8 ranks[2] = {};
	u8 channels = 0;
	u8 retained_coarse;
	u32 pmsts;
	int warm_status;
	int retained_memclk;
	bool remap;
	int i;
	u32 fsb_strap;

	if (boot_path != BOOT_PATH_NORMAL && boot_path != BOOT_PATH_WARM_RESET) {
		die_with_post_code(G31_POST_UNSUPPORTED_RESUME,
				   "G31: S3 RAM init is not supported\n");
	}


	g31_award_chipset_preinit();

	s.boot_path = boot_path;
	s.spd_type = DDR2;
	for (i = 0; i < 4; i++)
		s.spd_map[i] = spd_map[i];

	/* OEM detect reads CLKCFG[2:0] to select FSB mode. */
	fsb_strap = mchbar_read32(CLKCFG_MCHBAR) & CLKCFG_FSBCLK_MASK;
	switch (fsb_strap) {
	case 0:
		s.selected_timings.fsb_clk = FSB_CLOCK_1066MHz;
		break;
	case 2:
		s.selected_timings.fsb_clk = FSB_CLOCK_800MHz;
		break;
	case 4:
		s.selected_timings.fsb_clk = FSB_CLOCK_1333MHz;
		break;
	default:
		die("G31: unknown FSB strap in CLKCFG\n");
	}
	s.max_fsb = s.selected_timings.fsb_clk;

	/* Reference detect phase 1 writes MCHBAR 0xc23 before reading SPD. */
	mchbar_write8(0xc23, 0x06);
	g31_read_spd(dimms, spd_map);
	if (!dimms[0].present && !dimms[1].present &&
	    !dimms[2].present && !dimms[3].present) {
		post_code(G31_POST_NO_MEMORY);
		die("G31: no DIMM detected\n");
	}
	/* Reference detect phase 3: 0xc20 and 0xc22, then 0xc21 at its end. */
	mchbar_write8(0xc20, 0x44);
	mchbar_write8(0xc22, 0x59);
	mchbar_write8(0xc21, 0x44);

	g31_decode_geometry(&s, dimms);
	g31_dimm_config(&s);
	for (i = 0; i < 8; i++)
		if (RANK_IS_POPULATED(s.dimms, i >> 2, i & 3))
			ranks[i >> 2] |= 1 << (i & 3);
	for (i = 0; i < 2; i++)
		if (ranks[i])
			channels |= 1 << i;
	pmsts = mchbar_read32(PMSTS_MCHBAR);
	retained_coarse = mchbar_read8(G31_RCVEN_COARSE_OUT);
	if (g31_pick_timings(&s, dimms) != CB_SUCCESS)
		die("G31: cannot pick a DRAM frequency\n");
	if (g31_pick_dram_timings(&s, dimms) != CB_SUCCESS)
		die("G31: cannot pick DRAM timings\n");
	retained_memclk = (mchbar_read32(CLKCFG_MCHBAR) & CLKCFG_MEMCLK_MASK) >>
			  CLKCFG_MEMCLK_SHIFT;
	warm_status = g31_warm_preflight(boot_path, pmsts, retained_coarse, channels,
					   s.selected_timings.mem_clk, retained_memclk);
	if (warm_status == G31_WARM_UNSAFE_RESET)
		die_with_post_code(G31_POST_UNSAFE_WARM_STATE,
				   "G31: warm reset without channel self-refresh; power cycle\n");
	if (warm_status == G31_WARM_CLOCK_MISMATCH)
		die_with_post_code(G31_POST_WARM_CLOCK_MISMATCH,
				   "G31: retained DDR clock %d disagrees with SPD clock %d\n",
				   retained_memclk, s.selected_timings.mem_clk);
	if (warm_status != G31_WARM_OK)
		die_with_post_code(G31_POST_LOST_WARM_TRAINING,
				   "G31: no coherent retained receive-enable state\n");

	/* Award's vtable marks DRAM_INIT before the OEM dispatch's POST 0x01. */
	if (g31_mrc_mark_start(&service_ops, NULL)) {
		die_with_post_code(G31_POST_RESET_FAILED,
				   "G31: CF9 reset returned after DRAM_INIT recovery\n");
	}
	g31_mrc_timer_enable(&service_ops, NULL, &timer);

	/* Clear the self-refresh status the reference code clears at POST 0x03. */
	mchbar_setbits32(PMSTS_MCHBAR, PMSTS_BOTH_SELFREFRESH);

	if (g31_step_selected(G31_STEP_CLOCK_CFG, boot_path) &&
	    g31_clkcfg(&s) != CB_SUCCESS)
		die("G31: memory clock is not compatible with the FSB\n");

	mchbar_setbits16(0xc1c, 0x8000);
	g31_clock_dll(&s);
	if (g31_step_selected(G31_STEP_PATTERN_SETUP, boot_path))
		g31_stage28(&s);
	g31_stage29(&s);
	g31_program_timings(&s);
	if (g31_step_selected(G31_STEP_DLL_SWEEP, boot_path))
		g31_dll_sweep(&s);
	g31_after_dll(channels, &ops, NULL);
	if (g31_step_selected(G31_STEP_SIGNAL_GROUPS, boot_path))
		g31_signal_groups(&s);
	g31_script34(&s);
	if (g31_step_selected(G31_STEP_WAIT_CAL, boot_path))
		g31_wait_calibration();
	g31_temp_decode();
	g31_pre_jedec(&s);
	g31_jedec_init(&s, &ops);
	g31_refresh_config(&s);
	g31_refresh_enable();
	g31_after_refresh(ranks, &ops, NULL);

	if (g31_step_selected(G31_STEP_RCVEN_TRAIN, boot_path))
		g31_rcven_train(&s);
	if (g31_step_selected(G31_STEP_RCVEN_RESTORE, boot_path)) {
		if (g31_warm_restore(&warm_ops, NULL, retained_coarse, &recovered))
			die_with_post_code(G31_POST_LOST_WARM_TRAINING,
				"G31: retained receive-enable state changed\n");
		for (i = 0; i < 2; i++) {
			s.rcven_t[i].min_common_coarse = recovered.coarse[i];
			memcpy(s.rcven_t[i].coarse_offset, recovered.offset[i],
			       sizeof(recovered.offset[i]));
		}
	}
	g31_rcven_apply(&s);
	g31_read_latency(&s);

	g31_script30(&s);
	g31_rank_decode(&s);
	g31_channel_decode(&s);
	if (!is_devfn_enabled(PCI_DEVFN(2, 0))) {
		/*
		 * Like the vendor BIOS with an add-in graphics card: disable
		 * the IGD and its stolen memory so it neither decodes legacy
		 * VGA cycles nor reserves memory below TOLUD.
		 */
		pci_write_config16(HOST_BRIDGE, D0F0_GGC, 1 << 1);
		pci_and_config32(HOST_BRIDGE, D0F0_DEVEN, ~(IGD0EN | IGD1EN));
	}
	remap = g31_host_map(&s);
	g31_final_decode(&s, remap);

	mchbar_setbits8(G31_CAL_CTRL, 0x82);
	g31_mrc_mark_end(&service_ops, NULL);
	mchbar_setbits32(0xa30, 1 << 26);
	g31_mrc_timer_disable(&service_ops, NULL, &timer);

	/* S3 resume is not supported here, so CBMEM always starts empty. */
	if (cbmem_recovery(0))
		die("G31: CBMEM initialization failed\n");
}
