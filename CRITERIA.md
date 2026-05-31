# akida-pcie — BDD acceptance criteria

Derived from the driver's userspace ABI + lifecycle. Tests map 1:1 (AC n → T.n).

**AC1 — Build.** GIVEN Linux 7.0 with kernel headers + `CONFIG_DW_EDMA`, WHEN `make`, THEN
`akida-pcie.ko` builds with no errors and `modinfo` reports `depends: dw-edma`.

**AC2 — Probe / load.** GIVEN an AKD1000 (`1e7c:bca1`) present, WHEN `modprobe akida_pcie`,
THEN: `dw_edma` auto-loads first; BAR0/2/4 map; iATU inbound regions are written; the in-tree
`dw_edma_probe()` succeeds; 2 RX + 2 TX channels are acquired; `/dev/akida0` is created (0666 via
udev); `lspci -k` shows `akida_pcie` bound. No oops.

**AC3 — Probe failure is leak-free.** GIVEN any probe sub-step fails (irq alloc, iATU,
`dw_edma_probe`, channel request, `misc_register`), WHEN probe returns error, THEN every
already-acquired resource is released (no channel/dw_edma/misc leak; pcim_* auto-frees BARs+irq).

**AC4 — Enumerate.** WHEN the `akida` runtime calls `devices()`, THEN it returns the AKD1000 with
a readable hardware version (`BC.00.000.002`).

**AC5 — DMA read/write.** WHEN `read()`/`write()` at a valid file offset, THEN data transfers
correctly in ≤1024-byte chunks, return value = bytes done, `*ppos` advances. WHEN the range
overlaps `[0x20000000, 0x20000100)`, THEN it is rejected with `-EINVAL` and no DMA is issued.

**AC6 — mmap.** WHEN `mmap()` a region within BAR0's length, THEN BAR0 maps noncached. WHEN the
requested size (or offset) exceeds BAR0, THEN `-EINVAL` (no out-of-BAR mapping).

**AC7 — Inference.** WHEN a v1 KWS model is mapped + run on the device, THEN inference completes
through the read/write DMA path with correct output shape, no dmesg DMA error.

**AC8 — Clean unload.** WHEN `rmmod akida_pcie`, THEN channels are released, `/dev/akida0` is
gone, the module unloads with no oops, and the module refcount is balanced (no leak that blocks
unload, no underflow).

**AC9 — Autoload.** GIVEN `akida_pcie` in `/etc/modules` + the udev rule installed, WHEN the boot
module-load runs (`systemd-modules-load.service`), THEN both modules load and `/dev/akida0` is
ready at 0666.
