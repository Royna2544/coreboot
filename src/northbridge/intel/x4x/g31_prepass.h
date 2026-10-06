/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef __NORTHBRIDGE_INTEL_G31_PREPASS_H__
#define __NORTHBRIDGE_INTEL_G31_PREPASS_H__

#include <stdbool.h>
#include <stdint.h>

struct g31_prepass_ops {
	void (*launch)(void *ctx);	/* 0x1f8 bit 19, then the 1 ms wait */
	void (*arm)(void *ctx);	/* 0x180 bit 4 */
	uint32_t (*result)(void *ctx);	/* 0x180 bit 8, then error words */
};

/* OEM outcome code, or -1 when the port's total-launch cap is exhausted. */
int g31_edge_prepass(unsigned int budget, bool special, unsigned int max_launches,
		     const struct g31_prepass_ops *ops, void *ctx);

struct g31_dll_sample_ops {
	void (*step)(void *ctx);
	void (*arm)(void *ctx);
	uint32_t (*result)(void *ctx);
};

uint32_t g31_dll_first_sample(const struct g31_dll_sample_ops *ops, void *ctx);

#endif /* __NORTHBRIDGE_INTEL_G31_PREPASS_H__ */
