/* SPDX-License-Identifier: GPL-2.0-only */

#include <acpi/acpi.h>
#include <cpu/x86/smm.h>
#include <option.h>
#include <southbridge/intel/common/pmbase.h>
#include <southbridge/intel/common/pmutil.h>

/* GPE0 event of ICH7 GPI12, which the RTL8168 drives through LAN_PMEJ. */
#define GPE0_GPIO12	(1 << 28)

/*
 * Like the vendor BIOS "Wake Up On LAN" item (awardeyt.rom 0x9518 on the
 * S3-S5 paths), arm the onboard LAN wake GPI after the common handler has
 * cleared GPE0_EN. The NIC only asserts it when the OS armed its PME.
 */
void mainboard_smi_sleep_finalize(u8 slp_typ)
{
	if (slp_typ < ACPI_S3 || !get_uint_option("wake_on_lan", 1))
		return;

	write_pmbase32(GPE0_STS, GPE0_GPIO12);
	write_pmbase32(GPE0_EN, read_pmbase32(GPE0_EN) | GPE0_GPIO12);
}
