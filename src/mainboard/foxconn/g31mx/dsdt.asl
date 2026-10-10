/* SPDX-License-Identifier: GPL-2.0-only */
#include <acpi/acpi.h>
#define ICH7_ACPI_GPIO 0
DefinitionBlock(
	"dsdt.aml",
	"DSDT",
	ACPI_DSDT_REV_2,
	OEM_ID,
	ACPI_TABLE_CREATOR,
	0x00000001
)
{
	#include <acpi/dsdt_top.asl>
	#include <southbridge/intel/common/acpi/platform.asl>
	#include <southbridge/intel/i82801gx/acpi/globalnvs.asl>

	Scope (\_SB) {
		Device (PCI0)
		{
			/*
			 * As the vendor DSDT: Windows derives the instance IDs of all
			 * PCI devices from this, so it keeps the drivers it installed.
			 */
			Name (_UID, 1)
			#include <northbridge/intel/x4x/acpi/x4x.asl>
			#include <southbridge/intel/i82801gx/acpi/ich7.asl>
		}
	}

	#include <southbridge/intel/common/acpi/sleepstates.asl>
}
