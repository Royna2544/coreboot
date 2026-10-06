/* SPDX-License-Identifier: GPL-2.0-only */

#include <commonlib/helpers.h>
#include <device/device.h>
#include <device/pci.h>
#include <smbios.h>

/* Called from mainboard_ops.enable_dev; table generation follows enumeration. */
void g31mx_smbios_init(struct device *dev);

/*
 * G31MX/46GMX User's Manual V1.2, printed pages 2-3; G31M04 schematic
 * sheets 24, 26, 28, 32, 34-38. These are physical connectors, not an
 * assertion that disabled/unqualified controllers currently service them.
 * COM2, IrDA, TPM and the six-jack audio option are not population evidence.
 * DSP0134 3.1.0 section 7.9.1: soldered rear ports have no internal
 * connector; internal headers have no claimed external case connector.
 */
static const struct port_information g31mx_ports[] = {
	{ NULL, CONN_NONE, "PS/2 Keyboard", CONN_PS_2, TYPE_KEYBOARD_PORT },
	{ NULL, CONN_NONE, "PS/2 Mouse", CONN_PS_2, TYPE_MOUSE_PORT },
	{ NULL, CONN_NONE, "COM1", CONN_DB_9_PIN_MALE, TYPE_SERIAL_PORT_XT_AT_COMPATIBLE },
	{ NULL, CONN_NONE, "Parallel", CONN_DB_25_PIN_FEMALE,
		TYPE_PARALLEL_PORT_XT_AT_COMPATIBLE },
	{ NULL, CONN_NONE, "VGA", CONN_DB_15_PIN_FEMALE, TYPE_VIDEO_PORT },
	{ NULL, CONN_NONE, "Rear USB (USB, port 1)", CONN_ACCESS_BUS_USB, TYPE_USB },
	{ NULL, CONN_NONE, "Rear USB (USB, port 2)", CONN_ACCESS_BUS_USB, TYPE_USB },
	{ NULL, CONN_NONE, "Rear USB (NIC_USB, port 1)", CONN_ACCESS_BUS_USB, TYPE_USB },
	{ NULL, CONN_NONE, "Rear USB (NIC_USB, port 2)", CONN_ACCESS_BUS_USB, TYPE_USB },
	{ NULL, CONN_NONE, "LAN", CONN_RJ_45, TYPE_NETWORK_PORT },
	{ NULL, CONN_NONE, "Line In", CONN_MINI_JACK_HEADPHONES, TYPE_AUDIO_PORT },
	{ NULL, CONN_NONE, "Line Out", CONN_MINI_JACK_HEADPHONES, TYPE_AUDIO_PORT },
	{ NULL, CONN_NONE, "Mic In", CONN_MINI_JACK_HEADPHONES, TYPE_AUDIO_PORT },
	{ "SATA_1", CONN_SAS_SATA, NULL, CONN_NONE, TYPE_SATA },
	{ "SATA_2", CONN_SAS_SATA, NULL, CONN_NONE, TYPE_SATA },
	{ "SATA_3", CONN_SAS_SATA, NULL, CONN_NONE, TYPE_SATA },
	{ "SATA_4", CONN_SAS_SATA, NULL, CONN_NONE, TYPE_SATA },
	{ "PIDE", CONN_ON_BOARD_IDE, NULL, CONN_NONE, TYPE_OTHER_PORT },
	{ "FLOPPY", CONN_ON_BOARD_FLOPPY, NULL, CONN_NONE, TYPE_OTHER_PORT },
	/* These keys differ from CONN_9_PIN_DUAL_INLINE's pin-10-cut definition. */
	{ "F_USB1 port 1 (keyed 2x5)", CONN_OTHER, NULL, CONN_NONE, TYPE_USB },
	{ "F_USB1 port 2 (keyed 2x5)", CONN_OTHER, NULL, CONN_NONE, TYPE_USB },
	{ "F_USB2 port 1 (keyed 2x5)", CONN_OTHER, NULL, CONN_NONE, TYPE_USB },
	{ "F_USB2 port 2 (keyed 2x5)", CONN_OTHER, NULL, CONN_NONE, TYPE_USB },
	{ "F_AUDIO (keyed 2x5)", CONN_OTHER, NULL, CONN_NONE, TYPE_AUDIO_PORT },
	{ "CD_IN", CONN_ON_BOARD_SOUND_INPUT_FROM_CD_ROM, NULL, CONN_NONE, TYPE_AUDIO_PORT },
	{ "SPDIF_OUT (keyed 1x4)", CONN_OTHER, NULL, CONN_NONE, TYPE_AUDIO_PORT },
};

struct g31mx_slot {
	const char *name;
	u8 bridge_devfn;
	u8 slot_devfn;
	enum misc_slot_type type;
	enum slot_data_bus_bandwidth width;
	u8 characteristics_1;
	u8 characteristics_2;
};

/*
 * G31M04 sheets 23, 30-31: PEG, ICH7 PCIe port 1, and two PCI slots.
 * PCI1 IDSEL0 -> R133 -> AD18; PCI2 IDSEL3 -> R126 -> AD17.
 * ICH7 datasheet section 5.1.7 maps AD16+n to PCI device n.
 * Root port 2 feeds the onboard LAN, not a physical expansion slot.
 * DSP0134 3.1.0 section 7.10.1 uses the generic PCIe type when physical
 * and maximum electrical widths agree. Card-length clearance is unknown.
 */
static const struct g31mx_slot g31mx_slots[] = {
	{ "PCI-E1_16X", PCI_DEVFN(1, 0), PCI_DEVFN(0, 0),
		SlotTypePciExpress, SlotDataBusWidth16X, SMBIOS_SLOT_3P3V, SMBIOS_SLOT_SMBUS },
	{ "PCI-E1_1X", PCI_DEVFN(0x1c, 0), PCI_DEVFN(0, 0),
		SlotTypePciExpress, SlotDataBusWidth1X, SMBIOS_SLOT_3P3V, SMBIOS_SLOT_SMBUS },
	{ "PCI1", PCI_DEVFN(0x1e, 0), PCI_DEVFN(2, 0), SlotTypePci, SlotDataBusWidth32Bit,
		SMBIOS_SLOT_5V | SMBIOS_SLOT_3P3V, SMBIOS_SLOT_PME | SMBIOS_SLOT_SMBUS },
	{ "PCI2", PCI_DEVFN(0x1e, 0), PCI_DEVFN(1, 0), SlotTypePci, SlotDataBusWidth32Bit,
		SMBIOS_SLOT_5V | SMBIOS_SLOT_3P3V, SMBIOS_SLOT_PME | SMBIOS_SLOT_SMBUS },
};

static int g31mx_write_slot(const struct g31mx_slot *slot, int *handle,
			    unsigned long *current)
{
	const struct device *bridge = pcidev_path_on_root(slot->bridge_devfn);
	const struct device *card = NULL;
	enum misc_slot_usage usage = SlotUsageUnknown;
	u16 segment = 0xffff;
	u8 bus_number = 0xff, devfn = 0xff;

	if (bridge && bridge->enabled && bridge->downstream) {
		const struct bus *bus = bridge->downstream;

		/* Disabled/unassigned buses cannot establish card presence or a BDF. */
		if (bus->secondary && bus->secondary <= 0xff) {
			segment = bus->segment_group;
			bus_number = bus->secondary;
			devfn = slot->slot_devfn;
			usage = SlotUsageAvailable;
			for (const struct device *child = bus->children; child;
			     child = child->sibling) {
				if (child->path.type != DEVICE_PATH_PCI ||
				    child->path.pci.devfn > 0xff ||
				    PCI_SLOT(child->path.pci.devfn) != PCI_SLOT(slot->slot_devfn) ||
				    !child->vendor || child->vendor == 0xffff)
					continue;
				if (!card || child->path.pci.devfn < card->path.pci.devfn)
					card = child;
			}
			if (card) {
				usage = SlotUsageInUse;
				devfn = card->path.pci.devfn;
			}
		}
	}

	/*
	 * Use the device in the slot, never the upstream root/PCI bridge BDF.
	 * No _SUN/$PIR Slot ID is qualified; retain zero rather than invent one.
	 * The shared Type9 helper appends 3.2 width/peer fields even though
	 * this platform advertises SMBIOS 3.1. Emit its 17-byte layout using
	 * the shared carving/string helpers, without unqualified extensions.
	 */
	const u8 formatted_length = offsetof(struct smbios_type9, data_bus_width);
	struct smbios_type9 *t = smbios_carve_table(*current, SMBIOS_SYSTEM_SLOTS,
		formatted_length + 2, *handle);
	u8 *eos = (u8 *)t + formatted_length;

	t->slot_designation = smbios_add_string(eos, slot->name);
	t->slot_type = slot->type;
	t->slot_data_bus_width = slot->width;
	t->current_usage = usage;
	t->slot_length = SlotLengthUnknown;
	t->slot_id = 0;
	t->slot_characteristics_1 = slot->characteristics_1;
	t->slot_characteristics_2 = slot->characteristics_2;
	t->segment_group_number = segment;
	t->bus_number = bus_number;
	t->device_function_number = devfn;

	const int len = smbios_full_table_len(&t->header, eos);
	*current += len;
	*handle += 1;
	return len;
}

u8 smbios_mainboard_feature_flags(void)
{
	/* G31MX/46GMX manual: motherboard; DSP0134 3.1.0 section 7.3.1. */
	return SMBIOS_FEATURE_FLAGS_HOSTING_BOARD;
}

smbios_wakeup_type smbios_system_wakeup_type(void)
{
	/* No board-qualified wake-source provider; 0 is Reserved, not Unknown. */
	return SMBIOS_WAKEUP_TYPE_UNKNOWN;
}

smbios_enclosure_type smbios_mainboard_enclosure_type(void)
{
	/* Micro-ATX describes the board, not the user's enclosure. */
	return SMBIOS_ENCLOSURE_UNKNOWN;
}

static int g31mx_smbios_data(struct device *dev, int *handle, unsigned long *current)
{
	int len = smbios_write_type8(current, handle, g31mx_ports, ARRAY_SIZE(g31mx_ports));

	for (size_t i = 0; i < ARRAY_SIZE(g31mx_slots); i++)
		len += g31mx_write_slot(&g31mx_slots[i], handle, current);

	// TODO: Upstream EPS maximum-size accounting uses this aggregate length.
	return len;
}

void g31mx_smbios_init(struct device *dev)
{
	dev->ops->get_smbios_data = g31mx_smbios_data;
}
