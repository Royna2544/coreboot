/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * Intel G31 (Bearlake) DRAM initialisation.
 *
 * Everything here was recovered from the OEM BIOS' BLMRC220.006 memory
 * reference code; see reference/re/memory-training.md in the project root for
 * the evidence behind each register and table, and reference/mrc/ for the
 * scripts that check the recovered values against a capture of the stock
 * firmware's runtime state.
 *
 * G31 shares the host bridge device ID, the fixed BAR addresses, the rank
 * boundary/attribute registers, CLKCFG, PMSTS, the host memory map registers
 * and the JEDEC command mechanism with Intel 4-series (x4x). It differs in the
 * DRAM timing register field layout, in the per-bytelane training register
 * layout, and in needing its own per-frequency table set.
 */

#ifndef __NORTHBRIDGE_INTEL_G31_H__
#define __NORTHBRIDGE_INTEL_G31_H__

#include <stdint.h>
#include "raminit.h"

/* The x4x FSB enum starts at 800 MT/s; the OEM tables start at 533 MT/s. */
static inline int g31_fsb_mode(enum fsb_clock fsb)
{
	return fsb + 1;
}

/* Channel 1 register window offset. */
#define G31_CH1			0x400

/*
 * POST codes. These are the codes the OEM reference code passes to its
 * progress callback, in its own order, so a port-0x80 capture of this
 * implementation lines up with a capture of the stock firmware.
 */
#define G31_POST_CLKCFG		0x24
#define G31_POST_CLKDLL		0x27
#define G31_POST_SCRIPT28	0x28
#define G31_POST_SCRIPT29	0x29
#define G31_POST_TIMINGS	0x31
#define G31_POST_ANALOG		0x32
#define G31_POST_SIGGROUPS	0x33
#define G31_POST_SCRIPT34	0x34
#define G31_POST_WAIT_CAL	0x35
#define G31_POST_TEMP_DECODE	0x36
#define G31_POST_PRE_JEDEC	0x38
#define G31_POST_JEDEC		0x39
#define G31_POST_REFRESH_CFG	0x41
#define G31_POST_REFRESH_EN	0x42
#define G31_POST_RCVEN		0x81
#define G31_POST_RCVEN_APPLY	0x83
#define G31_POST_READ_LATENCY	0x84
#define G31_POST_SCRIPT30	0x30
#define G31_POST_RANK_DECODE	0x43
#define G31_POST_CHAN_DECODE	0x44
#define G31_POST_HOST_MAP	0x45
#define G31_POST_FINAL_DECODE	0x46
#define G31_POST_DONE		0x47

/* Codes this implementation adds for the parts the OEM code does silently. */
#define G31_POST_SPD		0x20
#define G31_POST_NO_MEMORY	0xe0	/* not unique to missing memory */
#define G31_POST_UNSAFE_WARM_STATE	0xe6
#define G31_POST_UNSUPPORTED_RESUME	0xe7
#define G31_POST_LOST_WARM_TRAINING	0xe8
#define G31_POST_WARM_CLOCK_MISMATCH	0xe9
#define G31_POST_RESET_FAILED	0xec
#define G31_POST_INITIAL_LATENCY_BAD	0xee
#define G31_POST_RCVEN_RESTORE	0x82


/*
 * MCHBAR registers. Offsets without a channel note are global; the rest take
 * G31_CH1 added for channel 1.
 */
#define G31_DLL_STATUS		0x18c	/* bits 5:0 from ds:0x54a4, bit 0 set */
#define G31_DLL_PHASE		0x190	/* [3:0] coarse, [6:4] fine */
#define G31_PT_ERR_LO		0x184	/* pattern test error count, low word */
#define G31_PT_ERR_HI		0x188	/* pattern test error count, high word */
#define G31_PT_CTRL		0x180	/* bit 4 test control, bit 8 done */
#define G31_PT_START		0x1f8	/* bit 19 advances the pattern test */
#define G31_CAL_CTRL		0x130	/* bit 0 starts calibration, self-clearing */
#define G31_CHDECMISC		0x111	/* bit 1 STACKED_MEM, bits 7:2 rank code */

#define G31_DRB(ch, r)		(0x200 + (ch) * G31_CH1 + (r) * 2)
#define G31_DRA(ch)		(0x208 + (ch) * G31_CH1)
#define G31_CKECTRL(ch)		(0x260 + (ch) * G31_CH1)
#define G31_JEDEC_CMD(ch)	(0x271 + (ch) * G31_CH1)

/* Receive-enable training registers. Note the negative per-lane stride. */
#define G31_RCVEN_FINE(ch)	(0x52c + (ch) * G31_CH1)
#define G31_RCVEN_MEDIUM(ch)	(0x53d + (ch) * G31_CH1)
#define G31_RCVEN_LANE_EN(ch)	(0x53c + (ch) * G31_CH1)
#define G31_RCVEN_CTL(ch, lane)	(0x5c4 + (ch) * G31_CH1 - (lane) * 0x10)
#define G31_RCVEN_SAMPLE(ch, l)	(0x5cc + (ch) * G31_CH1 - (l) * 0x10)
#define G31_RCVEN_TRIG(ch)	(0x5f4 + (ch) * G31_CH1)
#define G31_RCVEN_RESET		0x5ff
#define G31_RCVEN_COARSE_OUT	0xa0c	/* nibble per channel, non-zero once trained */
#define G31_READ_LATENCY(ch)	(0x248 + (ch) * G31_CH1)
#define G31_READ_LAT_MASK(ch)	(0x23e + (ch) * G31_CH1)

/* Signal group blocks, seven per channel. */
#define G31_SIGGROUPS		7

/* Tables recovered from the reference code; see g31_tables.c. */
extern const uint16_t g31_tck_ps[6];
extern const uint16_t g31_fsb_ps[4];
extern const uint16_t g31_mem_rate[6];
extern const uint16_t g31_fsb_rate[4];
extern const uint8_t g31_jedec_tck[4][2];
extern const uint16_t g31_trefi[5];
extern const uint32_t g31_clk_c04[16];
extern const uint32_t g31_clk_c50[16];
extern const uint32_t g31_clk_c54[16];
extern const uint32_t g31_clk_dll[16][10];
extern const uint8_t g31_stage28_ctrl[16][2];
extern const uint8_t g31_stage28_5f8[4];
extern const uint8_t g31_stage28_5d0[5];
extern const uint8_t g31_prepass_budget[17];
extern const uint32_t g31_stage29_224[13];
extern const uint8_t g31_dra_code[32];
extern const uint8_t g31_rank_size[17];
extern const uint16_t g31_siggroup_off[G31_SIGGROUPS];
extern const uint8_t g31_siggroup_flag[G31_SIGGROUPS];
extern const uint8_t g31_siggroup_val[G31_SIGGROUPS];
extern const uint8_t g31_siggroup_val_g1[11];
extern const uint16_t g31_siggroup_28[12];
extern const uint8_t g31_siggroup_27[12];
extern const uint8_t g31_odt_drive[5];
extern const uint8_t g31_global_164[11];
extern const uint32_t g31_global_134[4];
extern const uint32_t g31_global_130[4];
extern const uint32_t g31_global_138[5];
extern const uint8_t g31_analog_199[5];
extern const uint8_t g31_analog_1e4[5];
extern const uint8_t g31_analog_1ac_lo[5];
extern const uint8_t g31_analog_1ac_hi[5];
extern const uint16_t g31_analog_19c[4][3];
extern const uint16_t g31_analog_1a0[4][3];
extern const uint16_t g31_analog_1a4[4][3];
extern const uint16_t g31_lane_reg[8];
extern const uint8_t g31_lane_55c[8][8];
extern const uint8_t g31_lane_550[4][4];
extern const uint16_t g31_sweep_offset[4];
extern const uint16_t g31_sweep_coarse[4];
extern const uint16_t g31_sweep_fine5[4];
extern const uint16_t g31_phase_offset[4][2];
extern const uint16_t g31_phase_limit[5];
extern const uint8_t g31_phase_rec[6][48];
extern const uint8_t g31_dll_status[4][2];
extern const uint8_t g31_refresh_274[16][2];
extern const uint8_t g31_timing_246[5];
extern const uint8_t g31_timing_252[12];
extern const uint16_t g31_script30_115[12];
extern const uint32_t g31_script30_117[12];
extern const uint8_t g31_script30_264[12];
extern const uint16_t g31_script30_23c[12];
extern const uint16_t g31_script34_298[4];
extern const uint32_t g31_fa8[4];
extern const uint16_t g31_jedec_odt[16][4];
extern const uint8_t g31_rl_count[16];
extern const uint16_t g31_rl_cand[16][5];
extern const uint8_t g31_rl_ab[4][2];
extern const uint16_t g31_rl_base[4];
extern const uint8_t g31_rl_fine[4];
extern const uint8_t g31_rl_d[4];
extern const uint8_t g31_rl_f[5][2];
extern const uint8_t g31_rl_rec[8][3];

void g31_sdram_initialize(int boot_path, const uint8_t *spd_map);

/* g31_train.c */
void g31_dll_sweep(struct sysinfo *s);
void g31_rcven_train(struct sysinfo *s);
void g31_rcven_apply(struct sysinfo *s);
void g31_read_latency(struct sysinfo *s);

#endif /* __NORTHBRIDGE_INTEL_G31_H__ */
