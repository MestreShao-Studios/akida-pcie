# akida-pcie — a native Akida AKD1000 driver for modern Linux

A from-scratch Linux **kernel-7.0** PCIe driver for the BrainChip **Akida AKD1000**, built on the
kernel's own in-tree DesignWare eDMA (`CONFIG_DW_EDMA`).

## Why I wrote this

The official `akida_dw_edma` driver won't build on a current kernel. It carries its own old copy
of the kernel's `dw-edma` code, plus some kernel-internal headers copied straight in, and it's
pinned to Linux **≤ 6.9** — past that, its Makefile just stops with an error.

I needed an AKD1000 running on a 7.0 host (the box needs a recent kernel for its GPU), and going
backwards wasn't an option. So I rewrote the driver.

This one:

- drives the AKD1000's eDMA through the kernel's **in-tree `dw-edma`** (`dw_edma_probe()` from
  `<linux/dma/edma.h>`) — nothing bundled, nothing frozen, no copied headers (~3,600 lines of old
  DMA code gone);
- sticks to modern kernel APIs (`miscdevice`, `pcim_*`, `io_remap_pfn_range`, the public dmaengine
  client API);
- keeps the `/dev/akida0` interface **exactly the same**, so the stock `akida` Python runtime works
  untouched.

## What I've actually tested

On a real AKD1000, Linux **7.0.0-15-generic**, `akida` runtime **2.19.1**:

- builds clean (`modinfo` shows `depends: dw-edma`);
- loads and binds the card (`lspci -k`: `Kernel driver in use: akida_pcie`), creates `/dev/akida0`;
- the runtime sees it as a live device (`BC.00.000.002`);
- runs a DS-CNN keyword-spotting model on the chip at **~2100–2270 inferences/sec**, drawing about
  **890 mW** (onboard meter);
- unloads cleanly.

Before putting it out I went through it properly: bounded the `mmap`, made DMA errors actually
surface, made the per-transfer object safe against a timeout (the kernel's `dw-edma` has no
`.device_synchronize`, so I refcount it instead of trusting teardown), gave each card its own
`/dev/akida<N>`, freed the IRQ vectors explicitly, balanced the module refcount so `rmmod` works,
made the channel pool lock-free, and made `remove` wait for in-flight I/O to drain. All re-checked
on the hardware.

## Where it stands — honestly

One card, one kernel. There's no automated fuzz or regression suite yet — the error paths I went
through by reading the code, not by breaking them on purpose. Cold-boot autoload I've only
simulated (`systemd-modules-load`), not power-cycled. No multi-card host tested.

If you've got an Akida on a recent kernel, I'd like to know whether it works for you.

## Build

Needs the kernel headers + `CONFIG_DW_EDMA` (module or built-in):

```sh
make
```

## Load

```sh
sudo modprobe dw_edma
sudo insmod akida-pcie.ko
# to make it stick: copy akida-pcie.ko into /lib/modules/$(uname -r)/kernel/drivers/ and run
# sudo depmod; copy 99-akida-pcie.rules into /etc/udev/rules.d/; add `akida_pcie` to /etc/modules.
```

`kws_power_test.py` reproduces the on-hardware KWS run + power reading.

## License

**GPL-2.0.** This is a derivative of BrainChip's `akida_dw_edma`, whose sources carry
`SPDX-License-Identifier: GPL-2.0` and `Copyright (c) 2022 Brainchip` (the `akida-pcie.c` header
keeps that notice). The kernel-7.0 rewrite is by Mestre Shao Studio. It has to be GPL-2.0 anyway:
the in-tree `dw_edma_probe()` is an `EXPORT_SYMBOL_GPL` symbol.

> Tested against one **AKD1000** (`1e7c:bca1`) on kernel 7.0. The AKD1500 (`1e7c:a500`) has a
> different host-DDR/iATU layout and is intentionally **not** in the id table (out of scope, not
> stubbed). Use at your own risk.
