/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ICH7 services around the BLMRC220.006 dispatcher, POST 0x01/0x51/0x52. */
#include "g31_mrc_services.h"

#define GEN_PMCON_2_DRAM_INIT	0x80
#define GEN_PMCON_2_W1C	0x1d

int g31_mrc_mark_start(const struct g31_mrc_service_ops *ops, void *ctx)
{
	uint8_t value = ops->read_pmcon2(ctx);

	if (value & GEN_PMCON_2_DRAM_INIT) {
		/* OEM clears the marker before requesting a full CF9 reset. */
		ops->write_pmcon2(ctx, value & ~(GEN_PMCON_2_DRAM_INIT |
					       GEN_PMCON_2_W1C));
		ops->full_reset(ctx);
		return 1;
	}
	ops->write_pmcon2(ctx, (value & ~GEN_PMCON_2_W1C) |
				GEN_PMCON_2_DRAM_INIT);
	return 0;
}

void g31_mrc_timer_enable(const struct g31_mrc_service_ops *ops, void *ctx,
			  struct g31_mrc_timer_state *timer)
{
	timer->saved_hptc = ops->read_hptc(ctx);
	ops->write_hptc(ctx, (timer->saved_hptc & ~3U) | 0x80);
	ops->read_hptc(ctx); /* Flush the posted HPTC write before accessing HPET. */
	ops->write_hpet_conf(ctx, ops->read_hpet_conf(ctx) | 1U);
}

void g31_mrc_mark_end(const struct g31_mrc_service_ops *ops, void *ctx)
{
	uint8_t value = ops->read_pmcon2(ctx);

	ops->write_pmcon2(ctx, value & ~(GEN_PMCON_2_DRAM_INIT | GEN_PMCON_2_W1C));
}

void g31_mrc_timer_disable(const struct g31_mrc_service_ops *ops, void *ctx,
			   const struct g31_mrc_timer_state *timer)
{
	ops->write_hpet_conf(ctx, ops->read_hpet_conf(ctx) & ~3U);
	ops->write_hptc(ctx, timer->saved_hptc);
}
