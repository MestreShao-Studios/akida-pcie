// SPDX-License-Identifier: GPL-2.0
/*
 * akida-pcie.c — native Linux 7.0 PCIe driver for the BrainChip Akida AKD1000
 *
 * Derivative work of BrainChip's akida_dw_edma (SPDX GPL-2.0).
 * Original: Copyright (c) 2022 Brainchip.
 * Kernel-7.0 rewrite: Copyright (c) 2026 Mestre Shao Studio.
 *
 * A clean rewrite of BrainChip's akida_dw_edma driver that drives the AKD1000's
 * Synopsys DesignWare eDMA via the kernel's IN-TREE dw-edma (CONFIG_DW_EDMA)
 * instead of vendoring a frozen copy + kernel-internal headers. Targets Linux 7.0.
 *
 * Userspace ABI preserved exactly (so the `akida` Python runtime is unchanged):
 *   - /dev/akida<N> (miscdevice, N from an IDA so multi-card never collides),
 *     0666 via udev
 *   - read()/write() at a file offset: DMA in 1024-byte chunks
 *   - address guard rejecting the eDMA linked-list window [0x20000000,0x20000100)
 *   - mmap() of BAR0 (noncached), bounded to BAR0
 *
 * AKD1000 only (1e7c:bca1). Derived from BrainChip's GPL-2.0 akida_dw_edma. GPL-2.0.
 *
 * STATUS: verified on hardware (AKD1000, kernel 7.0.0-15) + hardened after a
 * structured code review (mmap bound, DMA-error reporting, kref'd transfer
 * object vs the dw-edma no-synchronize teardown, IDA naming, irq-vector free,
 * THIS_MODULE ref-balance, partial-probe cleanup, in-flight-I/O drain on remove).
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/dmaengine.h>
#include <linux/dma/edma.h>
#include <linux/dma-mapping.h>
#include <linux/delay.h>
#include <linux/idr.h>
#include <linux/kref.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/rwsem.h>
#include <linux/completion.h>

#define AKIDA_VENDOR_ID		0x1e7c
#define AKIDA_1000_DEV_ID	0xbca1

#define AKIDA_EDMA_REG_OFFSET	0x970		/* eDMA CSRs within BAR2 (AKD1000) */
#define AKIDA_DMA_RAM_PHY_ADDR	0x20000000UL	/* device-side eDMA RAM base */
#define AKIDA_DMA_RAM_PHY_SIZE	0x100UL		/* protected LL window */
#define AKIDA_XFER_CHUNK	1024
#define AKIDA_N_WR_CH		2
#define AKIDA_N_RD_CH		2
#define AKIDA_N_CH		(AKIDA_N_WR_CH + AKIDA_N_RD_CH)

static DEFINE_IDA(akida_ida);

struct akida_dev {
	struct pci_dev		*pdev;
	struct dw_edma_chip	edma_chip;
	struct miscdevice	miscdev;
	char			name[16];
	int			id;		/* IDA-allocated instance index */
	void __iomem		*bar0;
	void __iomem		*bar2;
	void __iomem		*bar4;
	struct dma_chan		*rx_chan[AKIDA_N_RD_CH];
	struct dma_chan		*tx_chan[AKIDA_N_WR_CH];
	wait_queue_head_t	chan_wq;
	unsigned long		chan_busy;	/* bit 0..1 = rx, 2..3 = tx (atomic) */
	struct rw_semaphore	remove_lock;	/* read-held per I/O; write-held to drain on remove */
};

/*
 * Per-transfer state. Heap-allocated + refcounted because dw-edma does NOT
 * implement .device_synchronize, so dmaengine_terminate_sync() does NOT
 * guarantee the completion callback has finished — a late vchan tasklet could
 * touch this object after a timeout. The kref keeps it alive until both the
 * waiter and the callback are done with it.
 */
struct akida_xfer {
	struct completion done;
	int status;
	struct kref ref;
};

static void akida_xfer_free(struct kref *ref)
{
	kfree(container_of(ref, struct akida_xfer, ref));
}

static void akida_dma_cb(void *arg, const struct dmaengine_result *res)
{
	struct akida_xfer *x = arg;

	x->status = (res && res->result == DMA_TRANS_NOERROR) ? 0 : -EIO;
	complete(&x->done);
	kref_put(&x->ref, akida_xfer_free);
}

/* Reject transfers overlapping the protected eDMA LL window. Overflow-safe. */
static bool akida_addr_allowed(loff_t addr, size_t size)
{
	if (addr < 0 || size > (size_t)(LLONG_MAX - addr))
		return false;
	return (addr + (loff_t)size) <= (loff_t)AKIDA_DMA_RAM_PHY_ADDR ||
	       addr >= (loff_t)(AKIDA_DMA_RAM_PHY_ADDR + AKIDA_DMA_RAM_PHY_SIZE);
}

static int akida_dma_one(struct akida_dev *ak, struct dma_chan *chan,
			 void *buf, size_t len, loff_t dev_addr,
			 enum dma_transfer_direction dir)
{
	struct device *dev = &ak->pdev->dev;
	enum dma_data_direction map_dir =
		(dir == DMA_DEV_TO_MEM) ? DMA_FROM_DEVICE : DMA_TO_DEVICE;
	struct dma_slave_config cfg = {};
	struct dma_async_tx_descriptor *desc;
	struct akida_xfer *xfer;
	dma_addr_t dma_addr;
	dma_cookie_t cookie;
	int ret;

	dma_addr = dma_map_single(dev, buf, len, map_dir);
	if (dma_mapping_error(dev, dma_addr))
		return -ENOMEM;

	cfg.direction = dir;
	if (dir == DMA_DEV_TO_MEM) {
		cfg.src_addr = (phys_addr_t)dev_addr;
		cfg.src_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
	} else {
		cfg.dst_addr = (phys_addr_t)dev_addr;
		cfg.dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
	}
	ret = dmaengine_slave_config(chan, &cfg);
	if (ret)
		goto unmap;

	desc = dmaengine_prep_slave_single(chan, dma_addr, len, dir,
					   DMA_PREP_INTERRUPT);
	if (!desc) {
		ret = -EIO;
		goto unmap;
	}

	xfer = kzalloc(sizeof(*xfer), GFP_KERNEL);
	if (!xfer) {
		ret = -ENOMEM;
		goto unmap;
	}
	init_completion(&xfer->done);
	xfer->status = -EIO;
	kref_init(&xfer->ref);			/* ref = 1 (waiter) */
	kref_get(&xfer->ref);			/* ref = 2 (callback) */
	desc->callback_result = akida_dma_cb;
	desc->callback_param = xfer;

	cookie = dmaengine_submit(desc);
	ret = dma_submit_error(cookie);
	if (ret) {
		/* not submitted: callback will never run — drop both refs */
		kref_put(&xfer->ref, akida_xfer_free);
		kref_put(&xfer->ref, akida_xfer_free);
		goto unmap;
	}

	dma_async_issue_pending(chan);
	if (!wait_for_completion_timeout(&xfer->done, msecs_to_jiffies(5000))) {
		dmaengine_terminate_sync(chan);	/* stops the HW channel */
		ret = -ETIMEDOUT;
		/*
		 * A late tasklet may still run the callback; it holds its own ref,
		 * so this is leak-safe, NOT use-after-free. Residual: if the HW is
		 * truly wedged and the callback is never delivered, this xfer (~40 B)
		 * leaks. That is the deliberate cost of dw-edma lacking
		 * .device_synchronize — we choose a rare leak over a possible crash.
		 */
		kref_put(&xfer->ref, akida_xfer_free);
		goto unmap;
	}
	ret = xfer->status;
	kref_put(&xfer->ref, akida_xfer_free);

unmap:
	dma_unmap_single(dev, dma_addr, len, map_dir);
	return ret;
}

/* True iff at least one slot in [base, base+n) is free. Lockless (atomic reads). */
static bool akida_subpool_free(struct akida_dev *ak, int base, int n)
{
	int i;

	for (i = base; i < base + n; i++)
		if (!test_bit(i, &ak->chan_busy))
			return true;
	return false;
}

/*
 * Claim a free channel from the per-direction sub-pool (blocking). Lock-free:
 * test_and_set_bit is the atomic claim, and the wait predicate is a lockless
 * read of THIS direction's sub-pool — so an RX waiter is never spuriously woken
 * by a freed TX channel (and vice-versa), and no blocking op runs inside the
 * wait_event condition.
 */
static struct dma_chan *akida_get_chan(struct akida_dev *ak, bool is_read,
				       int *slot_out)
{
	int base = is_read ? 0 : AKIDA_N_RD_CH;
	int n = is_read ? AKIDA_N_RD_CH : AKIDA_N_WR_CH;
	int i;

	for (;;) {
		for (i = 0; i < n; i++) {
			if (!test_and_set_bit(base + i, &ak->chan_busy)) {
				*slot_out = base + i;
				return is_read ? ak->rx_chan[i] : ak->tx_chan[i];
			}
		}
		if (wait_event_interruptible(ak->chan_wq,
					     akida_subpool_free(ak, base, n)))
			return ERR_PTR(-ERESTARTSYS);
	}
}

static void akida_put_chan(struct akida_dev *ak, int slot)
{
	clear_bit(slot, &ak->chan_busy);
	wake_up_interruptible(&ak->chan_wq);	/* wakes all non-exclusive waiters */
}

static struct akida_dev *akida_from_file(struct file *f)
{
	return container_of(f->private_data, struct akida_dev, miscdev);
}

static ssize_t akida_rw(struct akida_dev *ak, char __user *ubuf, size_t count,
			loff_t *ppos, bool is_read)
{
	size_t done = 0;
	void *bounce;
	int ret = 0;

	if (!akida_addr_allowed(*ppos, count))
		return -EINVAL;

	/*
	 * Hold the removal lock read-side for the whole transfer. akida_remove()
	 * takes it write-side, so it cannot release the channels / tear down the
	 * eDMA while an I/O is in flight (closes the unbind/hot-unplug-vs-IO race).
	 * trylock (not down_read) so a reader arriving once removal has begun is
	 * rejected rather than blocked until after teardown.
	 */
	if (!down_read_trylock(&ak->remove_lock))
		return -ESHUTDOWN;

	bounce = kmalloc(AKIDA_XFER_CHUNK, GFP_KERNEL);
	if (!bounce) {
		ret = -ENOMEM;
		goto out;
	}

	while (done < count) {
		size_t chunk = min_t(size_t, count - done, AKIDA_XFER_CHUNK);
		struct dma_chan *chan;
		int slot;

		if (!is_read) {
			if (copy_from_user(bounce, ubuf + done, chunk)) {
				ret = -EFAULT;
				break;
			}
		}

		chan = akida_get_chan(ak, is_read, &slot);
		if (IS_ERR(chan)) {
			ret = PTR_ERR(chan);
			break;
		}
		ret = akida_dma_one(ak, chan, bounce, chunk, *ppos + done,
				    is_read ? DMA_DEV_TO_MEM : DMA_MEM_TO_DEV);
		akida_put_chan(ak, slot);
		if (ret)
			break;

		if (is_read) {
			if (copy_to_user(ubuf + done, bounce, chunk)) {
				ret = -EFAULT;
				break;
			}
		}
		done += chunk;
	}

	kfree(bounce);
out:
	up_read(&ak->remove_lock);
	if (done) {
		*ppos += done;
		return done;
	}
	return ret;
}

static ssize_t akida_read(struct file *f, char __user *buf, size_t n, loff_t *ppos)
{
	return akida_rw(akida_from_file(f), buf, n, ppos, true);
}

static ssize_t akida_write(struct file *f, const char __user *buf, size_t n, loff_t *ppos)
{
	return akida_rw(akida_from_file(f), (char __user *)buf, n, ppos, false);
}

static int akida_mmap(struct file *f, struct vm_area_struct *vma)
{
	struct akida_dev *ak = akida_from_file(f);
	resource_size_t bar_len = pci_resource_len(ak->pdev, 0);
	unsigned long vsize = vma->vm_end - vma->vm_start;
	unsigned long bar_pfn = pci_resource_start(ak->pdev, 0) >> PAGE_SHIFT;

	/* bound offset+size to BAR0 — vm_pgoff is user-controlled */
	if ((u64)vma->vm_pgoff + (vsize >> PAGE_SHIFT) > (bar_len >> PAGE_SHIFT))
		return -EINVAL;

	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	return io_remap_pfn_range(vma, vma->vm_start, bar_pfn + vma->vm_pgoff,
				  vsize, vma->vm_page_prot);
}

static const struct file_operations akida_fops = {
	.owner	= THIS_MODULE,
	.read	= akida_read,
	.write	= akida_write,
	.mmap	= akida_mmap,
	.llseek	= default_llseek,
};

/* --- AKD1000 iATU inbound config (verbatim from BrainChip akida-pcie-core.c) --- */
struct akida_iatu_conf {
	int addr;
	u32 val;
};

static const struct akida_iatu_conf akida_1000_iatu_conf[] = {
	/* Region 0 inbound — Akida core APB, BAR0 */
	{ 0x0900, 0x80000000 }, { 0x0904, 0x00000000 },
	{ 0x0918, 0xFCC00000 }, { 0x0908, 0xC0080000 },
	/* Region 1 inbound — DBI APB / eDMA controller (offset 0x970), BAR2 */
	{ 0x0900, 0x80000001 }, { 0x0904, 0x00000000 },
	{ 0x0918, 0xF8C00000 }, { 0x0908, 0xC0080200 },
	/* Region 2 inbound — LPDDR (LL + data), BAR4 */
	{ 0x0900, 0x80000002 }, { 0x0904, 0x00000000 },
	{ 0x0918, AKIDA_DMA_RAM_PHY_ADDR }, { 0x0908, 0xC0080400 },
	{ 0 }
};

static int akida_setup_iatu(struct akida_dev *ak)
{
	const struct akida_iatu_conf *c = akida_1000_iatu_conf;
	int ret;

	while (c->addr) {
		ret = pci_write_config_dword(ak->pdev, c->addr, c->val);
		if (ret) {
			pci_err(ak->pdev, "iATU write 0x%x=0x%x failed (%d)\n",
				c->addr, c->val, ret);
			return ret;
		}
		c++;
	}
	msleep(1000);	/* let the iATU settle (per BrainChip's driver) */
	return 0;
}

/* --- dw-edma chip population (in-tree dw_edma_probe) --- */
#define AKIDA_LL_SZ		0x40
#define AKIDA_LL_OFF_TX0	0x00
#define AKIDA_LL_OFF_TX1	0x40
#define AKIDA_LL_OFF_RX0	0x80
#define AKIDA_LL_OFF_RX1	0xC0

static int akida_irq_vector(struct device *dev, unsigned int nr)
{
	WARN_ON_ONCE(nr);	/* nr_irqs == 1; guards a future vector-count bump */
	return pci_irq_vector(to_pci_dev(dev), 0);	/* single shared vector */
}

static const struct dw_edma_plat_ops akida_edma_ops = {
	.irq_vector = akida_irq_vector,
};

static void akida_fill_ll(struct dw_edma_region *r, void __iomem *bar4,
			  unsigned long off)
{
	r->vaddr.io = bar4 + off;
	r->paddr = AKIDA_DMA_RAM_PHY_ADDR + off;
	r->sz = AKIDA_LL_SZ;
}

static int akida_edma_init(struct akida_dev *ak)
{
	struct dw_edma_chip *chip = &ak->edma_chip;

	chip->dev = &ak->pdev->dev;
	chip->nr_irqs = 1;
	chip->ops = &akida_edma_ops;
	chip->reg_base = ak->bar2 + AKIDA_EDMA_REG_OFFSET;
	chip->ll_wr_cnt = AKIDA_N_WR_CH;
	chip->ll_rd_cnt = AKIDA_N_RD_CH;
	chip->mf = EDMA_MF_EDMA_LEGACY;

	akida_fill_ll(&chip->ll_region_wr[0], ak->bar4, AKIDA_LL_OFF_TX0);
	akida_fill_ll(&chip->ll_region_wr[1], ak->bar4, AKIDA_LL_OFF_TX1);
	akida_fill_ll(&chip->ll_region_rd[0], ak->bar4, AKIDA_LL_OFF_RX0);
	akida_fill_ll(&chip->ll_region_rd[1], ak->bar4, AKIDA_LL_OFF_RX1);

	return dw_edma_probe(chip);
}

/* --- channel request via direction-filtered dma_request_channel ---
 *
 * dw_edma_probe() registers its dma_device against chip->dev (= our pci_dev),
 * so the channels' owner is THIS_MODULE. dma_request_channel() does an implicit
 * module_get(THIS_MODULE) — which would self-pin us and block rmmod — so we
 * module_put(THIS_MODULE) after each request, and __module_get(THIS_MODULE)
 * before each dma_release_channel() (which puts it again). Using THIS_MODULE
 * directly avoids a fragile ->driver->owner deref at teardown.
 */
struct akida_filter_param {
	struct akida_dev *akida;
	u32 dir_mask;
	u32 dir_exp;
};

static bool akida_chan_filter(struct dma_chan *chan, void *param)
{
	struct akida_filter_param *p = param;
	struct dma_slave_caps caps;

	if (&p->akida->pdev->dev != chan->device->dev)
		return false;
	if (dma_get_slave_caps(chan, &caps))
		return false;
	return (caps.directions & p->dir_mask) == p->dir_exp;
}

static void akida_release_channels(struct akida_dev *ak)
{
	int i;

	for (i = 0; i < AKIDA_N_RD_CH; i++) {
		if (!IS_ERR_OR_NULL(ak->rx_chan[i])) {
			__module_get(THIS_MODULE);
			dma_release_channel(ak->rx_chan[i]);
			ak->rx_chan[i] = NULL;
		}
	}
	for (i = 0; i < AKIDA_N_WR_CH; i++) {
		if (!IS_ERR_OR_NULL(ak->tx_chan[i])) {
			__module_get(THIS_MODULE);
			dma_release_channel(ak->tx_chan[i]);
			ak->tx_chan[i] = NULL;
		}
	}
}

static int akida_request_channels(struct akida_dev *ak)
{
	struct akida_filter_param p = {
		.akida = ak,
		.dir_mask = BIT(DMA_DEV_TO_MEM) | BIT(DMA_MEM_TO_DEV),
	};
	dma_cap_mask_t mask;
	int i;

	dma_cap_zero(mask);
	dma_cap_set(DMA_SLAVE, mask);

	for (i = 0; i < AKIDA_N_RD_CH; i++) {
		p.dir_exp = BIT(DMA_DEV_TO_MEM);
		ak->rx_chan[i] = dma_request_channel(mask, akida_chan_filter, &p);
		if (!ak->rx_chan[i])
			goto fail;
		module_put(THIS_MODULE);
	}
	for (i = 0; i < AKIDA_N_WR_CH; i++) {
		p.dir_exp = BIT(DMA_MEM_TO_DEV);
		ak->tx_chan[i] = dma_request_channel(mask, akida_chan_filter, &p);
		if (!ak->tx_chan[i])
			goto fail;
		module_put(THIS_MODULE);
	}
	return 0;

fail:
	/* release the channels acquired so far (leaves none held on error) */
	akida_release_channels(ak);
	return -ENODEV;
}

static int akida_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct akida_dev *ak;
	int ret, nvec;

	ak = devm_kzalloc(&pdev->dev, sizeof(*ak), GFP_KERNEL);
	if (!ak)
		return -ENOMEM;
	ak->pdev = pdev;
	ak->id = -1;
	init_waitqueue_head(&ak->chan_wq);
	init_rwsem(&ak->remove_lock);

	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;
	pci_set_master(pdev);

	ret = pcim_iomap_regions(pdev, BIT(0) | BIT(2) | BIT(4), KBUILD_MODNAME);
	if (ret)
		return ret;
	ak->bar0 = pcim_iomap_table(pdev)[0];
	ak->bar2 = pcim_iomap_table(pdev)[2];
	ak->bar4 = pcim_iomap_table(pdev)[4];

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret)
		ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	nvec = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI | PCI_IRQ_MSIX);
	if (nvec <= 0)
		return nvec < 0 ? nvec : -EIO;

	ret = akida_setup_iatu(ak);		/* must precede dw_edma_probe */
	if (ret)
		goto err_irq;

	ret = akida_edma_init(ak);
	if (ret)
		goto err_irq;

	ret = akida_request_channels(ak);	/* self-cleans on partial failure */
	if (ret)
		goto err_edma;

	ak->id = ida_alloc(&akida_ida, GFP_KERNEL);
	if (ak->id < 0) {
		ret = ak->id;
		goto err_chan;
	}
	scnprintf(ak->name, sizeof(ak->name), "akida%d", ak->id);
	ak->miscdev.minor = MISC_DYNAMIC_MINOR;
	ak->miscdev.name = ak->name;
	ak->miscdev.fops = &akida_fops;
	ak->miscdev.parent = &pdev->dev;
	ret = misc_register(&ak->miscdev);
	if (ret)
		goto err_ida;

	pci_set_drvdata(pdev, ak);
	dev_info(&pdev->dev, "akida-pcie: AKD1000 ready as /dev/%s (%d IRQ)\n",
		 ak->name, nvec);
	return 0;

err_ida:
	ida_free(&akida_ida, ak->id);
err_chan:
	akida_release_channels(ak);
err_edma:
	dw_edma_remove(&ak->edma_chip);
err_irq:
	pci_free_irq_vectors(pdev);
	return ret;
}

static void akida_remove(struct pci_dev *pdev)
{
	struct akida_dev *ak = pci_get_drvdata(pdev);

	if (!ak)
		return;
	misc_deregister(&ak->miscdev);		/* no new opens */
	down_write(&ak->remove_lock);		/* wait for in-flight read()/write() to drain */
	if (ak->id >= 0)
		ida_free(&akida_ida, ak->id);
	akida_release_channels(ak);
	dw_edma_remove(&ak->edma_chip);
	pci_free_irq_vectors(pdev);
}

static const struct pci_device_id akida_pci_ids[] = {
	{ PCI_DEVICE(AKIDA_VENDOR_ID, AKIDA_1000_DEV_ID) },
	{ 0 }
};
MODULE_DEVICE_TABLE(pci, akida_pci_ids);

static struct pci_driver akida_pci_driver = {
	.name		= KBUILD_MODNAME,
	.id_table	= akida_pci_ids,
	.probe		= akida_probe,
	.remove		= akida_remove,
};
module_pci_driver(akida_pci_driver);

MODULE_DESCRIPTION("BrainChip Akida AKD1000 PCIe driver (native, in-tree dw-edma)");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Mestre Shao Studio (port); BrainChip Inc (original)");
