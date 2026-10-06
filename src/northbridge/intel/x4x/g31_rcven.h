/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef __NORTHBRIDGE_INTEL_G31_RCVEN_H__
#define __NORTHBRIDGE_INTEL_G31_RCVEN_H__

#include <stdbool.h>
#include <stdint.h>

struct g31_rcven_ops {
	void (*set_delay)(void *ctx, uint8_t delay);
	void (*set_fine)(void *ctx, uint8_t fine);
	bool (*sample)(void *ctx, bool high, unsigned int count);
};

struct g31_rcven_result {
	uint8_t coarse;
	uint8_t fine;
};

/* Return -1 instead of committing an unmeasured or out-of-range transition. */
int g31_rcven_search(uint8_t start, const struct g31_rcven_ops *ops, void *ctx,
		     struct g31_rcven_result *result);
int g31_rcven_pack(const uint8_t offsets[8], uint16_t regs[2]);
int g31_rcven_install(const uint8_t offsets[8], uint32_t base,
		      void (*write)(void *ctx, uint32_t offset, uint16_t value),
		      void *ctx);

#endif /* __NORTHBRIDGE_INTEL_G31_RCVEN_H__ */
