/* SPDX-License-Identifier: GPL-2.0-only */

#include <cpu/cpu.h>
#include <cpu/intel/fsb.h>
#include <cpu/intel/speedstep.h>
#include <cpu/x86/msr.h>
#include <cpu/x86/name.h>
#include <smbios.h>
#include <string.h>

/* DMTF processor-family values, kept local to this board. */
#define SMBIOS_PROCESSOR_FAMILY_CORE2_DUO		0xbf
#define SMBIOS_PROCESSOR_FAMILY_CORE2_SOLO		0xc0
#define SMBIOS_PROCESSOR_FAMILY_CORE2_EXTREME	0xc1
#define SMBIOS_PROCESSOR_FAMILY_CORE2_QUAD		0xc2

static unsigned int core2_model(struct cpuid_result signature)
{
	struct cpuinfo_x86 cpu;
	char vendor[13];

	if (!cpu_have_cpuid())
		return 0;

	const struct cpuid_result id = cpuid(0);

	memcpy(vendor, &id.ebx, 4);
	memcpy(vendor + 4, &id.edx, 4);
	memcpy(vendor + 8, &id.ecx, 4);
	vendor[12] = '\0';
	if (strcmp(vendor, "GenuineIntel"))
		return 0;

	get_fms(&cpu, signature.eax);
	if (cpu.x86 != 6 || (cpu.x86_model != 0xf && cpu.x86_model != 0x17))
		return 0;
	return cpu.x86_model;
}

unsigned int smbios_processor_family(struct cpuid_result signature)
{
	char name[49];

	if (!core2_model(signature) || cpu_cpuid_extended_level() < 0x80000004)
		return SMBIOS_PROCESSOR_FAMILY_UNKNOWN;

	fill_processor_name(name);
	if (!strstr(name, "Core(TM)2"))
		return SMBIOS_PROCESSOR_FAMILY_UNKNOWN;
	if (strstr(name, "Extreme"))
		return SMBIOS_PROCESSOR_FAMILY_CORE2_EXTREME;
	if (strstr(name, "Quad"))
		return SMBIOS_PROCESSOR_FAMILY_CORE2_QUAD;
	if (strstr(name, "Duo"))
		return SMBIOS_PROCESSOR_FAMILY_CORE2_DUO;
	if (strstr(name, "Solo"))
		return SMBIOS_PROCESSOR_FAMILY_CORE2_SOLO;
	return SMBIOS_PROCESSOR_FAMILY_UNKNOWN;
}

unsigned int smbios_processor_external_clock(void)
{
	unsigned int model, strap;

	if (!cpu_have_cpuid())
		return 0;
	model = core2_model(cpuid(1));
	if (!model)
		return 0;

	strap = rdmsr(MSR_FSB_FREQ).lo & 7;
	/* Reject reserved encodings before the timebase's fallback clock. */
	if (strap == 7 || (model == 0xf && strap == 6))
		return 0;

	return (get_ia32_fsb_x3() + 1) / 3;
}

unsigned int smbios_cpu_get_max_speed_mhz(void)
{
	// TODO: Replace this placeholder with the qualified socket speed limit.
	return 0;
}

unsigned int smbios_cpu_get_current_speed_mhz(void)
{
	// TODO: Replace this placeholder with the detected processor boot speed.
	return 0;
}

unsigned int smbios_cpu_get_voltage(void)
{
	// TODO: Replace this placeholder with a qualified runtime voltage provider.
	return 0;
}
