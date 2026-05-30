// SPDX-License-Identifier: GPL-2.0
/*
 * camss-csid-4-7.c
 *
 * Qualcomm MSM Camera Subsystem - CSID (CSI Decoder) Module
 *
 * Copyright (C) 2020 Linaro Ltd.
 */
#include <linux/completion.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/of.h>

#include "camss-csid.h"
#include "camss-csid-gen2.h"
#include "camss-vfe.h"
#include "camss.h"

/* The CSID 2 IP-block is different from the others,
 * and is of a bare-bones Lite version, with no PIX
 * interface support. As a result of that it has an
 * alternate register layout.
 */

#define CSID_RST_STROBES	0x10
#define		RST_STROBES	0

#define CSID_CSI2_RX_IRQ_STATUS	0x20
#define	CSID_CSI2_RX_IRQ_MASK	0x24
#define CSID_CSI2_RX_IRQ_CLEAR	0x28

#define CSID_CSI2_RDIN_IRQ_STATUS(rdi)		((csid_is_lite(csid) ? 0x30 : 0x40) \
						 + 0x10 * (rdi))
#define CSID_CSI2_RDIN_IRQ_MASK(rdi)		((csid_is_lite(csid) ? 0x34 : 0x44) \
						 + 0x10 * (rdi))
#define CSID_CSI2_RDIN_IRQ_CLEAR(rdi)		((csid_is_lite(csid) ? 0x38 : 0x48) \
						 + 0x10 * (rdi))
#define CSID_CSI2_RDIN_IRQ_SET(rdi)		((csid_is_lite(csid) ? 0x3C : 0x4C) \
						 + 0x10 * (rdi))

#define CSID_TOP_IRQ_STATUS	0x70
#define		TOP_IRQ_STATUS_RESET_DONE 0
#define CSID_TOP_IRQ_MASK	0x74
#define CSID_TOP_IRQ_CLEAR	0x78
#define CSID_TOP_IRQ_SET	0x7C
#define CSID_IRQ_CMD		0x80
#define		IRQ_CMD_CLEAR	0
#define		IRQ_CMD_SET	4

#define CSID_CSI2_RX_CFG0	0x100
#define		CSI2_RX_CFG0_NUM_ACTIVE_LANES	0
#define		CSI2_RX_CFG0_DL0_INPUT_SEL	4
#define		CSI2_RX_CFG0_DL1_INPUT_SEL	8
#define		CSI2_RX_CFG0_DL2_INPUT_SEL	12
#define		CSI2_RX_CFG0_DL3_INPUT_SEL	16
#define		CSI2_RX_CFG0_PHY_NUM_SEL	20
#define		CSI2_RX_CFG0_PHY_TYPE_SEL	24

#define CSID_CSI2_RX_CFG1	0x104
#define		CSI2_RX_CFG1_PACKET_ECC_CORRECTION_EN		0
#define		CSI2_RX_CFG1_DE_SCRAMBLE_EN			1
#define		CSI2_RX_CFG1_VC_MODE				2
#define		CSI2_RX_CFG1_COMPLETE_STREAM_EN			4
#define		CSI2_RX_CFG1_COMPLETE_STREAM_FRAME_TIMING	5
#define		CSI2_RX_CFG1_MISR_EN				6
#define		CSI2_RX_CFG1_CGC_MODE				7
#define			CGC_MODE_DYNAMIC_GATING		0
#define			CGC_MODE_ALWAYS_ON		1

#define CSID_RDI_CFG0(rdi)			((csid_is_lite(csid) ? 0x200 : 0x300) \
						 + 0x100 * (rdi))
#define		RDI_CFG0_BYTE_CNTR_EN		0
#define		RDI_CFG0_FORMAT_MEASURE_EN	1
#define		RDI_CFG0_TIMESTAMP_EN		2
#define		RDI_CFG0_DROP_H_EN		3
#define		RDI_CFG0_DROP_V_EN		4
#define		RDI_CFG0_CROP_H_EN		5
#define		RDI_CFG0_CROP_V_EN		6
#define		RDI_CFG0_MISR_EN		7
#define		RDI_CFG0_CGC_MODE		8
#define			CGC_MODE_DYNAMIC	0
#define			CGC_MODE_ALWAYS_ON	1
#define		RDI_CFG0_PLAIN_ALIGNMENT	9
#define			PLAIN_ALIGNMENT_LSB	0
#define			PLAIN_ALIGNMENT_MSB	1
#define		RDI_CFG0_PLAIN_FORMAT		10
#define		RDI_CFG0_DECODE_FORMAT		12
#define		RDI_CFG0_DATA_TYPE		16
#define		RDI_CFG0_VIRTUAL_CHANNEL	22
#define		RDI_CFG0_DT_ID			27
#define		RDI_CFG0_EARLY_EOF_EN		29
#define		RDI_CFG0_PACKING_FORMAT		30
#define		RDI_CFG0_ENABLE			31

#define CSID_RDI_CFG1(rdi)			((csid_is_lite(csid) ? 0x204 : 0x304)\
						+ 0x100 * (rdi))
#define		RDI_CFG1_TIMESTAMP_STB_SEL	0

#define CSID_RDI_CTRL(rdi)			((csid_is_lite(csid) ? 0x208 : 0x308)\
						+ 0x100 * (rdi))
#define		RDI_CTRL_HALT_CMD		0
#define			HALT_CMD_HALT_AT_FRAME_BOUNDARY		0
#define			HALT_CMD_RESUME_AT_FRAME_BOUNDARY	1
#define		RDI_CTRL_HALT_MODE		2

#define CSID_RDI_FRM_DROP_PATTERN(rdi)			((csid_is_lite(csid) ? 0x20C : 0x30C)\
							+ 0x100 * (rdi))
#define CSID_RDI_FRM_DROP_PERIOD(rdi)			((csid_is_lite(csid) ? 0x210 : 0x310)\
							+ 0x100 * (rdi))
#define CSID_RDI_IRQ_SUBSAMPLE_PATTERN(rdi)		((csid_is_lite(csid) ? 0x214 : 0x314)\
							+ 0x100 * (rdi))
#define CSID_RDI_IRQ_SUBSAMPLE_PERIOD(rdi)		((csid_is_lite(csid) ? 0x218 : 0x318)\
							+ 0x100 * (rdi))
#define CSID_RDI_RPP_PIX_DROP_PATTERN(rdi)		((csid_is_lite(csid) ? 0x224 : 0x324)\
							+ 0x100 * (rdi))
#define CSID_RDI_RPP_PIX_DROP_PERIOD(rdi)		((csid_is_lite(csid) ? 0x228 : 0x328)\
							+ 0x100 * (rdi))
#define CSID_RDI_RPP_LINE_DROP_PATTERN(rdi)		((csid_is_lite(csid) ? 0x22C : 0x32C)\
							+ 0x100 * (rdi))
#define CSID_RDI_RPP_LINE_DROP_PERIOD(rdi)		((csid_is_lite(csid) ? 0x230 : 0x330)\
							+ 0x100 * (rdi))

#define CSID_PXL_PATH_IPP	0
#define CSID_PXL_PATH_PPP	1
#define CSID_PXL_SRC_STREAM	3
#define CSID_PXL_VC		0
#define CSID_PXL_BASE(path)	((path) == CSID_PXL_PATH_IPP ? 0x200 : 0x700)

#define CSID_PXL_IRQ_STATUS(path)		(CSID_PXL_BASE(path) == 0x200 ? 0x30 : 0xa0)
#define CSID_PXL_IRQ_MASK(path)			(CSID_PXL_IRQ_STATUS(path) + 0x04)
#define CSID_PXL_IRQ_CLEAR(path)			(CSID_PXL_IRQ_STATUS(path) + 0x08)
#define CSID_PXL_IRQ_SET(path)			(CSID_PXL_IRQ_STATUS(path) + 0x0c)

#define CSID_PXL_CFG0(path)			(CSID_PXL_BASE(path) + 0x00)
#define		PXL_CFG0_BYTE_CNTR_EN		0
#define		PXL_CFG0_FORMAT_MEASURE_EN	1
#define		PXL_CFG0_TIMESTAMP_EN		2
#define		PXL_CFG0_DROP_H_EN		3
#define		PXL_CFG0_DROP_V_EN		4
#define		PXL_CFG0_CROP_H_EN		5
#define		PXL_CFG0_CROP_V_EN		6
#define		PXL_CFG0_PIX_STORE_EN		7
#define		PXL_CFG0_CGC_MODE		8
#define		PXL_CFG0_PLAIN_FORMAT		10
#define		PXL_CFG0_DECODE_FORMAT		12
#define		PXL_CFG0_DATA_TYPE		16
#define		PXL_CFG0_VIRTUAL_CHANNEL		22
#define		PXL_CFG0_DT_ID			27
#define		PXL_CFG0_EARLY_EOF_EN		29
#define		PXL_CFG0_PACKING_FORMAT		30
#define		PXL_CFG0_ENABLE			31
#define CSID_PXL_CFG1(path)			(CSID_PXL_BASE(path) + 0x04)
#define		PXL_CFG1_TIMESTAMP_STB_SEL	0
#define CSID_PXL_CTRL(path)			(CSID_PXL_BASE(path) + 0x08)
#define		PXL_CTRL_HALT_CMD		0
#define		PXL_CTRL_HALT_MODE		2
#define CSID_PXL_FRM_DROP_PATTERN(path)		(CSID_PXL_BASE(path) + 0x0c)
#define CSID_PXL_FRM_DROP_PERIOD(path)		(CSID_PXL_BASE(path) + 0x10)
#define CSID_PXL_IRQ_SUBSAMPLE_PATTERN(path)	(CSID_PXL_BASE(path) + 0x14)
#define CSID_PXL_IRQ_SUBSAMPLE_PERIOD(path)	(CSID_PXL_BASE(path) + 0x18)
#define CSID_PXL_HCROP(path)			(CSID_PXL_BASE(path) + 0x1c)
#define CSID_PXL_VCROP(path)			(CSID_PXL_BASE(path) + 0x20)
#define CSID_PXL_PIX_DROP_PATTERN(path)		(CSID_PXL_BASE(path) + 0x24)
#define CSID_PXL_PIX_DROP_PERIOD(path)		(CSID_PXL_BASE(path) + 0x28)
#define CSID_PXL_LINE_DROP_PATTERN(path)	(CSID_PXL_BASE(path) + 0x2c)
#define CSID_PXL_LINE_DROP_PERIOD(path)		(CSID_PXL_BASE(path) + 0x30)
#define CSID_PXL_RST_STROBES(path)		(CSID_PXL_BASE(path) + 0x40)
#define CSID_PXL_STATUS(path)			(CSID_PXL_BASE(path) + 0x54)
#define CSID_PXL_MISR_VAL(path)			(CSID_PXL_BASE(path) + 0x58)
#define CSID_PXL_FORMAT_MEASURE_CFG0(path)	(CSID_PXL_BASE(path) + 0x70)
#define		PXL_FORMAT_MEASURE_CFG0_EN	0x3
#define CSID_PXL_FORMAT_MEASURE_CFG1(path)	(CSID_PXL_BASE(path) + 0x74)
#define CSID_PXL_FORMAT_MEASURE0(path)		(CSID_PXL_BASE(path) + 0x78)
#define CSID_PXL_FORMAT_MEASURE1(path)		(CSID_PXL_BASE(path) + 0x7c)
#define CSID_PXL_FORMAT_MEASURE2(path)		(CSID_PXL_BASE(path) + 0x80)
#define CSID_PXL_TIMESTAMP_CURR0_SOF(path)	(CSID_PXL_BASE(path) + 0x90)
#define CSID_PXL_TIMESTAMP_CURR1_SOF(path)	(CSID_PXL_BASE(path) + 0x94)
#define CSID_PXL_TIMESTAMP_PREV0_SOF(path)	(CSID_PXL_BASE(path) + 0x98)
#define CSID_PXL_TIMESTAMP_PREV1_SOF(path)	(CSID_PXL_BASE(path) + 0x9c)
#define CSID_PXL_TIMESTAMP_CURR0_EOF(path)	(CSID_PXL_BASE(path) + 0xa0)
#define CSID_PXL_TIMESTAMP_CURR1_EOF(path)	(CSID_PXL_BASE(path) + 0xa4)
#define CSID_PXL_TIMESTAMP_PREV0_EOF(path)	(CSID_PXL_BASE(path) + 0xa8)
#define CSID_PXL_TIMESTAMP_PREV1_EOF(path)	(CSID_PXL_BASE(path) + 0xac)
#define CSID_PXL_IRQ_RST_DONE			BIT(1)
#define CSID_PXL_IRQ_FIFO_OVERFLOW		BIT(2)
#define CSID_PXL_IRQ_INPUT_EOF			BIT(9)
#define CSID_PXL_IRQ_INPUT_SOF			BIT(12)
#define CSID_PXL_IRQ_PIX_COUNT			BIT(13)
#define CSID_PXL_IRQ_LINE_COUNT			BIT(14)
#define CSID_PXL_IRQ_ERROR			(CSID_PXL_IRQ_FIFO_OVERFLOW | \
						 CSID_PXL_IRQ_PIX_COUNT | \
						 CSID_PXL_IRQ_LINE_COUNT)
#define CSID_PXL_IRQ_MASK_CONFIGURED		(CSID_PXL_IRQ_RST_DONE | \
						 CSID_PXL_IRQ_INPUT_EOF | \
						 CSID_PXL_IRQ_INPUT_SOF | \
						 CSID_PXL_IRQ_ERROR)
#define CSID_PXL_IRQ_MASK_ALL			0x7fff
#define CSID_PXL_ERR_RECOVERY_CFG0(path)	(CSID_PXL_BASE(path) + 0xd0)
#define CSID_PXL_ERR_RECOVERY_CFG1(path)	(CSID_PXL_BASE(path) + 0xd4)
#define CSID_PXL_ERR_RECOVERY_CFG2(path)	(CSID_PXL_BASE(path) + 0xd8)
#define CSID_PXL_MULTI_VCDT_CFG0(path)		(CSID_PXL_BASE(path) + 0xdc)

#define CSID_TPG_CTRL		0x600
#define		TPG_CTRL_TEST_EN		0
#define		TPG_CTRL_FS_PKT_EN		1
#define		TPG_CTRL_FE_PKT_EN		2
#define		TPG_CTRL_NUM_ACTIVE_LANES	4
#define		TPG_CTRL_CYCLES_BETWEEN_PKTS	8
#define		TPG_CTRL_NUM_TRAIL_BYTES	20

#define CSID_TPG_VC_CFG0	0x604
#define		TPG_VC_CFG0_VC_NUM			0
#define		TPG_VC_CFG0_NUM_ACTIVE_SLOTS		8
#define			NUM_ACTIVE_SLOTS_0_ENABLED	0
#define			NUM_ACTIVE_SLOTS_0_1_ENABLED	1
#define			NUM_ACTIVE_SLOTS_0_1_2_ENABLED	2
#define			NUM_ACTIVE_SLOTS_0_1_3_ENABLED	3
#define		TPG_VC_CFG0_LINE_INTERLEAVING_MODE	10
#define			INTELEAVING_MODE_INTERLEAVED	0
#define			INTELEAVING_MODE_ONE_SHOT	1
#define		TPG_VC_CFG0_NUM_FRAMES			16

#define CSID_TPG_VC_CFG1	0x608
#define		TPG_VC_CFG1_H_BLANKING_COUNT		0
#define		TPG_VC_CFG1_V_BLANKING_COUNT		12
#define		TPG_VC_CFG1_V_BLANK_FRAME_WIDTH_SEL	24

#define CSID_TPG_LFSR_SEED	0x60C

#define CSID_TPG_DT_n_CFG_0(n)	(0x610 + (n) * 0xC)
#define		TPG_DT_n_CFG_0_FRAME_HEIGHT	0
#define		TPG_DT_n_CFG_0_FRAME_WIDTH	16

#define CSID_TPG_DT_n_CFG_1(n)	(0x614 + (n) * 0xC)
#define		TPG_DT_n_CFG_1_DATA_TYPE	0
#define		TPG_DT_n_CFG_1_ECC_XOR_MASK	8
#define		TPG_DT_n_CFG_1_CRC_XOR_MASK	16

#define CSID_TPG_DT_n_CFG_2(n)	(0x618 + (n) * 0xC)
#define		TPG_DT_n_CFG_2_PAYLOAD_MODE		0
#define		TPG_DT_n_CFG_2_USER_SPECIFIED_PAYLOAD	4
#define		TPG_DT_n_CFG_2_ENCODE_FORMAT		16

#define CSID_TPG_COLOR_BARS_CFG	0x640
#define		TPG_COLOR_BARS_CFG_UNICOLOR_BAR_EN	0
#define		TPG_COLOR_BARS_CFG_UNICOLOR_BAR_SEL	4
#define		TPG_COLOR_BARS_CFG_SPLIT_EN		5
#define		TPG_COLOR_BARS_CFG_ROTATE_PERIOD	8

#define CSID_TPG_COLOR_BOX_CFG	0x644
#define		TPG_COLOR_BOX_CFG_MODE		0
#define		TPG_COLOR_BOX_PATTERN_SEL	2

static void __csid_configure_rx(struct csid_device *csid,
				struct csid_phy_config *phy, int vc)
{
	u8 lane_cnt = csid->phy.lane_cnt;
	int val;

	if (!lane_cnt)
		lane_cnt = 4;

	val = (lane_cnt - 1) << CSI2_RX_CFG0_NUM_ACTIVE_LANES;
	val |= phy->lane_assign << CSI2_RX_CFG0_DL0_INPUT_SEL;
	val |= phy->csiphy_id << CSI2_RX_CFG0_PHY_NUM_SEL;
	writel_relaxed(val, csid->base + CSID_CSI2_RX_CFG0);

	val = 1 << CSI2_RX_CFG1_PACKET_ECC_CORRECTION_EN;
	if (vc > 3)
		val |= 1 << CSI2_RX_CFG1_VC_MODE;
	val |= 1 << CSI2_RX_CFG1_MISR_EN;
	writel_relaxed(val, csid->base + CSID_CSI2_RX_CFG1);
}

static bool csid_use_pxl_stream(struct csid_device *csid, u8 src)
{
	return !csid_is_lite(csid) && src == CSID_PXL_SRC_STREAM;
}

static void csid_reissue_vfe_reg_update(struct csid_device *csid, u8 src)
{
	struct media_pad *remote;

	remote = media_pad_remote_pad_first(&csid->pads[MSM_CSID_PAD_FIRST_SRC + src]);
	if (remote && is_media_entity_v4l2_subdev(remote->entity))
		vfe_reissue_reg_update(media_entity_to_v4l2_subdev(remote->entity));
}

static void __csid_ctrl_rdi(struct csid_device *csid, int enable, u8 rdi)
{
	int val;

	if (enable)
		val = HALT_CMD_RESUME_AT_FRAME_BOUNDARY << RDI_CTRL_HALT_CMD;
	else
		val = HALT_CMD_HALT_AT_FRAME_BOUNDARY << RDI_CTRL_HALT_CMD;
	writel_relaxed(val, csid->base + CSID_RDI_CTRL(rdi));
}

static void __csid_ctrl_pxl(struct csid_device *csid, int enable, u8 path)
{
	int val;

	if (enable)
		val = HALT_CMD_RESUME_AT_FRAME_BOUNDARY << PXL_CTRL_HALT_CMD;
	else
		val = HALT_CMD_HALT_AT_FRAME_BOUNDARY << PXL_CTRL_HALT_CMD;
	writel_relaxed(val, csid->base + CSID_PXL_CTRL(path));
}

static void __csid_configure_testgen(struct csid_device *csid, u8 enable, u8 vc)
{
	struct csid_testgen_config *tg = &csid->testgen;
	struct v4l2_mbus_framefmt *input_format = &csid->fmt[MSM_CSID_PAD_FIRST_SRC + vc];
	const struct csid_format_info *format = csid_get_fmt_entry(csid->res->formats->formats,
								   csid->res->formats->nformats,
								   input_format->code);
	u8 lane_cnt = csid->phy.lane_cnt;
	u32 val;

	if (!lane_cnt)
		lane_cnt = 4;

	/* configure one DT, infinite frames */
	val = vc << TPG_VC_CFG0_VC_NUM;
	val |= INTELEAVING_MODE_ONE_SHOT << TPG_VC_CFG0_LINE_INTERLEAVING_MODE;
	val |= 0 << TPG_VC_CFG0_NUM_FRAMES;
	writel_relaxed(val, csid->base + CSID_TPG_VC_CFG0);

	val = 0x740 << TPG_VC_CFG1_H_BLANKING_COUNT;
	val |= 0x3ff << TPG_VC_CFG1_V_BLANKING_COUNT;
	writel_relaxed(val, csid->base + CSID_TPG_VC_CFG1);

	writel_relaxed(0x12345678, csid->base + CSID_TPG_LFSR_SEED);

	val = (input_format->height & 0x1fff) << TPG_DT_n_CFG_0_FRAME_HEIGHT;
	val |= (input_format->width & 0x1fff) << TPG_DT_n_CFG_0_FRAME_WIDTH;
	writel_relaxed(val, csid->base + CSID_TPG_DT_n_CFG_0(0));

	val = format->data_type << TPG_DT_n_CFG_1_DATA_TYPE;
	writel_relaxed(val, csid->base + CSID_TPG_DT_n_CFG_1(0));

	val = (tg->mode - 1) << TPG_DT_n_CFG_2_PAYLOAD_MODE;
	val |= 0xBE << TPG_DT_n_CFG_2_USER_SPECIFIED_PAYLOAD;
	val |= format->decode_format << TPG_DT_n_CFG_2_ENCODE_FORMAT;
	writel_relaxed(val, csid->base + CSID_TPG_DT_n_CFG_2(0));

	writel_relaxed(0, csid->base + CSID_TPG_COLOR_BARS_CFG);

	writel_relaxed(0, csid->base + CSID_TPG_COLOR_BOX_CFG);

	val = enable << TPG_CTRL_TEST_EN;
	val |= 1 << TPG_CTRL_FS_PKT_EN;
	val |= 1 << TPG_CTRL_FE_PKT_EN;
	val |= (lane_cnt - 1) << TPG_CTRL_NUM_ACTIVE_LANES;
	val |= 0x64 << TPG_CTRL_CYCLES_BETWEEN_PKTS;
	val |= 0xA << TPG_CTRL_NUM_TRAIL_BYTES;
	writel_relaxed(val, csid->base + CSID_TPG_CTRL);
}

static void __csid_configure_rdi_stream(struct csid_device *csid, u8 enable, u8 vc)
{
	/* Source pads matching RDI channels on hardware. Pad 1 -> RDI0, Pad 2 -> RDI1, etc. */
	struct v4l2_mbus_framefmt *input_format = &csid->fmt[MSM_CSID_PAD_FIRST_SRC + vc];
	const struct csid_format_info *format = csid_get_fmt_entry(csid->res->formats->formats,
								   csid->res->formats->nformats,
								   input_format->code);
	u8 dt_id = vc & 0x03;
	u32 val;

	val = 1 << RDI_CFG0_BYTE_CNTR_EN;
	val |= 1 << RDI_CFG0_FORMAT_MEASURE_EN;
	val |= 1 << RDI_CFG0_TIMESTAMP_EN;
	val |= DECODE_FORMAT_PAYLOAD_ONLY << RDI_CFG0_DECODE_FORMAT;
	val |= format->data_type << RDI_CFG0_DATA_TYPE;
	val |= vc << RDI_CFG0_VIRTUAL_CHANNEL;
	val |= dt_id << RDI_CFG0_DT_ID;
	writel_relaxed(val, csid->base + CSID_RDI_CFG0(vc));

	/* CSID_TIMESTAMP_STB_POST_IRQ */
	val = 2 << RDI_CFG1_TIMESTAMP_STB_SEL;
	writel_relaxed(val, csid->base + CSID_RDI_CFG1(vc));

	val = 1;
	writel_relaxed(val, csid->base + CSID_RDI_FRM_DROP_PERIOD(vc));

	val = 0;
	writel_relaxed(val, csid->base + CSID_RDI_FRM_DROP_PATTERN(vc));

	val = 1;
	writel_relaxed(val, csid->base + CSID_RDI_IRQ_SUBSAMPLE_PERIOD(vc));

	val = 0;
	writel_relaxed(val, csid->base + CSID_RDI_IRQ_SUBSAMPLE_PATTERN(vc));

	val = 1;
	writel_relaxed(val, csid->base + CSID_RDI_RPP_PIX_DROP_PERIOD(vc));

	val = 0;
	writel_relaxed(val, csid->base + CSID_RDI_RPP_PIX_DROP_PATTERN(vc));

	val = 1;
	writel_relaxed(val, csid->base + CSID_RDI_RPP_LINE_DROP_PERIOD(vc));

	val = 0;
	writel_relaxed(val, csid->base + CSID_RDI_RPP_LINE_DROP_PATTERN(vc));

	val = 0;
	writel_relaxed(val, csid->base + CSID_RDI_CTRL(vc));

	val = readl_relaxed(csid->base + CSID_RDI_CFG0(vc));
	val |= enable << RDI_CFG0_ENABLE;
	writel_relaxed(val, csid->base + CSID_RDI_CFG0(vc));
}

static void __csid_configure_pxl_stream(struct csid_device *csid, u8 enable,
					 u8 src, u8 path)
{
	struct v4l2_mbus_framefmt *input_format =
		&csid->fmt[MSM_CSID_PAD_FIRST_SRC + src];
	const struct csid_format_info *format = csid_get_fmt_entry(csid->res->formats->formats,
								   csid->res->formats->nformats,
								   input_format->code);
	u32 last_pixel = input_format->width ? input_format->width - 1 : 0;
	u32 last_line = input_format->height ? input_format->height - 1 : 0;
	u8 dt_id = CSID_PXL_VC & 0x03;
	u32 val;

	writel_relaxed(0, csid->base + CSID_PXL_IRQ_MASK(path));

	val = 1 << PXL_CFG0_BYTE_CNTR_EN;
	val |= 1 << PXL_CFG0_FORMAT_MEASURE_EN;
	val |= 1 << PXL_CFG0_TIMESTAMP_EN;
	val |= 1 << PXL_CFG0_PIX_STORE_EN;
	val |= format->decode_format << PXL_CFG0_DECODE_FORMAT;
	val |= format->data_type << PXL_CFG0_DATA_TYPE;
	val |= CSID_PXL_VC << PXL_CFG0_VIRTUAL_CHANNEL;
	val |= dt_id << PXL_CFG0_DT_ID;
	writel_relaxed(val, csid->base + CSID_PXL_CFG0(path));

	val = 2 << PXL_CFG1_TIMESTAMP_STB_SEL;
	writel_relaxed(val, csid->base + CSID_PXL_CFG1(path));

	val = 1;
	writel_relaxed(val, csid->base + CSID_PXL_FRM_DROP_PERIOD(path));

	val = 0;
	writel_relaxed(val, csid->base + CSID_PXL_FRM_DROP_PATTERN(path));

	val = 1;
	writel_relaxed(val, csid->base + CSID_PXL_IRQ_SUBSAMPLE_PERIOD(path));

	val = 0;
	writel_relaxed(val, csid->base + CSID_PXL_IRQ_SUBSAMPLE_PATTERN(path));

	val = 1;
	writel_relaxed(val, csid->base + CSID_PXL_PIX_DROP_PERIOD(path));

	val = 0;
	writel_relaxed(val, csid->base + CSID_PXL_PIX_DROP_PATTERN(path));

	val = 1;
	writel_relaxed(val, csid->base + CSID_PXL_LINE_DROP_PERIOD(path));

	val = 0;
	writel_relaxed(val, csid->base + CSID_PXL_LINE_DROP_PATTERN(path));

	val = last_pixel << 16;
	writel_relaxed(val, csid->base + CSID_PXL_HCROP(path));

	val = last_line << 16;
	writel_relaxed(val, csid->base + CSID_PXL_VCROP(path));

	val = (input_format->height & 0xffff) << 16;
	val |= input_format->width & 0xffff;
	writel_relaxed(val, csid->base + CSID_PXL_FORMAT_MEASURE_CFG1(path));
	writel_relaxed(enable ? PXL_FORMAT_MEASURE_CFG0_EN : 0,
		       csid->base + CSID_PXL_FORMAT_MEASURE_CFG0(path));

	val = 0;
	writel_relaxed(val, csid->base + CSID_PXL_CTRL(path));

	val = readl_relaxed(csid->base + CSID_PXL_CFG0(path));
	val |= enable << PXL_CFG0_ENABLE;
	writel_relaxed(val, csid->base + CSID_PXL_CFG0(path));

	writel_relaxed(CSID_PXL_IRQ_MASK_ALL,
		       csid->base + CSID_PXL_IRQ_CLEAR(path));
	writel_relaxed(1 << IRQ_CMD_CLEAR, csid->base + CSID_IRQ_CMD);

	if (enable)
		writel_relaxed(CSID_PXL_IRQ_MASK_CONFIGURED,
			       csid->base + CSID_PXL_IRQ_MASK(path));

	dev_dbg(csid->camss->dev,
		"CSID%u PXL path %u %s: src=%u en_vc=%#x %ux%u code=%#x dt=%#x decode=%#x cfg0=%#x mask=%#x status=%#x measure=%#x/%#x/%#x/%#x/%#x\n",
		csid->id, path, enable ? "enable" : "disable", src,
		csid->phy.en_vc, input_format->width, input_format->height,
		input_format->code, format->data_type, format->decode_format,
		readl_relaxed(csid->base + CSID_PXL_CFG0(path)),
		readl_relaxed(csid->base + CSID_PXL_IRQ_MASK(path)),
		readl_relaxed(csid->base + CSID_PXL_STATUS(path)),
		readl_relaxed(csid->base + CSID_PXL_FORMAT_MEASURE_CFG0(path)),
		readl_relaxed(csid->base + CSID_PXL_FORMAT_MEASURE_CFG1(path)),
		readl_relaxed(csid->base + CSID_PXL_FORMAT_MEASURE0(path)),
		readl_relaxed(csid->base + CSID_PXL_FORMAT_MEASURE1(path)),
		readl_relaxed(csid->base + CSID_PXL_FORMAT_MEASURE2(path)));
}

static void csid_pxl_irq_clear(struct csid_device *csid, u8 src, u8 path)
{
	u32 status = readl_relaxed(csid->base + CSID_PXL_IRQ_STATUS(path));
	u32 error = status & CSID_PXL_IRQ_ERROR;

	writel_relaxed(status, csid->base + CSID_PXL_IRQ_CLEAR(path));

	if ((status & CSID_PXL_IRQ_INPUT_SOF) && !csid->pxl_sof_vfe_reg_update) {
		csid->pxl_sof_vfe_reg_update = true;
		csid_reissue_vfe_reg_update(csid, src);
		dev_dbg(csid->camss->dev,
			"CSID%u PXL first SOF: reissued VFE reg_update\n",
			csid->id);
	}

	if (status)
		dev_dbg(csid->camss->dev,
			"CSID%u PXL path %u irq status: %#x error: %#x path-status: %#x measure: %#x/%#x/%#x/%#x/%#x\n",
			csid->id, path, status, error,
			readl_relaxed(csid->base + CSID_PXL_STATUS(path)),
			readl_relaxed(csid->base +
				      CSID_PXL_FORMAT_MEASURE_CFG0(path)),
			readl_relaxed(csid->base +
				      CSID_PXL_FORMAT_MEASURE_CFG1(path)),
			readl_relaxed(csid->base + CSID_PXL_FORMAT_MEASURE0(path)),
			readl_relaxed(csid->base + CSID_PXL_FORMAT_MEASURE1(path)),
			readl_relaxed(csid->base + CSID_PXL_FORMAT_MEASURE2(path)));
}

static void csid_configure_stream(struct csid_device *csid, u8 enable)
{
	struct csid_testgen_config *tg = &csid->testgen;
	u8 i;

	/* Source stream 3 is reserved for the future full-VFE PIX/IPP path. */
	for (i = 0; i < MSM_CSID_MAX_SRC_STREAMS; i++)
		if (csid->phy.en_vc & BIT(i)) {
			if (tg->enabled)
				__csid_configure_testgen(csid, enable, i);

			if (csid_use_pxl_stream(csid, i)) {
				__csid_configure_pxl_stream(csid, enable, i,
							    CSID_PXL_PATH_IPP);
				__csid_configure_rx(csid, &csid->phy, CSID_PXL_VC);
				csid->pxl_sof_vfe_reg_update = false;
				__csid_ctrl_pxl(csid, enable, CSID_PXL_PATH_IPP);
			} else {
				__csid_configure_rdi_stream(csid, enable, i);
				__csid_configure_rx(csid, &csid->phy, i);
				__csid_ctrl_rdi(csid, enable, i);
			}
		}
}

static int csid_configure_testgen_pattern(struct csid_device *csid, s32 val)
{
	if (val > 0 && val <= csid->testgen.nmodes)
		csid->testgen.mode = val;

	return 0;
}

/*
 * csid_isr - CSID module interrupt service routine
 * @irq: Interrupt line
 * @dev: CSID device
 *
 * Return IRQ_HANDLED on success
 */
static irqreturn_t csid_isr(int irq, void *dev)
{
	struct csid_device *csid = dev;
	u32 val;
	u8 reset_done;
	int i;

	val = readl_relaxed(csid->base + CSID_TOP_IRQ_STATUS);
	writel_relaxed(val, csid->base + CSID_TOP_IRQ_CLEAR);
	reset_done = val & BIT(TOP_IRQ_STATUS_RESET_DONE);
	if ((csid->phy.en_vc & BIT(CSID_PXL_SRC_STREAM)) && val)
		dev_dbg(csid->camss->dev,
			"CSID%u PXL top irq status: %#x en_vc: %#x\n",
			csid->id, val, csid->phy.en_vc);

	val = readl_relaxed(csid->base + CSID_CSI2_RX_IRQ_STATUS);
	writel_relaxed(val, csid->base + CSID_CSI2_RX_IRQ_CLEAR);
	if ((csid->phy.en_vc & BIT(CSID_PXL_SRC_STREAM)) && val)
		dev_dbg(csid->camss->dev,
			"CSID%u PXL rx irq status: %#x en_vc: %#x\n",
			csid->id, val, csid->phy.en_vc);

	/* Read and clear IRQ status for each enabled output path. */
	for (i = 0; i < MSM_CSID_MAX_SRC_STREAMS; i++)
		if (csid->phy.en_vc & BIT(i)) {
			if (csid_use_pxl_stream(csid, i)) {
				csid_pxl_irq_clear(csid, i, CSID_PXL_PATH_IPP);
			} else {
				val = readl_relaxed(csid->base + CSID_CSI2_RDIN_IRQ_STATUS(i));
				writel_relaxed(val, csid->base + CSID_CSI2_RDIN_IRQ_CLEAR(i));
			}
		}

	val = 1 << IRQ_CMD_CLEAR;
	writel_relaxed(val, csid->base + CSID_IRQ_CMD);

	if (reset_done)
		complete(&csid->reset_complete);

	return IRQ_HANDLED;
}

/*
 * csid_reset - Trigger reset on CSID module and wait to complete
 * @csid: CSID device
 *
 * Return 0 on success or a negative error code otherwise
 */
static int csid_reset(struct csid_device *csid)
{
	unsigned long time;
	u32 val;

	reinit_completion(&csid->reset_complete);

	writel_relaxed(1, csid->base + CSID_TOP_IRQ_CLEAR);
	writel_relaxed(1, csid->base + CSID_IRQ_CMD);
	writel_relaxed(1, csid->base + CSID_TOP_IRQ_MASK);
	writel_relaxed(1, csid->base + CSID_IRQ_CMD);

	/* preserve registers */
	val = 0x1e << RST_STROBES;
	writel_relaxed(val, csid->base + CSID_RST_STROBES);

	time = wait_for_completion_timeout(&csid->reset_complete,
					   msecs_to_jiffies(CSID_RESET_TIMEOUT_MS));
	if (!time) {
		dev_err(csid->camss->dev, "CSID reset timeout\n");
		return -EIO;
	}

	return 0;
}

static void csid_subdev_init(struct csid_device *csid)
{
	csid->testgen.modes = csid_testgen_modes;
	csid->testgen.nmodes = CSID_PAYLOAD_MODE_NUM_SUPPORTED_GEN2;
}

const struct csid_hw_ops csid_ops_gen2 = {
	.configure_stream = csid_configure_stream,
	.configure_testgen_pattern = csid_configure_testgen_pattern,
	.hw_version = csid_hw_version,
	.isr = csid_isr,
	.reset = csid_reset,
	.src_pad_code = csid_src_pad_code,
	.subdev_init = csid_subdev_init,
};
