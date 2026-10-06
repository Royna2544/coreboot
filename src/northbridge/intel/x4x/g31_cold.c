/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "g31_cold.h"

void g31_after_dll(uint8_t channels, const struct g31_cold_ops *ops, void *ctx)
{
	unsigned int ch;

	for (ch = 0; ch < 2; ch++)
		if (channels & (1 << ch))
			ops->set_bit0(ctx, ch);
}

void g31_after_refresh(const uint8_t ranks[2], const struct g31_cold_ops *ops,
		       void *ctx)
{
	unsigned int ch, rank, bank;

	for (ch = 0; ch < 2; ch++)
		for (rank = 0; rank < 4; rank++) {
			if (!(ranks[ch] & (1 << rank)))
				continue;
			for (bank = 0; bank < 4; bank++)
				ops->read32(ctx, (ch << 29) | (rank << 27) |
					    0x800000 | (bank << 12));
		}
}

void g31_jedec_command(unsigned int channel, unsigned int rank, uint8_t command,
		       uint32_t payload, const struct g31_cold_ops *ops, void *ctx)
{
	ops->write_command(ctx, channel, command);
	ops->read32(ctx, (channel << 29) | (rank << 27) | (payload << 3));
}
