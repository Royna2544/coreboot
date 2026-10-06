/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Intel G31 DRAM training: the DLL phase sweep against the hardware pattern
 * test, receive-enable training, and the read latency that falls out of both.
 *
 * Recovered from the OEM BIOS' BLMRC220.006 reference code. See sections 8, 9
 * and 9a of reference/re/memory-training.md.
 */

#include <arch/io.h>
#include <commonlib/helpers.h>
#include <lib.h>
#include <types.h>
#include <console/console.h>
#include <delay.h>
#include <device/mmio.h>

#include "g31.h"
#include "g31_limits.h"
#include "g31_phase.h"
#include "g31_prepass.h"
#include "g31_rcven.h"
#include "raminit.h"
#include "x4x.h"

/* Derate a picosecond figure the way the reference code does. */
#define DERATE(ps) ((u32)(ps) * 100 / 110)

/* Collect the error count from the last pattern test run. */
static u32 pt_result(void)
{
	int timeout = 100000;

	while (!(mchbar_read16(G31_PT_CTRL) & (1 << 8)) && --timeout)
		udelay(1);
	if (!timeout) {
		printk(BIOS_ERR, "G31: pattern test did not complete\n");
		die("G31: pattern test timed out\n");
	}
	return ((u32)mchbar_read16(G31_PT_ERR_HI) << 16) | mchbar_read16(G31_PT_ERR_LO);
}

static void g31_prepass_launch(void *ctx)
{
	mchbar_setbits32(G31_PT_START, 1U << 19);
	udelay(1000);
}

static void g31_prepass_arm(void *ctx)
{
	mchbar_setbits8(G31_PT_CTRL, 1 << 4);
}

static u32 g31_prepass_result(void *ctx)
{
	return pt_result();
}

/*
 * Step the DLL phase state machine in MCHBAR 0x190: bits 3:0 are a coarse
 * counter and bits 6:4 a per-step state that alternates 2 and 6.
 */
static void dll_step(void)
{
	const u8 v = mchbar_read8(G31_DLL_PHASE);

	switch (v & 0x70) {
	case 0x20:
		mchbar_write8(G31_DLL_PHASE, v | 0x60);
		break;
	case 0x60:
		if ((v & 0x0f) != 0x0f) {
			mchbar_write8(G31_DLL_PHASE, v + 1);
			mchbar_clrsetbits8(G31_DLL_PHASE, 0x50, 0x20);
		}
		break;
	default:
		mchbar_clrsetbits8(G31_DLL_PHASE, 0x50, 0x20);
		break;
	}
}

static void g31_dll_step(void *ctx)
{
	dll_step();
}

/* Convert the current MCHBAR 0x190 contents into a phase in picoseconds. */
static u32 dll_phase_ps(int mem_clk)
{
	const u8 v = mchbar_read8(G31_DLL_PHASE);
	const int i = mem_clk - 1;

	return (v & 0x0f) * g31_sweep_coarse[i]
	       + ((v >> 4) & 7) * g31_sweep_fine5[i] / 5
	       + g31_sweep_offset[i];
}

/* The success branch of the OEM phase engine, with measured DLL state. */
static void g31_program_lane_phase(const struct sysinfo *s)
{
	const int mem_clk = s->selected_timings.mem_clk;
	const int i = mem_clk - 1;
	const u8 phase = mchbar_read8(G31_DLL_PHASE);
	const unsigned int coarse = (phase & 0x0f) + g31_rl_d[i];
	int ch, n;

	if (coarse > 0x0f)
		die("G31: post-sweep DLL phase out of range\n");
	mchbar_clrsetbits8(G31_DLL_PHASE, 0x0f, coarse);
	for (ch = 0; ch < 2; ch++) {
		struct g31_phase_regs regs;
		const u32 o = ch * G31_CH1;

		if (!s->dimm_config[ch])
			continue;
		if (g31_phase_calculate(g31_phase_rec[ch + 2 * i], coarse,
					(phase >> 4) & 7, g31_sweep_coarse[i],
					g31_sweep_fine5[i], g31_phase_offset[i][ch],
					g31_tck_ps[mem_clk] / 2, g31_phase_limit[mem_clk],
					&regs))
			die("G31: per-lane phase outside qualified range\n");
		for (n = 0; n < 3; n++) {
			mchbar_write16(o + 0x508 + 4 * n, regs.coarse[n]);
			mchbar_clrsetbits16(o + 0x514 + 4 * n, 0x0fff,
					    regs.fine[n]);
		}
		mchbar_clrsetbits32(o + 0x5d0, 0x000001e0, regs.ctrl_5d0);
		mchbar_clrsetbits32(o + 0x5d8, 0x00f00000, regs.ctrl_5d8);
		mchbar_clrsetbits32(o + 0x5dc, 0x0003ccc0, regs.ctrl_5dc);
		mchbar_write32(o + 0x5e0, regs.ctrl_5e0);
	}
}

struct g31_post_sweep_context {
	const struct sysinfo *s;
	int i;
};

static void g31_post_sweep_mask_channel(void *ctx, unsigned int ch)
{
	const struct g31_post_sweep_context *state = ctx;
	const struct sysinfo *s = state->s;
	const u32 o = ch * G31_CH1;
	u32 mask = 0;
	u8 nib;
	int rank;

	if (s->dimm_config[ch]) {
		mchbar_clrbits32(o + 0x5d0, 1);
		mchbar_clrbits32(o + 0x5d4, 3);
	} else {
		mchbar_clrbits8(o + 0x528, 1);
	}

	for (rank = 0; rank < 4; rank++)
		if (!RANK_IS_POPULATED(s->dimms, ch, rank))
			mask |= 0x11000004U << rank;
	mchbar_clrsetbits32(o + 0x5d8, ~0x00ffffc3U, mask);

	switch (s->dimm_config[ch]) {
	case 0:
		nib = 0x3f;
		break;
	case 1:
	case 2:
	case 3:
		nib = 0x38;
		break;
	case 4:
	case 8:
	case 12:
		nib = 0x07;
		break;
	default:
		nib = 0x00;
		break;
	}
	mchbar_clrsetbits32(o + 0x5dc, 0x3f000000, (u32)nib << 24);
}

static void g31_post_sweep_enable_channels(void *ctx)
{
	(void)ctx;
	mchbar_setbits8(0x1e8, 0x90);
}

static void g31_post_sweep_program_channel(void *ctx, unsigned int ch)
{
	const struct g31_post_sweep_context *state = ctx;
	const u32 o = ch * G31_CH1;
	int lane;

	mchbar_clrsetbits8(o + 0x5d0, 0x02, 0x04);
	mchbar_setbits8(o + 0x5d8, 0xc0);
	mchbar_clrsetbits8(o + 0x5d9, 0x28, 0x17);
	mchbar_write8(o + 0x5dc,
			     g31_post_sweep_5dc(mchbar_read8(o + 0x5dc)));
	mchbar_clrsetbits8(o + 0x5de, 0x40, 0x80);

	for (lane = 0; lane < 8; lane++)
		mchbar_clrsetbits8(o + g31_lane_reg[lane], 0x0f,
					   g31_lane_55c[ch * 4 + state->i][lane]);
	for (lane = 0; lane < 8; lane++) {
		mchbar_clrsetbits8(o + 0x550 + lane * 0x10, 0x0f,
					   g31_lane_550[state->i][ch * 2]);
		mchbar_clrsetbits8(o + 0x554 + lane * 0x10, 0x07,
					   g31_lane_550[state->i][ch * 2 + 1]);
	}
}

static uint8_t g31_dll_status_read(void *ctx)
{
	(void)ctx;
	return mchbar_read8(G31_DLL_STATUS);
}

static void g31_dll_status_write(void *ctx, uint8_t value)
{
	(void)ctx;
	mchbar_write8(G31_DLL_STATUS, value);
}

static uint16_t g31_analog_19c_read(void *ctx)
{
	(void)ctx;
	return mchbar_read16(0x19c);
}

static void g31_analog_19c_write(void *ctx, uint16_t value)
{
	(void)ctx;
	mchbar_write16(0x19c, value);
}

/*
 * Step POST 0x32's phase engine. Walk up until the pattern test stops failing
 * everywhere, note that edge, walk on to the first error-free phase, and settle
 * on the midpoint.
 */
void g31_dll_sweep(struct sysinfo *s)
{
	static const struct g31_prepass_ops ops = {
		.launch = g31_prepass_launch,
		.arm = g31_prepass_arm,
		.result = g31_prepass_result,
	};
	static const struct g31_dll_sample_ops sample_ops = {
		.step = g31_dll_step,
		.arm = g31_prepass_arm,
		.result = g31_prepass_result,
	};
	static const struct g31_post_sweep_ops post_sweep_ops = {
		.mask_channel = g31_post_sweep_mask_channel,
		.enable_channels = g31_post_sweep_enable_channels,
		.program_channel = g31_post_sweep_program_channel,
	};
	static const struct g31_dll_status_ops status_ops = {
		.read = g31_dll_status_read,
		.write = g31_dll_status_write,
	};
	static const struct g31_analog_19c_ops analog_19c_ops = {
		.read = g31_analog_19c_read,
		.write = g31_analog_19c_write,
	};
	const int mem_clk = s->selected_timings.mem_clk;
	const int i = mem_clk - 1;
	const int fsb = g31_fsb_mode(s->selected_timings.fsb_clk);
	const unsigned int populated = !!s->dimm_config[0] |
				      (!!s->dimm_config[1] << 1);
	struct g31_post_sweep_context post_sweep = { .s = s, .i = i };
	u32 left, right, mid, err;
	int guard, allfail = 0, prepass;
	u8 coarse, fine;

	/* Preamble, in the reference code's order. */
	mchbar_write8(0x199, g31_analog_199[mem_clk]);
	mchbar_clrbits8(0x199, 1);
	mchbar_clrsetbits8(0x1e4, 0x0f, g31_analog_1e4[mem_clk]);
	for (guard = 0; guard < 8; guard++)
		mchbar_clrbits8(0x1b4 + guard * 4, 1);
	mchbar_write32(0x1e0, 0x00551803);
	mchbar_clrsetbits16(0x1ac, 0x1f9e,
			    (((u16)g31_analog_1ac_hi[mem_clk] << 6)
			     | g31_analog_1ac_lo[mem_clk]) << 1);
	mchbar_clrbits8(0x1ac, 1);
	mchbar_setbits16(0x1f0, 0x8000);
	mchbar_write16(G31_DLL_PHASE, 0);
	mchbar_write16(0x19c, 0xbfff);
	mchbar_write16(0x1a4, 0x1000);

	/* OEM 0xfffb84b6; its result is discarded before the main sweep. */
	prepass = g31_edge_prepass(g31_prepass_budget[fsb * 4 + mem_clk],
				    fsb == 2 && mem_clk == 4, 128, &ops, NULL);
	if (prepass < 0)
		die("G31: pattern-test edge walk exceeded launch limit\n");

	/* OEM steps the phase once before the first main-sweep sample. */
	err = g31_dll_first_sample(&sample_ops, NULL);
	for (guard = 0; guard <= 0x46 && err == 0xffffffff; guard++) {
		dll_step();
		mchbar_setbits8(G31_PT_CTRL, 1 << 4);
		err = pt_result();
	}
	if (guard > 0x46) {
		printk(BIOS_ERR, "G31: DLL sweep found no passing region\n");
		goto fallback;
	}
	if (!err)
		goto done;

	left = dll_phase_ps(mem_clk);

	/* Walk to the first error-free phase. */
	for (guard = 0; guard <= 0x46; guard++) {
		dll_step();
		mchbar_setbits8(G31_PT_CTRL, 1 << 4);
		err = pt_result();
		if (!err)
			break;
		if (err == 0xffffffff && ++allfail > 5) {
			printk(BIOS_ERR, "G31: DLL sweep lost the passing region\n");
			goto fallback;
		}
	}
	if (guard > 0x46) {
		printk(BIOS_ERR, "G31: DLL sweep found no error free phase\n");
		goto fallback;
	}

	right = dll_phase_ps(mem_clk);
	mid = ((right + left + g31_sweep_offset[i]) / 2) - g31_sweep_offset[i];

	coarse = mid / g31_sweep_coarse[i];
	fine = (mid - coarse * g31_sweep_coarse[i]) / (g31_sweep_fine5[i] / 5);
	if ((mid - coarse * g31_sweep_coarse[i]) % (g31_sweep_fine5[i] / 5)) {
		if (++fine > 7) {
			fine = 0;
			coarse++;
		}
	}
	if (coarse > 0x0f || fine > 7) {
		printk(BIOS_ERR, "G31: DLL midpoint out of range (%d, %d)\n", coarse, fine);
		goto fallback;
	}

	mchbar_clrsetbits8(G31_DLL_PHASE, 0x7f, (fine << 4) | coarse);

done:
	mchbar_clrbits16(0x5e8, 1 << 14);
	mchbar_setbits16(0x9e8, 1 << 14);
	g31_program_lane_phase(s);
	g31_dll_status_apply(g31_dll_status[i][(mchbar_read8(0x5f8) >> 1) & 1],
			     &status_ops, NULL);
	goto program;

fallback:
	die("G31: DLL sweep failed\n");

program:
	/* Post-sweep analog programming, from the same step. */
	{
		const int sel = (s->dimm_config[0] && s->dimm_config[1]) ? 2
				: (s->dimm_config[0] ? 0 : 1);

		if (s->dimm_config[0] || s->dimm_config[1])
			g31_analog_19c_program(g31_analog_19c[mem_clk][sel],
						 &analog_19c_ops, NULL);

		g31_post_sweep_channels(populated, &post_sweep_ops, &post_sweep);
		/* The reference code selects 0x1a0/0x1a4 after both channel blocks. */
		if (s->dimm_config[0] || s->dimm_config[1]) {
			mchbar_write16(0x1a0, g31_analog_1a0[i][sel]);
			mchbar_write16(0x1a4, g31_analog_1a4[i][sel]);
		}
		g31_dll_status_apply(g31_dll_status[i][(mchbar_read8(0x5f8) >> 1) & 1],
				     &status_ops, NULL);
		mchbar_setbits8(0x1e8, 1);
	}
}

/*
 * Receive-enable delay: a six-bit value split across two registers, bits 1:0
 * into 0x53d and bits 5:2 into 0x248.
 */
static void rcven_set_delay(int ch, u8 delay)
{
	mchbar_clrsetbits8(G31_RCVEN_MEDIUM(ch), 0x0c, (delay & 3) << 2);
	mchbar_clrsetbits32(G31_READ_LATENCY(ch), 0x000f0000,
			    ((u32)(delay >> 2) & 0x0f) << 16);
}

/* Take count samples and report whether every one matched expect. */
static bool rcven_sample(int ch, int lane, u32 probe, int expect, int count)
{
	bool ok = true;

	while (count--) {
		mchbar_clrbits8(G31_RCVEN_TRIG(0), 1 << 1);
		mchbar_setbits8(G31_RCVEN_TRIG(0), 1 << 1);
		mchbar_clrbits8(G31_RCVEN_TRIG(1), 1 << 1);
		mchbar_setbits8(G31_RCVEN_TRIG(1), 1 << 1);

		read32p(probe);
		if ((int)((mchbar_read32(G31_RCVEN_SAMPLE(ch, lane)) >> 6) & 1) != expect)
			ok = false;
	}
	return ok;
}

struct rcven_context {
	int ch, lane;
	u32 probe;
};

static void rcven_search_set_delay(void *ctx, uint8_t delay)
{
	const struct rcven_context *c = ctx;

	rcven_set_delay(c->ch, delay);
}

static void rcven_search_set_fine(void *ctx, uint8_t fine)
{
	const struct rcven_context *c = ctx;

	mchbar_clrsetbits8(G31_RCVEN_FINE(c->ch), 0x0f, fine);
}

static bool rcven_search_sample(void *ctx, bool high, unsigned int count)
{
	const struct rcven_context *c = ctx;

	return rcven_sample(c->ch, c->lane, c->probe, high, count);
}

static void rcven_write16(void *ctx, uint32_t offset, uint16_t value)
{
	(void)ctx;
	mchbar_write16(offset, value);
}

/* Step POST 0x81, cold boot only. */
void g31_rcven_train(struct sysinfo *s)
{
	static const struct g31_rcven_ops ops = {
		.set_delay = rcven_search_set_delay,
		.set_fine = rcven_search_set_fine,
		.sample = rcven_search_sample,
	};
	const int mem_clk = s->selected_timings.mem_clk;
	const u16 tck = g31_tck_ps[mem_clk];
	int ch, lane, rank;

	mchbar_clrbits8(G31_RCVEN_TRIG(0), 0x0c);
	mchbar_clrbits8(G31_RCVEN_TRIG(1), 0x0c);
	mchbar_clrbits8(G31_RCVEN_RESET, 0x80);

	for (ch = 0; ch < 2; ch++) {
		u32 results[TOTAL_BYTELANES];
		u32 probe;
		u32 min = ~0U;

		if (!s->dimm_config[ch])
			continue;

		for (rank = 0; rank < 4; rank++)
			if (RANK_IS_POPULATED(s->dimms, ch, rank))
				break;
		if (rank == 4)
			continue;
		probe = test_address(ch, rank);

		mchbar_clrbits32(G31_READ_LATENCY(ch), 1 << 20);
		mchbar_clrbits8(G31_RCVEN_LANE_EN(ch), 1);
		mchbar_write16(0x534 + ch * G31_CH1, 0x0924);
		mchbar_write16(0x538 + ch * G31_CH1, 0x0924);

		for (lane = 0; lane < TOTAL_BYTELANES; lane++) {
			struct rcven_context ctx = { ch, lane, probe };
			struct g31_rcven_result trained;

			mchbar_clrbits32(G31_RCVEN_CTL(ch, lane), 0x7000);
			mchbar_clrbits8(G31_RCVEN_FINE(ch), 0x0f);
			if (g31_rcven_search((s->selected_timings.CAS + 1) * 4,
					     &ops, &ctx, &trained))
				die("G31: receive-enable edge search did not converge\n");

			results[lane] = (u32)(trained.coarse + 1) * tck / 4
					+ trained.fine * g31_rl_fine[mem_clk - 1];
			min = MIN(min, results[lane]);
		}

		/* Pack as one common coarse value plus eight per-lane offsets. */
		s->rcven_t[ch].min_common_coarse = min * 4 / tck;
		for (lane = 0; lane < TOTAL_BYTELANES; lane++) {
			u32 base = (u32)s->rcven_t[ch].min_common_coarse * tck / 4;
			u32 off = (results[lane] - base) / g31_rl_fine[mem_clk - 1];

			if (off > 0x0f)
				die("G31: receive-enable lane offset is out of range\n");
			s->rcven_t[ch].coarse_offset[lane] = off;
		}
		if (g31_rcven_install(s->rcven_t[ch].coarse_offset,
				      G31_RCVEN_FINE(ch), rcven_write16, NULL))
			die("G31: receive-enable lane offsets are invalid\n");

	}
}

/* Step POST 0x83: apply the trained values on every boot path. */
void g31_rcven_apply(struct sysinfo *s)
{
	int ch, lane;

	for (ch = 0; ch < 2; ch++) {
		const u32 o = ch * G31_CH1;

		for (lane = 0; lane < TOTAL_BYTELANES; lane++) {
			mchbar_clrsetbits32(G31_RCVEN_CTL(ch, lane), 0x7000,
					    (u32)lane << 12);
			mchbar_clrbits8(G31_RCVEN_LANE_EN(ch), 1 << lane);
		}
		mchbar_clrbits8(o + 0x5f4, 0x0e);
		mchbar_setbits8(o + 0x5f4, 1 << 1);
		mchbar_setbits8(o + 0x5f4, 1 << 2);
		mchbar_setbits8(o + 0x5f4, 1 << 3);

		mchbar_clrsetbits8(G31_RCVEN_COARSE_OUT, 0x0f << (ch * 4),
				   (u32)((s->rcven_t[ch].min_common_coarse >> 2) & 0x0f)
				   << (ch * 4));
	}

	mchbar_setbits8(G31_RCVEN_RESET, 0x80);
	mchbar_clrbits8(G31_RCVEN_RESET, 0x80);
	mchbar_setbits8(G31_RCVEN_RESET, 0x80);
}

/*
 * Step POST 0x84: read latency. Builds a delay in picoseconds from the trained
 * receive-enable result plus a set of per-frequency table terms, divides it by
 * the derated FSB period, and writes the largest of several candidates.
 *
 * reference/mrc/step84.py checks this formula against the stock firmware's
 * runtime state: it reproduces the captured 0x248 and 0x23e for the subset of
 * trained values the capture leaves possible.
 */
void g31_read_latency(struct sysinfo *s)
{
	const int mem_clk = s->selected_timings.mem_clk;
	const int i = mem_clk - 1;
	const int fsb = g31_fsb_mode(s->selected_timings.fsb_clk);
	const u32 tck_der = DERATE(g31_tck_ps[mem_clk]);
	const u32 fsb_der = DERATE(g31_fsb_ps[fsb]);
	const int idx = fsb * 4 + i;
	const int count = g31_rl_count[idx];
	const int edi = 2 * g31_rl_f[mem_clk][0] + 4 - g31_rl_f[mem_clk][1];
	int ch;

	if (!count || count > 5 || !fsb_der)
		die("G31: no valid read latency candidates for this speed\n");

	for (ch = 0; ch < 2; ch++) {
		const int k = ch + i * 2;
		u32 total, term_a, term_b;
		u8 max_nibble = 0;
		u8 vals[5];
		u8 top = 0, mask = 0;
		int j, lane;

		if (!s->dimm_config[ch])
			continue;

		for (lane = 0; lane < TOTAL_BYTELANES; lane++)
			max_nibble = MAX(max_nibble, s->rcven_t[ch].coarse_offset[lane]);

		term_a = (u32)g31_rl_ab[i][1] * g31_lane_550[i][ch * 2 + 1]
			 + (u32)g31_rl_ab[i][0] * g31_lane_550[i][ch * 2];

		term_b = (u32)(g31_rl_d[i] + g31_rl_rec[k][1]) * g31_sweep_coarse[i];
		term_b += (u32)g31_tck_ps[mem_clk] * g31_rl_rec[k][0] / 2;
		term_b += (u32)g31_sweep_fine5[i] * g31_rl_rec[k][2] / 5;

		total = (u32)(max_nibble + 1) * g31_rl_fine[i]
			+ (u32)(s->rcven_t[ch].min_common_coarse + edi * 2) * tck_der / 4
			+ term_b + g31_rl_base[i] + term_a + 0x110f;

		if ((mchbar_read8(G31_DLL_STATUS) & 0x3e) == 0x1a)
			total += tck_der;

		for (j = 0; j < count; j++) {
			int value = g31_rl_candidate(total,
					DERATE(g31_rl_cand[idx][j]), fsb_der);

			if (value < 0)
				die("G31: read latency candidate is out of range\n");
			vals[j] = value;
			top = MAX(top, vals[j]);
		}
		for (j = 0; j < count; j++)
			if (vals[j] < top)
				mask |= 1 << j;

		mchbar_clrsetbits16(G31_READ_LATENCY(ch), 0x1f00, (u32)top << 8);
		mchbar_write16(G31_READ_LAT_MASK(ch), mask);
	}
}
