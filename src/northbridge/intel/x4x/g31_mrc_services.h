/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef __NORTHBRIDGE_INTEL_G31_MRC_SERVICES_H__
#define __NORTHBRIDGE_INTEL_G31_MRC_SERVICES_H__

#include <stdint.h>
#include <stdbool.h>

struct g31_mrc_service_ops {
	uint8_t (*read_pmcon2)(void *ctx);
	void (*write_pmcon2)(void *ctx, uint8_t value);
	uint32_t (*read_hptc)(void *ctx);
	void (*write_hptc)(void *ctx, uint32_t value);
	uint32_t (*read_hpet_conf)(void *ctx);
	void (*write_hpet_conf)(void *ctx, uint32_t value);
	void (*full_reset)(void *ctx);
};

struct g31_mrc_timer_state {
	uint32_t saved_hptc;
};

/* A nonzero return means a reset was requested, but the callback returned. */
int g31_mrc_mark_start(const struct g31_mrc_service_ops *ops, void *ctx);
uint8_t g31_mrc_interrupted_post(uint8_t pmcon2, bool warm);
uint8_t g31_mrc_interrupted_state_post(uint8_t pmcon2, bool warm,
						   bool warm_reset);
uint8_t g31_mrc_raw_pmcon_post(uint8_t pmcon2);
void g31_mrc_timer_enable(const struct g31_mrc_service_ops *ops, void *ctx,
			  struct g31_mrc_timer_state *timer);
void g31_mrc_mark_end(const struct g31_mrc_service_ops *ops, void *ctx);
void g31_mrc_timer_disable(const struct g31_mrc_service_ops *ops, void *ctx,
			   const struct g31_mrc_timer_state *timer);

#endif /* __NORTHBRIDGE_INTEL_G31_MRC_SERVICES_H__ */
