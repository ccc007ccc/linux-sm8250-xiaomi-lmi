// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018-2020, The Linux Foundation. All rights reserved.
 *
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-direction.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/list.h>
#include <linux/mhi.h>
#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/string.h>
#include "internal.h"
#include "trace.h"

static bool mhi_is_sahara_chan(const struct mhi_chan *mhi_chan)
{
	return mhi_chan && mhi_chan->name && !strcmp(mhi_chan->name, "SAHARA");
}

static bool mhi_is_sbl_boot_chan(const struct mhi_chan *mhi_chan)
{
	return mhi_is_sahara_chan(mhi_chan) ||
		(mhi_chan && mhi_chan->name && !strcmp(mhi_chan->name, "BL"));
}

static bool mhi_sahara_queue_force_resume;
module_param_named(sahara_queue_force_resume,
		   mhi_sahara_queue_force_resume, bool, 0644);
MODULE_PARM_DESC(sahara_queue_force_resume,
		 "Force an MHI runtime resume cycle before queueing SAHARA TREs");

static bool mhi_sahara_queue_chan_lock_db;
module_param_named(sahara_queue_chan_lock_db,
		   mhi_sahara_queue_chan_lock_db, bool, 0644);
MODULE_PARM_DESC(sahara_queue_chan_lock_db,
		 "Ring the SAHARA channel doorbell while holding the channel lock");

static bool mhi_sbl_accept_multi_tre_completion = true;
module_param_named(sbl_accept_multi_tre_completion,
		   mhi_sbl_accept_multi_tre_completion, bool, 0644);
MODULE_PARM_DESC(sbl_accept_multi_tre_completion,
		 "Accept downstream-style multi-TRE completions on SBL boot channels");

#define MHI_SBL_SAHARA_MIN_LEN		8
#define MHI_SBL_SAHARA_MAX_LEN		4096
#define MHI_SBL_SAHARA_CMD_HELLO		1
#define MHI_SBL_SAHARA_CMD_READ_DATA	3
#define MHI_SBL_SAHARA_CMD_END_IMAGE	4
#define MHI_SBL_SAHARA_CMD_DONE_RESP	6
#define MHI_SBL_SAHARA_CMD_RESET_RESP	8
#define MHI_SBL_SAHARA_CMD_READY		11
#define MHI_SBL_SAHARA_CMD_EXEC_RESP	14

static bool mhi_sbl_sahara_len_valid(u32 cmd, u32 pkt_len)
{
	switch (cmd) {
	case MHI_SBL_SAHARA_CMD_HELLO:
		return pkt_len == 48;
	case MHI_SBL_SAHARA_CMD_READ_DATA:
		return pkt_len == 20;
	case MHI_SBL_SAHARA_CMD_END_IMAGE:
		return pkt_len == 16;
	case MHI_SBL_SAHARA_CMD_DONE_RESP:
		return pkt_len == 12;
	case MHI_SBL_SAHARA_CMD_RESET_RESP:
	case MHI_SBL_SAHARA_CMD_READY:
		return pkt_len == 8;
	case MHI_SBL_SAHARA_CMD_EXEC_RESP:
		return pkt_len == 16;
	default:
		return false;
	}
}

static u16 mhi_sbl_infer_rx_len(const struct mhi_buf_info *buf_info,
					const __le32 **out_words)
{
	const __le32 *words = buf_info->cb_buf;
	u32 cmd, pkt_len;

	*out_words = words;
	if (!words || buf_info->len < MHI_SBL_SAHARA_MIN_LEN)
		return 0;

	cmd = le32_to_cpu(words[0]);
	pkt_len = le32_to_cpu(words[1]);

	if (!mhi_sbl_sahara_len_valid(cmd, pkt_len) ||
	    pkt_len > buf_info->len || pkt_len > MHI_SBL_SAHARA_MAX_LEN)
		return 0;

	return pkt_len;
}

int __must_check mhi_read_reg(struct mhi_controller *mhi_cntrl,
			      void __iomem *base, u32 offset, u32 *out)
{
	return mhi_cntrl->read_reg(mhi_cntrl, base + offset, out);
}

int __must_check mhi_read_reg_field(struct mhi_controller *mhi_cntrl,
				    void __iomem *base, u32 offset,
				    u32 mask, u32 *out)
{
	u32 tmp;
	int ret;

	ret = mhi_read_reg(mhi_cntrl, base, offset, &tmp);
	if (ret)
		return ret;

	*out = (tmp & mask) >> __ffs(mask);

	return 0;
}

int __must_check mhi_poll_reg_field(struct mhi_controller *mhi_cntrl,
				    void __iomem *base, u32 offset,
				    u32 mask, u32 val, u32 delayus,
				    u32 timeout_ms)
{
	int ret;
	u32 out, retry = (timeout_ms * 1000) / delayus;

	while (retry--) {
		ret = mhi_read_reg_field(mhi_cntrl, base, offset, mask, &out);
		if (ret)
			return ret;

		if (out == val)
			return 0;

		fsleep(delayus);
	}

	return -ETIMEDOUT;
}

void mhi_write_reg(struct mhi_controller *mhi_cntrl, void __iomem *base,
		   u32 offset, u32 val)
{
	mhi_cntrl->write_reg(mhi_cntrl, base + offset, val);
}

int __must_check mhi_write_reg_field(struct mhi_controller *mhi_cntrl,
				     void __iomem *base, u32 offset, u32 mask,
				     u32 val)
{
	int ret;
	u32 tmp;

	ret = mhi_read_reg(mhi_cntrl, base, offset, &tmp);
	if (ret)
		return ret;

	tmp &= ~mask;
	tmp |= (val << __ffs(mask));
	mhi_write_reg(mhi_cntrl, base, offset, tmp);

	return 0;
}

void mhi_write_db(struct mhi_controller *mhi_cntrl, void __iomem *db_addr,
		  dma_addr_t db_val)
{
	mhi_write_reg(mhi_cntrl, db_addr, 4, upper_32_bits(db_val));
	mhi_write_reg(mhi_cntrl, db_addr, 0, lower_32_bits(db_val));
}

void mhi_db_brstmode(struct mhi_controller *mhi_cntrl,
		     struct db_cfg *db_cfg,
		     void __iomem *db_addr,
		     dma_addr_t db_val)
{
	if (db_cfg->db_mode) {
		db_cfg->db_val = db_val;
		mhi_write_db(mhi_cntrl, db_addr, db_val);
		db_cfg->db_mode = 0;
	}
}

void mhi_db_brstmode_disable(struct mhi_controller *mhi_cntrl,
			     struct db_cfg *db_cfg,
			     void __iomem *db_addr,
			     dma_addr_t db_val)
{
	db_cfg->db_val = db_val;
	mhi_write_db(mhi_cntrl, db_addr, db_val);
}

void mhi_ring_er_db(struct mhi_event *mhi_event)
{
	struct mhi_ring *ring = &mhi_event->ring;

	mhi_event->db_cfg.process_db(mhi_event->mhi_cntrl, &mhi_event->db_cfg,
				     ring->db_addr, le64_to_cpu(*ring->ctxt_wp));
}

void mhi_ring_cmd_db(struct mhi_controller *mhi_cntrl, struct mhi_cmd *mhi_cmd)
{
	dma_addr_t db;
	struct mhi_ring *ring = &mhi_cmd->ring;

	db = ring->iommu_base + (ring->wp - ring->base);
	*ring->ctxt_wp = cpu_to_le64(db);
	mhi_write_db(mhi_cntrl, ring->db_addr, db);
}

void mhi_ring_chan_db(struct mhi_controller *mhi_cntrl,
		      struct mhi_chan *mhi_chan)
{
	struct mhi_ring *ring = &mhi_chan->tre_ring;
	dma_addr_t db;

	db = ring->iommu_base + (ring->wp - ring->base);

	/*
	 * Writes to the new ring element must be visible to the hardware
	 * before letting h/w know there is new element to fetch.
	 */
	dma_wmb();
	*ring->ctxt_wp = cpu_to_le64(db);

	mhi_chan->db_cfg.process_db(mhi_cntrl, &mhi_chan->db_cfg,
				    ring->db_addr, db);
}

enum mhi_ee_type mhi_get_exec_env(struct mhi_controller *mhi_cntrl)
{
	u32 exec;
	int ret = mhi_read_reg(mhi_cntrl, mhi_cntrl->bhi, BHI_EXECENV, &exec);

	return (ret) ? MHI_EE_MAX : exec;
}
EXPORT_SYMBOL_GPL(mhi_get_exec_env);

enum mhi_state mhi_get_mhi_state(struct mhi_controller *mhi_cntrl)
{
	u32 state;
	int ret = mhi_read_reg_field(mhi_cntrl, mhi_cntrl->regs, MHISTATUS,
				     MHISTATUS_MHISTATE_MASK, &state);
	return ret ? MHI_STATE_MAX : state;
}
EXPORT_SYMBOL_GPL(mhi_get_mhi_state);

void mhi_soc_reset(struct mhi_controller *mhi_cntrl)
{
	if (mhi_cntrl->reset) {
		mhi_cntrl->reset(mhi_cntrl);
		return;
	}

	/* Generic MHI SoC reset */
	mhi_write_reg(mhi_cntrl, mhi_cntrl->regs, MHI_SOC_RESET_REQ_OFFSET,
		      MHI_SOC_RESET_REQ);
}
EXPORT_SYMBOL_GPL(mhi_soc_reset);

int mhi_map_single_no_bb(struct mhi_controller *mhi_cntrl,
			 struct mhi_buf_info *buf_info)
{
	buf_info->p_addr = dma_map_single(mhi_cntrl->cntrl_dev,
					  buf_info->v_addr, buf_info->len,
					  buf_info->dir);
	if (dma_mapping_error(mhi_cntrl->cntrl_dev, buf_info->p_addr))
		return -ENOMEM;

	return 0;
}

int mhi_map_single_use_bb(struct mhi_controller *mhi_cntrl,
			  struct mhi_buf_info *buf_info)
{
	void *buf = dma_alloc_coherent(mhi_cntrl->cntrl_dev, buf_info->len,
				       &buf_info->p_addr, GFP_ATOMIC);

	if (!buf)
		return -ENOMEM;

	if (buf_info->dir == DMA_TO_DEVICE)
		memcpy(buf, buf_info->v_addr, buf_info->len);

	buf_info->bb_addr = buf;

	return 0;
}

void mhi_unmap_single_no_bb(struct mhi_controller *mhi_cntrl,
			    struct mhi_buf_info *buf_info)
{
	dma_unmap_single(mhi_cntrl->cntrl_dev, buf_info->p_addr, buf_info->len,
			 buf_info->dir);
}

void mhi_unmap_single_use_bb(struct mhi_controller *mhi_cntrl,
			     struct mhi_buf_info *buf_info)
{
	if (buf_info->dir == DMA_FROM_DEVICE)
		memcpy(buf_info->v_addr, buf_info->bb_addr, buf_info->len);

	dma_free_coherent(mhi_cntrl->cntrl_dev, buf_info->len,
			  buf_info->bb_addr, buf_info->p_addr);
}

static int get_nr_avail_ring_elements(struct mhi_controller *mhi_cntrl,
				      struct mhi_ring *ring)
{
	int nr_el;

	if (ring->wp < ring->rp) {
		nr_el = ((ring->rp - ring->wp) / ring->el_size) - 1;
	} else {
		nr_el = (ring->rp - ring->base) / ring->el_size;
		nr_el += ((ring->base + ring->len - ring->wp) /
			  ring->el_size) - 1;
	}

	return nr_el;
}

static void *mhi_to_virtual(struct mhi_ring *ring, dma_addr_t addr)
{
	return (addr - ring->iommu_base) + ring->base;
}

static void mhi_add_ring_element(struct mhi_controller *mhi_cntrl,
				 struct mhi_ring *ring)
{
	ring->wp += ring->el_size;
	if (ring->wp >= (ring->base + ring->len))
		ring->wp = ring->base;
	/* smp update */
	smp_wmb();
}

static void mhi_del_ring_element(struct mhi_controller *mhi_cntrl,
				 struct mhi_ring *ring)
{
	ring->rp += ring->el_size;
	if (ring->rp >= (ring->base + ring->len))
		ring->rp = ring->base;
	/* smp update */
	smp_wmb();
}

static bool is_valid_ring_ptr(struct mhi_ring *ring, dma_addr_t addr)
{
	return addr >= ring->iommu_base && addr < ring->iommu_base + ring->len &&
			!(addr & (sizeof(struct mhi_ring_element) - 1));
}

#define MHI_SAHARA_EVENT_DUMP_COUNT	16

static bool mhi_sahara_event_ring_used(struct mhi_controller *mhi_cntrl,
					       u32 er_index)
{
	struct mhi_chan *mhi_chan;
	u32 i;

	for (i = 0; i < mhi_cntrl->max_chan; i++) {
		mhi_chan = &mhi_cntrl->mhi_chan[i];
		if (mhi_chan->configured && mhi_chan->er_index == er_index &&
		    mhi_is_sbl_boot_chan(mhi_chan))
			return true;
	}

	return false;
}

static bool mhi_sahara_available(struct mhi_controller *mhi_cntrl)
{
	struct mhi_chan *mhi_chan;
	u32 i;

	for (i = 0; i < mhi_cntrl->max_chan; i++) {
		mhi_chan = &mhi_cntrl->mhi_chan[i];
		if (mhi_chan->configured && mhi_is_sbl_boot_chan(mhi_chan))
			return true;
	}

	return false;
}

static bool mhi_sahara_event_ring_relevant(struct mhi_controller *mhi_cntrl,
						   u32 er_index)
{
	return mhi_sahara_event_ring_used(mhi_cntrl, er_index) ||
		(er_index == 0 && mhi_sahara_available(mhi_cntrl));
}

static void mhi_sahara_dump_event_ring(struct mhi_controller *mhi_cntrl,
					       struct mhi_event *mhi_event,
					       const char *reason)
{
	struct mhi_event_ctxt *er_ctxt;
	struct mhi_ring *ev_ring;
	struct mhi_ring_element *event;
	struct mhi_ring *cmd_ring;
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	u32 i, entries;

	if (!mhi_cntrl->mhi_ctxt || mhi_event->er_index >= mhi_cntrl->total_ev_rings)
		return;

	er_ctxt = &mhi_cntrl->mhi_ctxt->er_ctxt[mhi_event->er_index];
	ev_ring = &mhi_event->ring;
	cmd_ring = &mhi_cntrl->mhi_cmd[PRIMARY_CMD_RING].ring;
	entries = min_t(u32, ev_ring->elements, MHI_SAHARA_EVENT_DUMP_COUNT);

	dev_info(dev,
		 "SAHARA %s er%u ring: local_rp 0x%llx local_wp 0x%llx ctxt_rp 0x%llx ctxt_wp 0x%llx base 0x%llx len 0x%zx type 0x%x msivec %u irq %u linux_irq %d chan %d intmod 0x%x\n",
		 reason, mhi_event->er_index,
		 (unsigned long long)(ev_ring->iommu_base +
			((u8 *)ev_ring->rp - (u8 *)ev_ring->base)),
		 (unsigned long long)(ev_ring->iommu_base +
			((u8 *)ev_ring->wp - (u8 *)ev_ring->base)),
		 (unsigned long long)le64_to_cpu(er_ctxt->rp),
		 (unsigned long long)le64_to_cpu(er_ctxt->wp),
		 (unsigned long long)ev_ring->iommu_base, ev_ring->len,
		 le32_to_cpu(er_ctxt->ertype), le32_to_cpu(er_ctxt->msivec),
		 mhi_event->irq, mhi_event->irq < mhi_cntrl->nr_irqs ?
		 mhi_cntrl->irq[mhi_event->irq] : -1, mhi_event->chan,
		 le32_to_cpu(er_ctxt->intmod));

	event = ev_ring->rp;
	for (i = 0; i < entries; i++) {
		dma_addr_t event_dma = ev_ring->iommu_base +
			((u8 *)event - (u8 *)ev_ring->base);
		dma_addr_t ev_ptr = MHI_TRE_GET_EV_PTR(event);
		u32 cmd_chan = U32_MAX;
		u32 type = MHI_TRE_GET_EV_TYPE(event);

		if (type == MHI_PKT_TYPE_CMD_COMPLETION_EVENT &&
		    is_valid_ring_ptr(cmd_ring, ev_ptr)) {
			struct mhi_ring_element *cmd_pkt;

			cmd_pkt = mhi_to_virtual(cmd_ring, ev_ptr);
			cmd_chan = MHI_TRE_GET_CMD_CHID(cmd_pkt);
		}

		dev_info(dev,
			 "SAHARA %s er%u[%u] dma 0x%llx raw 0x%llx 0x%x 0x%x type 0x%x chan %u cmd_chan %u code 0x%x len %u ptr 0x%llx\n",
			 reason, mhi_event->er_index, i,
			 (unsigned long long)event_dma,
			 (unsigned long long)le64_to_cpu(event->ptr),
			 le32_to_cpu(event->dword[0]), le32_to_cpu(event->dword[1]),
			 type, (u32)MHI_TRE_GET_EV_CHID(event), cmd_chan,
			 (u32)MHI_TRE_GET_EV_CODE(event),
			 (u32)MHI_TRE_GET_EV_LEN(event),
			 (unsigned long long)ev_ptr);

		event = (void *)event + ev_ring->el_size;
		if ((void *)event >= ev_ring->base + ev_ring->len)
			event = ev_ring->base;
	}
}

static void mhi_sahara_dump_event_rings(struct mhi_controller *mhi_cntrl,
						struct mhi_chan *mhi_chan,
						const char *reason)
{
	if (!mhi_cntrl->mhi_event || !mhi_cntrl->mhi_ctxt)
		return;

	mhi_sahara_dump_event_ring(mhi_cntrl, &mhi_cntrl->mhi_event[0], reason);
	if (mhi_chan->er_index && mhi_chan->er_index < mhi_cntrl->total_ev_rings)
		mhi_sahara_dump_event_ring(mhi_cntrl,
					       &mhi_cntrl->mhi_event[mhi_chan->er_index],
					       reason);
}

int mhi_destroy_device(struct device *dev, void *data)
{
	struct mhi_chan *ul_chan, *dl_chan;
	struct mhi_device *mhi_dev;
	struct mhi_controller *mhi_cntrl;
	enum mhi_ee_type ee = MHI_EE_MAX;

	if (dev->bus != &mhi_bus_type)
		return 0;

	mhi_dev = to_mhi_device(dev);
	mhi_cntrl = mhi_dev->mhi_cntrl;

	/* Only destroy virtual devices thats attached to bus */
	if (mhi_dev->dev_type == MHI_DEVICE_CONTROLLER)
		return 0;

	ul_chan = mhi_dev->ul_chan;
	dl_chan = mhi_dev->dl_chan;

	/*
	 * If execution environment is specified, remove only those devices that
	 * started in them based on ee_mask for the channels as we move on to a
	 * different execution environment
	 */
	if (data)
		ee = *(enum mhi_ee_type *)data;

	/*
	 * For the suspend and resume case, this function will get called
	 * without mhi_unregister_controller(). Hence, we need to drop the
	 * references to mhi_dev created for ul and dl channels. We can
	 * be sure that there will be no instances of mhi_dev left after
	 * this.
	 */
	if (ul_chan) {
		if (ee != MHI_EE_MAX && !(ul_chan->ee_mask & BIT(ee)))
			return 0;

		put_device(&ul_chan->mhi_dev->dev);
	}

	if (dl_chan) {
		if (ee != MHI_EE_MAX && !(dl_chan->ee_mask & BIT(ee)))
			return 0;

		put_device(&dl_chan->mhi_dev->dev);
	}

	dev_dbg(&mhi_cntrl->mhi_dev->dev, "destroy device for chan:%s\n",
		 mhi_dev->name);

	/* Notify the client and remove the device from MHI bus */
	device_del(dev);
	put_device(dev);

	return 0;
}

int mhi_get_free_desc_count(struct mhi_device *mhi_dev,
				enum dma_data_direction dir)
{
	struct mhi_controller *mhi_cntrl = mhi_dev->mhi_cntrl;
	struct mhi_chan *mhi_chan = (dir == DMA_TO_DEVICE) ?
		mhi_dev->ul_chan : mhi_dev->dl_chan;
	struct mhi_ring *tre_ring = &mhi_chan->tre_ring;

	return get_nr_avail_ring_elements(mhi_cntrl, tre_ring);
}
EXPORT_SYMBOL_GPL(mhi_get_free_desc_count);

void mhi_notify(struct mhi_device *mhi_dev, enum mhi_callback cb_reason)
{
	struct mhi_driver *mhi_drv;

	if (!mhi_dev->dev.driver)
		return;

	mhi_drv = to_mhi_driver(mhi_dev->dev.driver);

	if (mhi_drv->status_cb)
		mhi_drv->status_cb(mhi_dev, cb_reason);
}
EXPORT_SYMBOL_GPL(mhi_notify);

/* Bind MHI channels to MHI devices */
void mhi_create_devices(struct mhi_controller *mhi_cntrl)
{
	struct mhi_chan *mhi_chan;
	struct mhi_device *mhi_dev;
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	int i, ret;

	mhi_chan = mhi_cntrl->mhi_chan;
	for (i = 0; i < mhi_cntrl->max_chan; i++, mhi_chan++) {
		if (!mhi_chan->configured || mhi_chan->mhi_dev ||
		    !(mhi_chan->ee_mask & BIT(mhi_cntrl->ee)))
			continue;
		mhi_dev = mhi_alloc_device(mhi_cntrl);
		if (IS_ERR(mhi_dev))
			return;

		mhi_dev->dev_type = MHI_DEVICE_XFER;
		switch (mhi_chan->dir) {
		case DMA_TO_DEVICE:
			mhi_dev->ul_chan = mhi_chan;
			mhi_dev->ul_chan_id = mhi_chan->chan;
			break;
		case DMA_FROM_DEVICE:
			/* We use dl_chan as offload channels */
			mhi_dev->dl_chan = mhi_chan;
			mhi_dev->dl_chan_id = mhi_chan->chan;
			break;
		default:
			dev_err(dev, "Direction not supported\n");
			put_device(&mhi_dev->dev);
			return;
		}

		get_device(&mhi_dev->dev);
		mhi_chan->mhi_dev = mhi_dev;

		/* Check next channel if it matches */
		if ((i + 1) < mhi_cntrl->max_chan && mhi_chan[1].configured) {
			if (!strcmp(mhi_chan[1].name, mhi_chan->name)) {
				i++;
				mhi_chan++;
				if (mhi_chan->dir == DMA_TO_DEVICE) {
					mhi_dev->ul_chan = mhi_chan;
					mhi_dev->ul_chan_id = mhi_chan->chan;
				} else {
					mhi_dev->dl_chan = mhi_chan;
					mhi_dev->dl_chan_id = mhi_chan->chan;
				}
				get_device(&mhi_dev->dev);
				mhi_chan->mhi_dev = mhi_dev;
			}
		}

		/* Channel name is same for both UL and DL */
		mhi_dev->name = mhi_chan->name;
		dev_set_name(&mhi_dev->dev, "%s_%s",
			     dev_name(&mhi_cntrl->mhi_dev->dev),
			     mhi_dev->name);

		/* Init wakeup source if available */
		if (mhi_dev->dl_chan && mhi_dev->dl_chan->wake_capable)
			device_init_wakeup(&mhi_dev->dev, true);

		ret = device_add(&mhi_dev->dev);
		if (ret)
			put_device(&mhi_dev->dev);
	}
}

irqreturn_t mhi_irq_handler(int irq_number, void *dev)
{
	struct mhi_event *mhi_event = dev;
	struct mhi_controller *mhi_cntrl = mhi_event->mhi_cntrl;
	struct mhi_event_ctxt *er_ctxt;
	struct mhi_ring *ev_ring = &mhi_event->ring;
	dma_addr_t ptr;
	void *dev_rp;

	/*
	 * If CONFIG_DEBUG_SHIRQ is set, the IRQ handler will get invoked during __free_irq()
	 * and by that time mhi_ctxt() would've freed. So check for the existence of mhi_ctxt
	 * before handling the IRQs.
	 */
	if (!mhi_cntrl->mhi_ctxt) {
		dev_dbg(&mhi_cntrl->mhi_dev->dev,
			"mhi_ctxt has been freed\n");
		return IRQ_HANDLED;
	}

	er_ctxt = &mhi_cntrl->mhi_ctxt->er_ctxt[mhi_event->er_index];
	ptr = le64_to_cpu(er_ctxt->rp);

	if (!is_valid_ring_ptr(ev_ring, ptr)) {
		dev_err(&mhi_cntrl->mhi_dev->dev,
			"Event ring rp points outside of the event ring\n");
		return IRQ_HANDLED;
	}

	dev_rp = mhi_to_virtual(ev_ring, ptr);

	if (mhi_sahara_event_ring_relevant(mhi_cntrl, mhi_event->er_index))
		dev_info(&mhi_cntrl->mhi_dev->dev,
			 "SAHARA irq %d er%u: local_rp 0x%llx dev_rp 0x%llx ctxt_rp 0x%llx irq_index %u pending %d pm %s\n",
			 irq_number, mhi_event->er_index,
			 (unsigned long long)(ev_ring->iommu_base +
				((u8 *)ev_ring->rp - (u8 *)ev_ring->base)),
			 (unsigned long long)(ev_ring->iommu_base +
				((u8 *)dev_rp - (u8 *)ev_ring->base)),
			 (unsigned long long)ptr, mhi_event->irq,
			 ev_ring->rp != dev_rp,
			 to_mhi_pm_state_str(READ_ONCE(mhi_cntrl->pm_state)));

	/* Only proceed if event ring has pending events */
	if (ev_ring->rp == dev_rp)
		return IRQ_HANDLED;

	/* For client managed event ring, notify pending data */
	if (mhi_event->cl_manage) {
		struct mhi_chan *mhi_chan = mhi_event->mhi_chan;
		struct mhi_device *mhi_dev = mhi_chan->mhi_dev;

		if (mhi_dev)
			mhi_notify(mhi_dev, MHI_CB_PENDING_DATA);
	} else {
		tasklet_schedule(&mhi_event->task);
	}

	return IRQ_HANDLED;
}

irqreturn_t mhi_intvec_threaded_handler(int irq_number, void *priv)
{
	struct mhi_controller *mhi_cntrl = priv;
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	enum mhi_state state;
	enum mhi_pm_state pm_state = 0;
	enum mhi_ee_type ee;

	write_lock_irq(&mhi_cntrl->pm_lock);
	if (!MHI_REG_ACCESS_VALID(mhi_cntrl->pm_state)) {
		write_unlock_irq(&mhi_cntrl->pm_lock);
		goto exit_intvec;
	}

	state = mhi_get_mhi_state(mhi_cntrl);
	ee = mhi_get_exec_env(mhi_cntrl);

	trace_mhi_intvec_states(mhi_cntrl, ee, state);
	if (state == MHI_STATE_SYS_ERR) {
		dev_dbg(dev, "System error detected\n");
		pm_state = mhi_tryset_pm_state(mhi_cntrl,
					       MHI_PM_SYS_ERR_DETECT);
	}
	write_unlock_irq(&mhi_cntrl->pm_lock);

	if (pm_state != MHI_PM_SYS_ERR_DETECT)
		goto exit_intvec;

	switch (ee) {
	case MHI_EE_RDDM:
		/* proceed if power down is not already in progress */
		if (mhi_cntrl->rddm_image && mhi_is_active(mhi_cntrl)) {
			mhi_cntrl->status_cb(mhi_cntrl, MHI_CB_EE_RDDM);
			mhi_cntrl->ee = ee;
			mhi_uevent_notify(mhi_cntrl, mhi_cntrl->ee);
			wake_up_all(&mhi_cntrl->state_event);
		}
		break;
	case MHI_EE_PBL:
	case MHI_EE_EDL:
	case MHI_EE_PTHRU:
		mhi_cntrl->status_cb(mhi_cntrl, MHI_CB_FATAL_ERROR);
		mhi_cntrl->ee = ee;
		wake_up_all(&mhi_cntrl->state_event);
		mhi_pm_sys_err_handler(mhi_cntrl);
		break;
	default:
		wake_up_all(&mhi_cntrl->state_event);
		mhi_pm_sys_err_handler(mhi_cntrl);
		break;
	}

exit_intvec:

	return IRQ_HANDLED;
}

irqreturn_t mhi_intvec_handler(int irq_number, void *dev)
{
	struct mhi_controller *mhi_cntrl = dev;

	/* Wake up events waiting for state change */
	wake_up_all(&mhi_cntrl->state_event);

	return IRQ_WAKE_THREAD;
}

static void mhi_recycle_ev_ring_element(struct mhi_controller *mhi_cntrl,
					struct mhi_ring *ring)
{
	/* Update the WP */
	ring->wp += ring->el_size;

	if (ring->wp >= (ring->base + ring->len))
		ring->wp = ring->base;

	*ring->ctxt_wp = cpu_to_le64(ring->iommu_base + (ring->wp - ring->base));

	/* Update the RP */
	ring->rp += ring->el_size;
	if (ring->rp >= (ring->base + ring->len))
		ring->rp = ring->base;

	/* Update to all cores */
	smp_wmb();
}

static int parse_xfer_event(struct mhi_controller *mhi_cntrl,
			    struct mhi_ring_element *event,
			    struct mhi_chan *mhi_chan)
{
	struct mhi_ring *buf_ring, *tre_ring;
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	struct mhi_result result;
	unsigned long flags = 0;
	u32 ev_code;

	ev_code = MHI_TRE_GET_EV_CODE(event);
	buf_ring = &mhi_chan->buf_ring;
	tre_ring = &mhi_chan->tre_ring;

	if (mhi_is_sbl_boot_chan(mhi_chan)) {
		enum mhi_ee_type reg_ee = mhi_cntrl->bhi ?
			mhi_get_exec_env(mhi_cntrl) : MHI_EE_MAX;
		enum mhi_state reg_state = mhi_cntrl->regs ?
			mhi_get_mhi_state(mhi_cntrl) : MHI_STATE_MAX;

		dev_info(dev,
			 "SAHARA xfer event: chan %u dir %u type 0x%x code 0x%x len %u ptr 0x%llx host_ch_state %u tre_rp 0x%llx tre_wp 0x%llx cached_ee=%s cached_state=%s reg_ee=%s reg_state=%s pm=0x%x\n",
			 mhi_chan->chan, mhi_chan->dir,
			 (u32)MHI_TRE_GET_EV_TYPE(event), ev_code,
			 (u32)MHI_TRE_GET_EV_LEN(event),
			 (unsigned long long)MHI_TRE_GET_EV_PTR(event),
			 mhi_chan->ch_state,
			 (unsigned long long)(tre_ring->iommu_base +
				((u8 *)tre_ring->rp - (u8 *)tre_ring->base)),
			 (unsigned long long)(tre_ring->iommu_base +
				((u8 *)tre_ring->wp - (u8 *)tre_ring->base)),
			 TO_MHI_EXEC_STR(mhi_cntrl->ee), mhi_state_str(mhi_cntrl->dev_state),
			 TO_MHI_EXEC_STR(reg_ee), mhi_state_str(reg_state), mhi_cntrl->pm_state);
	}

	result.transaction_status = (ev_code == MHI_EV_CC_OVERFLOW) ?
		-EOVERFLOW : 0;

	/*
	 * If it's a DB Event then we need to grab the lock
	 * with preemption disabled and as a write because we
	 * have to update db register and there are chances that
	 * another thread could be doing the same.
	 */
	if (ev_code >= MHI_EV_CC_OOB)
		write_lock_irqsave(&mhi_chan->lock, flags);
	else
		read_lock_bh(&mhi_chan->lock);

	if (mhi_chan->ch_state != MHI_CH_STATE_ENABLED)
		goto end_process_tx_event;

	switch (ev_code) {
	case MHI_EV_CC_OVERFLOW:
	case MHI_EV_CC_EOB:
	case MHI_EV_CC_EOT:
	{
		dma_addr_t ptr = MHI_TRE_GET_EV_PTR(event);
		struct mhi_ring_element *local_rp, *ev_tre;
		void *dev_rp, *next_rp;
		struct mhi_buf_info *buf_info;
		bool accepted_multi_tre_completion = false;
		u16 xfer_len;

		if (!is_valid_ring_ptr(tre_ring, ptr)) {
			dev_err(&mhi_cntrl->mhi_dev->dev,
				"Event element points outside of the tre ring\n");
			break;
		}
		/* Get the TRB this event points to */
		ev_tre = mhi_to_virtual(tre_ring, ptr);

		dev_rp = ev_tre + 1;
		if (dev_rp >= (tre_ring->base + tre_ring->len))
			dev_rp = tre_ring->base;

		result.dir = mhi_chan->dir;

		local_rp = tre_ring->rp;

		next_rp = local_rp + 1;
		if (next_rp >= tre_ring->base + tre_ring->len)
			next_rp = tre_ring->base;
		if (dev_rp != next_rp && !MHI_TRE_DATA_GET_CHAIN(local_rp)) {
			if (mhi_is_sbl_boot_chan(mhi_chan) &&
			    mhi_sbl_accept_multi_tre_completion) {
				accepted_multi_tre_completion = true;
				dev_warn(&mhi_cntrl->mhi_dev->dev,
					 "SBL channel %u accepting multi-TRE completion ptr 0x%llx local_rp 0x%llx dev_rp 0x%llx\n",
					 mhi_chan->chan, (unsigned long long)ptr,
					 (unsigned long long)(tre_ring->iommu_base +
						((u8 *)local_rp - (u8 *)tre_ring->base)),
					 (unsigned long long)(tre_ring->iommu_base +
						((u8 *)dev_rp - (u8 *)tre_ring->base)));
			} else {
				dev_err(&mhi_cntrl->mhi_dev->dev,
					"Event element points to an unexpected TRE\n");
				break;
			}
		}

		while (local_rp != dev_rp) {
			const __le32 *inferred_words = NULL;
			bool skipped_tre;

			buf_info = buf_ring->rp;
			skipped_tre = accepted_multi_tre_completion && local_rp != ev_tre;
			if (local_rp == ev_tre)
				xfer_len = MHI_TRE_GET_EV_LEN(event);
			else if (skipped_tre)
				xfer_len = 0;
			else
				xfer_len = buf_info->len;

			/* Unmap if it's not pre-mapped by client */
			if (likely(!buf_info->pre_mapped))
				mhi_cntrl->unmap_single(mhi_cntrl, buf_info);

			result.buf_addr = buf_info->cb_buf;
			if (skipped_tre && mhi_chan->dir == DMA_FROM_DEVICE) {
				xfer_len = mhi_sbl_infer_rx_len(buf_info, &inferred_words);
				if (inferred_words && buf_info->len >= MHI_SBL_SAHARA_MIN_LEN)
					dev_warn(dev,
						 "SBL channel %u skipped RX TRE cmd %u pkt_len %u inferred_len %u buf_len %zu tre_rp 0x%llx ev_tre 0x%llx\n",
						 mhi_chan->chan, le32_to_cpu(inferred_words[0]),
						 le32_to_cpu(inferred_words[1]), xfer_len, buf_info->len,
						 (unsigned long long)(tre_ring->iommu_base +
							((u8 *)local_rp - (u8 *)tre_ring->base)),
						 (unsigned long long)(tre_ring->iommu_base +
							((u8 *)ev_tre - (u8 *)tre_ring->base)));
			}

			/* truncate to buf len if xfer_len is larger */
			result.bytes_xferd =
				min_t(u16, xfer_len, buf_info->len);
			mhi_del_ring_element(mhi_cntrl, buf_ring);
			mhi_del_ring_element(mhi_cntrl, tre_ring);
			local_rp = tre_ring->rp;

			read_unlock_bh(&mhi_chan->lock);

			/* notify client */
			mhi_chan->xfer_cb(mhi_chan->mhi_dev, &result);

			if (mhi_chan->dir == DMA_TO_DEVICE) {
				atomic_dec(&mhi_cntrl->pending_pkts);
				/* Release the reference got from mhi_queue() */
				mhi_cntrl->runtime_put(mhi_cntrl);
			}

			read_lock_bh(&mhi_chan->lock);
		}
		break;
	} /* CC_EOT */
	case MHI_EV_CC_OOB:
	case MHI_EV_CC_DB_MODE:
	{
		unsigned long pm_lock_flags;

		mhi_chan->db_cfg.db_mode = 1;
		read_lock_irqsave(&mhi_cntrl->pm_lock, pm_lock_flags);
		if (tre_ring->wp != tre_ring->rp &&
		    MHI_DB_ACCESS_VALID(mhi_cntrl)) {
			mhi_ring_chan_db(mhi_cntrl, mhi_chan);
		}
		read_unlock_irqrestore(&mhi_cntrl->pm_lock, pm_lock_flags);
		break;
	}
	case MHI_EV_CC_BAD_TRE:
	default:
		dev_err(dev, "Unknown event 0x%x\n", ev_code);
		break;
	} /* switch(MHI_EV_READ_CODE(EV_TRB_CODE,event)) */

end_process_tx_event:
	if (ev_code >= MHI_EV_CC_OOB)
		write_unlock_irqrestore(&mhi_chan->lock, flags);
	else
		read_unlock_bh(&mhi_chan->lock);

	return 0;
}

static int parse_rsc_event(struct mhi_controller *mhi_cntrl,
			   struct mhi_ring_element *event,
			   struct mhi_chan *mhi_chan)
{
	struct mhi_ring *buf_ring, *tre_ring;
	struct mhi_buf_info *buf_info;
	struct mhi_result result;
	int ev_code;
	u32 cookie; /* offset to local descriptor */
	u16 xfer_len;

	buf_ring = &mhi_chan->buf_ring;
	tre_ring = &mhi_chan->tre_ring;

	ev_code = MHI_TRE_GET_EV_CODE(event);
	cookie = MHI_TRE_GET_EV_COOKIE(event);
	xfer_len = MHI_TRE_GET_EV_LEN(event);

	/* Received out of bound cookie */
	WARN_ON(cookie >= buf_ring->len);

	buf_info = buf_ring->base + cookie;

	result.transaction_status = (ev_code == MHI_EV_CC_OVERFLOW) ?
		-EOVERFLOW : 0;

	/* truncate to buf len if xfer_len is larger */
	result.bytes_xferd = min_t(u16, xfer_len, buf_info->len);
	result.buf_addr = buf_info->cb_buf;
	result.dir = mhi_chan->dir;

	read_lock_bh(&mhi_chan->lock);

	if (mhi_chan->ch_state != MHI_CH_STATE_ENABLED)
		goto end_process_rsc_event;

	WARN_ON(!buf_info->used);

	/* notify the client */
	mhi_chan->xfer_cb(mhi_chan->mhi_dev, &result);

	/*
	 * Note: We're arbitrarily incrementing RP even though, completion
	 * packet we processed might not be the same one, reason we can do this
	 * is because device guaranteed to cache descriptors in order it
	 * receive, so even though completion event is different we can re-use
	 * all descriptors in between.
	 * Example:
	 * Transfer Ring has descriptors: A, B, C, D
	 * Last descriptor host queue is D (WP) and first descriptor
	 * host queue is A (RP).
	 * The completion event we just serviced is descriptor C.
	 * Then we can safely queue descriptors to replace A, B, and C
	 * even though host did not receive any completions.
	 */
	mhi_del_ring_element(mhi_cntrl, tre_ring);
	buf_info->used = false;

end_process_rsc_event:
	read_unlock_bh(&mhi_chan->lock);

	return 0;
}

static void mhi_process_cmd_completion(struct mhi_controller *mhi_cntrl,
				       struct mhi_ring_element *tre)
{
	dma_addr_t ptr = MHI_TRE_GET_EV_PTR(tre);
	struct mhi_cmd *cmd_ring = &mhi_cntrl->mhi_cmd[PRIMARY_CMD_RING];
	struct mhi_ring *mhi_ring = &cmd_ring->ring;
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	struct mhi_ring_element *cmd_pkt;
	struct mhi_chan *mhi_chan;
	u32 chan, ev_code;

	if (!is_valid_ring_ptr(mhi_ring, ptr)) {
		dev_err(dev, "Event element points outside of the cmd ring\n");
		return;
	}

	cmd_pkt = mhi_to_virtual(mhi_ring, ptr);

	chan = MHI_TRE_GET_CMD_CHID(cmd_pkt);
	ev_code = MHI_TRE_GET_EV_CODE(tre);

	if (chan < mhi_cntrl->max_chan &&
	    mhi_cntrl->mhi_chan[chan].configured) {
		mhi_chan = &mhi_cntrl->mhi_chan[chan];
		if (mhi_is_sbl_boot_chan(mhi_chan))
			dev_info(dev,
				 "SAHARA cmd completion: chan %u code 0x%x event_ptr 0x%llx\n",
				 chan, ev_code, (unsigned long long)ptr);
		write_lock_bh(&mhi_chan->lock);
		mhi_chan->ccs = ev_code;
		complete(&mhi_chan->completion);
		write_unlock_bh(&mhi_chan->lock);
	} else {
		dev_err(dev, "Completion packet for invalid channel ID: %d\n", chan);
	}

	mhi_del_ring_element(mhi_cntrl, mhi_ring);
}

int mhi_process_ctrl_ev_ring(struct mhi_controller *mhi_cntrl,
			     struct mhi_event *mhi_event,
			     u32 event_quota)
{
	struct mhi_ring_element *dev_rp, *local_rp;
	struct mhi_ring *ev_ring = &mhi_event->ring;
	struct mhi_event_ctxt *er_ctxt =
		&mhi_cntrl->mhi_ctxt->er_ctxt[mhi_event->er_index];
	struct mhi_chan *mhi_chan;
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	u32 chan;
	int count = 0;
	dma_addr_t ptr = le64_to_cpu(er_ctxt->rp);

	/*
	 * This is a quick check to avoid unnecessary event processing
	 * in case MHI is already in error state, but it's still possible
	 * to transition to error state while processing events
	 */
	if (unlikely(MHI_EVENT_ACCESS_INVALID(mhi_cntrl->pm_state)))
		return -EIO;

	if (!is_valid_ring_ptr(ev_ring, ptr)) {
		dev_err(&mhi_cntrl->mhi_dev->dev,
			"Event ring rp points outside of the event ring\n");
		return -EIO;
	}

	dev_rp = mhi_to_virtual(ev_ring, ptr);
	local_rp = ev_ring->rp;

	while (dev_rp != local_rp) {
		enum mhi_pkt_type type = MHI_TRE_GET_EV_TYPE(local_rp);

		trace_mhi_ctrl_event(mhi_cntrl, local_rp);

		if (mhi_sahara_event_ring_used(mhi_cntrl, mhi_event->er_index))
			dev_info(dev,
				 "SAHARA ctrl er%u event: type 0x%x code 0x%x ptr 0x%llx local_rp 0x%llx ctxt_rp 0x%llx ctxt_wp 0x%llx er_type 0x%x msivec %u irq %u linux_irq %d intmod 0x%x\n",
				 mhi_event->er_index, type,
				 (u32)MHI_TRE_GET_EV_CODE(local_rp),
				 (unsigned long long)MHI_TRE_GET_EV_PTR(local_rp),
				 (unsigned long long)(ev_ring->iommu_base +
					((u8 *)local_rp - (u8 *)ev_ring->base)),
				 (unsigned long long)le64_to_cpu(er_ctxt->rp),
				 (unsigned long long)le64_to_cpu(er_ctxt->wp),
				 le32_to_cpu(er_ctxt->ertype), le32_to_cpu(er_ctxt->msivec),
				 mhi_event->irq, mhi_event->irq < mhi_cntrl->nr_irqs ?
				 mhi_cntrl->irq[mhi_event->irq] : -1,
				 le32_to_cpu(er_ctxt->intmod));

		switch (type) {
		case MHI_PKT_TYPE_BW_REQ_EVENT:
		{
			struct mhi_link_info *link_info;

			link_info = &mhi_cntrl->mhi_link_info;
			write_lock_irq(&mhi_cntrl->pm_lock);
			link_info->target_link_speed =
				MHI_TRE_GET_EV_LINKSPEED(local_rp);
			link_info->target_link_width =
				MHI_TRE_GET_EV_LINKWIDTH(local_rp);
			write_unlock_irq(&mhi_cntrl->pm_lock);
			dev_dbg(dev, "Received BW_REQ event\n");
			mhi_cntrl->status_cb(mhi_cntrl, MHI_CB_BW_REQ);
			break;
		}
		case MHI_PKT_TYPE_STATE_CHANGE_EVENT:
		{
			enum mhi_state new_state;

			new_state = MHI_TRE_GET_EV_STATE(local_rp);

			dev_dbg(dev, "State change event to state: %s\n",
				mhi_state_str(new_state));

			switch (new_state) {
			case MHI_STATE_M0:
				mhi_pm_m0_transition(mhi_cntrl);
				break;
			case MHI_STATE_M1:
				mhi_pm_m1_transition(mhi_cntrl);
				break;
			case MHI_STATE_M3:
				mhi_pm_m3_transition(mhi_cntrl);
				break;
			case MHI_STATE_SYS_ERR:
			{
				enum mhi_pm_state pm_state;

				dev_dbg(dev, "System error detected\n");
				write_lock_irq(&mhi_cntrl->pm_lock);
				pm_state = mhi_tryset_pm_state(mhi_cntrl,
							MHI_PM_SYS_ERR_DETECT);
				write_unlock_irq(&mhi_cntrl->pm_lock);
				if (pm_state == MHI_PM_SYS_ERR_DETECT)
					mhi_pm_sys_err_handler(mhi_cntrl);
				break;
			}
			default:
				dev_err(dev, "Invalid state: %s\n",
					mhi_state_str(new_state));
			}

			break;
		}
		case MHI_PKT_TYPE_CMD_COMPLETION_EVENT:
			mhi_process_cmd_completion(mhi_cntrl, local_rp);
			break;
		case MHI_PKT_TYPE_EE_EVENT:
		{
			enum dev_st_transition st = DEV_ST_TRANSITION_MAX;
			enum mhi_ee_type event = MHI_TRE_GET_EV_EXECENV(local_rp);

			dev_dbg(dev, "Received EE event: %s\n",
				TO_MHI_EXEC_STR(event));
			switch (event) {
			case MHI_EE_SBL:
				st = DEV_ST_TRANSITION_SBL;
				break;
			case MHI_EE_WFW:
			case MHI_EE_AMSS:
				st = DEV_ST_TRANSITION_MISSION_MODE;
				break;
			case MHI_EE_FP:
				st = DEV_ST_TRANSITION_FP;
				break;
			case MHI_EE_RDDM:
				mhi_cntrl->status_cb(mhi_cntrl, MHI_CB_EE_RDDM);
				write_lock_irq(&mhi_cntrl->pm_lock);
				mhi_cntrl->ee = event;
				write_unlock_irq(&mhi_cntrl->pm_lock);
				wake_up_all(&mhi_cntrl->state_event);
				break;
			default:
				dev_err(dev,
					"Unhandled EE event: 0x%x\n", type);
			}
			if (st != DEV_ST_TRANSITION_MAX)
				mhi_queue_state_transition(mhi_cntrl, st);

			break;
		}
		case MHI_PKT_TYPE_TX_EVENT:
			chan = MHI_TRE_GET_EV_CHID(local_rp);

			WARN_ON(chan >= mhi_cntrl->max_chan);

			/*
			 * Only process the event ring elements whose channel
			 * ID is within the maximum supported range.
			 */
			if (chan < mhi_cntrl->max_chan) {
				mhi_chan = &mhi_cntrl->mhi_chan[chan];
				if (!mhi_chan->configured)
					break;
				parse_xfer_event(mhi_cntrl, local_rp, mhi_chan);
			}
			break;
		default:
			dev_err(dev, "Unhandled event type: %d\n", type);
			break;
		}

		mhi_recycle_ev_ring_element(mhi_cntrl, ev_ring);
		local_rp = ev_ring->rp;

		ptr = le64_to_cpu(er_ctxt->rp);
		if (!is_valid_ring_ptr(ev_ring, ptr)) {
			dev_err(&mhi_cntrl->mhi_dev->dev,
				"Event ring rp points outside of the event ring\n");
			return -EIO;
		}

		dev_rp = mhi_to_virtual(ev_ring, ptr);
		count++;
	}

	read_lock_bh(&mhi_cntrl->pm_lock);

	/* Ring EV DB only if there is any pending element to process */
	if (likely(MHI_DB_ACCESS_VALID(mhi_cntrl)) && count)
		mhi_ring_er_db(mhi_event);
	read_unlock_bh(&mhi_cntrl->pm_lock);

	return count;
}

int mhi_process_data_event_ring(struct mhi_controller *mhi_cntrl,
				struct mhi_event *mhi_event,
				u32 event_quota)
{
	struct mhi_ring_element *dev_rp, *local_rp;
	struct mhi_ring *ev_ring = &mhi_event->ring;
	struct mhi_event_ctxt *er_ctxt =
		&mhi_cntrl->mhi_ctxt->er_ctxt[mhi_event->er_index];
	int count = 0;
	u32 chan;
	struct mhi_chan *mhi_chan;
	dma_addr_t ptr = le64_to_cpu(er_ctxt->rp);

	if (unlikely(MHI_EVENT_ACCESS_INVALID(mhi_cntrl->pm_state)))
		return -EIO;

	if (!is_valid_ring_ptr(ev_ring, ptr)) {
		dev_err(&mhi_cntrl->mhi_dev->dev,
			"Event ring rp points outside of the event ring\n");
		return -EIO;
	}

	dev_rp = mhi_to_virtual(ev_ring, ptr);
	local_rp = ev_ring->rp;

	while (dev_rp != local_rp && event_quota > 0) {
		enum mhi_pkt_type type = MHI_TRE_GET_EV_TYPE(local_rp);

		trace_mhi_data_event(mhi_cntrl, local_rp);

		chan = MHI_TRE_GET_EV_CHID(local_rp);

		if (mhi_sahara_event_ring_used(mhi_cntrl, mhi_event->er_index))
			dev_info(&mhi_cntrl->mhi_dev->dev,
				 "SAHARA data er%u event: type 0x%x chan %u code 0x%x len %u ptr 0x%llx local_rp 0x%llx ctxt_rp 0x%llx ctxt_wp 0x%llx er_type 0x%x msivec %u irq %u linux_irq %d intmod 0x%x\n",
				 mhi_event->er_index, type, chan,
				 (u32)MHI_TRE_GET_EV_CODE(local_rp),
				 (u32)MHI_TRE_GET_EV_LEN(local_rp),
				 (unsigned long long)MHI_TRE_GET_EV_PTR(local_rp),
				 (unsigned long long)(ev_ring->iommu_base +
					((u8 *)local_rp - (u8 *)ev_ring->base)),
				 (unsigned long long)le64_to_cpu(er_ctxt->rp),
				 (unsigned long long)le64_to_cpu(er_ctxt->wp),
				 le32_to_cpu(er_ctxt->ertype), le32_to_cpu(er_ctxt->msivec),
				 mhi_event->irq, mhi_event->irq < mhi_cntrl->nr_irqs ?
				 mhi_cntrl->irq[mhi_event->irq] : -1,
				 le32_to_cpu(er_ctxt->intmod));

		WARN_ON(chan >= mhi_cntrl->max_chan);

		/*
		 * Only process the event ring elements whose channel
		 * ID is within the maximum supported range.
		 */
		if (chan < mhi_cntrl->max_chan &&
		    mhi_cntrl->mhi_chan[chan].configured) {
			mhi_chan = &mhi_cntrl->mhi_chan[chan];

			if (likely(type == MHI_PKT_TYPE_TX_EVENT)) {
				parse_xfer_event(mhi_cntrl, local_rp, mhi_chan);
				event_quota--;
			} else if (type == MHI_PKT_TYPE_RSC_TX_EVENT) {
				parse_rsc_event(mhi_cntrl, local_rp, mhi_chan);
				event_quota--;
			}
		}

		mhi_recycle_ev_ring_element(mhi_cntrl, ev_ring);
		local_rp = ev_ring->rp;

		ptr = le64_to_cpu(er_ctxt->rp);
		if (!is_valid_ring_ptr(ev_ring, ptr)) {
			dev_err(&mhi_cntrl->mhi_dev->dev,
				"Event ring rp points outside of the event ring\n");
			return -EIO;
		}

		dev_rp = mhi_to_virtual(ev_ring, ptr);
		count++;
	}
	read_lock_bh(&mhi_cntrl->pm_lock);

	/* Ring EV DB only if there is any pending element to process */
	if (likely(MHI_DB_ACCESS_VALID(mhi_cntrl)) && count)
		mhi_ring_er_db(mhi_event);
	read_unlock_bh(&mhi_cntrl->pm_lock);

	return count;
}

void mhi_ev_task(unsigned long data)
{
	struct mhi_event *mhi_event = (struct mhi_event *)data;
	struct mhi_controller *mhi_cntrl = mhi_event->mhi_cntrl;

	if (mhi_sahara_event_ring_used(mhi_cntrl, mhi_event->er_index))
		dev_info(&mhi_cntrl->mhi_dev->dev,
			 "SAHARA event task er%u: local_rp 0x%llx local_wp 0x%llx\n",
			 mhi_event->er_index,
			 (unsigned long long)(mhi_event->ring.iommu_base +
				((u8 *)mhi_event->ring.rp - (u8 *)mhi_event->ring.base)),
			 (unsigned long long)(mhi_event->ring.iommu_base +
				((u8 *)mhi_event->ring.wp - (u8 *)mhi_event->ring.base)));

	/* process all pending events */
	spin_lock_bh(&mhi_event->lock);
	mhi_event->process_event(mhi_cntrl, mhi_event, U32_MAX);
	spin_unlock_bh(&mhi_event->lock);
}

void mhi_ctrl_ev_task(unsigned long data)
{
	struct mhi_event *mhi_event = (struct mhi_event *)data;
	struct mhi_controller *mhi_cntrl = mhi_event->mhi_cntrl;
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	enum mhi_state state;
	enum mhi_pm_state pm_state = 0;
	int ret;

	if (mhi_sahara_event_ring_relevant(mhi_cntrl, mhi_event->er_index))
		dev_info(dev,
			 "SAHARA ctrl event task er%u: local_rp 0x%llx local_wp 0x%llx\n",
			 mhi_event->er_index,
			 (unsigned long long)(mhi_event->ring.iommu_base +
				((u8 *)mhi_event->ring.rp - (u8 *)mhi_event->ring.base)),
			 (unsigned long long)(mhi_event->ring.iommu_base +
				((u8 *)mhi_event->ring.wp - (u8 *)mhi_event->ring.base)));

	/*
	 * We can check PM state w/o a lock here because there is no way
	 * PM state can change from reg access valid to no access while this
	 * thread being executed.
	 */
	if (!MHI_REG_ACCESS_VALID(mhi_cntrl->pm_state)) {
		/*
		 * We may have a pending event but not allowed to
		 * process it since we are probably in a suspended state,
		 * so trigger a resume.
		 */
		mhi_trigger_resume(mhi_cntrl);

		return;
	}

	/* Process ctrl events */
	ret = mhi_event->process_event(mhi_cntrl, mhi_event, U32_MAX);

	/*
	 * We received an IRQ but no events to process, maybe device went to
	 * SYS_ERR state? Check the state to confirm.
	 */
	if (!ret) {
		write_lock_irq(&mhi_cntrl->pm_lock);
		state = mhi_get_mhi_state(mhi_cntrl);
		if (state == MHI_STATE_SYS_ERR) {
			dev_dbg(dev, "System error detected\n");
			pm_state = mhi_tryset_pm_state(mhi_cntrl,
						       MHI_PM_SYS_ERR_DETECT);
		}
		write_unlock_irq(&mhi_cntrl->pm_lock);
		if (pm_state == MHI_PM_SYS_ERR_DETECT)
			mhi_pm_sys_err_handler(mhi_cntrl);
	}
}

static int mhi_sahara_process_event_ring(struct mhi_controller *mhi_cntrl,
						 struct mhi_event *mhi_event,
						 const char *reason)
{
	int ret;

	if (mhi_event->offload_ev || !mhi_sahara_event_ring_relevant(mhi_cntrl,
								       mhi_event->er_index))
		return 0;

	spin_lock_bh(&mhi_event->lock);
	ret = mhi_event->process_event(mhi_cntrl, mhi_event, U32_MAX);
	spin_unlock_bh(&mhi_event->lock);

	if (ret > 0)
		dev_info(&mhi_cntrl->mhi_dev->dev,
			 "SAHARA %s drained er%u count %d\n",
			 reason, mhi_event->er_index, ret);

	return ret;
}

static void __mhi_sahara_drain_events(struct mhi_controller *mhi_cntrl,
					      const char *reason)
{
	struct mhi_event *mhi_event;
	u32 i;

	if (!mhi_cntrl->mhi_event || !mhi_cntrl->mhi_ctxt)
		return;

	mhi_event = mhi_cntrl->mhi_event;
	for (i = 0; i < mhi_cntrl->total_ev_rings; i++, mhi_event++)
		mhi_sahara_process_event_ring(mhi_cntrl, mhi_event, reason);
}

void mhi_sahara_drain_events(struct mhi_device *mhi_dev, const char *reason)
{
	if (!mhi_dev || !mhi_dev->mhi_cntrl)
		return;

	__mhi_sahara_drain_events(mhi_dev->mhi_cntrl, reason);
}

static int mhi_sahara_wait_for_completion(struct mhi_controller *mhi_cntrl,
						  struct mhi_chan *mhi_chan,
						  const char *reason)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(mhi_cntrl->timeout_ms);
	unsigned long interval = msecs_to_jiffies(20) ?: 1;
	int ret;

	do {
		ret = wait_for_completion_timeout(&mhi_chan->completion, interval);
		if (ret || mhi_chan->ccs == MHI_EV_CC_SUCCESS)
			return ret ?: 1;

		__mhi_sahara_drain_events(mhi_cntrl, reason);
		if (mhi_chan->ccs == MHI_EV_CC_SUCCESS)
			return 1;
	} while (time_before(jiffies, timeout));

	return 0;
}

static bool mhi_is_ring_full(struct mhi_controller *mhi_cntrl,
			     struct mhi_ring *ring)
{
	void *tmp = ring->wp + ring->el_size;

	if (tmp >= (ring->base + ring->len))
		tmp = ring->base;

	return (tmp == ring->rp);
}

static int mhi_queue(struct mhi_device *mhi_dev, struct mhi_buf_info *buf_info,
		     enum dma_data_direction dir, enum mhi_flags mflags)
{
	struct mhi_controller *mhi_cntrl = mhi_dev->mhi_cntrl;
	struct mhi_chan *mhi_chan = (dir == DMA_TO_DEVICE) ? mhi_dev->ul_chan :
							     mhi_dev->dl_chan;
	struct mhi_ring *tre_ring = &mhi_chan->tre_ring;
	bool sahara_chan = mhi_is_sahara_chan(mhi_chan);
	bool queue_diag = sahara_chan && (mhi_sahara_queue_force_resume ||
					   mhi_sahara_queue_chan_lock_db);
	bool force_resume = false;
	bool chan_lock_db = false;
	bool db_valid = false;
	unsigned long chan_flags;
	unsigned long flags;
	u32 pm_state_before = 0;
	u32 pm_state_after = 0;
	u32 db_access = 0;
	int ret;

	if (unlikely(MHI_PM_IN_ERROR_STATE(mhi_cntrl->pm_state)))
		return -EIO;

	ret = mhi_is_ring_full(mhi_cntrl, tre_ring);
	if (unlikely(ret))
		return -EAGAIN;

	ret = mhi_gen_tre(mhi_cntrl, mhi_chan, buf_info, mflags);
	if (unlikely(ret))
		return ret;

	read_lock_irqsave(&mhi_cntrl->pm_lock, flags);

	pm_state_before = mhi_cntrl->pm_state;
	db_access = mhi_cntrl->db_access;

	if (sahara_chan && mhi_sahara_queue_force_resume) {
		mhi_trigger_resume(mhi_cntrl);
		force_resume = true;
	}

	/* Packet is queued, take a usage ref to exit M3 if necessary
	 * for host->device buffer, balanced put is done on buffer completion
	 * for device->host buffer, balanced put is after ringing the DB
	 */
	mhi_cntrl->runtime_get(mhi_cntrl);

	/* Assert dev_wake (to exit/prevent M1/M2)*/
	mhi_cntrl->wake_toggle(mhi_cntrl);

	if (mhi_chan->dir == DMA_TO_DEVICE)
		atomic_inc(&mhi_cntrl->pending_pkts);

	db_valid = MHI_DB_ACCESS_VALID(mhi_cntrl);
	if (likely(db_valid)) {
		if (sahara_chan && mhi_sahara_queue_chan_lock_db) {
			read_lock_irqsave(&mhi_chan->lock, chan_flags);
			mhi_ring_chan_db(mhi_cntrl, mhi_chan);
			read_unlock_irqrestore(&mhi_chan->lock, chan_flags);
			chan_lock_db = true;
		} else {
			mhi_ring_chan_db(mhi_cntrl, mhi_chan);
		}
	}

	if (dir == DMA_FROM_DEVICE)
		mhi_cntrl->runtime_put(mhi_cntrl);

	pm_state_after = mhi_cntrl->pm_state;
	read_unlock_irqrestore(&mhi_cntrl->pm_lock, flags);

	if (queue_diag)
		dev_info(&mhi_dev->dev,
			 "SAHARA queue dir=%d len=%zu flags=0x%x pm_before=0x%x pm_after=0x%x db_access=0x%x db_valid=%u force_resume=%u chan_lock_db=%u\n",
			 dir, buf_info->len, mflags, pm_state_before, pm_state_after,
			 db_access, db_valid, force_resume, chan_lock_db);

	return ret;
}

int mhi_queue_skb(struct mhi_device *mhi_dev, enum dma_data_direction dir,
		  struct sk_buff *skb, size_t len, enum mhi_flags mflags)
{
	struct mhi_buf_info buf_info = { };

	buf_info.v_addr = skb->data;
	buf_info.cb_buf = skb;
	buf_info.len = len;

	return mhi_queue(mhi_dev, &buf_info, dir, mflags);
}
EXPORT_SYMBOL_GPL(mhi_queue_skb);

int mhi_gen_tre(struct mhi_controller *mhi_cntrl, struct mhi_chan *mhi_chan,
			struct mhi_buf_info *info, enum mhi_flags flags)
{
	struct mhi_ring *buf_ring, *tre_ring;
	struct mhi_ring_element *mhi_tre;
	struct mhi_buf_info *buf_info;
	int eot, eob, chain, bei;
	int ret = 0;

	/* Protect accesses for reading and incrementing WP */
	write_lock_bh(&mhi_chan->lock);

	if (mhi_chan->ch_state != MHI_CH_STATE_ENABLED) {
		ret = -ENODEV;
		goto out;
	}

	buf_ring = &mhi_chan->buf_ring;
	tre_ring = &mhi_chan->tre_ring;

	buf_info = buf_ring->wp;
	WARN_ON(buf_info->used);
	buf_info->pre_mapped = info->pre_mapped;
	if (info->pre_mapped)
		buf_info->p_addr = info->p_addr;
	else
		buf_info->v_addr = info->v_addr;
	buf_info->cb_buf = info->cb_buf;
	buf_info->wp = tre_ring->wp;
	buf_info->dir = mhi_chan->dir;
	buf_info->len = info->len;

	if (!info->pre_mapped) {
		ret = mhi_cntrl->map_single(mhi_cntrl, buf_info);
		if (ret)
			goto out;
	}

	eob = !!(flags & MHI_EOB);
	eot = !!(flags & MHI_EOT);
	chain = !!(flags & MHI_CHAIN);
	bei = !!(mhi_chan->intmod);

	mhi_tre = tre_ring->wp;
	mhi_tre->ptr = MHI_TRE_DATA_PTR(buf_info->p_addr);
	mhi_tre->dword[0] = MHI_TRE_DATA_DWORD0(info->len);
	mhi_tre->dword[1] = MHI_TRE_DATA_DWORD1(bei, eot, eob, chain);

	trace_mhi_gen_tre(mhi_cntrl, mhi_chan, mhi_tre);
	/* increment WP */
	mhi_add_ring_element(mhi_cntrl, tre_ring);
	mhi_add_ring_element(mhi_cntrl, buf_ring);

out:
	write_unlock_bh(&mhi_chan->lock);

	return ret;
}

int mhi_queue_buf(struct mhi_device *mhi_dev, enum dma_data_direction dir,
		  void *buf, size_t len, enum mhi_flags mflags)
{
	struct mhi_buf_info buf_info = { };

	buf_info.v_addr = buf;
	buf_info.cb_buf = buf;
	buf_info.len = len;

	return mhi_queue(mhi_dev, &buf_info, dir, mflags);
}
EXPORT_SYMBOL_GPL(mhi_queue_buf);

bool mhi_queue_is_full(struct mhi_device *mhi_dev, enum dma_data_direction dir)
{
	struct mhi_controller *mhi_cntrl = mhi_dev->mhi_cntrl;
	struct mhi_chan *mhi_chan = (dir == DMA_TO_DEVICE) ?
					mhi_dev->ul_chan : mhi_dev->dl_chan;
	struct mhi_ring *tre_ring = &mhi_chan->tre_ring;

	return mhi_is_ring_full(mhi_cntrl, tre_ring);
}
EXPORT_SYMBOL_GPL(mhi_queue_is_full);

int mhi_send_cmd(struct mhi_controller *mhi_cntrl,
		 struct mhi_chan *mhi_chan,
		 enum mhi_cmd_type cmd)
{
	struct mhi_ring_element *cmd_tre = NULL;
	struct mhi_cmd *mhi_cmd = &mhi_cntrl->mhi_cmd[PRIMARY_CMD_RING];
	struct mhi_ring *ring = &mhi_cmd->ring;
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	bool db_access = false, log_sahara;
	dma_addr_t cmd_dma = 0, db = 0, ctxt_wp = 0;
	u32 chcfg = 0, erindex = 0;
	int chan = 0;

	if (mhi_chan)
		chan = mhi_chan->chan;
	log_sahara = mhi_is_sbl_boot_chan(mhi_chan);

	spin_lock_bh(&mhi_cmd->lock);
	if (!get_nr_avail_ring_elements(mhi_cntrl, ring)) {
		spin_unlock_bh(&mhi_cmd->lock);
		if (log_sahara)
			dev_err(dev, "SAHARA cmd %u chan %d no command ring space\n",
				cmd, chan);
		return -ENOMEM;
	}

	/* prepare the cmd tre */
	cmd_tre = ring->wp;
	cmd_dma = ring->iommu_base +
		(cmd_tre - (struct mhi_ring_element *)ring->base) * sizeof(*cmd_tre);
	switch (cmd) {
	case MHI_CMD_RESET_CHAN:
		cmd_tre->ptr = MHI_TRE_CMD_RESET_PTR;
		cmd_tre->dword[0] = MHI_TRE_CMD_RESET_DWORD0;
		cmd_tre->dword[1] = MHI_TRE_CMD_RESET_DWORD1(chan);
		break;
	case MHI_CMD_STOP_CHAN:
		cmd_tre->ptr = MHI_TRE_CMD_STOP_PTR;
		cmd_tre->dword[0] = MHI_TRE_CMD_STOP_DWORD0;
		cmd_tre->dword[1] = MHI_TRE_CMD_STOP_DWORD1(chan);
		break;
	case MHI_CMD_START_CHAN:
		cmd_tre->ptr = MHI_TRE_CMD_START_PTR;
		cmd_tre->dword[0] = MHI_TRE_CMD_START_DWORD0;
		cmd_tre->dword[1] = MHI_TRE_CMD_START_DWORD1(chan);
		break;
	default:
		dev_err(dev, "Command not supported\n");
		break;
	}

	/* queue to hardware */
	mhi_add_ring_element(mhi_cntrl, ring);
	db = ring->iommu_base + (ring->wp - ring->base);
	if (log_sahara && mhi_cntrl->mhi_ctxt && chan < mhi_cntrl->max_chan) {
		struct mhi_chan_ctxt *chan_ctxt;

		chan_ctxt = &mhi_cntrl->mhi_ctxt->chan_ctxt[chan];
		chcfg = le32_to_cpu(chan_ctxt->chcfg);
		erindex = le32_to_cpu(chan_ctxt->erindex);
	}
	read_lock_bh(&mhi_cntrl->pm_lock);
	db_access = MHI_DB_ACCESS_VALID(mhi_cntrl);
	if (likely(db_access))
		mhi_ring_cmd_db(mhi_cntrl, mhi_cmd);
	if (ring->ctxt_wp)
		ctxt_wp = le64_to_cpu(*ring->ctxt_wp);
	read_unlock_bh(&mhi_cntrl->pm_lock);
	spin_unlock_bh(&mhi_cmd->lock);

	if (log_sahara)
		dev_info(dev,
			 "SAHARA cmd %u chan %d queued cmd_tre 0x%llx db 0x%llx ctxt_wp 0x%llx db_access %d chcfg 0x%x er %u pm %s ee %s\n",
			 cmd, chan, (unsigned long long)cmd_dma,
			 (unsigned long long)db, (unsigned long long)ctxt_wp,
			 db_access, chcfg, erindex,
			 to_mhi_pm_state_str(READ_ONCE(mhi_cntrl->pm_state)),
			 TO_MHI_EXEC_STR(READ_ONCE(mhi_cntrl->ee)));

	return 0;
}

static int mhi_update_channel_state(struct mhi_controller *mhi_cntrl,
				    struct mhi_chan *mhi_chan,
				    enum mhi_ch_state_type to_state)
{
	struct device *dev = &mhi_chan->mhi_dev->dev;
	enum mhi_cmd_type cmd = MHI_CMD_NOP;
	bool log_sahara = mhi_is_sbl_boot_chan(mhi_chan);
	bool sahara_running = false;
	bool sahara_drained_completion = false;
	int ret;

	trace_mhi_channel_command_start(mhi_cntrl, mhi_chan, to_state, TPS("Updating"));
	switch (to_state) {
	case MHI_CH_STATE_TYPE_RESET:
		write_lock_irq(&mhi_chan->lock);
		if (mhi_chan->ch_state != MHI_CH_STATE_STOP &&
		    mhi_chan->ch_state != MHI_CH_STATE_ENABLED &&
		    mhi_chan->ch_state != MHI_CH_STATE_SUSPENDED) {
			write_unlock_irq(&mhi_chan->lock);
			return -EINVAL;
		}
		mhi_chan->ch_state = MHI_CH_STATE_DISABLED;
		write_unlock_irq(&mhi_chan->lock);

		cmd = MHI_CMD_RESET_CHAN;
		break;
	case MHI_CH_STATE_TYPE_STOP:
		if (mhi_chan->ch_state != MHI_CH_STATE_ENABLED)
			return -EINVAL;

		cmd = MHI_CMD_STOP_CHAN;
		break;
	case MHI_CH_STATE_TYPE_START:
		if (mhi_chan->ch_state != MHI_CH_STATE_STOP &&
		    mhi_chan->ch_state != MHI_CH_STATE_DISABLED)
			return -EINVAL;

		cmd = MHI_CMD_START_CHAN;
		break;
	default:
		dev_err(dev, "%d: Channel state update to %s not allowed\n",
			mhi_chan->chan, TO_CH_STATE_TYPE_STR(to_state));
		return -EINVAL;
	}

	/* bring host and device out of suspended states */
	ret = mhi_device_get_sync(mhi_cntrl->mhi_dev);
	if (ret)
		return ret;
	mhi_cntrl->runtime_get(mhi_cntrl);

	if (log_sahara && mhi_cntrl->mhi_ctxt) {
		struct mhi_chan_ctxt *chan_ctxt;

		chan_ctxt = &mhi_cntrl->mhi_ctxt->chan_ctxt[mhi_chan->chan];
		dev_info(dev,
			 "SAHARA %s command start: chan %u host_ch_state %u chcfg 0x%x er %u rbase 0x%llx rlen 0x%llx rp 0x%llx wp 0x%llx\n",
			 TO_CH_STATE_TYPE_STR(to_state), mhi_chan->chan,
			 mhi_chan->ch_state, le32_to_cpu(chan_ctxt->chcfg),
			 le32_to_cpu(chan_ctxt->erindex),
			 (unsigned long long)le64_to_cpu(chan_ctxt->rbase),
			 (unsigned long long)le64_to_cpu(chan_ctxt->rlen),
			 (unsigned long long)le64_to_cpu(chan_ctxt->rp),
			 (unsigned long long)le64_to_cpu(chan_ctxt->wp));
	}

	reinit_completion(&mhi_chan->completion);
	write_lock_irq(&mhi_chan->lock);
	mhi_chan->ccs = MHI_EV_CC_INVALID;
	write_unlock_irq(&mhi_chan->lock);
	ret = mhi_send_cmd(mhi_cntrl, mhi_chan, cmd);
	if (ret) {
		dev_err(dev, "%d: Failed to send %s channel command\n",
			mhi_chan->chan, TO_CH_STATE_TYPE_STR(to_state));
		goto exit_channel_update;
	}

	if (log_sahara)
		ret = mhi_sahara_wait_for_completion(mhi_cntrl, mhi_chan,
						       TO_CH_STATE_TYPE_STR(to_state));
	else
		ret = wait_for_completion_timeout(&mhi_chan->completion,
				       msecs_to_jiffies(mhi_cntrl->timeout_ms));
	if (!ret || mhi_chan->ccs != MHI_EV_CC_SUCCESS) {
		if (log_sahara && mhi_cntrl->mhi_ctxt) {
			struct mhi_cmd_ctxt *cmd_ctxt;
			struct mhi_event_ctxt *er_ctxt = NULL;
			struct mhi_chan_ctxt *chan_ctxt;
			enum mhi_ee_type device_ee;
			enum mhi_state device_state;
			u64 er_rp = 0, er_wp = 0;
			u32 chcfg, ch_state;
			u32 er_index = mhi_chan->er_index;

			cmd_ctxt = &mhi_cntrl->mhi_ctxt->cmd_ctxt[PRIMARY_CMD_RING];
			chan_ctxt = &mhi_cntrl->mhi_ctxt->chan_ctxt[mhi_chan->chan];
			chcfg = le32_to_cpu(chan_ctxt->chcfg);
			ch_state = (chcfg & CHAN_CTX_CHSTATE_MASK) >>
				__ffs(CHAN_CTX_CHSTATE_MASK);
			if (er_index < mhi_cntrl->total_ev_rings) {
				er_ctxt = &mhi_cntrl->mhi_ctxt->er_ctxt[er_index];
				er_rp = le64_to_cpu(er_ctxt->rp);
				er_wp = le64_to_cpu(er_ctxt->wp);
			}
			device_ee = mhi_get_exec_env(mhi_cntrl);
			device_state = mhi_get_mhi_state(mhi_cntrl);
			sahara_running = to_state == MHI_CH_STATE_TYPE_START &&
				ch_state == MHI_CH_STATE_RUNNING &&
				device_ee == MHI_EE_SBL && device_state == MHI_STATE_M0;
			dev_err(dev,
				"SAHARA %s command timeout: chan %u wait_ret %d ccs 0x%x pm %s host_ee %s device_ee %s device_state %s cmd_rp 0x%llx cmd_wp 0x%llx er%u_rp 0x%llx er%u_wp 0x%llx chcfg 0x%x ch_rp 0x%llx ch_wp 0x%llx\n",
				TO_CH_STATE_TYPE_STR(to_state), mhi_chan->chan, ret,
				mhi_chan->ccs,
				to_mhi_pm_state_str(READ_ONCE(mhi_cntrl->pm_state)),
				TO_MHI_EXEC_STR(READ_ONCE(mhi_cntrl->ee)),
				TO_MHI_EXEC_STR(device_ee), mhi_state_str(device_state),
				(unsigned long long)le64_to_cpu(cmd_ctxt->rp),
				(unsigned long long)le64_to_cpu(cmd_ctxt->wp),
				er_index, (unsigned long long)er_rp,
				er_index, (unsigned long long)er_wp,
				chcfg,
				(unsigned long long)le64_to_cpu(chan_ctxt->rp),
				(unsigned long long)le64_to_cpu(chan_ctxt->wp));
			mhi_sahara_dump_event_rings(mhi_cntrl, mhi_chan,
						       TO_CH_STATE_TYPE_STR(to_state));
			__mhi_sahara_drain_events(mhi_cntrl, TO_CH_STATE_TYPE_STR(to_state));
			if (mhi_chan->ccs == MHI_EV_CC_SUCCESS) {
				sahara_drained_completion = true;
				dev_warn(dev,
					 "%d: Recovered %s channel command completion by draining event rings\n",
					 mhi_chan->chan, TO_CH_STATE_TYPE_STR(to_state));
			}
		}
		if (sahara_drained_completion) {
		} else if (sahara_running) {
			dev_warn(dev,
				 "%d: Missing %s channel command completion, but device channel is RUNNING\n",
				 mhi_chan->chan, TO_CH_STATE_TYPE_STR(to_state));
			write_lock_irq(&mhi_chan->lock);
			mhi_chan->ccs = MHI_EV_CC_SUCCESS;
			write_unlock_irq(&mhi_chan->lock);
		} else {
			dev_err(dev,
				"%d: Failed to receive %s channel command completion\n",
				mhi_chan->chan, TO_CH_STATE_TYPE_STR(to_state));
			ret = -EIO;
			goto exit_channel_update;
		}
	}

	ret = 0;

	if (to_state != MHI_CH_STATE_TYPE_RESET) {
		write_lock_irq(&mhi_chan->lock);
		mhi_chan->ch_state = (to_state == MHI_CH_STATE_TYPE_START) ?
				      MHI_CH_STATE_ENABLED : MHI_CH_STATE_STOP;
		write_unlock_irq(&mhi_chan->lock);
	}

	trace_mhi_channel_command_end(mhi_cntrl, mhi_chan, to_state, TPS("Updated"));
exit_channel_update:
	mhi_cntrl->runtime_put(mhi_cntrl);
	mhi_device_put(mhi_cntrl->mhi_dev);

	return ret;
}

static void mhi_unprepare_channel(struct mhi_controller *mhi_cntrl,
				  struct mhi_chan *mhi_chan)
{
	int ret;
	struct device *dev = &mhi_chan->mhi_dev->dev;

	mutex_lock(&mhi_chan->mutex);

	if (!(BIT(mhi_cntrl->ee) & mhi_chan->ee_mask)) {
		dev_dbg(dev, "Current EE: %s Required EE Mask: 0x%x\n",
			TO_MHI_EXEC_STR(mhi_cntrl->ee), mhi_chan->ee_mask);
		goto exit_unprepare_channel;
	}

	/* no more processing events for this channel */
	ret = mhi_update_channel_state(mhi_cntrl, mhi_chan,
				       MHI_CH_STATE_TYPE_RESET);
	if (ret)
		dev_err(dev, "%d: Failed to reset channel, still resetting\n",
			mhi_chan->chan);

exit_unprepare_channel:
	write_lock_irq(&mhi_chan->lock);
	mhi_chan->ch_state = MHI_CH_STATE_DISABLED;
	write_unlock_irq(&mhi_chan->lock);

	if (!mhi_chan->offload_ch) {
		mhi_reset_chan(mhi_cntrl, mhi_chan);
		mhi_deinit_chan_ctxt(mhi_cntrl, mhi_chan);
	}
	dev_dbg(dev, "%d: successfully reset\n", mhi_chan->chan);

	mutex_unlock(&mhi_chan->mutex);
}

static int mhi_prepare_channel(struct mhi_controller *mhi_cntrl,
			struct mhi_chan *mhi_chan, unsigned int flags)
{
	int ret = 0;
	struct device *dev = &mhi_chan->mhi_dev->dev;

	if (!(BIT(mhi_cntrl->ee) & mhi_chan->ee_mask)) {
		dev_err(dev, "Current EE: %s Required EE Mask: 0x%x\n",
			TO_MHI_EXEC_STR(mhi_cntrl->ee), mhi_chan->ee_mask);
		return -ENOTCONN;
	}

	mutex_lock(&mhi_chan->mutex);

	/* Check of client manages channel context for offload channels */
	if (!mhi_chan->offload_ch) {
		ret = mhi_init_chan_ctxt(mhi_cntrl, mhi_chan);
		if (ret)
			goto error_init_chan;
	}

	ret = mhi_update_channel_state(mhi_cntrl, mhi_chan,
				       MHI_CH_STATE_TYPE_START);
	if (ret)
		goto error_pm_state;

	mutex_unlock(&mhi_chan->mutex);

	return 0;

error_pm_state:
	if (!mhi_chan->offload_ch)
		mhi_deinit_chan_ctxt(mhi_cntrl, mhi_chan);

error_init_chan:
	mutex_unlock(&mhi_chan->mutex);

	return ret;
}

static void mhi_mark_stale_events(struct mhi_controller *mhi_cntrl,
				  struct mhi_event *mhi_event,
				  struct mhi_event_ctxt *er_ctxt,
				  int chan)

{
	struct mhi_ring_element *dev_rp, *local_rp;
	struct mhi_ring *ev_ring;
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	unsigned long flags;
	dma_addr_t ptr;

	dev_dbg(dev, "Marking all events for chan: %d as stale\n", chan);

	ev_ring = &mhi_event->ring;

	/* mark all stale events related to channel as STALE event */
	spin_lock_irqsave(&mhi_event->lock, flags);

	ptr = le64_to_cpu(er_ctxt->rp);
	if (!is_valid_ring_ptr(ev_ring, ptr)) {
		dev_err(&mhi_cntrl->mhi_dev->dev,
			"Event ring rp points outside of the event ring\n");
		dev_rp = ev_ring->rp;
	} else {
		dev_rp = mhi_to_virtual(ev_ring, ptr);
	}

	local_rp = ev_ring->rp;
	while (dev_rp != local_rp) {
		if (MHI_TRE_GET_EV_TYPE(local_rp) == MHI_PKT_TYPE_TX_EVENT &&
		    chan == MHI_TRE_GET_EV_CHID(local_rp))
			local_rp->dword[1] = MHI_TRE_EV_DWORD1(chan,
					MHI_PKT_TYPE_STALE_EVENT);
		local_rp++;
		if (local_rp == (ev_ring->base + ev_ring->len))
			local_rp = ev_ring->base;
	}

	dev_dbg(dev, "Finished marking events as stale events\n");
	spin_unlock_irqrestore(&mhi_event->lock, flags);
}

static void mhi_reset_data_chan(struct mhi_controller *mhi_cntrl,
				struct mhi_chan *mhi_chan)
{
	struct mhi_ring *buf_ring, *tre_ring;
	struct mhi_result result;

	/* Reset any pending buffers */
	buf_ring = &mhi_chan->buf_ring;
	tre_ring = &mhi_chan->tre_ring;
	result.transaction_status = -ENOTCONN;
	result.bytes_xferd = 0;
	while (tre_ring->rp != tre_ring->wp) {
		struct mhi_buf_info *buf_info = buf_ring->rp;

		if (mhi_chan->dir == DMA_TO_DEVICE) {
			atomic_dec(&mhi_cntrl->pending_pkts);
			/* Release the reference got from mhi_queue() */
			mhi_cntrl->runtime_put(mhi_cntrl);
		}

		if (!buf_info->pre_mapped)
			mhi_cntrl->unmap_single(mhi_cntrl, buf_info);

		mhi_del_ring_element(mhi_cntrl, buf_ring);
		mhi_del_ring_element(mhi_cntrl, tre_ring);

		result.buf_addr = buf_info->cb_buf;
		mhi_chan->xfer_cb(mhi_chan->mhi_dev, &result);
	}
}

void mhi_reset_chan(struct mhi_controller *mhi_cntrl, struct mhi_chan *mhi_chan)
{
	struct mhi_event *mhi_event;
	struct mhi_event_ctxt *er_ctxt;
	int chan = mhi_chan->chan;

	/* Nothing to reset, client doesn't queue buffers */
	if (mhi_chan->offload_ch)
		return;

	read_lock_bh(&mhi_cntrl->pm_lock);
	mhi_event = &mhi_cntrl->mhi_event[mhi_chan->er_index];
	er_ctxt = &mhi_cntrl->mhi_ctxt->er_ctxt[mhi_chan->er_index];

	mhi_mark_stale_events(mhi_cntrl, mhi_event, er_ctxt, chan);

	mhi_reset_data_chan(mhi_cntrl, mhi_chan);

	read_unlock_bh(&mhi_cntrl->pm_lock);
}

static int __mhi_prepare_for_transfer(struct mhi_device *mhi_dev, unsigned int flags)
{
	int ret, dir;
	struct mhi_controller *mhi_cntrl = mhi_dev->mhi_cntrl;
	struct mhi_chan *mhi_chan;

	for (dir = 0; dir < 2; dir++) {
		mhi_chan = dir ? mhi_dev->dl_chan : mhi_dev->ul_chan;
		if (!mhi_chan)
			continue;

		ret = mhi_prepare_channel(mhi_cntrl, mhi_chan, flags);
		if (ret)
			goto error_open_chan;
	}

	return 0;

error_open_chan:
	for (--dir; dir >= 0; dir--) {
		mhi_chan = dir ? mhi_dev->dl_chan : mhi_dev->ul_chan;
		if (!mhi_chan)
			continue;

		mhi_unprepare_channel(mhi_cntrl, mhi_chan);
	}

	return ret;
}

int mhi_prepare_for_transfer(struct mhi_device *mhi_dev)
{
	return __mhi_prepare_for_transfer(mhi_dev, 0);
}
EXPORT_SYMBOL_GPL(mhi_prepare_for_transfer);

void mhi_unprepare_from_transfer(struct mhi_device *mhi_dev)
{
	struct mhi_controller *mhi_cntrl = mhi_dev->mhi_cntrl;
	struct mhi_chan *mhi_chan;
	int dir;

	for (dir = 0; dir < 2; dir++) {
		mhi_chan = dir ? mhi_dev->ul_chan : mhi_dev->dl_chan;
		if (!mhi_chan)
			continue;

		mhi_unprepare_channel(mhi_cntrl, mhi_chan);
	}
}
EXPORT_SYMBOL_GPL(mhi_unprepare_from_transfer);

int mhi_get_channel_doorbell_offset(struct mhi_controller *mhi_cntrl, u32 *chdb_offset)
{
	struct device *dev = &mhi_cntrl->mhi_dev->dev;
	void __iomem *base = mhi_cntrl->regs;
	int ret;

	ret = mhi_read_reg(mhi_cntrl, base, CHDBOFF, chdb_offset);
	if (ret) {
		dev_err(dev, "Unable to read CHDBOFF register\n");
		return -EIO;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(mhi_get_channel_doorbell_offset);
