/* SPDX-License-Identifier: GPL-2.0-only */

#include <bootstate.h>
#include <console/console.h>
#include <delay.h>
#include <device/device.h>
#include <device/pci.h>
#include <device/pci_ids.h>
#include <device/pci_ops.h>
#include <device/pciexp.h>

#define G31_PEG_DEVICE_ID	0x29c1
#define G31MX_PEG_SLOT_NUMBER	32
#define VC0_ENABLE		(1U << 31)
#define VC_ID_MASK		(7U << 24)
#define VC_COUNT_MASK		7
#define VC0_NEGOTIATION_PENDING	(1 << 1)
#define VC0_POLL_COUNT		1000

enum peg_error {
	PEG_BAD_PCIE_CAP = 0xb8,
	PEG_ACTIVE,
	PEG_BAD_VC_CAP,
	PEG_EXTRA_VC,
	PEG_BAD_VC0,
	PEG_BAD_ROOT_MAP,
	PEG_NEGOTIATION_TIMEOUT,
	PEG_BAD_ROOT,
	PEG_NO_SLOT,
	PEG_BAD_ENDPOINT,
	PEG_LINK_TRAINING,
	PEG_LINK_WIDTH,
	PEG_RETAINED_SLOT,
	PEG_SLOT_WRITE_FAILED,
	PEG_MAP_WRITE_FAILED,
};

static void __noreturn peg_fail(enum peg_error checkpoint)
{
	post_code(checkpoint);
	die("G31MX: PEG initialization failed (%02x)\n", checkpoint);
}

static unsigned int pcie_capability(const struct device *dev)
{
	const unsigned int cap = pci_find_capability(dev, PCI_CAP_ID_PCIE);

	if (cap < 0x40 || cap > 0xe0 || (cap & 3))
		peg_fail(PEG_BAD_PCIE_CAP);
	return cap;
}

static void require_quiescent(const struct device *dev, unsigned int pcie_cap)
{
	if ((pci_read_config16(dev, PCI_COMMAND) & PCI_COMMAND_MASTER) ||
	    (pci_read_config16(dev, pcie_cap + PCI_EXP_DEVSTA) & PCI_EXP_DEVSTA_TRPND))
		peg_fail(PEG_ACTIVE);
}

static unsigned int vc0_capability(const struct device *dev, bool root)
{
	const unsigned int cap = pciexp_find_extended_cap(dev, PCI_EXT_CAP_ID_VC, 0);
	u32 control;

	if (!cap && !root)
		return 0;
	if (cap < 0x100 || cap > 0xfe4 || (cap & 3))
		peg_fail(PEG_BAD_VC_CAP);
	if (pci_read_config32(dev, cap + PCI_VC_PORT_REG1) & VC_COUNT_MASK)
		peg_fail(PEG_EXTRA_VC);
	control = pci_read_config32(dev, cap + PCI_VC_RES_CTRL);
	if ((control & (VC0_ENABLE | VC_ID_MASK | 1)) != (VC0_ENABLE | 1))
		peg_fail(PEG_BAD_VC0);
	if (root && (control & 0xff) != 1)
		peg_fail(PEG_BAD_ROOT_MAP);
	return cap;
}

static void wait_for_vc0(const struct device *root, unsigned int root_vc,
			const struct device *endpoint, unsigned int endpoint_vc)
{
	for (unsigned int i = 0; i < VC0_POLL_COUNT; i++) {
		u16 status = pci_read_config16(root, root_vc + PCI_VC_RES_STATUS);

		if (endpoint_vc)
			status |= pci_read_config16(endpoint, endpoint_vc + PCI_VC_RES_STATUS);
		if (!(status & VC0_NEGOTIATION_PENDING))
			return;
		udelay(10);
	}
	peg_fail(PEG_NEGOTIATION_TIMEOUT);
}

static void g31mx_peg_init(void *unused)
{
	const struct device *const root = pcidev_path_on_root(PCI_DEVFN(1, 0));
	const struct device *endpoint = NULL;
	const struct device *child;
	unsigned int root_pcie, root_vc, endpoint_vc, width, watts;
	u32 slot, desired_slot;
	u16 flags, link;
	const u32 slot_fields = PCI_EXP_SLTCAP_PSN | PCI_EXP_SLTCAP_SPLV |
				PCI_EXP_SLTCAP_SPLS;

	if (!root || !is_enabled_pci(root))
		return;
	if (root->vendor != PCI_VID_INTEL || root->device != G31_PEG_DEVICE_ID)
		peg_fail(PEG_BAD_ROOT);
	if (!root->downstream)
		return;
	for (child = root->downstream->children; child; child = child->sibling) {
		if (is_enabled_pci(child) && child->path.pci.devfn == PCI_DEVFN(0, 0))
			endpoint = child;
	}
	if (!endpoint)
		return;

	root_pcie = pcie_capability(root);
	flags = pci_read_config16(root, root_pcie + PCI_EXP_FLAGS);
	if ((flags & PCI_EXP_FLAGS_TYPE) != (PCI_EXP_TYPE_ROOT_PORT << 4) ||
	    !(flags & PCI_EXP_FLAGS_SLOT))
		peg_fail(PEG_NO_SLOT);
	require_quiescent(root, root_pcie);
	for (child = root->downstream->children; child; child = child->sibling) {
		unsigned int cap;

		if (!is_enabled_pci(child))
			continue;
		cap = pcie_capability(child);
		flags = pci_read_config16(child, cap + PCI_EXP_FLAGS) & PCI_EXP_FLAGS_TYPE;
		if (flags != (PCI_EXP_TYPE_ENDPOINT << 4) &&
		    flags != (PCI_EXP_TYPE_LEG_END << 4))
			peg_fail(PEG_BAD_ENDPOINT);
		require_quiescent(child, cap);
	}

	link = pci_read_config16(root, root_pcie + PCI_EXP_LNKSTA);
	width = (link >> 4) & 0x3f;
	if (link & PCI_EXP_LNKSTA_LT)
		peg_fail(PEG_LINK_TRAINING);
	/* Award extension 0xbd9c qualifies only x1 and x16 slot power. */
	if (width == 16)
		watts = 75;
	else if (width == 1)
		watts = 10;
	else
		peg_fail(PEG_LINK_WIDTH);

	root_vc = vc0_capability(root, true);
	endpoint_vc = vc0_capability(endpoint, false);
	wait_for_vc0(root, root_vc, endpoint, endpoint_vc);
	slot = pci_read_config32(root, root_pcie + PCI_EXP_SLTCAP);
	desired_slot = (G31MX_PEG_SLOT_NUMBER << 19) | (watts << 7);
	/* These are RWO fields: admit reset state or an already matching value. */
	if ((slot & slot_fields) && (slot & slot_fields) != desired_slot)
		peg_fail(PEG_RETAINED_SLOT);

	/* Stock POST 0x8d advertises slot power before POST 0x94 maps TC0. */
	if ((slot & slot_fields) != desired_slot) {
		slot = (slot & ~slot_fields) | desired_slot;
		pci_write_config32(root, root_pcie + PCI_EXP_SLTCAP, slot);
		if (pci_read_config32(root, root_pcie + PCI_EXP_SLTCAP) != slot)
			peg_fail(PEG_SLOT_WRITE_FAILED);
	}
	if (endpoint_vc) {
		/* Do not write the endpoint's read-only VC capability/count fields. */
		pci_write_config8(endpoint, endpoint_vc + PCI_VC_RES_CTRL,
				  pci_read_config8(endpoint, endpoint_vc + PCI_VC_RES_CTRL) & 1);
		if (pci_read_config8(endpoint, endpoint_vc + PCI_VC_RES_CTRL) != 1)
			peg_fail(PEG_MAP_WRITE_FAILED);
	}
	wait_for_vc0(root, root_vc, endpoint, endpoint_vc);
}

/* After discovery, before resource enables, GPU initialization or option ROMs. */
BOOT_STATE_INIT_ENTRY(BS_DEV_ENUMERATE, BS_ON_EXIT, g31mx_peg_init, NULL);
