// SPDX-License-Identifier: GPL-2.0-only

#include <linux/dma-direction.h>
#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/jiffies.h>
#include <linux/list.h>
#include <linux/mhi.h>
#include <linux/miscdevice.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/refcount.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include "internal.h"

#define MHI_SAHARA_MAX_BUFS	127
#define MHI_SAHARA_DEFAULT_MTU	0x8000
#define MHI_BL_DEFAULT_MTU	0x1000
#define MHI_SAHARA_DRAIN_INTERVAL_MS	20
#define MHI_SAHARA_START_DELAY_MS	2000
#define MHI_SAHARA_CMD_HELLO	1
#define MHI_SAHARA_CMD_HELLO_RESP	2
#define MHI_SAHARA_CMD_READ_DATA	3
#define MHI_SAHARA_HELLO_LEN	48
#define MHI_SAHARA_READ_DATA_LEN	20
#define MHI_SAHARA_HELLO_WORDS	12
#define MHI_SAHARA_STATUS_SUCCESS	0
#define MHI_SAHARA_MODE_IMAGE_TX	0

static bool mhi_sahara_auto_hello_resp;
module_param_named(auto_hello_resp, mhi_sahara_auto_hello_resp, bool, 0644);
MODULE_PARM_DESC(auto_hello_resp, "Automatically queue Sahara HELLO_RESP");

static unsigned int mhi_sahara_auto_hello_mode = MHI_SAHARA_MODE_IMAGE_TX;
module_param_named(auto_hello_mode, mhi_sahara_auto_hello_mode, uint, 0644);
MODULE_PARM_DESC(auto_hello_mode, "Sahara mode for automatic HELLO_RESP");

static bool mhi_sahara_keep_prepared_on_release;
module_param_named(keep_prepared_on_release, mhi_sahara_keep_prepared_on_release, bool, 0644);
MODULE_PARM_DESC(keep_prepared_on_release, "Keep Sahara transfer rings prepared when userspace closes the diagnostic device");

static bool mhi_sahara_ring_ul_db_after_ul;
module_param_named(ring_ul_db_after_ul, mhi_sahara_ring_ul_db_after_ul, bool, 0644);
MODULE_PARM_DESC(ring_ul_db_after_ul, "Ring the SAHARA UL channel doorbell after UL completion");

static bool mhi_sahara_ring_dl_db_after_ul;
module_param_named(ring_dl_db_after_ul, mhi_sahara_ring_dl_db_after_ul, bool, 0644);
MODULE_PARM_DESC(ring_dl_db_after_ul, "Ring the SAHARA DL channel doorbell after UL completion");

static bool mhi_sahara_restart_after_ul_completion;
module_param_named(restart_after_ul_completion,
		   mhi_sahara_restart_after_ul_completion, bool, 0644);
MODULE_PARM_DESC(restart_after_ul_completion,
		 "Restart the SAHARA channel after each successful UL completion");

static unsigned int mhi_sahara_restart_after_ul_delay_ms;
module_param_named(restart_after_ul_delay_ms,
		   mhi_sahara_restart_after_ul_delay_ms, uint, 0644);
MODULE_PARM_DESC(restart_after_ul_delay_ms,
		 "Delay before restarting the SAHARA channel after UL completion");

static unsigned int mhi_sahara_restart_track_read_data_len;
module_param_named(restart_track_read_data_len,
		   mhi_sahara_restart_track_read_data_len, uint, 0644);
MODULE_PARM_DESC(restart_track_read_data_len,
		 "Track READ_DATA payload bytes and defer restart until completion for payloads at least this large");

static unsigned int mhi_sahara_restart_suppress_read_data_len;
module_param_named(restart_suppress_read_data_len,
		   mhi_sahara_restart_suppress_read_data_len, uint, 0644);
MODULE_PARM_DESC(restart_suppress_read_data_len,
		 "Suppress SAHARA channel restart after READ_DATA payloads at least this large");

static bool mhi_sahara_restart_resync_db_val;
module_param_named(restart_resync_db_val,
		   mhi_sahara_restart_resync_db_val, bool, 0644);
MODULE_PARM_DESC(restart_resync_db_val,
		 "Resync cached SAHARA channel doorbell values after restart");

static bool mhi_sahara_restart_ring_ul_db;
module_param_named(restart_ring_ul_db,
		   mhi_sahara_restart_ring_ul_db, bool, 0644);
MODULE_PARM_DESC(restart_ring_ul_db,
		 "Ring the SAHARA UL channel doorbell after restart");

static bool mhi_sahara_bl_auto_start;
module_param_named(bl_auto_start, mhi_sahara_bl_auto_start, bool, 0644);
MODULE_PARM_DESC(bl_auto_start, "Automatically start the read-only BL diagnostic channel");

struct mhi_sahara_buf {
	struct list_head node;
	struct list_head queued_node;
	void *data;
	size_t len;
	bool queued;
};

struct mhi_sahara_tx_chunk {
	struct list_head node;
	void *data;
	size_t len;
};

struct mhi_sahara_dev {
	struct mhi_device *mhi_dev;
	struct miscdevice miscdev;
	refcount_t refs;
	struct mutex lock;
	spinlock_t rx_lock;
	struct delayed_work drain_work;
	struct delayed_work start_work;
	struct delayed_work restart_work;
	spinlock_t restart_lock;
	wait_queue_head_t read_wq;
	wait_queue_head_t write_wq;
	struct list_head pending_rx;
	struct list_head queued_rx;
	struct mhi_sahara_buf *cur_rx;
	size_t cur_rx_offset;
	size_t mtu;
	bool present;
	bool opened;
	bool prepared;
	bool allow_write;
	bool auto_start;
	bool delayed_auto_start;
	bool keep_rx_without_open;
	bool auto_hello_resp;
	bool auto_hello_sent;
	bool restart_suppress_active;
	u32 restart_suppress_image;
	bool restart_suppress_after_read;
	size_t restart_suppress_remaining;
};

static void mhi_sahara_put(struct mhi_sahara_dev *sdev)
{
	if (!refcount_dec_and_test(&sdev->refs))
		return;

	kfree(sdev->miscdev.name);
	kfree(sdev);
}

static void mhi_sahara_schedule_drain(struct mhi_sahara_dev *sdev)
{
	if (!READ_ONCE(sdev->present) || !READ_ONCE(sdev->prepared))
		return;

	schedule_delayed_work(&sdev->drain_work,
				      msecs_to_jiffies(MHI_SAHARA_DRAIN_INTERVAL_MS));
}

static void mhi_sahara_drain_work(struct work_struct *work)
{
	struct mhi_sahara_dev *sdev = container_of(to_delayed_work(work),
							struct mhi_sahara_dev, drain_work);

	if (!READ_ONCE(sdev->present) || !READ_ONCE(sdev->prepared))
		return;

	mhi_sahara_drain_events(sdev->mhi_dev, "poll");
	mhi_sahara_schedule_drain(sdev);
}

static struct mhi_sahara_buf *mhi_sahara_rx_from_data(struct mhi_sahara_dev *sdev,
						      void *data)
{
	return (struct mhi_sahara_buf *)((u8 *)data + sdev->mtu);
}

static u64 mhi_sahara_ring_ptr(struct mhi_ring *ring, const void *ptr)
{
	if (!ring->base || !ptr)
		return 0;

	return ring->iommu_base + ((const u8 *)ptr - (const u8 *)ring->base);
}

static u64 mhi_sahara_context_wp(struct mhi_ring *ring)
{
	return ring->ctxt_wp ? le64_to_cpu(*ring->ctxt_wp) : 0;
}

static void mhi_sahara_log_chan(struct mhi_sahara_dev *sdev,
					struct mhi_chan *mhi_chan, const char *tag)
{
	struct mhi_controller *mhi_cntrl = sdev->mhi_dev->mhi_cntrl;
	struct mhi_chan_ctxt *chan_ctxt = NULL;
	struct mhi_ring *buf_ring;
	struct mhi_ring *tre_ring;
	unsigned long flags;
	u32 ch_state;
	u32 chcfg = 0;
	u32 chtype = 0;
	u32 ccs;
	u32 db_mode;
	u32 erindex = 0;
	u64 buf_rp;
	u64 buf_wp;
	u64 ctx_rbase = 0;
	u64 ctx_rlen = 0;
	u64 ctx_rp = 0;
	u64 ctx_wp = 0;
	u64 tre_rp;
	u64 tre_wp;
	u64 ctxt_wp;
	u64 db_val;

	if (!mhi_chan)
		return;

	if (mhi_cntrl->mhi_ctxt)
		chan_ctxt = &mhi_cntrl->mhi_ctxt->chan_ctxt[mhi_chan->chan];

	buf_ring = &mhi_chan->buf_ring;
	tre_ring = &mhi_chan->tre_ring;
	read_lock_irqsave(&mhi_chan->lock, flags);
	ch_state = mhi_chan->ch_state;
	ccs = mhi_chan->ccs;
	db_mode = mhi_chan->db_cfg.db_mode;
	buf_rp = mhi_sahara_ring_ptr(buf_ring, buf_ring->rp);
	buf_wp = mhi_sahara_ring_ptr(buf_ring, buf_ring->wp);
	tre_rp = mhi_sahara_ring_ptr(tre_ring, tre_ring->rp);
	tre_wp = mhi_sahara_ring_ptr(tre_ring, tre_ring->wp);
	ctxt_wp = mhi_sahara_context_wp(tre_ring);
	db_val = mhi_chan->db_cfg.db_val;
	if (chan_ctxt) {
		chcfg = le32_to_cpu(chan_ctxt->chcfg);
		chtype = le32_to_cpu(chan_ctxt->chtype);
		erindex = le32_to_cpu(chan_ctxt->erindex);
		ctx_rbase = le64_to_cpu(chan_ctxt->rbase);
		ctx_rlen = le64_to_cpu(chan_ctxt->rlen);
		ctx_rp = le64_to_cpu(chan_ctxt->rp);
		ctx_wp = le64_to_cpu(chan_ctxt->wp);
	}
	read_unlock_irqrestore(&mhi_chan->lock, flags);

	dev_info(&sdev->mhi_dev->dev,
		 "SAHARA %s chan%u dir=%u state=%u ccs=%u chcfg=0x%x chtype=0x%x erindex=%u ctx_rbase=0x%llx ctx_rlen=0x%llx ctx_rp=0x%llx ctx_wp=0x%llx tre_rp=0x%llx tre_wp=0x%llx buf_rp=0x%llx buf_wp=0x%llx ctxt_wp=0x%llx db_mode=%u db_val=0x%llx\n",
		 tag, mhi_chan->chan, mhi_chan->dir, ch_state, ccs,
		 chcfg, chtype, erindex, ctx_rbase, ctx_rlen, ctx_rp,
		 ctx_wp, tre_rp, tre_wp, buf_rp, buf_wp, ctxt_wp,
		 db_mode, db_val);
}

static void mhi_sahara_log_channels(struct mhi_sahara_dev *sdev, const char *tag)
{
	if (!sdev->allow_write)
		return;

	mhi_sahara_log_chan(sdev, sdev->mhi_dev->ul_chan, tag);
	mhi_sahara_log_chan(sdev, sdev->mhi_dev->dl_chan, tag);
}

static void mhi_sahara_log_state(struct mhi_sahara_dev *sdev, const char *tag)
{
	struct mhi_controller *mhi_cntrl = sdev->mhi_dev->mhi_cntrl;
	enum mhi_ee_type reg_ee = MHI_EE_MAX;
	enum mhi_state reg_state = MHI_STATE_MAX;

	if (mhi_cntrl->bhi)
		reg_ee = mhi_get_exec_env(mhi_cntrl);
	if (mhi_cntrl->regs)
		reg_state = mhi_get_mhi_state(mhi_cntrl);

	dev_info(&sdev->mhi_dev->dev,
		 "SAHARA %s: cached_ee=%s cached_state=%s reg_ee=%s reg_state=%s pm_state=0x%x\n",
		 tag, TO_MHI_EXEC_STR(mhi_cntrl->ee), mhi_state_str(mhi_cntrl->dev_state),
		 TO_MHI_EXEC_STR(reg_ee), mhi_state_str(reg_state), mhi_cntrl->pm_state);
	mhi_sahara_log_channels(sdev, tag);
}

static void mhi_sahara_ring_chan_db_now(struct mhi_sahara_dev *sdev,
					 struct mhi_chan *mhi_chan, const char *tag)
{
	struct mhi_controller *mhi_cntrl = sdev->mhi_dev->mhi_cntrl;
	unsigned long flags;
	bool db_valid;

	if (!mhi_chan)
		return;

	read_lock_irqsave(&mhi_cntrl->pm_lock, flags);
	db_valid = MHI_DB_ACCESS_VALID(mhi_cntrl);
	if (db_valid)
		mhi_ring_chan_db(mhi_cntrl, mhi_chan);
	read_unlock_irqrestore(&mhi_cntrl->pm_lock, flags);

	dev_info(&sdev->mhi_dev->dev, "SAHARA %s chan%u ring_db db_valid=%u\n",
		 tag, mhi_chan->chan, db_valid);
	mhi_sahara_log_chan(sdev, mhi_chan, tag);
}

static void mhi_sahara_resync_chan_db_val(struct mhi_sahara_dev *sdev,
						   struct mhi_chan *mhi_chan,
						   const char *tag)
{
	struct mhi_ring *ring;
	unsigned long flags;
	dma_addr_t db = 0;

	if (!mhi_chan)
		return;

	ring = &mhi_chan->tre_ring;
	write_lock_irqsave(&mhi_chan->lock, flags);
	if (ring->ctxt_wp)
		db = le64_to_cpu(*ring->ctxt_wp);
	else if (ring->base)
		db = ring->iommu_base + (ring->wp - ring->base);
	mhi_chan->db_cfg.db_val = db;
	write_unlock_irqrestore(&mhi_chan->lock, flags);

	dev_info(&sdev->mhi_dev->dev, "SAHARA %s chan%u resync_db_val=0x%llx\n",
		 tag, mhi_chan->chan, (u64)db);
	mhi_sahara_log_chan(sdev, mhi_chan, tag);
}

static void mhi_sahara_dump_words(struct mhi_sahara_dev *sdev, const char *tag,
					  const __le32 *words, size_t len)
{
	u32 val[MHI_SAHARA_HELLO_WORDS];
	int i;

	for (i = 0; i < MHI_SAHARA_HELLO_WORDS; i++)
		val[i] = le32_to_cpu(words[i]);

	dev_info(&sdev->mhi_dev->dev,
		 "SAHARA %s len %zu words: %u %u %u %u %u %u %u %u %u %u %u %u\n",
		 tag, len, val[0], val[1], val[2], val[3], val[4], val[5],
		 val[6], val[7], val[8], val[9], val[10], val[11]);
	if (!strcmp(tag, "HELLO_RESP"))
		dev_info(&sdev->mhi_dev->dev,
			 "SAHARA %s decoded: cmd=%u length=%u version=%u compatible=%u status=%u mode=%u\n",
			 tag, val[0], val[1], val[2], val[3], val[4], val[5]);
	else
		dev_info(&sdev->mhi_dev->dev,
			 "SAHARA %s decoded: cmd=%u length=%u version=%u compatible=%u max_cmd_len=%u mode=%u\n",
			 tag, val[0], val[1], val[2], val[3], val[4], val[5]);
	mhi_sahara_log_state(sdev, tag);
}

static void mhi_sahara_log_rx_packet(struct mhi_sahara_dev *sdev,
					     const void *buf, size_t len)
{
	const __le32 *words = buf;
	u32 val[MHI_SAHARA_HELLO_WORDS] = {};
	u32 cmd = len >= sizeof(*words) ? le32_to_cpu(words[0]) : 0;
	u32 pkt_len = len >= 2 * sizeof(*words) ? le32_to_cpu(words[1]) : 0;
	size_t count = min_t(size_t, len / sizeof(*words), MHI_SAHARA_HELLO_WORDS);
	size_t i;

	if (!sdev->allow_write)
		return;

	dev_info(&sdev->mhi_dev->dev, "SAHARA RX packet len %zu cmd %u pkt_len %u\n",
		 len, cmd, pkt_len);

	if (len >= MHI_SAHARA_HELLO_LEN) {
		mhi_sahara_dump_words(sdev,
			cmd == MHI_SAHARA_CMD_HELLO ? "HELLO" : "RX", words, len);
		return;
	}

	for (i = 0; i < count; i++)
		val[i] = le32_to_cpu(words[i]);

	dev_info(&sdev->mhi_dev->dev,
		 "SAHARA RX short len %zu word_count %zu words: %u %u %u %u %u %u %u %u %u %u %u %u\n",
		 len, count, val[0], val[1], val[2], val[3], val[4], val[5],
		 val[6], val[7], val[8], val[9], val[10], val[11]);
	mhi_sahara_log_state(sdev, "RX short packet");
}

static bool mhi_sahara_rx_packet_valid(const void *buf, size_t len)
{
	const __le32 *words = buf;
	u32 cmd, pkt_len;

	if (len < 2 * sizeof(*words))
		return false;

	cmd = le32_to_cpu(words[0]);
	pkt_len = le32_to_cpu(words[1]);

	return cmd && pkt_len >= 2 * sizeof(*words) && pkt_len <= len;
}

static void mhi_sahara_note_rx_for_restart(struct mhi_sahara_dev *sdev,
						   const void *buf, size_t len)
{
	const __le32 *words = buf;
	unsigned long flags;
	u32 cmd, image, length;
	bool track_read;
	bool suppress_after_read;

	if (!sdev->allow_write || len < 2 * sizeof(*words))
		return;

	cmd = le32_to_cpu(words[0]);
	if (cmd != MHI_SAHARA_CMD_READ_DATA) {
		spin_lock_irqsave(&sdev->restart_lock, flags);
		sdev->restart_suppress_active = false;
		sdev->restart_suppress_after_read = false;
		sdev->restart_suppress_remaining = 0;
		spin_unlock_irqrestore(&sdev->restart_lock, flags);
		return;
	}

	if (len < MHI_SAHARA_READ_DATA_LEN)
		return;

	image = le32_to_cpu(words[2]);
	length = le32_to_cpu(words[4]);
	track_read = mhi_sahara_restart_track_read_data_len &&
		length >= mhi_sahara_restart_track_read_data_len;
	suppress_after_read = mhi_sahara_restart_suppress_read_data_len &&
		length >= mhi_sahara_restart_suppress_read_data_len;

	spin_lock_irqsave(&sdev->restart_lock, flags);
	if (track_read || suppress_after_read) {
		sdev->restart_suppress_active = true;
		sdev->restart_suppress_image = image;
		sdev->restart_suppress_after_read = suppress_after_read;
		sdev->restart_suppress_remaining = length;
	} else {
		sdev->restart_suppress_active = false;
		sdev->restart_suppress_after_read = false;
		sdev->restart_suppress_remaining = 0;
	}
	spin_unlock_irqrestore(&sdev->restart_lock, flags);

	if (track_read || suppress_after_read)
		dev_info(&sdev->mhi_dev->dev,
			 "SAHARA tracking restart for READ_DATA image=%u length=%u track_threshold=%u suppress_threshold=%u suppress_after=%u\n",
			 image, length, mhi_sahara_restart_track_read_data_len,
			 mhi_sahara_restart_suppress_read_data_len, suppress_after_read);
}

static bool mhi_sahara_restart_deferred_for_ul(struct mhi_sahara_dev *sdev,
						       size_t bytes, u32 *image,
						       size_t *remaining,
						       bool *suppress_after_read)
{
	unsigned long flags;
	bool deferred = false;

	*suppress_after_read = false;
	spin_lock_irqsave(&sdev->restart_lock, flags);
	if (sdev->restart_suppress_active) {
		*image = sdev->restart_suppress_image;
		if (bytes >= sdev->restart_suppress_remaining)
			sdev->restart_suppress_remaining = 0;
		else
			sdev->restart_suppress_remaining -= bytes;
		*remaining = sdev->restart_suppress_remaining;
		*suppress_after_read = !sdev->restart_suppress_remaining &&
			sdev->restart_suppress_after_read;
		deferred = sdev->restart_suppress_remaining || *suppress_after_read;
		if (!sdev->restart_suppress_remaining) {
			sdev->restart_suppress_active = false;
			sdev->restart_suppress_after_read = false;
		}
	}
	spin_unlock_irqrestore(&sdev->restart_lock, flags);

	return deferred;
}

static int mhi_sahara_queue_rx_buf(struct mhi_sahara_dev *sdev,
					   struct mhi_sahara_buf *rx)
{
	int ret;

	INIT_LIST_HEAD(&rx->node);
	INIT_LIST_HEAD(&rx->queued_node);
	rx->len = 0;
	rx->queued = true;
	memset(rx->data, 0, sdev->mtu);

	spin_lock_bh(&sdev->rx_lock);
	list_add_tail(&rx->queued_node, &sdev->queued_rx);
	spin_unlock_bh(&sdev->rx_lock);

	ret = mhi_queue_buf(sdev->mhi_dev, DMA_FROM_DEVICE, rx->data,
			    sdev->mtu, MHI_EOT);
	if (ret) {
		spin_lock_bh(&sdev->rx_lock);
		if (rx->queued) {
			list_del_init(&rx->queued_node);
			rx->queued = false;
		}
		spin_unlock_bh(&sdev->rx_lock);
		kfree(rx->data);
	}

	return ret;
}

static int mhi_sahara_alloc_queue_rx(struct mhi_sahara_dev *sdev)
{
	struct mhi_sahara_buf *rx;
	void *data;

	data = kzalloc(sdev->mtu + sizeof(*rx), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	rx = mhi_sahara_rx_from_data(sdev, data);
	rx->data = data;

	return mhi_sahara_queue_rx_buf(sdev, rx);
}

static void mhi_sahara_maybe_queue_hello_resp(struct mhi_sahara_dev *sdev,
						      const void *buf, size_t len)
{
	const __le32 *hello = buf;
	__le32 *resp;
	u32 version, compatible, hello_mode, resp_mode;
	int ret;

	if (!sdev->auto_hello_resp || sdev->auto_hello_sent || len < MHI_SAHARA_HELLO_LEN)
		return;

	if (le32_to_cpu(hello[0]) != MHI_SAHARA_CMD_HELLO ||
	    le32_to_cpu(hello[1]) != MHI_SAHARA_HELLO_LEN)
		return;

	version = le32_to_cpu(hello[2]);
	compatible = le32_to_cpu(hello[3]);
	hello_mode = le32_to_cpu(hello[5]);
	resp_mode = mhi_sahara_auto_hello_mode;

	resp = kzalloc(MHI_SAHARA_HELLO_LEN, GFP_ATOMIC);
	if (!resp)
		return;

	resp[0] = cpu_to_le32(MHI_SAHARA_CMD_HELLO_RESP);
	resp[1] = cpu_to_le32(MHI_SAHARA_HELLO_LEN);
	resp[2] = cpu_to_le32(version);
	resp[3] = cpu_to_le32(compatible);
	resp[4] = cpu_to_le32(MHI_SAHARA_STATUS_SUCCESS);
	resp[5] = cpu_to_le32(resp_mode);

	mhi_sahara_dump_words(sdev, "HELLO_RESP", resp, MHI_SAHARA_HELLO_LEN);

	ret = mhi_queue_buf(sdev->mhi_dev, DMA_TO_DEVICE, resp,
			    MHI_SAHARA_HELLO_LEN, MHI_EOT);
	if (ret) {
		kfree(resp);
		dev_warn(&sdev->mhi_dev->dev, "failed to auto queue Sahara HELLO_RESP: %d\n", ret);
		return;
	}

	sdev->auto_hello_sent = true;
	dev_info(&sdev->mhi_dev->dev,
		 "auto queued Sahara HELLO_RESP version %u compatible %u hello_mode %u resp_mode %u\n",
		 version, compatible, hello_mode, resp_mode);
}

static int mhi_sahara_refill_rx(struct mhi_sahara_dev *sdev)
{
	int count, i, ret;

	count = mhi_get_free_desc_count(sdev->mhi_dev, DMA_FROM_DEVICE);
	if (count <= 0)
		return count ?: -ENOSPC;

	count = min(count, MHI_SAHARA_MAX_BUFS);
	for (i = 0; i < count; i++) {
		ret = mhi_sahara_alloc_queue_rx(sdev);
		if (ret)
			return i ? 0 : ret;
	}

	return 0;
}

static void mhi_sahara_free_rx_list(struct list_head *list)
{
	struct mhi_sahara_buf *rx, *tmp;

	list_for_each_entry_safe(rx, tmp, list, node) {
		list_del(&rx->node);
		kfree(rx->data);
	}
}

static void mhi_sahara_free_tx_chunks(struct list_head *list)
{
	struct mhi_sahara_tx_chunk *chunk, *tmp;

	list_for_each_entry_safe(chunk, tmp, list, node) {
		list_del(&chunk->node);
		kfree(chunk->data);
		kfree(chunk);
	}
}

static void mhi_sahara_purge_rx(struct mhi_sahara_dev *sdev)
{
	struct mhi_sahara_buf *rx, *tmp;
	LIST_HEAD(to_free);

	spin_lock_bh(&sdev->rx_lock);
	if (sdev->cur_rx) {
		list_add_tail(&sdev->cur_rx->node, &to_free);
		sdev->cur_rx = NULL;
		sdev->cur_rx_offset = 0;
	}
	list_splice_init(&sdev->pending_rx, &to_free);
	list_for_each_entry_safe(rx, tmp, &sdev->queued_rx, queued_node) {
		list_del_init(&rx->queued_node);
		rx->queued = false;
		list_add_tail(&rx->node, &to_free);
	}
	spin_unlock_bh(&sdev->rx_lock);

	mhi_sahara_free_rx_list(&to_free);
}

static void mhi_sahara_stop_locked(struct mhi_sahara_dev *sdev)
{
	sdev->opened = false;
	cancel_delayed_work(&sdev->restart_work);
	if (sdev->allow_write)
		sdev->keep_rx_without_open = false;
	if (sdev->prepared) {
		cancel_delayed_work_sync(&sdev->drain_work);
		mhi_unprepare_from_transfer(sdev->mhi_dev);
		sdev->prepared = false;
	}
	mhi_sahara_purge_rx(sdev);
	wake_up_all(&sdev->read_wq);
	wake_up_all(&sdev->write_wq);
}

static void mhi_sahara_start_work(struct work_struct *work)
{
	struct mhi_sahara_dev *sdev = container_of(to_delayed_work(work),
							struct mhi_sahara_dev, start_work);
	int ret;

	mutex_lock(&sdev->lock);
	if (!sdev->present || sdev->prepared)
		goto out_unlock;

	sdev->auto_hello_sent = false;
	ret = mhi_prepare_for_transfer(sdev->mhi_dev);
	if (ret)
		goto warn;

	sdev->prepared = true;
	ret = mhi_sahara_refill_rx(sdev);
	if (ret) {
		mhi_sahara_stop_locked(sdev);
		goto warn;
	}

	mhi_sahara_schedule_drain(sdev);
	dev_info(&sdev->mhi_dev->dev, "delayed auto-started /dev/%s\n",
		 sdev->miscdev.name);
	goto out_unlock;

warn:
	dev_warn(&sdev->mhi_dev->dev, "failed to delayed auto-start /dev/%s: %d\n",
		 sdev->miscdev.name, ret);

out_unlock:
	mutex_unlock(&sdev->lock);
}

static void mhi_sahara_restart_work(struct work_struct *work)
{
	struct mhi_sahara_dev *sdev = container_of(to_delayed_work(work),
							struct mhi_sahara_dev, restart_work);
	int ret;

	mutex_lock(&sdev->lock);
	if (!sdev->present || !sdev->opened || !sdev->prepared)
		goto out_unlock;

	dev_info(&sdev->mhi_dev->dev, "SAHARA restarting channel after UL completion\n");
	mhi_sahara_log_state(sdev, "restart before");
	cancel_delayed_work_sync(&sdev->drain_work);
	mhi_unprepare_from_transfer(sdev->mhi_dev);
	sdev->prepared = false;
	mhi_sahara_purge_rx(sdev);
	sdev->auto_hello_sent = false;

	ret = mhi_prepare_for_transfer(sdev->mhi_dev);
	if (ret)
		goto warn;

	sdev->prepared = true;
	ret = mhi_sahara_refill_rx(sdev);
	if (ret) {
		mhi_unprepare_from_transfer(sdev->mhi_dev);
		sdev->prepared = false;
		mhi_sahara_purge_rx(sdev);
		goto warn;
	}

	if (mhi_sahara_restart_resync_db_val) {
		mhi_sahara_resync_chan_db_val(sdev, sdev->mhi_dev->ul_chan,
						   "restart resync UL");
		mhi_sahara_resync_chan_db_val(sdev, sdev->mhi_dev->dl_chan,
						   "restart resync DL");
	}
	if (mhi_sahara_restart_ring_ul_db)
		mhi_sahara_ring_chan_db_now(sdev, sdev->mhi_dev->ul_chan,
					       "restart UL re-ring");

	mhi_sahara_schedule_drain(sdev);
	mhi_sahara_log_state(sdev, "restart after");
	wake_up_all(&sdev->read_wq);
	wake_up_all(&sdev->write_wq);
	goto out_unlock;

warn:
	dev_warn(&sdev->mhi_dev->dev, "failed to restart SAHARA channel after UL completion: %d\n",
		 ret);
	wake_up_all(&sdev->read_wq);
	wake_up_all(&sdev->write_wq);

out_unlock:
	mutex_unlock(&sdev->lock);
}

static void mhi_sahara_close_locked(struct mhi_sahara_dev *sdev)
{
	if (!sdev->opened)
		return;

	if (sdev->auto_start || sdev->delayed_auto_start ||
	    (sdev->allow_write && mhi_sahara_keep_prepared_on_release)) {
		if (sdev->allow_write && mhi_sahara_keep_prepared_on_release) {
			sdev->keep_rx_without_open = true;
			dev_info(&sdev->mhi_dev->dev, "SAHARA keeping transfer prepared on close\n");
		}
		sdev->opened = false;
		wake_up_all(&sdev->read_wq);
		wake_up_all(&sdev->write_wq);
		return;
	}

	mhi_sahara_stop_locked(sdev);
}

static bool mhi_sahara_rx_available(struct mhi_sahara_dev *sdev)
{
	bool available;

	spin_lock_bh(&sdev->rx_lock);
	available = sdev->cur_rx || !list_empty(&sdev->pending_rx);
	spin_unlock_bh(&sdev->rx_lock);

	return available;
}

static int mhi_sahara_open(struct inode *inode, struct file *file)
{
	struct mhi_sahara_dev *sdev;
	int ret;

	sdev = container_of(file->private_data, struct mhi_sahara_dev, miscdev);

	ret = mutex_lock_interruptible(&sdev->lock);
	if (ret)
		return ret;

	if (!sdev->present) {
		ret = -ENODEV;
		goto out_unlock;
	}

	if (sdev->opened) {
		ret = -EBUSY;
		goto out_unlock;
	}

	refcount_inc(&sdev->refs);
	sdev->opened = true;

	if (!sdev->prepared) {
		sdev->auto_hello_sent = false;
		ret = mhi_prepare_for_transfer(sdev->mhi_dev);
		if (ret)
			goto err_put;
		sdev->prepared = true;

		ret = mhi_sahara_refill_rx(sdev);
		if (ret)
			goto err_close;

		mhi_sahara_schedule_drain(sdev);
	} else if (sdev->prepared) {
		ret = mhi_sahara_refill_rx(sdev);
		if (ret && ret != -ENOSPC)
			dev_warn(&sdev->mhi_dev->dev, "failed to refill prepared SAHARA RX buffers: %d\n", ret);
	}

	file->private_data = sdev;
	mutex_unlock(&sdev->lock);

	return nonseekable_open(inode, file);

err_close:
	mhi_sahara_close_locked(sdev);
err_put:
	sdev->opened = false;
	mutex_unlock(&sdev->lock);
	mhi_sahara_put(sdev);
	return ret;

out_unlock:
	mutex_unlock(&sdev->lock);
	return ret;
}

static int mhi_sahara_release(struct inode *inode, struct file *file)
{
	struct mhi_sahara_dev *sdev = file->private_data;

	mutex_lock(&sdev->lock);
	mhi_sahara_close_locked(sdev);
	mutex_unlock(&sdev->lock);
	mhi_sahara_put(sdev);

	return 0;
}

static ssize_t mhi_sahara_read(struct file *file, char __user *buf,
				       size_t count, loff_t *ppos)
{
	struct mhi_sahara_dev *sdev = file->private_data;
	struct mhi_sahara_buf *rx;
	size_t copied, available;
	int ret;

	if (!count)
		return 0;

	ret = mutex_lock_interruptible(&sdev->lock);
	if (ret)
		return ret;

	for (;;) {
		if (!sdev->present || !sdev->opened) {
			ret = -ENODEV;
			goto out_unlock;
		}

		mhi_sahara_drain_events(sdev->mhi_dev, "read");

		spin_lock_bh(&sdev->rx_lock);
		if (!sdev->cur_rx && !list_empty(&sdev->pending_rx)) {
			sdev->cur_rx = list_first_entry(&sdev->pending_rx,
							 struct mhi_sahara_buf, node);
			list_del_init(&sdev->cur_rx->node);
			sdev->cur_rx_offset = 0;
		}
		rx = sdev->cur_rx;
		spin_unlock_bh(&sdev->rx_lock);

		if (rx)
			break;

		if (file->f_flags & O_NONBLOCK) {
			ret = -EAGAIN;
			goto out_unlock;
		}

		mutex_unlock(&sdev->lock);
		ret = wait_event_interruptible_timeout(sdev->read_wq,
				!READ_ONCE(sdev->present) ||
				!READ_ONCE(sdev->opened) ||
				mhi_sahara_rx_available(sdev),
				msecs_to_jiffies(20));
		if (ret < 0)
			return ret;

		ret = mutex_lock_interruptible(&sdev->lock);
		if (ret)
			return ret;
	}

	available = rx->len - sdev->cur_rx_offset;
	copied = min(count, available);
	if (copy_to_user(buf, (u8 *)rx->data + sdev->cur_rx_offset, copied)) {
		ret = -EFAULT;
		goto out_unlock;
	}

	spin_lock_bh(&sdev->rx_lock);
	sdev->cur_rx_offset += copied;
	if (sdev->cur_rx_offset == rx->len) {
		sdev->cur_rx = NULL;
		sdev->cur_rx_offset = 0;
	}
	spin_unlock_bh(&sdev->rx_lock);

	if (!sdev->cur_rx) {
		ret = mhi_sahara_queue_rx_buf(sdev, rx);
		if (ret)
			dev_warn(&sdev->mhi_dev->dev, "failed to requeue RX buffer: %d\n", ret);
	}

	mutex_unlock(&sdev->lock);

	return copied;

out_unlock:
	mutex_unlock(&sdev->lock);
	return ret;
}

static ssize_t mhi_sahara_write(struct file *file, const char __user *buf,
					    size_t count, loff_t *ppos)
{
	struct mhi_sahara_dev *sdev = file->private_data;
	struct mhi_sahara_tx_chunk *chunk, *tmp;
	const char __user *pos = buf;
	struct mhi_chan *ul_chan;
	LIST_HEAD(chunks);
	size_t needed_desc;
	size_t capacity;
	size_t remaining;
	int free_desc;
	int ret;

	if (!count)
		return 0;

	if (!sdev->allow_write)
		return -EOPNOTSUPP;

	if (!sdev->mtu)
		return -EINVAL;

	needed_desc = DIV_ROUND_UP(count, sdev->mtu);
	if (needed_desc > INT_MAX)
		return -EMSGSIZE;

	ul_chan = sdev->mhi_dev->ul_chan;
	if (!ul_chan || ul_chan->tre_ring.elements <= 1)
		return -ENODEV;

	capacity = ul_chan->tre_ring.elements - 1;
	if (needed_desc > capacity)
		return -EMSGSIZE;

	remaining = count;
	while (remaining) {
		size_t xfer_size = min_t(size_t, remaining, sdev->mtu);

		chunk = kzalloc(sizeof(*chunk), GFP_KERNEL);
		if (!chunk) {
			ret = -ENOMEM;
			goto out_free_chunks;
		}

		chunk->data = memdup_user(pos, xfer_size);
		if (IS_ERR(chunk->data)) {
			ret = PTR_ERR(chunk->data);
			kfree(chunk);
			goto out_free_chunks;
		}

		chunk->len = xfer_size;
		list_add_tail(&chunk->node, &chunks);
		pos += xfer_size;
		remaining -= xfer_size;
	}

	ret = mutex_lock_interruptible(&sdev->lock);
	if (ret)
		goto out_free_chunks;

	if (!sdev->present || !sdev->opened) {
		ret = -ENODEV;
		goto out_unlock;
	}

	while ((free_desc = mhi_get_free_desc_count(sdev->mhi_dev,
							 DMA_TO_DEVICE)) < (int)needed_desc) {
		if (file->f_flags & O_NONBLOCK) {
			ret = -EAGAIN;
			goto out_unlock;
		}

		mutex_unlock(&sdev->lock);
		ret = wait_event_interruptible(sdev->write_wq,
				!READ_ONCE(sdev->present) ||
				!READ_ONCE(sdev->opened) ||
				mhi_get_free_desc_count(sdev->mhi_dev,
							DMA_TO_DEVICE) >= (int)needed_desc);
		if (ret)
			goto out_free_chunks;

		ret = mutex_lock_interruptible(&sdev->lock);
		if (ret)
			goto out_free_chunks;

		if (!sdev->present || !sdev->opened) {
			ret = -ENODEV;
			goto out_unlock;
		}
	}

	if (count >= MHI_SAHARA_HELLO_LEN) {
		chunk = list_first_entry(&chunks, struct mhi_sahara_tx_chunk, node);
		if (chunk->len >= MHI_SAHARA_HELLO_LEN) {
			const __le32 *words = chunk->data;

			if (le32_to_cpu(words[0]) == MHI_SAHARA_CMD_HELLO_RESP &&
			    le32_to_cpu(words[1]) == MHI_SAHARA_HELLO_LEN)
				mhi_sahara_dump_words(sdev, "HELLO_RESP", words, count);
		}
	}

	list_for_each_entry_safe(chunk, tmp, &chunks, node) {
		enum mhi_flags flags = list_is_last(&chunk->node, &chunks) ?
			MHI_EOT : MHI_CHAIN;

		ret = mhi_queue_buf(sdev->mhi_dev, DMA_TO_DEVICE, chunk->data,
					    chunk->len, flags);
		if (ret) {
			kfree(chunk->data);
			list_del(&chunk->node);
			kfree(chunk);
			goto out_unlock;
		}

		list_del(&chunk->node);
		kfree(chunk);
	}

	if (needed_desc > 1)
		dev_info(&sdev->mhi_dev->dev,
			 "SAHARA queued grouped UL transfer bytes=%zu desc=%zu mtu=%zu free_desc=%d\n",
			 count, needed_desc, sdev->mtu, free_desc);

	mutex_unlock(&sdev->lock);

	return count;

out_unlock:
	mutex_unlock(&sdev->lock);
out_free_chunks:
	mhi_sahara_free_tx_chunks(&chunks);
	return ret;
}

static __poll_t mhi_sahara_poll(struct file *file, poll_table *wait)
{
	struct mhi_sahara_dev *sdev = file->private_data;
	__poll_t mask = 0;

	poll_wait(file, &sdev->read_wq, wait);
	poll_wait(file, &sdev->write_wq, wait);

	if (!READ_ONCE(sdev->present) || !READ_ONCE(sdev->opened))
		return EPOLLERR | EPOLLHUP;

	if (mhi_sahara_rx_available(sdev))
		mask |= EPOLLIN | EPOLLRDNORM;

	if (sdev->allow_write &&
	    mhi_get_free_desc_count(sdev->mhi_dev, DMA_TO_DEVICE) > 0)
		mask |= EPOLLOUT | EPOLLWRNORM;

	return mask;
}

static const struct file_operations mhi_sahara_fops = {
	.owner = THIS_MODULE,
	.open = mhi_sahara_open,
	.release = mhi_sahara_release,
	.read = mhi_sahara_read,
	.write = mhi_sahara_write,
	.poll = mhi_sahara_poll,
	.llseek = noop_llseek,
};

static void mhi_sahara_dl_xfer_cb(struct mhi_device *mhi_dev,
					  struct mhi_result *result)
{
	struct mhi_sahara_dev *sdev = dev_get_drvdata(&mhi_dev->dev);
	struct mhi_sahara_buf *rx;

	if (!result->buf_addr)
		return;

	rx = mhi_sahara_rx_from_data(sdev, result->buf_addr);
	spin_lock_bh(&sdev->rx_lock);
	if (rx->queued) {
		list_del_init(&rx->queued_node);
		rx->queued = false;
	}
	spin_unlock_bh(&sdev->rx_lock);

	if (result->transaction_status || !result->bytes_xferd) {
		kfree(result->buf_addr);
		wake_up_all(&sdev->read_wq);
		return;
	}

	if (!mhi_sahara_rx_packet_valid(result->buf_addr, result->bytes_xferd)) {
		mhi_sahara_log_rx_packet(sdev, result->buf_addr, result->bytes_xferd);
		dev_info(&mhi_dev->dev, "SAHARA dropping invalid RX packet len %zu\n",
			 result->bytes_xferd);
		kfree(result->buf_addr);
		wake_up_all(&sdev->read_wq);
		return;
	}

	mhi_sahara_note_rx_for_restart(sdev, result->buf_addr, result->bytes_xferd);

	if (sdev->allow_write && mhi_sahara_restart_after_ul_completion &&
	    cancel_delayed_work(&sdev->restart_work))
		dev_info(&mhi_dev->dev,
			 "SAHARA canceled pending channel restart after DL packet\n");

	rx->data = result->buf_addr;
	rx->len = result->bytes_xferd;

	mhi_sahara_log_rx_packet(sdev, rx->data, rx->len);
	mhi_sahara_maybe_queue_hello_resp(sdev, rx->data, rx->len);

	spin_lock_bh(&sdev->rx_lock);
	if (sdev->opened || sdev->keep_rx_without_open ||
	    (sdev->allow_write && mhi_sahara_keep_prepared_on_release))
		list_add_tail(&rx->node, &sdev->pending_rx);
	else
		kfree(result->buf_addr);
	spin_unlock_bh(&sdev->rx_lock);

	wake_up_all(&sdev->read_wq);
}

static void mhi_sahara_ul_xfer_cb(struct mhi_device *mhi_dev,
					  struct mhi_result *result)
{
	struct mhi_sahara_dev *sdev = dev_get_drvdata(&mhi_dev->dev);

	if (sdev->allow_write) {
		dev_info(&mhi_dev->dev, "SAHARA UL completion status %d bytes %zu\n",
			 result->transaction_status, result->bytes_xferd);
		mhi_sahara_log_state(sdev, "UL completion");
		if (!result->transaction_status && mhi_sahara_ring_ul_db_after_ul)
			mhi_sahara_ring_chan_db_now(sdev, mhi_dev->ul_chan,
							    "UL completion UL re-ring");
		if (!result->transaction_status && mhi_sahara_ring_dl_db_after_ul)
			mhi_sahara_ring_chan_db_now(sdev, mhi_dev->dl_chan,
							    "UL completion DL re-ring");
		if (!result->transaction_status && mhi_sahara_restart_after_ul_completion) {
			u32 image = 0;
			size_t remaining = 0;
			bool suppress_after_read = false;

			if (!result->bytes_xferd) {
				dev_info(&mhi_dev->dev,
					 "SAHARA not scheduling channel restart after zero-byte UL completion\n");
			} else if (mhi_sahara_restart_deferred_for_ul(sdev,
									 result->bytes_xferd,
									 &image, &remaining,
									 &suppress_after_read)) {
				dev_info(&mhi_dev->dev,
					 "SAHARA %s channel restart after READ_DATA image=%u remaining=%zu\n",
					 suppress_after_read ? "suppressing" : "deferring",
					 image, remaining);
			} else {
				dev_info(&mhi_dev->dev,
					 "SAHARA scheduling channel restart after UL completion delay_ms=%u\n",
					 mhi_sahara_restart_after_ul_delay_ms);
				mod_delayed_work(system_wq, &sdev->restart_work,
						 msecs_to_jiffies(mhi_sahara_restart_after_ul_delay_ms));
			}
		}
	}

	kfree(result->buf_addr);
	wake_up_all(&sdev->write_wq);
}

static void mhi_sahara_status_cb(struct mhi_device *mhi_dev,
					 enum mhi_callback reason)
{
	struct mhi_sahara_dev *sdev = dev_get_drvdata(&mhi_dev->dev);

	if (reason == MHI_CB_SYS_ERROR || reason == MHI_CB_FATAL_ERROR ||
	    reason == MHI_CB_EE_RDDM) {
		wake_up_all(&sdev->read_wq);
		wake_up_all(&sdev->write_wq);
	}
}

static int mhi_sahara_probe(struct mhi_device *mhi_dev,
			    const struct mhi_device_id *id)
{
	struct mhi_sahara_dev *sdev;
	const char *name;
	bool is_bl = !strcmp(id->chan, "BL");
	int ret;

	if (!mhi_dev->dl_chan || (!is_bl && !mhi_dev->ul_chan))
		return -ENODEV;

	sdev = kzalloc(sizeof(*sdev), GFP_KERNEL);
	if (!sdev)
		return -ENOMEM;

	name = kasprintf(GFP_KERNEL, is_bl ? "mhi_bl%d" : "mhi_sahara%d",
			 mhi_dev->mhi_cntrl->index);
	if (!name) {
		kfree(sdev);
		return -ENOMEM;
	}

	sdev->mhi_dev = mhi_dev;
	sdev->miscdev.minor = MISC_DYNAMIC_MINOR;
	sdev->miscdev.name = name;
	sdev->miscdev.fops = &mhi_sahara_fops;
	sdev->miscdev.parent = &mhi_dev->dev;
	sdev->mtu = id->driver_data ?: MHI_SAHARA_DEFAULT_MTU;
	if (mhi_dev->mhi_cntrl->mru)
		sdev->mtu = min_t(size_t, sdev->mtu, mhi_dev->mhi_cntrl->mru);
	sdev->allow_write = !!mhi_dev->ul_chan;
	sdev->auto_start = is_bl && mhi_sahara_bl_auto_start;
	sdev->delayed_auto_start = false;
	sdev->keep_rx_without_open = sdev->auto_start;
	sdev->auto_hello_resp = !is_bl && mhi_sahara_auto_hello_resp;
	refcount_set(&sdev->refs, 1);
	mutex_init(&sdev->lock);
	spin_lock_init(&sdev->rx_lock);
	spin_lock_init(&sdev->restart_lock);
	INIT_DELAYED_WORK(&sdev->drain_work, mhi_sahara_drain_work);
	INIT_DELAYED_WORK(&sdev->start_work, mhi_sahara_start_work);
	INIT_DELAYED_WORK(&sdev->restart_work, mhi_sahara_restart_work);
	init_waitqueue_head(&sdev->read_wq);
	init_waitqueue_head(&sdev->write_wq);
	INIT_LIST_HEAD(&sdev->pending_rx);
	INIT_LIST_HEAD(&sdev->queued_rx);
	sdev->present = true;

	dev_set_drvdata(&mhi_dev->dev, sdev);

	ret = misc_register(&sdev->miscdev);
	if (ret) {
		dev_err(&mhi_dev->dev, "failed to register /dev/%s: %d\n", name, ret);
		goto err_free;
	}

	if (sdev->auto_start) {
		ret = mhi_prepare_for_transfer(mhi_dev);
		if (ret)
			goto err_deregister;
		sdev->prepared = true;

		ret = mhi_sahara_refill_rx(sdev);
		if (ret)
			goto err_stop;

		mhi_sahara_schedule_drain(sdev);
	}

	if (sdev->delayed_auto_start)
		schedule_delayed_work(&sdev->start_work,
				      msecs_to_jiffies(MHI_SAHARA_START_DELAY_MS));

	dev_info(&mhi_dev->dev, "registered /dev/%s auto_start=%d auto_hello_resp=%d auto_hello_mode=%u\n",
		 sdev->miscdev.name, sdev->auto_start, sdev->auto_hello_resp, mhi_sahara_auto_hello_mode);

	return 0;

err_stop:
	mutex_lock(&sdev->lock);
	mhi_sahara_stop_locked(sdev);
	mutex_unlock(&sdev->lock);
err_deregister:
	misc_deregister(&sdev->miscdev);
err_free:
	dev_set_drvdata(&mhi_dev->dev, NULL);
	kfree(name);
	kfree(sdev);
	return ret;
}

static void mhi_sahara_remove(struct mhi_device *mhi_dev)
{
	struct mhi_sahara_dev *sdev = dev_get_drvdata(&mhi_dev->dev);

	cancel_delayed_work_sync(&sdev->start_work);
	cancel_delayed_work_sync(&sdev->restart_work);
	misc_deregister(&sdev->miscdev);

	mutex_lock(&sdev->lock);
	sdev->present = false;
	mhi_sahara_stop_locked(sdev);
	mutex_unlock(&sdev->lock);

	wake_up_all(&sdev->read_wq);
	wake_up_all(&sdev->write_wq);
	mhi_sahara_put(sdev);
}

static const struct mhi_device_id mhi_sahara_match_table[] = {
	{ .chan = "SAHARA", .driver_data = MHI_SAHARA_DEFAULT_MTU },
	{ .chan = "BL", .driver_data = MHI_BL_DEFAULT_MTU },
	{}
};
MODULE_DEVICE_TABLE(mhi, mhi_sahara_match_table);

static struct mhi_driver mhi_sahara_driver = {
	.id_table = mhi_sahara_match_table,
	.probe = mhi_sahara_probe,
	.remove = mhi_sahara_remove,
	.ul_xfer_cb = mhi_sahara_ul_xfer_cb,
	.dl_xfer_cb = mhi_sahara_dl_xfer_cb,
	.status_cb = mhi_sahara_status_cb,
	.driver = {
		.name = "mhi_sahara_diag",
	},
};
module_mhi_driver(mhi_sahara_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MHI Sahara diagnostic character device");
