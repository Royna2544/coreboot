/* SPDX-License-Identifier: GPL-2.0-only */

Device (LPT1)
{
	Name (_HID, EISAID("PNP0400"))
	Name (_CRS, ResourceTemplate () {
		IO (Decode16, 0x0378, 0x0378, 0x08, 0x08)
		IRQNoFlags () { 7 }
	})
}
