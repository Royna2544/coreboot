/* SPDX-License-Identifier: GPL-2.0-only */

#include <bootblock_common.h>
#include <northbridge/intel/x4x/x4x.h>
#include <superio/ite/common/ite.h>
#include <superio/ite/it8718f/it8718f.h>
#include "stock_preinit.h"

#define SERIAL_DEV PNP_DEV(0x2e, IT8718F_SP1)

void bootblock_mainboard_early_init(void)
{
	ite_enable_serial(SERIAL_DEV, CONFIG_TTYS0_BASE);
}

void mb_pre_raminit_setup(int s3_resume)
{
	g31mx_stock_preinit();
}

void mb_get_spd_map(u8 spd_map[4])
{
	spd_map[0] = 0x50;
	spd_map[2] = 0x52;
}
