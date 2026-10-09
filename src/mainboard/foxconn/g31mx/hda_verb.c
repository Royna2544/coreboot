/* SPDX-License-Identifier: GPL-2.0-only */

#include <device/azalia_device.h>

/* The vendor BIOS table for this codec (System BIOS module offset 0xaeae). */
static const u32 realtek_alc662_verbs[] = {
	AZALIA_SUBVENDOR(2, 0x105b0df7),
	AZALIA_PIN_CFG(2, 0x14, 0x01014010),	/* Rear line out, green */
	AZALIA_PIN_CFG(2, 0x15, AZALIA_PIN_CFG_NC(0)),
	AZALIA_PIN_CFG(2, 0x16, AZALIA_PIN_CFG_NC(0)),
	AZALIA_PIN_CFG(2, 0x18, 0x01a19830),	/* Rear mic in, pink */
	AZALIA_PIN_CFG(2, 0x19, 0x02a19c3f),	/* Front mic in, pink */
	AZALIA_PIN_CFG(2, 0x1a, 0x01813031),	/* Rear line in, blue */
	AZALIA_PIN_CFG(2, 0x1b, 0x02214c1f),	/* Front headphone, green */
	AZALIA_PIN_CFG(2, 0x1c, 0x593301f0),	/* CD in, not connected */
	AZALIA_PIN_CFG(2, 0x1d, 0x40048603),	/* PC beep */
	AZALIA_PIN_CFG(2, 0x1e, 0x99430120),	/* OEM pin default; no fitted S/PDIF output */
};

const u32 pc_beep_verbs[0] = {};

static struct azalia_codec mainboard_azalia_codecs[] = {
	{
		.name         = "Realtek ALC662",
		.vendor_id    = 0x10ec0662,
		.subsystem_id = 0x105b0df7,
		.address      = 2,
		.verbs        = realtek_alc662_verbs,
		.verb_count   = ARRAY_SIZE(realtek_alc662_verbs),
	},
};

AZALIA_ARRAY_SIZES;
