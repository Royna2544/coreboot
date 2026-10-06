/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef __NORTHBRIDGE_INTEL_G31_PHASE_H__
#define __NORTHBRIDGE_INTEL_G31_PHASE_H__

#include <stdint.h>

struct g31_phase_regs {
	uint16_t coarse[3];	/* 0x508, 0x50c, 0x510 */
	uint16_t fine[3];	/* 0x514, 0x518, 0x51c */
	uint32_t ctrl_5d0;
	uint32_t ctrl_5d8;
	uint32_t ctrl_5dc;
	uint32_t ctrl_5e0;
};

/* Calculate the OEM success-path phase values without accessing hardware. */
int g31_phase_calculate(const uint8_t records[48], unsigned int coarse,
			unsigned int fine, unsigned int unit, unsigned int fine5,
			unsigned int offset, unsigned int half, unsigned int limit,
			struct g31_phase_regs *regs);

uint8_t g31_post_sweep_5dc(uint8_t value);

struct g31_post_sweep_ops {
	void (*mask_channel)(void *ctx, unsigned int channel);
	void (*enable_channels)(void *ctx);
	void (*program_channel)(void *ctx, unsigned int channel);
};

void g31_post_sweep_channels(unsigned int populated,
			     const struct g31_post_sweep_ops *ops, void *ctx);

struct g31_dll_status_ops {
	uint8_t (*read)(void *ctx);
	void (*write)(void *ctx, uint8_t value);
};

void g31_dll_status_apply(uint8_t selected,
			  const struct g31_dll_status_ops *ops, void *ctx);

struct g31_analog_19c_ops {
	uint16_t (*read)(void *ctx);
	void (*write)(void *ctx, uint16_t value);
};

void g31_analog_19c_program(uint16_t selected,
			    const struct g31_analog_19c_ops *ops, void *ctx);

struct g31_timing_12d_ops {
	uint8_t (*read8)(void *ctx);
	uint32_t (*read32)(void *ctx, unsigned int offset);
	void (*write8)(void *ctx, uint8_t value);
};

void g31_timing_12d_program(const struct g31_timing_12d_ops *ops, void *ctx);

#endif /* __NORTHBRIDGE_INTEL_G31_PHASE_H__ */
