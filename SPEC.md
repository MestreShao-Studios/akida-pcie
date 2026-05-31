# Native Akida PCIe Driver (Linux 7.0) — Technical Spec

Replace BrainChip's `akida_dw_edma` (won't build on kernels >6.9 — vendors a frozen dw-edma +
copied internal headers) with a clean driver **native to Linux 7.0**, driving the AKD1000's
DesignWare eDMA via the **in-tree** dw-edma. The userspace ABI is preserved exactly so the
`akida` Python runtime (2.19.1) works unchanged.

GPL-v2 (derivative of BrainChip's GPL-v2 driver — attribution retained).

## Hardware — AKD1000 (PCI 1e7c:bca1)
- **BAR0** — Akida core registers.
- **BAR2** — DesignWare eDMA CSRs (controller reg base at +0x970).
- **BAR4** — eDMA linked-list + data region (LL at device phys `0x20000000`).
- 2 write + 2 read eDMA channels; **legacy** eDMA map format; MSI/MSI-X (1 vector).
- **AKD1000 only** (`1e7c:bca1`). The AKD1500 (`1e7c:a500`) differs materially (host-DDR + a
  different iATU layout); it is deliberately **not** in the id table — driving it with this
  iATU/LL config would be wrong, so it is out of scope (not "stubbed").

## Userspace ABI — FROZEN (must match the old driver byte-for-byte)
- `/dev/akida0` — miscdevice, dynamic minor; `0666` via `99-akida-pcie.rules`.
- `read(fd, buf, n)` at file offset `ppos`: DMA device→host, 1024-byte chunks.
- `write(fd, buf, n)` at file offset `ppos`: DMA host→device, 1024-byte chunks.
- **Address guard:** reject any transfer overlapping `[0x20000000, 0x20000100)` (the eDMA LL region).
- `mmap()`: map BAR0 noncached via `io_remap_pfn_range`.

## Design — kernel 7.0 native
- **PCI:** `pcim_enable_device` + `pcim_iomap_regions(BIT(0)|BIT(2)|BIT(4))`; `pci_set_master`; `pci_alloc_irq_vectors(pdev,1,1,PCI_IRQ_MSI|PCI_IRQ_MSIX)` (success = ret>0). NOTE: `pci_alloc_irq_vectors` is **NOT** devres-managed by `pcim_enable_device` — the driver MUST call `pci_free_irq_vectors(pdev)` explicitly in `remove` and on every probe error path. (Confirmed: the in-tree `dw-edma-pcie` reference does the same; verified no double-free on `rmmod`.)
- **DMA via in-tree dw-edma:** populate `struct dw_edma_chip` (`dev`, `nr_irqs=1`, `ops`, `reg_base = BAR2+0x970`, `ll_wr_cnt=2`, `ll_rd_cnt=2`, four `ll_region_wr/rd[]` at BAR4 offsets 0x00/0x40/0x80/0xC0 size 0x40, `mf = EDMA_MF_EDMA_LEGACY`) and call the in-tree `dw_edma_probe(&chip)` (`<linux/dma/edma.h>`). Drop the entire vendored dw-edma tree. iATU inbound regions are written (verbatim AKD1000 config) **before** `dw_edma_probe`.
- **Channel acquisition:** `dma_request_channel` with a filter matching (a) our `chan->device->dev` and (b) the per-channel `dma_slave_caps.directions` (recent in-tree dw-edma reports direction per channel via `device_caps`, so the filter distinguishes the 2 RX from the 2 TX). The eDMA dma_device's owner is `THIS_MODULE` (provider == consumer), so `dma_request_channel`'s implicit `module_get` would self-pin and block `rmmod`; we balance it with `module_put(THIS_MODULE)` after each request and `__module_get(THIS_MODULE)` before each `dma_release_channel` (which puts it again). `request_channels` self-releases any acquired channels on partial failure.
- **Transfers:** per op — `dmaengine_slave_config` (dev addr = BAR offset, 4-byte buswidth), `dmaengine_prep_slave_single(..., DMA_PREP_INTERRUPT)`, `dmaengine_submit` (checked with `dma_submit_error`), `dma_async_issue_pending`, wait on a per-transfer completion. The per-transfer object (`struct akida_xfer`: completion + status) is **heap-allocated and `kref`-refcounted** because dw-edma implements no `.device_synchronize`, so `dmaengine_terminate_sync` does NOT drain a pending callback on timeout — the waiter and the callback each hold a ref; last-put frees, so a late tasklet can never touch freed memory. DMA errors are reported via `callback_result` (`dmaengine_result.result != DMA_TRANS_NOERROR` → `-EIO`); a 5 s timeout → `terminate_sync` + `-ETIMEDOUT`.
- **Channel pool:** the 4 channels are a lock-free pool — an `unsigned long` bitmap claimed with `test_and_set_bit`, released with `clear_bit` + `wake_up_interruptible`. A blocked `read`/`write` waits on a per-**direction** predicate (`wait_event_interruptible` over a lockless sub-pool scan), so an RX waiter is never spuriously woken by a freed TX channel.
- **Char dev:** `miscdevice` + `misc_register` (avoids the `class_create()` owner-arg break). The instance name is `akida<N>` where `N` comes from a global **IDA** (`ida_alloc`/`ida_free`), so multiple cards never collide on `/dev/akida0`. `read`/`write` use a kmalloc bounce buffer + `dma_map_single`/`dma_unmap_single`; `mmap` uses `io_remap_pfn_range` with an **offset+size** bound against `pci_resource_len(BAR0)` (`vm_pgoff` is user-controlled).
- **Removal drain:** a per-device `rw_semaphore` is read-held for the duration of each `read`/`write` (via `down_read_trylock` — new I/O is rejected with `-ESHUTDOWN` once removal starts) and write-held in `remove` (`down_write`) **before** channels/eDMA are torn down. `misc_register`'s `.owner` already blocks `rmmod` while an fd is open; the rw_semaphore additionally closes the **PCI unbind / hot-unplug vs in-flight-I/O** race (those teardown paths don't honour the module refcount).
- **Device claim:** PCI id_table `{1e7c:bca1}` (AKD1000 only) — BrainChip-specific, no in-tree driver claims it.

## Build
- Out-of-tree Kbuild: `obj-m += akida-pcie.o`, `KDIR ?= /lib/modules/$(shell uname -r)/build`.
- Requires `CONFIG_DW_EDMA` (`=m` or built-in). `modinfo akida-pcie.ko` must show `depends: dw-edma`.
- No vendored headers, no version-gating.

## Risks / open questions — RESOLVED at bring-up (AKD1000, kernel 7.0.0-15)
- The AKD1000's eDMA register layout + legacy format match what the **in-tree** dw-edma expects
  when fed via `dw_edma_chip` — **resolved**: `dw_edma_probe` succeeds, channels enumerate, and a
  real KWS model transfers correctly through the read/write DMA path.
- LL region sizing + the `0x20000000` hardcode — **resolved**: ported the AKD1000's exact values
  (4 × 0x40 LL regions at BAR4 0x00/0x40/0x80/0xC0; iATU region 2 at `0x20000000`).
- In-tree dw-edma does **not** claim `1e7c:bca1` — **resolved**: `lspci -k` shows
  `Kernel driver in use: akida_pcie`.

## Verification — DONE
- **Build:** compiles clean on the 7.0 headers (`BUILD_EXIT=0`); `modinfo` shows `depends: dw-edma`.
- **Load:** no panic/oops/WARN; `dw_edma` auto-loads; `/dev/akida0` appears at `0666`;
  `lspci -k` binds `akida_pcie`; `dmesg`: `AKD1000 ready as /dev/akida0 (1 IRQ)`.
- **Enumerate + ABI:** `akida.devices()` returns the AKD1000 (`BC.00.000.002`); a v1 DS-CNN KWS
  model maps + runs on-hardware at **~2272 fps**; measured board power **~891 mW**.
- **Unload:** `rmmod` returns 0, no oops, **no IRQ double-free**, `/dev/akida0` gone, module
  refcount balanced (`rmmod` succeeds while channels were held → the `THIS_MODULE` balance works).
- **Autoload:** `/etc/modules` + udev installed; `systemd-modules-load` restart loads both modules
  + recreates `/dev/akida0` (boot-path simulation). Cold-boot/initramfs confirm deferred to the
  next natural reboot.
