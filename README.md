# akida-pcie — native BrainChip Akida AKD1000 PCIe driver for modern Linux

A clean, **kernel-7.0-native** PCIe driver for the BrainChip **Akida AKD1000** neural
co-processor, built on the kernel's **in-tree DesignWare eDMA** (`CONFIG_DW_EDMA`).

## Why this exists

BrainChip's official [`akida_dw_edma`](https://github.com/Brainchip-Inc/akida_dw_edma) driver
vendors a frozen copy of `dw-edma` plus copied kernel-internal headers, version-gated to Linux
**≤ 6.9** — its Makefile hard-`$(error)`s on anything newer. On a current kernel (e.g. **7.0**,
as shipped on hosts that need recent kernels for new GPUs) it **will not build at all**.

This driver is a from-scratch rewrite that:

- drives the AKD1000's DesignWare eDMA via the **in-tree** `dw-edma` (`dw_edma_probe()` from
  `<linux/dma/edma.h>`) — **no vendored DMA code, no copied internal headers** (~3,600 LOC gone);
- uses modern kernel APIs (`miscdevice`, `pcim_*`, `io_remap_pfn_range`, the public dmaengine
  client API);
- **preserves the userspace ABI exactly**, so the stock `akida` Python runtime works unchanged
  (`/dev/akida0`, the read/write DMA path, and `mmap`).

## Status — verified on hardware

Tested on an AKD1000 PCIe card, Linux **7.0.0-15-generic**, `akida` runtime **2.19.1**:

- builds clean (`modinfo` shows `depends: dw-edma`);
- loads + binds the card (`lspci -k`: `Kernel driver in use: akida_pcie`), creates `/dev/akida0`;
- the runtime enumerates it as a live `HardwareDevice` (`BC.00.000.002`);
- runs a v1 DS-CNN keyword-spotting model on-hardware at **~2100–2270 inferences/sec**
  (run-to-run variance on the onboard meter);
- measured board power (onboard meter): **~890 mW**;
- clean `rmmod`; autoload via `systemd-modules-load` verified.

The driver went through a structured review before release: an `mmap` offset+size bound, DMA-error
reporting via `callback_result`, a `kref`-refcounted per-transfer object (dw-edma implements no
`.device_synchronize`, so a timeout can't be allowed to free a struct a late callback still holds),
IDA-based `/dev/akida<N>` naming for multi-card hosts, explicit `pci_free_irq_vectors` (not
devres-managed), a `THIS_MODULE` request/release ref-balance that keeps `rmmod` working, and a
lock-free per-direction channel pool, and an `rw_semaphore` that drains in-flight I/O on remove
(closing a PCI-unbind/hot-unplug-vs-I/O race). All re-verified on hardware (build → probe → KWS →
clean `rmmod`, no oops/double-free).

## Test surface — honest scope

Verified on **one** AKD1000 card on a single kernel (7.0.0-15). The happy paths (build, probe,
enumerate, DMA read/write, `mmap`, inference, unload) and the negative ABI paths (address-guard
rejection, oversized/odd-offset `mmap`, channel-pool contention) have hardware/live evidence. What
is **not** yet covered: an automated fault-injection + `kmemleak` suite for the probe-failure and
`dma_submit_error` unwind paths (those are verified by code reading, not by breaking them); a
regression suite; and cold-boot/initramfs autoload on a real power-cycle (`systemd-modules-load`
boot-path is simulated, not power-cycled). No multi-card host has been tested.

## Build

Requires kernel headers + `CONFIG_DW_EDMA` (module or built-in):

```sh
make
```

## Load

```sh
sudo modprobe dw_edma
sudo insmod akida-pcie.ko
# permanent: cp akida-pcie.ko into /lib/modules/$(uname -r)/kernel/drivers/ && sudo depmod,
# cp 99-akida-pcie.rules into /etc/udev/rules.d/, and add `akida_pcie` to /etc/modules.
```

`kws_power_test.py` reproduces the on-hardware KWS + power measurement.

## License

**GPL-2.0.** This is a derivative work of BrainChip's `akida_dw_edma`, whose sources carry
`SPDX-License-Identifier: GPL-2.0` and `Copyright (c) 2022 Brainchip` (the `akida-pcie.c` file
header retains that notice). The kernel-7.0 rewrite is by Mestre Shao Studio. GPL-2.0 is also
required regardless: the in-tree `dw_edma_probe()` is an `EXPORT_SYMBOL_GPL` symbol.

> Tested against one **AKD1000** (`1e7c:bca1`) on kernel 7.0. The AKD1500 (`1e7c:a500`) has a
> different host-DDR/iATU layout and is intentionally **not** in the id table (out of scope, not
> stubbed). Use at your own risk.
