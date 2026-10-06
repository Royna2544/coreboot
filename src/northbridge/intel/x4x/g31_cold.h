/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef __NORTHBRIDGE_INTEL_G31_COLD_H__
#define __NORTHBRIDGE_INTEL_G31_COLD_H__

#include <stdint.h>

struct g31_cold_ops {
	void (*set_bit0)(void *ctx, unsigned int channel);
	void (*read32)(void *ctx, uint32_t address);
	void (*write_command)(void *ctx, unsigned int channel, uint8_t command);
};

/* Silent OEM step after POST 0x32, before signal-group programming. */
void g31_after_dll(uint8_t channels, const struct g31_cold_ops *ops, void *ctx);
/* Silent OEM step after POST 0x42, before receive-enable training. */
void g31_after_refresh(const uint8_t ranks[2], const struct g31_cold_ops *ops,
		       void *ctx);
/* OEM JEDEC command: selected-channel write followed by its rank-address read. */
void g31_jedec_command(unsigned int channel, unsigned int rank, uint8_t command,
		       uint32_t payload, const struct g31_cold_ops *ops, void *ctx);

#endif /* __NORTHBRIDGE_INTEL_G31_COLD_H__ */
