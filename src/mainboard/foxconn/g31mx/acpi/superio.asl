/* SPDX-License-Identifier: GPL-2.0-only */

Device (FDC0)
{
	Name (_HID, EISAID("PNP0700"))
	Name (_CRS, ResourceTemplate () {
		IO (Decode16, 0x03f0, 0x03f0, 0x01, 0x06)
		IO (Decode16, 0x03f7, 0x03f7, 0x01, 0x01)
		IRQNoFlags () { 6 }
		DMA (Compatibility, NotBusMaster, Transfer8) { 2 }
	})
}

Device (LPT1)
{
	Name (_HID, EISAID("PNP0400"))
	Name (_CRS, ResourceTemplate () {
		IO (Decode16, 0x0378, 0x0378, 0x08, 0x08)
		IRQNoFlags () { 7 }
	})
}
