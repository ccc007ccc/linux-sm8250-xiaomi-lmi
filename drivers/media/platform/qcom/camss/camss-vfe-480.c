// SPDX-License-Identifier: GPL-2.0
/*
 * camss-vfe-480.c
 *
 * Qualcomm MSM Camera Subsystem - VFE (Video Front End) Module v480 (SM8250)
 *
 * Copyright (C) 2020-2021 Linaro Ltd.
 * Copyright (C) 2021 Jonathan Marek
 */

#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/moduleparam.h>

#include "camss.h"
#include "camss-vfe.h"

static int vfe480_pix_mux_override = -1;
module_param_named(vfe480_pix_mux_override, vfe480_pix_mux_override, int, 0644);
MODULE_PARM_DESC(vfe480_pix_mux_override,
		 "override VFE480 PP input mux (0..3, -1 = use VFE id)");

static int vfe480_raw_dump_width_mode;
module_param_named(vfe480_raw_dump_width_mode, vfe480_raw_dump_width_mode, int, 0644);

static int vfe480_raw_dump_stride_mode;
module_param_named(vfe480_raw_dump_stride_mode, vfe480_raw_dump_stride_mode, int, 0644);

#define VFE_GLOBAL_RESET_CMD		(vfe_is_lite(vfe) ? 0x0c : 0x1c)
#define	    GLOBAL_RESET_HW_AND_REG	(vfe_is_lite(vfe) ? BIT(1) : BIT(0))

#define VFE_REG_UPDATE_CMD		(vfe_is_lite(vfe) ? 0x20 : 0x34)
#define VFE480_REG_UPDATE_PIX		(BIT(0) | BIT(6))
static inline int vfe480_reg_update(struct vfe_device *vfe, int n)
{
	if (n < VFE_LINE_RDI0 || n >= vfe->res->line_num)
		return 0;

	if (!vfe_is_lite(vfe) && n == VFE_LINE_PIX)
		return VFE480_REG_UPDATE_PIX;

	return vfe_is_lite(vfe) ? BIT(n) : BIT(1 + (n));
}

#define	    REG_UPDATE_RDI		vfe480_reg_update
#define VFE_IRQ_CMD			(vfe_is_lite(vfe) ? 0x24 : 0x38)
#define     IRQ_CMD_GLOBAL_CLEAR	BIT(0)

#define VFE_IRQ_MASK(n)			((vfe_is_lite(vfe) ? 0x28 : 0x3c) + (n) * 4)
#define	    IRQ_MASK_0_RESET_ACK	(vfe_is_lite(vfe) ? BIT(17) : BIT(0))
#define	    IRQ_MASK_0_BUS_TOP_IRQ	(vfe_is_lite(vfe) ? BIT(4) : BIT(7))
#define VFE480_IRQ_STATUS_0_CAMIF_ERROR	0x82000200
#define VFE480_IRQ_STATUS_2_CAMIF_ERROR	0x30301f80
#define VFE_IRQ_CLEAR(n)		((vfe_is_lite(vfe) ? 0x34 : 0x48) + (n) * 4)
#define VFE_IRQ_STATUS(n)		((vfe_is_lite(vfe) ? 0x40 : 0x54) + (n) * 4)

#define VFE_TOP_CORE_CGC_OVERRIDE_0	0x20
#define VFE_TOP_AHB_CGC_OVERRIDE	0x24
#define VFE_TOP_NOC_CGC_OVERRIDE	0x28
#define VFE_TOP_CORE_CFG_0		0x2c
#define VFE_TOP_CORE_CFG_1		0x30
#define		TOP_CORE_CFG_VID_DS_R2PD	(BIT(30) | BIT(29))
#define		TOP_CORE_CFG_DISP_DS_R2PD	(BIT(28) | BIT(27))
#define		TOP_CORE_CFG_DS_R2PD		(TOP_CORE_CFG_VID_DS_R2PD | TOP_CORE_CFG_DISP_DS_R2PD)
#define		TOP_CORE_CFG_OPERATING_MODE	11
#define		TOP_CORE_CFG_OPERATING_MODE_MASK	(0x3 << TOP_CORE_CFG_OPERATING_MODE)
#define		TOP_CORE_CFG_OPERATING_MODE_ONLINE	(0x1 << TOP_CORE_CFG_OPERATING_MODE)
#define		TOP_CORE_CFG_STATS_IHIST		BIT(10)
#define		TOP_CORE_CFG_STATS_HDR_BE	BIT(9)
#define		TOP_CORE_CFG_STATS_HDR_BHIST	BIT(8)
#define		TOP_CORE_CFG_STATS_SRC		(TOP_CORE_CFG_STATS_IHIST | TOP_CORE_CFG_STATS_HDR_BE | TOP_CORE_CFG_STATS_HDR_BHIST)
#define		TOP_CORE_CFG_INPUTMUX_PP_SHIFT	5
#define		TOP_CORE_CFG_INPUTMUX_PP(n)	(((n) & 0x3) << TOP_CORE_CFG_INPUTMUX_PP_SHIFT)
#define		TOP_CORE_CFG_INPUTMUX_PP_MASK	TOP_CORE_CFG_INPUTMUX_PP(0x3)
#define VFE_TOP_DIAG_CONFIG		0x64
#define VFE_TOP_DIAG_SENSOR_STATUS_0	0x68
#define VFE_TOP_VIOLATION_STATUS	0x74
#define VFE_TOP_DSP_STATUS		0x7c
#define VFE_TOP_DEBUG_0			0x80
#define VFE_TOP_DEBUG_1			0x84
#define VFE_TOP_DEBUG_2			0x88
#define VFE_TOP_DEBUG_3			0x8c
#define VFE_TOP_CORE_CGC_OVERRIDE_1	0x94
#define VFE_TOP_DIAG_SENSOR_STATUS_1	0x98
#define VFE_TOP_DEBUG_CFG		0xdc
#define		TOP_DEBUG_CFG_EN		BIT(0)

#define VFE480_CAMIF_HW_VERSION		0x2600
#define VFE480_CAMIF_HW_STATUS		0x2604
#define VFE480_CAMIF_MODULE_CFG		0x2660
#define		CAMIF_MODULE_CFG_EN		BIT(0)
#define		CAMIF_MODULE_CFG_IFE_OUT_EN	BIT(8)
#define VFE480_CAMIF_PDAF_RAW_CROP_WIDTH_CFG	0x2668
#define VFE480_CAMIF_PDAF_RAW_CROP_HEIGHT_CFG	0x266c
#define VFE480_CAMIF_LINE_SKIP_PATTERN	0x2670
#define VFE480_CAMIF_PIXEL_SKIP_PATTERN	0x2674
#define VFE480_CAMIF_PERIOD_CFG		0x2678
#define VFE480_CAMIF_IRQ_SUBSAMPLE_PATTERN	0x267c
#define VFE480_CAMIF_EPOCH_IRQ_CFG	0x2680
#define VFE480_CAMIF_DEBUG_1		0x27f0
#define VFE480_CAMIF_DEBUG_0		0x27f4
#define VFE480_CAMIF_TEST_BUS_CTRL	0x27f8
#define VFE480_CAMIF_SPARE		0x27fc
#define		CAMIF_EPOCH1_LINE_CFG		0x14

#define VFE480_PP_PREPROCESS_STATUS_0	0x2200
#define VFE480_PP_PREPROCESS_STATUS_1	0x2204
#define VFE480_PP_PREPROCESS_CFG		0x2260
#define VFE480_PP_PREPROCESS_DEBUG_0	0x23f8
#define VFE480_PP_PREPROCESS_DEBUG_1	0x23fc

#define VFE480_DEMUX_HW_VERSION		0x2800
#define VFE480_DEMUX_HW_STATUS		0x2804
#define VFE480_DEMUX_CLC_CFG		0x2808
#define VFE480_DEMUX_MODULE_CFG		0x2860
#define		DEMUX_MODULE_CFG_EN		BIT(0)
#define VFE480_DEMUX_EVEN_CFG		0x2868
#define VFE480_DEMUX_ODD_CFG		0x286c
#define VFE480_CHROMA_UP_MODULE_CFG	0x2a60
#define VFE480_PEDESTAL_MODULE_CFG	0x2c60
#define VFE480_LINEARIZATION_MODULE_CFG	0x2e60
#define VFE480_BPC_PDPC_MODULE_CFG	0x3060
#define VFE480_BPC_PDPC_DEMUX_CFG	0x3090
#define VFE480_HDR_BINCORRECT_MODULE_CFG	0x3260
#define VFE480_ABF_MODULE_CFG		0x3460
#define VFE480_LSC_MODULE_CFG		0x3660
#define VFE480_DEMOSAIC_MODULE_CFG	0x3860
#define		DEMOSAIC_MODULE_CFG_EN		BIT(0)
#define VFE480_COLOR_CORRECT_MODULE_CFG	0x3a60
#define		COLOR_CORRECT_MODULE_CFG_EN	BIT(0)
#define VFE480_GTM_MODULE_CFG		0x3c60
#define VFE480_GLUT_MODULE_CFG		0x3e60
#define VFE480_COLOR_XFORM_MODULE_CFG	0x4060
#define		COLOR_XFORM_MODULE_CFG_EN	BIT(0)
#define VFE480_COLOR_XFORM_CH0_COEFF_CFG_0	0x4068
#define VFE480_COLOR_XFORM_CH0_COEFF_CFG_1	0x406c
#define VFE480_COLOR_XFORM_CH0_OFFSET_CFG	0x4070
#define VFE480_COLOR_XFORM_CH0_CLAMP_CFG	0x4074
#define VFE480_COLOR_XFORM_CH1_COEFF_CFG_0	0x4078
#define VFE480_COLOR_XFORM_CH1_COEFF_CFG_1	0x407c
#define VFE480_COLOR_XFORM_CH1_OFFSET_CFG	0x4080
#define VFE480_COLOR_XFORM_CH1_CLAMP_CFG	0x4084
#define VFE480_COLOR_XFORM_CH2_COEFF_CFG_0	0x4088
#define VFE480_COLOR_XFORM_CH2_COEFF_CFG_1	0x408c
#define VFE480_COLOR_XFORM_CH2_OFFSET_CFG	0x4090
#define VFE480_COLOR_XFORM_CH2_CLAMP_CFG	0x4094
#define		CST12_COEFF_MASK		0x0fff
#define		CST12_10BIT_MASK		0x03ff
#define		CST12_COEFF(v)			((u32)(v) & CST12_COEFF_MASK)
#define		CST12_COEFF_CFG_0(g, b)		((CST12_COEFF(b) << 16) | \
						 CST12_COEFF(g))
#define		CST12_COEFF_CFG_1(r)		CST12_COEFF(r)
#define		CST12_OFFSET(v)			(((v) & CST12_10BIT_MASK) << 16)
#define		CST12_CLAMP(min, max)		((((max) & CST12_10BIT_MASK) << 16) | \
						 ((min) & CST12_10BIT_MASK))
#define VFE480_PIXEL_RAW_OUT_MODULE_CFG	0x4260
#define VFE480_VID_Y_MNDS_MODULE_CFG	0x6460
#define VFE480_VID_Y_MNDS_CFG		0x6464
#define VFE480_VID_Y_MNDS_IMAGE_SIZE_CFG	0x6468
#define VFE480_VID_Y_MNDS_H_CFG		0x646c
#define VFE480_VID_Y_MNDS_H_PHASE_CFG	0x6470
#define VFE480_VID_Y_MNDS_V_CFG		0x6474
#define VFE480_VID_Y_MNDS_V_PHASE_CFG	0x6478
#define VFE480_VID_Y_MNDS_CROP_LINE_CFG	0x647c
#define VFE480_VID_Y_MNDS_CROP_PIXEL_CFG	0x6480
#define VFE480_VID_C_MNDS_MODULE_CFG	0x6660
#define VFE480_VID_C_MNDS_CFG		0x6664
#define VFE480_VID_C_MNDS_IMAGE_SIZE_CFG	0x6668
#define VFE480_VID_C_MNDS_H_CFG		0x666c
#define VFE480_VID_C_MNDS_H_PHASE_CFG	0x6670
#define VFE480_VID_C_MNDS_V_CFG		0x6674
#define VFE480_VID_C_MNDS_V_PHASE_CFG	0x6678
#define VFE480_VID_C_MNDS_CROP_LINE_CFG	0x667c
#define VFE480_VID_C_MNDS_CROP_PIXEL_CFG	0x6680
#define		MNDS_MODULE_CFG_EN		BIT(0)
#define		MNDS_MODULE_CFG_CROP_EN		BIT(8)
#define		MNDS_CFG_H_SCALE_EN		BIT(9)
#define		MNDS_CFG_V_SCALE_EN		BIT(10)
#define		MNDS_14BIT_MASK			0x3fff
#define		MNDS_IMAGE_SIZE(w, h)		((((w) & MNDS_14BIT_MASK) << 16) | \
						 ((h) & MNDS_14BIT_MASK))
#define		MNDS_CROP(first, last)		((((first) & MNDS_14BIT_MASK) << 16) | \
						 ((last) & MNDS_14BIT_MASK))
#define		CROP_RND_CLAMP_VAL(min, max)	((((max) & 0x3ff) << 16) | \
						 ((min) & 0x3ff))
#define VFE480_VID_Y_CROP_RND_CLAMP_MODULE_CFG	0x6860
#define VFE480_VID_Y_POST_CROP_LINE_CFG	0x6868
#define VFE480_VID_Y_POST_CROP_PIXEL_CFG	0x686c
#define VFE480_VID_Y_CLAMP_CFG		0x6870
#define VFE480_VID_Y_ROUNDING_CFG	0x6874
#define VFE480_VID_C_CROP_RND_CLAMP_MODULE_CFG	0x6a60
#define VFE480_VID_C_POST_CROP_LINE_CFG	0x6a68
#define VFE480_VID_C_POST_CROP_PIXEL_CFG	0x6a6c
#define VFE480_VID_C_CLAMP_CFG		0x6a70
#define VFE480_VID_C_ROUNDING_CFG	0x6a74
#define		CROP_RND_CLAMP_MODULE_CFG_EN	BIT(0)
#define		CROP_RND_CLAMP_MODULE_CFG_CROP_EN	BIT(9)
#define		CROP_RND_CLAMP_MODULE_CFG_CH0_ROUND_EN	BIT(10)
#define		CROP_RND_CLAMP_MODULE_CFG_CH0_CLAMP_EN	BIT(11)
#define		CROP_RND_CLAMP_MODULE_CFG_CH1_ROUND_EN	BIT(12)
#define		CROP_RND_CLAMP_MODULE_CFG_CH1_CLAMP_EN	BIT(13)
#define		CROP_RND_CLAMP_MODULE_CFG_CH2_ROUND_EN	BIT(14)
#define		CROP_RND_CLAMP_MODULE_CFG_CH2_CLAMP_EN	BIT(15)
#define VFE480_BLS_MODULE_CFG		0x7c60

#define BUS_REG_BASE			(vfe_is_lite(vfe) ? 0x1a00 : 0xaa00)

#define VFE_BUS_WM_CGC_OVERRIDE		(BUS_REG_BASE + 0x08)
#define		WM_CGC_OVERRIDE_ALL	(0x3FFFFFF)

#define VFE_BUS_WM_TEST_BUS_CTRL	(BUS_REG_BASE + 0xdc)

#define VFE_BUS_IRQ_MASK(n)		(BUS_REG_BASE + 0x18 + (n) * 4)
static inline int bus_irq_mask_0_rdi_rup(struct vfe_device *vfe, int n)
{
	if (!vfe_is_lite(vfe) && n == VFE_LINE_PIX)
		return BIT(0);

	return vfe_is_lite(vfe) ? BIT(n) : BIT(3 + (n));
}

#define     BUS_IRQ_MASK_0_RDI_RUP	bus_irq_mask_0_rdi_rup
static inline int bus_irq_mask_0_comp_done(struct vfe_device *vfe, int n)
{
	return vfe_is_lite(vfe) ? BIT(4 + (n)) : BIT(6 + (n));
}

#define     BUS_IRQ_MASK_0_COMP_DONE	bus_irq_mask_0_comp_done
#define VFE_BUS_IRQ_CLEAR(n)		(BUS_REG_BASE + 0x20 + (n) * 4)
#define VFE_BUS_IRQ_STATUS(n)		(BUS_REG_BASE + 0x28 + (n) * 4)
#define VFE_BUS_IRQ_CLEAR_GLOBAL	(BUS_REG_BASE + 0x30)
#define VFE480_BUS_IRQ_STATUS0_CCIF_VIOLATION	BIT(30)
#define VFE480_BUS_IRQ_STATUS0_IMAGE_SIZE_VIOLATION	BIT(31)
#define VFE480_BUS_IRQ_STATUS0_VIOLATION		\
	(VFE480_BUS_IRQ_STATUS0_CCIF_VIOLATION | \
	 VFE480_BUS_IRQ_STATUS0_IMAGE_SIZE_VIOLATION)
#define VFE480_BUS_CCIF_VIOLATION_STATUS	(BUS_REG_BASE + 0x64)
#define VFE480_BUS_OVERFLOW_STATUS	(BUS_REG_BASE + 0x68)
#define VFE480_BUS_IMAGE_SIZE_VIOLATION_STATUS	(BUS_REG_BASE + 0x70)
#define VFE480_BUS_IMAGE_SIZE_RAW_DUMP	BIT(10)
#define VFE480_BUS_DEBUG_STATUS_TOP_CFG	(BUS_REG_BASE + 0xd4)
#define VFE480_BUS_DEBUG_STATUS_TOP	(BUS_REG_BASE + 0xd8)
#define VFE480_BUS_TEST_BUS_CTRL		(BUS_REG_BASE + 0xdc)

#define VFE_BUS_WM_CFG(n)		(BUS_REG_BASE + 0x200 + (n) * 0x100)
#define		WM_CFG_EN			(0)
#define		WM_CFG_MODE			(16)
#define			MODE_QCOM_PLAIN	(0)
#define			MODE_MIPI_RAW	(1)
#define VFE_BUS_WM_IMAGE_ADDR(n)	(BUS_REG_BASE + 0x204 + (n) * 0x100)
#define VFE_BUS_WM_FRAME_INCR(n)	(BUS_REG_BASE + 0x208 + (n) * 0x100)
#define VFE_BUS_WM_IMAGE_CFG_0(n)	(BUS_REG_BASE + 0x20c + (n) * 0x100)
#define		WM_IMAGE_CFG_0_DEFAULT_WIDTH	(0xFFFF)
#define VFE_BUS_WM_IMAGE_CFG_1(n)	(BUS_REG_BASE + 0x210 + (n) * 0x100)
#define VFE_BUS_WM_IMAGE_CFG_2(n)	(BUS_REG_BASE + 0x214 + (n) * 0x100)
#define VFE_BUS_WM_PACKER_CFG(n)	(BUS_REG_BASE + 0x218 + (n) * 0x100)
#define		VFE_BUS_WM_PACKER_FMT_PLAIN_8_LSB_MSB_10	3
#define		VFE_BUS_WM_PACKER_FMT_PLAIN16_10_LSB	0x15
#define VFE_BUS_WM_HEADER_ADDR(n)	(BUS_REG_BASE + 0x220 + (n) * 0x100)
#define VFE_BUS_WM_HEADER_INCR(n)	(BUS_REG_BASE + 0x224 + (n) * 0x100)
#define VFE_BUS_WM_HEADER_CFG(n)	(BUS_REG_BASE + 0x228 + (n) * 0x100)

#define VFE_BUS_WM_IRQ_SUBSAMPLE_PERIOD(n)	(BUS_REG_BASE + 0x230 + (n) * 0x100)
#define VFE_BUS_WM_IRQ_SUBSAMPLE_PATTERN(n)	(BUS_REG_BASE + 0x234 + (n) * 0x100)
#define VFE_BUS_WM_FRAMEDROP_PERIOD(n)		(BUS_REG_BASE + 0x238 + (n) * 0x100)
#define VFE_BUS_WM_FRAMEDROP_PATTERN(n)		(BUS_REG_BASE + 0x23c + (n) * 0x100)

#define VFE_BUS_WM_SYSTEM_CACHE_CFG(n)	(BUS_REG_BASE + 0x260 + (n) * 0x100)
#define VFE_BUS_WM_BURST_LIMIT(n)	(BUS_REG_BASE + 0x264 + (n) * 0x100)
#define VFE_BUS_WM_ADDR_STATUS_0(n)	(BUS_REG_BASE + 0x268 + (n) * 0x100)
#define VFE_BUS_WM_ADDR_STATUS_1(n)	(BUS_REG_BASE + 0x26c + (n) * 0x100)
#define VFE_BUS_WM_ADDR_STATUS_2(n)	(BUS_REG_BASE + 0x270 + (n) * 0x100)
#define VFE_BUS_WM_ADDR_STATUS_3(n)	(BUS_REG_BASE + 0x274 + (n) * 0x100)
#define VFE_BUS_WM_DEBUG_STATUS_CFG(n)	(BUS_REG_BASE + 0x278 + (n) * 0x100)
#define VFE_BUS_WM_DEBUG_STATUS_0(n)	(BUS_REG_BASE + 0x27c + (n) * 0x100)
#define VFE_BUS_WM_DEBUG_STATUS_1(n)	(BUS_REG_BASE + 0x280 + (n) * 0x100)

#define VFE480_BUS_CLIENT_FULL_Y		0
#define VFE480_BUS_CLIENT_FULL_C		1
#define VFE480_BUS_CLIENT_FULL_DISP_Y	4
#define VFE480_BUS_CLIENT_FULL_DISP_C	5
#define VFE480_BUS_CLIENT_RAW_DUMP	10
#define VFE480_BUS_CLIENT_RDI0		23
#define VFE480_COMP_GROUP_FULL		0
#define VFE480_COMP_GROUP_RAW_DUMP	3
#define VFE480_COMP_GROUP_RDI0		11

#define VFE480_BUS_CLIENT_FULL(plane)		(VFE480_BUS_CLIENT_FULL_Y + (plane))
#define VFE480_BUS_CLIENT_FULL_DISP(plane)	(VFE480_BUS_CLIENT_FULL_DISP_Y + (plane))

static u8 vfe480_rdi_bus_client(struct vfe_device *vfe, u8 rdi)
{
	return (vfe_is_lite(vfe) ? 0 : VFE480_BUS_CLIENT_RDI0) + rdi;
}

static u8 vfe480_rdi_comp_group(struct vfe_device *vfe, u8 rdi)
{
	return (vfe_is_lite(vfe) ? 0 : VFE480_COMP_GROUP_RDI0) + rdi;
}

static bool vfe480_is_raw_dump_pix_format(u32 pixelformat)
{
	return pixelformat == V4L2_PIX_FMT_SGRBG10;
}

static u8 vfe480_comp_group(struct vfe_device *vfe, enum vfe_line_id line_id)
{
	if (!vfe_is_lite(vfe) && line_id == VFE_LINE_PIX) {
		struct vfe_line *line = &vfe->line[VFE_LINE_PIX];
		struct v4l2_pix_format_mplane *pix =
			&line->video_out.active_fmt.fmt.pix_mp;

		if (vfe480_is_raw_dump_pix_format(pix->pixelformat))
			return VFE480_COMP_GROUP_RAW_DUMP;

		return VFE480_COMP_GROUP_FULL;
	}

	return vfe480_rdi_comp_group(vfe, line_id);
}

static bool vfe480_is_yc_pix_format(u32 pixelformat)
{
	switch (pixelformat) {
	case V4L2_PIX_FMT_NV12:
	case V4L2_PIX_FMT_NV21:
	case V4L2_PIX_FMT_NV16:
	case V4L2_PIX_FMT_NV61:
		return true;
	default:
		return false;
	}
}

static bool vfe480_is_full_pix_output(struct vfe_device *vfe,
					      struct vfe_line *line)
{
	return !vfe_is_lite(vfe) && line->id == VFE_LINE_PIX;
}

static bool vfe480_is_yc_pix_output(struct vfe_device *vfe,
				    struct vfe_line *line)
{
	struct v4l2_pix_format_mplane *pix =
		&line->video_out.active_fmt.fmt.pix_mp;

	return vfe480_is_full_pix_output(vfe, line) &&
	       vfe480_is_yc_pix_format(pix->pixelformat);
}

static bool vfe480_is_raw_dump_pix_output(struct vfe_device *vfe,
						  struct vfe_line *line)
{
	struct v4l2_pix_format_mplane *pix =
		&line->video_out.active_fmt.fmt.pix_mp;

	return vfe480_is_full_pix_output(vfe, line) &&
	       vfe480_is_raw_dump_pix_format(pix->pixelformat);
}

static int vfe480_yc_plane(struct vfe_line *line, u8 wm)
{
	struct vfe_output *output = &line->output;
	unsigned int i;

	for (i = 0; i < output->wm_num; i++)
		if (output->wm_idx[i] == wm)
			return i;

	return -EINVAL;
}

static bool vfe480_wm_is_registered(struct vfe_device *vfe, u8 wm)
{
	enum vfe_line_id line_id;

	if (wm >= MSM_VFE_IMAGE_MASTERS_NUM)
		return false;

	line_id = vfe->wm_output_map[wm];

	return line_id >= VFE_LINE_RDI0 && line_id < vfe->res->line_num;
}

static bool vfe480_yc_pix_layout_valid(struct vfe_device *vfe,
					       struct vfe_line *line)
{
	struct vfe_output *output = &line->output;
	unsigned int i;

	if (!vfe480_is_yc_pix_output(vfe, line) || output->wm_num != 2)
		return false;

	if (output->wm_idx[0] == output->wm_idx[1])
		return false;

	for (i = 0; i < 2; i++) {
		if (!vfe480_wm_is_registered(vfe, output->wm_idx[i]) ||
		    vfe->wm_output_map[output->wm_idx[i]] != line->id)
			return false;
	}

	return true;
}

static bool vfe480_yc_pp_chain_configured(struct vfe_device *vfe,
						  struct vfe_line *line)
{
	u32 demux;
	u32 demosaic;
	u32 color_xform;
	u32 y_mnds;
	u32 c_mnds;
	u32 y_crop;
	u32 c_crop;

	if (vfe_is_lite(vfe) || !line)
		return false;

	demux = readl_relaxed(vfe->base + VFE480_DEMUX_MODULE_CFG);
	demosaic = readl_relaxed(vfe->base + VFE480_DEMOSAIC_MODULE_CFG);
	color_xform = readl_relaxed(vfe->base + VFE480_COLOR_XFORM_MODULE_CFG);
	y_mnds = readl_relaxed(vfe->base + VFE480_VID_Y_MNDS_MODULE_CFG);
	c_mnds = readl_relaxed(vfe->base + VFE480_VID_C_MNDS_MODULE_CFG);
	y_crop = readl_relaxed(vfe->base + VFE480_VID_Y_CROP_RND_CLAMP_MODULE_CFG);
	c_crop = readl_relaxed(vfe->base + VFE480_VID_C_CROP_RND_CLAMP_MODULE_CFG);

	if (!(demux & DEMUX_MODULE_CFG_EN) ||
	    !(demosaic & DEMOSAIC_MODULE_CFG_EN) ||
	    !(color_xform & COLOR_XFORM_MODULE_CFG_EN) ||
	    !(y_mnds & MNDS_MODULE_CFG_EN) ||
	    !(c_mnds & MNDS_MODULE_CFG_EN) ||
	    !(y_crop & CROP_RND_CLAMP_MODULE_CFG_EN) ||
	    !(c_crop & CROP_RND_CLAMP_MODULE_CFG_EN))
		return false;

	/*
	 * Keep the Y/C path closed until the VFE480 common path has a
	 * validated open DEMUX + DEMOSAIC + color model.  The terminal
	 * VideoFull Y/C blocks and CST12 setup below are not sufficient to
	 * prove real processed YUV output, and copied Android CDM/DMI state
	 * must not be used as a static mainline recipe.
	 */
	return false;
}

static bool vfe480_yc_pix_ready(struct vfe_device *vfe, struct vfe_line *line)
{
	return vfe480_yc_pix_layout_valid(vfe, line) &&
	       vfe480_yc_pp_chain_configured(vfe, line);
}

static bool vfe480_raw_dump_pix_layout_valid(struct vfe_device *vfe,
						     struct vfe_line *line)
{
	struct vfe_output *output = &line->output;
	u8 wm;

	if (!vfe480_is_raw_dump_pix_output(vfe, line) || output->wm_num != 1)
		return false;

	wm = output->wm_idx[0];

	return vfe480_wm_is_registered(vfe, wm) &&
	       vfe->wm_output_map[wm] == line->id;
}

static bool vfe480_has_pix_output(struct vfe_device *vfe)
{
	struct vfe_line *line;

	if (vfe_is_lite(vfe) || vfe->res->line_num <= VFE_LINE_PIX)
		return false;

	line = &vfe->line[VFE_LINE_PIX];

	if (line->output.state != VFE_OUTPUT_RESERVED &&
	    line->output.state != VFE_OUTPUT_ON)
		return false;

	return vfe480_yc_pix_ready(vfe, line) ||
	       vfe480_raw_dump_pix_layout_valid(vfe, line);
}

static u32 vfe480_yc_plane_height(struct v4l2_pix_format_mplane *pix, int plane)
{
	if (plane == 0)
		return pix->height;

	switch (pix->pixelformat) {
	case V4L2_PIX_FMT_NV12:
	case V4L2_PIX_FMT_NV21:
		return pix->height / 2;
	case V4L2_PIX_FMT_NV16:
	case V4L2_PIX_FMT_NV61:
		return pix->height;
	default:
		return 0;
	}
}

static void vfe480_write_yc_mnds(struct vfe_device *vfe, u32 base,
					 u32 width, u32 height)
{
	writel_relaxed(0, vfe->base + base + 0x4);
	writel_relaxed(MNDS_IMAGE_SIZE(width, height),
		       vfe->base + base + 0x8);
	writel_relaxed(0, vfe->base + base + 0xc);
	writel_relaxed(0, vfe->base + base + 0x10);
	writel_relaxed(0, vfe->base + base + 0x14);
	writel_relaxed(0, vfe->base + base + 0x18);
	writel_relaxed(MNDS_CROP(0, height - 1), vfe->base + base + 0x1c);
	writel_relaxed(MNDS_CROP(0, width - 1), vfe->base + base + 0x20);
	writel_relaxed(MNDS_MODULE_CFG_EN | MNDS_MODULE_CFG_CROP_EN,
		       vfe->base + base);
	wmb();
}

static void vfe480_write_yc_crop_rnd_clamp(struct vfe_device *vfe, u32 base,
						   u32 width, u32 height)
{
	writel_relaxed(MNDS_CROP(0, height - 1), vfe->base + base + 0x8);
	writel_relaxed(MNDS_CROP(0, width - 1), vfe->base + base + 0xc);
	writel_relaxed(CROP_RND_CLAMP_VAL(0, 255), vfe->base + base + 0x10);
	writel_relaxed(0, vfe->base + base + 0x14);
	writel_relaxed(CROP_RND_CLAMP_MODULE_CFG_EN |
		       CROP_RND_CLAMP_MODULE_CFG_CROP_EN |
		       CROP_RND_CLAMP_MODULE_CFG_CH0_CLAMP_EN,
		       vfe->base + base);
	wmb();
}

static void vfe480_configure_cst12_bt601_full(struct vfe_device *vfe)
{
	writel_relaxed(CST12_COEFF_CFG_0(601, 117),
		       vfe->base + VFE480_COLOR_XFORM_CH0_COEFF_CFG_0);
	writel_relaxed(CST12_COEFF_CFG_1(306),
		       vfe->base + VFE480_COLOR_XFORM_CH0_COEFF_CFG_1);
	writel_relaxed(CST12_OFFSET(0),
		       vfe->base + VFE480_COLOR_XFORM_CH0_OFFSET_CFG);
	writel_relaxed(CST12_CLAMP(0, 1023),
		       vfe->base + VFE480_COLOR_XFORM_CH0_CLAMP_CFG);

	writel_relaxed(CST12_COEFF_CFG_0(-339, 512),
		       vfe->base + VFE480_COLOR_XFORM_CH1_COEFF_CFG_0);
	writel_relaxed(CST12_COEFF_CFG_1(-173),
		       vfe->base + VFE480_COLOR_XFORM_CH1_COEFF_CFG_1);
	writel_relaxed(CST12_OFFSET(512),
		       vfe->base + VFE480_COLOR_XFORM_CH1_OFFSET_CFG);
	writel_relaxed(CST12_CLAMP(0, 1023),
		       vfe->base + VFE480_COLOR_XFORM_CH1_CLAMP_CFG);

	writel_relaxed(CST12_COEFF_CFG_0(-429, -83),
		       vfe->base + VFE480_COLOR_XFORM_CH2_COEFF_CFG_0);
	writel_relaxed(CST12_COEFF_CFG_1(512),
		       vfe->base + VFE480_COLOR_XFORM_CH2_COEFF_CFG_1);
	writel_relaxed(CST12_OFFSET(512),
		       vfe->base + VFE480_COLOR_XFORM_CH2_OFFSET_CFG);
	writel_relaxed(CST12_CLAMP(0, 1023),
		       vfe->base + VFE480_COLOR_XFORM_CH2_CLAMP_CFG);

	writel_relaxed(COLOR_XFORM_MODULE_CFG_EN,
		       vfe->base + VFE480_COLOR_XFORM_MODULE_CFG);
	wmb();
}

static void vfe480_configure_yc_video_full(struct vfe_device *vfe,
						   struct vfe_line *line)
{
	struct v4l2_pix_format_mplane *pix =
		&line->video_out.active_fmt.fmt.pix_mp;
	u32 c_height;

	if (!vfe480_yc_pix_layout_valid(vfe, line))
		return;

	c_height = vfe480_yc_plane_height(pix, 1);
	if (!pix->width || !pix->height || !c_height)
		return;

	vfe480_configure_cst12_bt601_full(vfe);

	vfe480_write_yc_mnds(vfe, VFE480_VID_Y_MNDS_MODULE_CFG,
			       pix->width, pix->height);
	vfe480_write_yc_mnds(vfe, VFE480_VID_C_MNDS_MODULE_CFG,
			       pix->width, c_height);
	vfe480_write_yc_crop_rnd_clamp(vfe,
					 VFE480_VID_Y_CROP_RND_CLAMP_MODULE_CFG,
					 pix->width, pix->height);
	vfe480_write_yc_crop_rnd_clamp(vfe,
					 VFE480_VID_C_CROP_RND_CLAMP_MODULE_CFG,
					 pix->width, c_height);

	dev_dbg(vfe->camss->dev,
		"VFE%u PIX VideoFull Y/C terminal config: %ux%u chroma-height=%u fourcc=%#x\n",
		vfe->id, pix->width, pix->height, c_height,
		pix->pixelformat);
}

static int vfe480_bus_client(struct vfe_device *vfe, u8 wm,
				     struct vfe_line *line)
{
	enum vfe_line_id line_id;
	int plane;

	if (!vfe480_wm_is_registered(vfe, wm))
		return -EINVAL;

	line_id = vfe->wm_output_map[wm];
	if (line && line->id != line_id)
		return -EINVAL;

	if (!line || !vfe480_is_full_pix_output(vfe, line))
		return vfe480_rdi_bus_client(vfe, line_id);

	if (vfe480_raw_dump_pix_layout_valid(vfe, line))
		return VFE480_BUS_CLIENT_RAW_DUMP;

	if (!vfe480_yc_pix_ready(vfe, line))
		return -EINVAL;

	plane = vfe480_yc_plane(line, wm);
	if (plane < 0 || plane > 1)
		return -EINVAL;

	return VFE480_BUS_CLIENT_FULL(plane);
}

static bool vfe480_camif_is_enabled(struct vfe_device *vfe)
{
	return readl_relaxed(vfe->base + VFE480_CAMIF_MODULE_CFG) & CAMIF_MODULE_CFG_EN;
}

static void vfe480_dump_pix_bus_clients(struct vfe_device *vfe);
static void vfe480_dump_pix_common_detail(struct vfe_device *vfe,
					       const char *reason);
static void vfe480_dump_window_row(struct vfe_device *vfe, const char *name,
				       u32 offset);

static void vfe480_dump_pix_common_path(struct vfe_device *vfe, const char *reason)
{
	dev_err_ratelimited(vfe->camss->dev,
			    "VFE%u PIX %s DEMUX[0x2800] version=%#x status=%#x clc=%#x module=%#x even=%#x odd=%#x bpc-pdpc-demux=%#x demosaic=%#x color-correct=%#x color-xform=%#x\n",
			    vfe->id, reason,
			    readl_relaxed(vfe->base + VFE480_DEMUX_HW_VERSION),
			    readl_relaxed(vfe->base + VFE480_DEMUX_HW_STATUS),
			    readl_relaxed(vfe->base + VFE480_DEMUX_CLC_CFG),
			    readl_relaxed(vfe->base + VFE480_DEMUX_MODULE_CFG),
			    readl_relaxed(vfe->base + VFE480_DEMUX_EVEN_CFG),
			    readl_relaxed(vfe->base + VFE480_DEMUX_ODD_CFG),
			    readl_relaxed(vfe->base + VFE480_BPC_PDPC_DEMUX_CFG),
			    readl_relaxed(vfe->base + VFE480_DEMOSAIC_MODULE_CFG),
			    readl_relaxed(vfe->base + VFE480_COLOR_CORRECT_MODULE_CFG),
			    readl_relaxed(vfe->base + VFE480_COLOR_XFORM_MODULE_CFG));
	vfe480_dump_pix_common_detail(vfe, reason);
}

static void vfe480_dump_window_row(struct vfe_device *vfe, const char *name,
					   u32 offset)
{
	dev_err(vfe->camss->dev,
		"VFE%u PIX window %s[0x%04x]: %08x %08x %08x %08x %08x %08x %08x %08x\n",
		vfe->id, name, offset,
		readl_relaxed(vfe->base + offset),
		readl_relaxed(vfe->base + offset + 0x04),
		readl_relaxed(vfe->base + offset + 0x08),
		readl_relaxed(vfe->base + offset + 0x0c),
		readl_relaxed(vfe->base + offset + 0x10),
		readl_relaxed(vfe->base + offset + 0x14),
		readl_relaxed(vfe->base + offset + 0x18),
		readl_relaxed(vfe->base + offset + 0x1c));
}

static void vfe480_dump_pix_common_detail(struct vfe_device *vfe,
					       const char *reason)
{
	dev_err_ratelimited(vfe->camss->dev,
			    "VFE%u PIX %s BPC/PDPC demux[0x3090]: %08x %08x %08x %08x %08x %08x %08x\n",
			    vfe->id, reason,
			    readl_relaxed(vfe->base + VFE480_BPC_PDPC_DEMUX_CFG),
			    readl_relaxed(vfe->base + VFE480_BPC_PDPC_DEMUX_CFG + 0x04),
			    readl_relaxed(vfe->base + VFE480_BPC_PDPC_DEMUX_CFG + 0x08),
			    readl_relaxed(vfe->base + VFE480_BPC_PDPC_DEMUX_CFG + 0x0c),
			    readl_relaxed(vfe->base + VFE480_BPC_PDPC_DEMUX_CFG + 0x10),
			    readl_relaxed(vfe->base + VFE480_BPC_PDPC_DEMUX_CFG + 0x14),
			    readl_relaxed(vfe->base + VFE480_BPC_PDPC_DEMUX_CFG + 0x18));
	dev_err_ratelimited(vfe->camss->dev,
			    "VFE%u PIX %s DEMOSAIC[0x3860]: %08x %08x %08x %08x %08x %08x %08x %08x\n",
			    vfe->id, reason,
			    readl_relaxed(vfe->base + VFE480_DEMOSAIC_MODULE_CFG),
			    readl_relaxed(vfe->base + VFE480_DEMOSAIC_MODULE_CFG + 0x04),
			    readl_relaxed(vfe->base + VFE480_DEMOSAIC_MODULE_CFG + 0x08),
			    readl_relaxed(vfe->base + VFE480_DEMOSAIC_MODULE_CFG + 0x0c),
			    readl_relaxed(vfe->base + VFE480_DEMOSAIC_MODULE_CFG + 0x10),
			    readl_relaxed(vfe->base + VFE480_DEMOSAIC_MODULE_CFG + 0x14),
			    readl_relaxed(vfe->base + VFE480_DEMOSAIC_MODULE_CFG + 0x18),
			    readl_relaxed(vfe->base + VFE480_DEMOSAIC_MODULE_CFG + 0x1c));
}

static void vfe480_dump_pix_compare_windows(struct vfe_device *vfe,
						    const char *reason)
{
	static unsigned long dumped;
	unsigned long mask = BIT(vfe->id);
	static const struct {
		const char *name;
		u32 offset;
	} windows[] = {
		{ "pp_camif", 0x2600 },
		{ "pp_camif_dbg", 0x27e0 },
		{ "pp_modules", 0x2800 },
		{ "pp_modules", 0x2e00 },
		{ "pp_modules", 0x3000 },
		{ "pp_modules", 0x3400 },
		{ "pp_modules", 0x3600 },
		{ "pp_modules", 0x3c00 },
		{ "pp_modules", 0x3e00 },
		{ "pp_modules", 0x7c00 },
		{ "pp_modules", 0x8000 },
		{ "pp_modules", 0x8200 },
		{ "pp_modules", 0x8800 },
		{ "pp_modules", 0x8a00 },
		{ "clc_pdlib", 0xa600 },
		{ "bus_disp_y", 0xb000 },
		{ "bus_disp_c", 0xb100 },
		{ "bus_fd_y", 0xb400 },
		{ "bus_fd_c", 0xb500 },
		{ "bus_raw_dump", 0xb600 },
		{ "bus_camif_pd", 0xb700 },
		{ "bus_stats", 0xb800 },
		{ "bus_stats", 0xbb00 },
		{ "bus_stats", 0xbc00 },
		{ "bus_stats", 0xc000 },
		{ "bus_rdi0", 0xc300 },
		{ "bus_rdi1", 0xc400 },
	};
	unsigned int i;

	if (!vfe480_has_pix_output(vfe) || (dumped & mask))
		return;

	dumped |= mask;

	dev_err(vfe->camss->dev,
		"VFE%u PIX %s compare windows: one-shot read-only dump\n",
		vfe->id, reason);

	for (i = 0; i < ARRAY_SIZE(windows); i++)
		vfe480_dump_window_row(vfe, windows[i].name, windows[i].offset);
}

static void vfe480_dump_pix_state(struct vfe_device *vfe, const char *reason)
{
	if (!vfe480_has_pix_output(vfe))
		return;

	dev_err_ratelimited(vfe->camss->dev,
			    "VFE%u PIX %s: irq=%#x/%#x/%#x bus=%#x rup=%#x top=%#x/%#x diag-cfg=%#x diag=%#x/%#x dsp=%#x dbg=%#x/%#x/%#x/%#x camif=%#x/%#x/%#x/%#x/%#x\n",
			    vfe->id, reason,
			    readl_relaxed(vfe->base + VFE_IRQ_STATUS(0)),
			    readl_relaxed(vfe->base + VFE_IRQ_STATUS(1)),
			    readl_relaxed(vfe->base + VFE_IRQ_STATUS(2)),
			    readl_relaxed(vfe->base + VFE_BUS_IRQ_STATUS(0)),
			    vfe->reg_update,
			    readl_relaxed(vfe->base + VFE_TOP_CORE_CFG_0),
				    readl_relaxed(vfe->base + VFE_TOP_CORE_CFG_1),
				    readl_relaxed(vfe->base + VFE_TOP_DIAG_CONFIG),
			    readl_relaxed(vfe->base + VFE_TOP_DIAG_SENSOR_STATUS_0),
			    readl_relaxed(vfe->base + VFE_TOP_DIAG_SENSOR_STATUS_1),
				    readl_relaxed(vfe->base + VFE_TOP_DSP_STATUS),
			    readl_relaxed(vfe->base + VFE_TOP_DEBUG_0),
			    readl_relaxed(vfe->base + VFE_TOP_DEBUG_1),
			    readl_relaxed(vfe->base + VFE_TOP_DEBUG_2),
			    readl_relaxed(vfe->base + VFE_TOP_DEBUG_3),
			    readl_relaxed(vfe->base + VFE480_CAMIF_HW_STATUS),
			    readl_relaxed(vfe->base + VFE480_CAMIF_MODULE_CFG),
			    readl_relaxed(vfe->base + VFE480_CAMIF_DEBUG_0),
			    readl_relaxed(vfe->base + VFE480_CAMIF_DEBUG_1),
			    readl_relaxed(vfe->base + VFE480_CAMIF_SPARE));
	vfe480_dump_pix_bus_clients(vfe);
}

static void vfe480_configure_pp_top(struct vfe_device *vfe)
{
	u32 mux = vfe->id;
	u32 val;

	if (vfe480_pix_mux_override >= 0 && vfe480_pix_mux_override <= 3)
		mux = vfe480_pix_mux_override;

	val = readl_relaxed(vfe->base + VFE_TOP_CORE_CFG_0);
	val &= ~(TOP_CORE_CFG_DS_R2PD | TOP_CORE_CFG_OPERATING_MODE_MASK |
		 TOP_CORE_CFG_STATS_SRC | TOP_CORE_CFG_INPUTMUX_PP_MASK);
	val |= TOP_CORE_CFG_VID_DS_R2PD;
	val |= TOP_CORE_CFG_STATS_SRC;
	val |= TOP_CORE_CFG_INPUTMUX_PP(mux);
	val |= TOP_CORE_CFG_OPERATING_MODE_ONLINE;
	writel_relaxed(val, vfe->base + VFE_TOP_CORE_CFG_0);
}

static void vfe480_camif_start(struct vfe_device *vfe, struct vfe_line *line)
{
	u32 epoch0 = line->fmt[MSM_VFE_PAD_SINK].height / 4;
	bool camif_enabled = vfe480_camif_is_enabled(vfe);

	if (!epoch0)
		epoch0 = 1;

	vfe480_configure_pp_top(vfe);
	if (camif_enabled) {
		wmb();
		dev_dbg(vfe->camss->dev,
			"VFE%u PIX CAMIF reassert: core_cfg0=%#x module=%#x\n",
			vfe->id,
			readl_relaxed(vfe->base + VFE_TOP_CORE_CFG_0),
			readl_relaxed(vfe->base + VFE480_CAMIF_MODULE_CFG));
		return;
	}

	writel_relaxed(0xffffffff, vfe->base + VFE480_CAMIF_LINE_SKIP_PATTERN);
	writel_relaxed(0xffffffff, vfe->base + VFE480_CAMIF_PIXEL_SKIP_PATTERN);
	writel_relaxed(0, vfe->base + VFE480_CAMIF_PERIOD_CFG);
	writel_relaxed(0xffffffff, vfe->base + VFE480_CAMIF_IRQ_SUBSAMPLE_PATTERN);
	writel_relaxed((CAMIF_EPOCH1_LINE_CFG << 16) | epoch0,
		       vfe->base + VFE480_CAMIF_EPOCH_IRQ_CFG);
	writel_relaxed(0, vfe->base + VFE480_CAMIF_TEST_BUS_CTRL);
	writel_relaxed(CAMIF_MODULE_CFG_EN | CAMIF_MODULE_CFG_IFE_OUT_EN,
		       vfe->base + VFE480_CAMIF_MODULE_CFG);
	wmb();

	dev_dbg(vfe->camss->dev,
		"VFE%u PIX CAMIF start: core_cfg0=%#x module=%#x epoch=%#x\n",
		vfe->id, readl_relaxed(vfe->base + VFE_TOP_CORE_CFG_0),
		readl_relaxed(vfe->base + VFE480_CAMIF_MODULE_CFG),
		readl_relaxed(vfe->base + VFE480_CAMIF_EPOCH_IRQ_CFG));
}

static void vfe480_camif_stop(struct vfe_device *vfe)
{
	u32 val;

	writel_relaxed(0, vfe->base + VFE480_CAMIF_MODULE_CFG);
	if (!vfe_is_lite(vfe)) {
		val = readl_relaxed(vfe->base + VFE_IRQ_MASK(0));
		val &= ~VFE480_IRQ_STATUS_0_CAMIF_ERROR;
		writel_relaxed(val, vfe->base + VFE_IRQ_MASK(0));
		writel_relaxed(0, vfe->base + VFE_IRQ_MASK(2));
	}
	wmb();
}

static void vfe_global_reset(struct vfe_device *vfe)
{
	writel_relaxed(IRQ_MASK_0_RESET_ACK, vfe->base + VFE_IRQ_MASK(0));
	writel_relaxed(GLOBAL_RESET_HW_AND_REG, vfe->base + VFE_GLOBAL_RESET_CMD);
}

static u32 vfe480_raw_dump_dimension(struct v4l2_pix_format_mplane *pix, int mode)
{
	switch (mode) {
	case 1:
		return pix->plane_fmt[0].bytesperline;
	case 2:
		return ALIGN(pix->width * 2, 16) / 16;
	case 3:
		return DIV_ROUND_UP(pix->width * 10, 8);
	case 4:
		return ALIGN(pix->width * 2, 16);
	default:
		return pix->width;
	}
}

static void vfe_wm_start(struct vfe_device *vfe, u8 wm, struct vfe_line *line)
{
	struct v4l2_pix_format_mplane *pix =
		&line->video_out.active_fmt.fmt.pix_mp;
	bool raw_dump_pix = vfe480_is_raw_dump_pix_output(vfe, line);
	bool yc_pix = vfe480_is_yc_pix_output(vfe, line);
	u32 stride = pix->plane_fmt[0].bytesperline;
	u32 image_stride = stride;
	u32 height = pix->height;
	u32 frame_incr;
	u32 image_cfg_0 = WM_IMAGE_CFG_0_DEFAULT_WIDTH;
	u32 packer_cfg = 0;
	u32 mode = MODE_MIPI_RAW;
	int plane = -EINVAL;
	int hw_wm;

	if (yc_pix)
		vfe480_configure_yc_video_full(vfe, line);

	hw_wm = vfe480_bus_client(vfe, wm, line);
	if (hw_wm < 0) {
		dev_err(vfe->camss->dev, "Unsupported VFE480 wm %u format\n", wm);
		return;
	}

	if (raw_dump_pix) {
		u32 image_width = vfe480_raw_dump_dimension(pix,
							    vfe480_raw_dump_width_mode);

		image_stride = vfe480_raw_dump_dimension(pix,
							 vfe480_raw_dump_stride_mode);
		image_cfg_0 = (height << 16) | image_width;
		packer_cfg = VFE_BUS_WM_PACKER_FMT_PLAIN16_10_LSB;
		plane = 0;
		mode = MODE_QCOM_PLAIN;
	} else if (yc_pix) {
		plane = vfe480_yc_plane(line, wm);
		height = vfe480_yc_plane_height(pix, plane);
		image_cfg_0 = (height << 16) | pix->width;
		packer_cfg = VFE_BUS_WM_PACKER_FMT_PLAIN_8_LSB_MSB_10;
		mode = MODE_QCOM_PLAIN;
	}

	frame_incr = (raw_dump_pix ? image_stride : stride) * height;

	/* no clock gating at bus input */
	writel_relaxed(WM_CGC_OVERRIDE_ALL, vfe->base + VFE_BUS_WM_CGC_OVERRIDE);

	writel_relaxed(0x0, vfe->base + VFE_BUS_WM_TEST_BUS_CTRL);

	writel_relaxed(frame_incr, vfe->base + VFE_BUS_WM_FRAME_INCR(hw_wm));
	writel_relaxed(0xf, vfe->base + VFE_BUS_WM_BURST_LIMIT(hw_wm));
	writel_relaxed(image_cfg_0, vfe->base + VFE_BUS_WM_IMAGE_CFG_0(hw_wm));
	if (raw_dump_pix || yc_pix)
		writel_relaxed(0, vfe->base + VFE_BUS_WM_IMAGE_CFG_1(hw_wm));
	writel_relaxed(image_stride, vfe->base + VFE_BUS_WM_IMAGE_CFG_2(hw_wm));
	writel_relaxed(packer_cfg, vfe->base + VFE_BUS_WM_PACKER_CFG(hw_wm));

	/* no dropped frames, one irq per frame */
	writel_relaxed(0, vfe->base + VFE_BUS_WM_FRAMEDROP_PERIOD(hw_wm));
	writel_relaxed(1, vfe->base + VFE_BUS_WM_FRAMEDROP_PATTERN(hw_wm));
	writel_relaxed(0, vfe->base + VFE_BUS_WM_IRQ_SUBSAMPLE_PERIOD(hw_wm));
	writel_relaxed(1, vfe->base + VFE_BUS_WM_IRQ_SUBSAMPLE_PATTERN(hw_wm));

	writel_relaxed(1 << WM_CFG_EN | mode << WM_CFG_MODE,
		       vfe->base + VFE_BUS_WM_CFG(hw_wm));

	if (raw_dump_pix || yc_pix)
		dev_dbg(vfe->camss->dev,
			"VFE%u PIX WM%u client%d plane%d start: cfg=%#x image=%#x stride=%u image_stride=%u frame_incr=%u packer=%#x width-mode=%d stride-mode=%d\n",
			vfe->id, wm, hw_wm, plane,
			readl_relaxed(vfe->base + VFE_BUS_WM_CFG(hw_wm)),
			readl_relaxed(vfe->base + VFE_BUS_WM_IMAGE_CFG_0(hw_wm)),
			stride, image_stride, frame_incr,
			readl_relaxed(vfe->base + VFE_BUS_WM_PACKER_CFG(hw_wm)),
			raw_dump_pix ? vfe480_raw_dump_width_mode : -1,
			raw_dump_pix ? vfe480_raw_dump_stride_mode : -1);
}

static void vfe_wm_stop(struct vfe_device *vfe, u8 wm)
{
	struct vfe_line *line = NULL;
	bool stop_camif = false;
	int hw_wm;

	if (wm < MSM_VFE_IMAGE_MASTERS_NUM &&
	    vfe->wm_output_map[wm] == VFE_LINE_PIX &&
	    vfe->res->line_num > VFE_LINE_PIX) {
		line = &vfe->line[VFE_LINE_PIX];
		stop_camif = vfe480_raw_dump_pix_layout_valid(vfe, line) ||
			      (vfe480_yc_pix_ready(vfe, line) &&
			       vfe480_yc_plane(line, wm) == 0);
	}

	hw_wm = vfe480_bus_client(vfe, wm, line);
	if (hw_wm < 0)
		return;

	if (stop_camif) {
		vfe480_dump_pix_state(vfe, "stop");
		vfe480_camif_stop(vfe);
	}
	writel_relaxed(0, vfe->base + VFE_BUS_WM_CFG(hw_wm));
}

static void vfe_wm_update(struct vfe_device *vfe, u8 wm, u32 addr,
			  struct vfe_line *line)
{
	int hw_wm;

	hw_wm = vfe480_bus_client(vfe, wm, line);
	if (hw_wm < 0)
		return;

	writel_relaxed(addr, vfe->base + VFE_BUS_WM_IMAGE_ADDR(hw_wm));

	if (line && (vfe480_is_yc_pix_output(vfe, line) ||
		     vfe480_is_raw_dump_pix_output(vfe, line)))
		dev_dbg(vfe->camss->dev,
			"VFE%u PIX WM%u client%d addr update: %#x\n",
			vfe->id, wm, hw_wm, addr);
}

static void vfe_reg_update(struct vfe_device *vfe, enum vfe_line_id line_id)
{
	u32 mask = REG_UPDATE_RDI(vfe, line_id);

	if (!mask)
		return;

	if (line_id >= VFE_LINE_RDI0 && line_id < vfe->res->line_num) {
		struct vfe_line *line = &vfe->line[line_id];

		if (vfe480_yc_pix_ready(vfe, line) ||
		    vfe480_is_raw_dump_pix_output(vfe, line))
			vfe480_camif_start(vfe, line);
	}

	vfe->reg_update |= mask;
	writel_relaxed(vfe->reg_update, vfe->base + VFE_REG_UPDATE_CMD);

	if (!vfe_is_lite(vfe) && line_id == VFE_LINE_PIX)
		dev_dbg(vfe->camss->dev,
			"VFE%u PIX reg_update: mask=%#x cmd=%#x camif=%#x bus_mask=%#x\n",
			vfe->id, mask,
			readl_relaxed(vfe->base + VFE_REG_UPDATE_CMD),
			readl_relaxed(vfe->base + VFE480_CAMIF_MODULE_CFG),
			readl_relaxed(vfe->base + VFE_BUS_IRQ_MASK(0)));
}

static inline void vfe_reg_update_clear(struct vfe_device *vfe,
					enum vfe_line_id line_id)
{
	u32 mask = REG_UPDATE_RDI(vfe, line_id);

	if (!mask)
		return;

	vfe->reg_update &= ~mask;
}

static void vfe_enable_irq(struct vfe_device *vfe)
{
	struct vfe_output *pix_output = NULL;
	bool pix = vfe480_has_pix_output(vfe);
	int i;
	u32 bus_irq_mask = 0;
	u32 irq_mask = IRQ_MASK_0_RESET_ACK | IRQ_MASK_0_BUS_TOP_IRQ;

	if (!vfe_is_lite(vfe) && vfe->res->line_num > VFE_LINE_PIX)
		pix_output = &vfe->line[VFE_LINE_PIX].output;

	if (pix && !vfe_is_lite(vfe))
		irq_mask |= VFE480_IRQ_STATUS_0_CAMIF_ERROR;

	if (!vfe->stream_count)
		writel(irq_mask, vfe->base + VFE_IRQ_MASK(0));
	else if (pix && !vfe_is_lite(vfe))
		writel(readl_relaxed(vfe->base + VFE_IRQ_MASK(0)) | irq_mask,
		       vfe->base + VFE_IRQ_MASK(0));

	if (pix && !vfe_is_lite(vfe))
		writel(VFE480_IRQ_STATUS_2_CAMIF_ERROR,
		       vfe->base + VFE_IRQ_MASK(2));

	for (i = 0; i < vfe->res->line_num; i++) {
		/* Enable IRQ for newly added lines, but also keep already running lines's IRQ */
		if (vfe->line[i].output.state == VFE_OUTPUT_RESERVED ||
		    vfe->line[i].output.state == VFE_OUTPUT_ON) {
			bus_irq_mask |= BUS_IRQ_MASK_0_RDI_RUP(vfe, i)
					| BUS_IRQ_MASK_0_COMP_DONE(vfe,
								 vfe480_comp_group(vfe, i));
		}
	}

	if (pix)
		bus_irq_mask |= VFE480_BUS_IRQ_STATUS0_VIOLATION;

	writel(bus_irq_mask, vfe->base + VFE_BUS_IRQ_MASK(0));

	if (pix_output && (pix_output->state == VFE_OUTPUT_RESERVED ||
			 pix_output->state == VFE_OUTPUT_ON))
		dev_dbg(vfe->camss->dev,
			"VFE%u PIX irq enable: pix=%d irq0=%#x irq2=%#x bus_mask=%#x state=%u wm_num=%u wm=%u/%u\n",
			vfe->id, pix,
			readl_relaxed(vfe->base + VFE_IRQ_MASK(0)),
			readl_relaxed(vfe->base + VFE_IRQ_MASK(2)),
			readl_relaxed(vfe->base + VFE_BUS_IRQ_MASK(0)),
			pix_output->state, pix_output->wm_num,
			pix_output->wm_num ? pix_output->wm_idx[0] : 0xff,
			pix_output->wm_num > 1 ? pix_output->wm_idx[1] : 0xff);
}

static void vfe_isr_reg_update(struct vfe_device *vfe, enum vfe_line_id line_id);

static void vfe480_dump_pix_bus_clients(struct vfe_device *vfe)
{
	struct vfe_line *line = &vfe->line[VFE_LINE_PIX];
	struct vfe_output *output = &line->output;
	unsigned int i;

	if (!vfe480_yc_pix_ready(vfe, line) &&
	    !vfe480_raw_dump_pix_layout_valid(vfe, line))
		return;

	for (i = 0; i < output->wm_num; i++) {
		int hw_wm = vfe480_bus_client(vfe, output->wm_idx[i], line);

		if (hw_wm < 0)
			continue;

		dev_err_ratelimited(vfe->camss->dev,
				    "VFE%u PIX client%d regs: cfg=%#x addr=%#x frame_incr=%#x image=%#x/%#x/%#x packer=%#x burst=%#x\n",
				    vfe->id, hw_wm,
				    readl_relaxed(vfe->base + VFE_BUS_WM_CFG(hw_wm)),
				    readl_relaxed(vfe->base + VFE_BUS_WM_IMAGE_ADDR(hw_wm)),
				    readl_relaxed(vfe->base + VFE_BUS_WM_FRAME_INCR(hw_wm)),
				    readl_relaxed(vfe->base + VFE_BUS_WM_IMAGE_CFG_0(hw_wm)),
				    readl_relaxed(vfe->base + VFE_BUS_WM_IMAGE_CFG_1(hw_wm)),
				    readl_relaxed(vfe->base + VFE_BUS_WM_IMAGE_CFG_2(hw_wm)),
				    readl_relaxed(vfe->base + VFE_BUS_WM_PACKER_CFG(hw_wm)),
				    readl_relaxed(vfe->base + VFE_BUS_WM_BURST_LIMIT(hw_wm)));
		dev_err_ratelimited(vfe->camss->dev,
				    "VFE%u PIX client%d status: addr=%#x/%#x/%#x/%#x debug=%#x/%#x/%#x\n",
				    vfe->id, hw_wm,
				    readl_relaxed(vfe->base + VFE_BUS_WM_ADDR_STATUS_0(hw_wm)),
				    readl_relaxed(vfe->base + VFE_BUS_WM_ADDR_STATUS_1(hw_wm)),
				    readl_relaxed(vfe->base + VFE_BUS_WM_ADDR_STATUS_2(hw_wm)),
				    readl_relaxed(vfe->base + VFE_BUS_WM_ADDR_STATUS_3(hw_wm)),
				    readl_relaxed(vfe->base + VFE_BUS_WM_DEBUG_STATUS_CFG(hw_wm)),
				    readl_relaxed(vfe->base + VFE_BUS_WM_DEBUG_STATUS_0(hw_wm)),
				    readl_relaxed(vfe->base + VFE_BUS_WM_DEBUG_STATUS_1(hw_wm)));
	}

	vfe480_dump_pix_common_path(vfe, "state");
}

static void vfe480_isr_bus_violation(struct vfe_device *vfe, u32 status)
{
	u32 violation = status & VFE480_BUS_IRQ_STATUS0_VIOLATION;
	u32 image_size;

	if (!violation || !vfe480_has_pix_output(vfe))
		return;

	image_size = readl_relaxed(vfe->base +
					 VFE480_BUS_IMAGE_SIZE_VIOLATION_STATUS);

	dev_err_ratelimited(vfe->camss->dev,
			    "VFE%u bus violation status0: %#x ccif: %#x image-size: %#x%s overflow: %#x top-violation: %#x\n",
			    vfe->id, violation,
			    readl_relaxed(vfe->base + VFE480_BUS_CCIF_VIOLATION_STATUS),
			    image_size,
			    image_size & VFE480_BUS_IMAGE_SIZE_RAW_DUMP ?
			    " (PIXEL RAW DUMP)" : "",
			    readl_relaxed(vfe->base + VFE480_BUS_OVERFLOW_STATUS),
			    readl_relaxed(vfe->base + VFE_TOP_VIOLATION_STATUS));
	dev_err_ratelimited(vfe->camss->dev,
			    "VFE%u bus debug: top=%#x/%#x test=%#x wm-cgc=%#x\n",
			    vfe->id,
			    readl_relaxed(vfe->base + VFE480_BUS_DEBUG_STATUS_TOP_CFG),
			    readl_relaxed(vfe->base + VFE480_BUS_DEBUG_STATUS_TOP),
			    readl_relaxed(vfe->base + VFE480_BUS_TEST_BUS_CTRL),
			    readl_relaxed(vfe->base + VFE_BUS_WM_CGC_OVERRIDE));
	dev_err_ratelimited(vfe->camss->dev,
			    "VFE%u TOP debug: diag=%#x/%#x dbg=%#x/%#x/%#x/%#x core=%#x\n",
			    vfe->id,
			    readl_relaxed(vfe->base + VFE_TOP_DIAG_SENSOR_STATUS_0),
			    readl_relaxed(vfe->base + VFE_TOP_DIAG_SENSOR_STATUS_1),
			    readl_relaxed(vfe->base + VFE_TOP_DEBUG_0),
			    readl_relaxed(vfe->base + VFE_TOP_DEBUG_1),
			    readl_relaxed(vfe->base + VFE_TOP_DEBUG_2),
			    readl_relaxed(vfe->base + VFE_TOP_DEBUG_3),
			    readl_relaxed(vfe->base + VFE_TOP_CORE_CFG_0));
	dev_err_ratelimited(vfe->camss->dev,
			    "VFE%u PP CAMIF[0x2600] ver=%#x status=%#x module=%#x raw-crop=%#x/%#x skip=%#x/%#x period=%#x irq=%#x epoch=%#x debug=%#x/%#x test=%#x spare=%#x\n",
			    vfe->id,
			    readl_relaxed(vfe->base + VFE480_CAMIF_HW_VERSION),
			    readl_relaxed(vfe->base + VFE480_CAMIF_HW_STATUS),
			    readl_relaxed(vfe->base + VFE480_CAMIF_MODULE_CFG),
			    readl_relaxed(vfe->base + VFE480_CAMIF_PDAF_RAW_CROP_WIDTH_CFG),
			    readl_relaxed(vfe->base + VFE480_CAMIF_PDAF_RAW_CROP_HEIGHT_CFG),
			    readl_relaxed(vfe->base + VFE480_CAMIF_LINE_SKIP_PATTERN),
			    readl_relaxed(vfe->base + VFE480_CAMIF_PIXEL_SKIP_PATTERN),
			    readl_relaxed(vfe->base + VFE480_CAMIF_PERIOD_CFG),
			    readl_relaxed(vfe->base + VFE480_CAMIF_IRQ_SUBSAMPLE_PATTERN),
			    readl_relaxed(vfe->base + VFE480_CAMIF_EPOCH_IRQ_CFG),
			    readl_relaxed(vfe->base + VFE480_CAMIF_DEBUG_0),
			    readl_relaxed(vfe->base + VFE480_CAMIF_DEBUG_1),
			    readl_relaxed(vfe->base + VFE480_CAMIF_TEST_BUS_CTRL),
			    readl_relaxed(vfe->base + VFE480_CAMIF_SPARE));
	dev_err_ratelimited(vfe->camss->dev,
			    "VFE%u PP PREPROCESS[0x2200] status=%#x/%#x cfg=%#x debug=%#x/%#x\n",
			    vfe->id,
			    readl_relaxed(vfe->base + VFE480_PP_PREPROCESS_STATUS_0),
			    readl_relaxed(vfe->base + VFE480_PP_PREPROCESS_STATUS_1),
			    readl_relaxed(vfe->base + VFE480_PP_PREPROCESS_CFG),
			    readl_relaxed(vfe->base + VFE480_PP_PREPROCESS_DEBUG_0),
			    readl_relaxed(vfe->base + VFE480_PP_PREPROCESS_DEBUG_1));
		dev_err_ratelimited(vfe->camss->dev,
				    "VFE%u DEMUX[0x2800] version=%#x status=%#x clc=%#x module=%#x even=%#x odd=%#x bpc-pdpc-demux=%#x\n",
				    vfe->id,
				    readl_relaxed(vfe->base + VFE480_DEMUX_HW_VERSION),
				    readl_relaxed(vfe->base + VFE480_DEMUX_HW_STATUS),
				    readl_relaxed(vfe->base + VFE480_DEMUX_CLC_CFG),
				    readl_relaxed(vfe->base + VFE480_DEMUX_MODULE_CFG),
				    readl_relaxed(vfe->base + VFE480_DEMUX_EVEN_CFG),
				    readl_relaxed(vfe->base + VFE480_DEMUX_ODD_CFG),
				    readl_relaxed(vfe->base + VFE480_BPC_PDPC_DEMUX_CFG));
	vfe480_dump_pix_compare_windows(vfe, "bus-violation");
	vfe480_dump_pix_bus_clients(vfe);
}

static void vfe480_isr_camif_error(struct vfe_device *vfe, u32 status0,
					   u32 status2)
{
	u32 error0 = status0 & VFE480_IRQ_STATUS_0_CAMIF_ERROR;
	u32 error2 = status2 & VFE480_IRQ_STATUS_2_CAMIF_ERROR;

	if ((!error0 && !error2) || !vfe480_has_pix_output(vfe))
		return;

	dev_err_ratelimited(vfe->camss->dev,
			    "VFE%u CAMIF error status0: %#x status2: %#x\n",
			    vfe->id, error0, error2);
}

static void vfe480_isr_comp_done(struct vfe_device *vfe, u32 status)
{
	struct vfe_output *output;
	struct vfe_line *line;
	enum vfe_line_id line_id;
	unsigned int i;
	u8 comp_group;

	for (i = 0; i < MSM_VFE_IMAGE_MASTERS_NUM; i++) {
		line_id = vfe->wm_output_map[i];
		if (line_id < VFE_LINE_RDI0 || line_id >= vfe->res->line_num)
			continue;

		line = &vfe->line[line_id];
		output = &line->output;
		if (line_id == VFE_LINE_PIX &&
		    ((!vfe480_yc_pix_ready(vfe, line) &&
		      !vfe480_raw_dump_pix_layout_valid(vfe, line)) ||
		     i != output->wm_idx[0]))
			continue;

		comp_group = vfe480_comp_group(vfe, line_id);
		if (status & BUS_IRQ_MASK_0_COMP_DONE(vfe, comp_group)) {
			if (line_id == VFE_LINE_PIX)
				dev_dbg(vfe->camss->dev,
					"VFE%u PIX comp_done: status=%#x wm=%u active=%u\n",
					vfe->id, status, i,
					output->gen2.active_num);
			vfe_buf_done(vfe, i);
		}
	}
}

/*
 * vfe_isr - VFE module interrupt handler
 * @irq: Interrupt line
 * @dev: VFE device
 *
 * Return IRQ_HANDLED on success
 */
static irqreturn_t vfe_isr(int irq, void *dev)
{
	struct vfe_device *vfe = dev;
	bool pix = vfe480_has_pix_output(vfe);
	u32 status;
	u32 status1 = 0;
	u32 status2 = 0;
	int i;

	status = readl_relaxed(vfe->base + VFE_IRQ_STATUS(0));
	if (!vfe_is_lite(vfe) && pix) {
		status1 = readl_relaxed(vfe->base + VFE_IRQ_STATUS(1));
		status2 = readl_relaxed(vfe->base + VFE_IRQ_STATUS(2));
	}
	if (pix && (status || status1 || status2))
		dev_dbg(vfe->camss->dev,
			"VFE%u PIX irq: status0=%#x status1=%#x status2=%#x top-violation=%#x\n",
			vfe->id, status, status1, status2,
			readl_relaxed(vfe->base + VFE_TOP_VIOLATION_STATUS));
	if (pix && !(status & IRQ_MASK_0_BUS_TOP_IRQ) && (status || status1 || status2))
		vfe480_dump_pix_state(vfe, "non-bus-irq");

	writel_relaxed(status, vfe->base + VFE_IRQ_CLEAR(0));
	if (status1)
		writel_relaxed(status1, vfe->base + VFE_IRQ_CLEAR(1));
	if (status2)
		writel_relaxed(status2, vfe->base + VFE_IRQ_CLEAR(2));
	writel_relaxed(IRQ_CMD_GLOBAL_CLEAR, vfe->base + VFE_IRQ_CMD);

	if (status & IRQ_MASK_0_RESET_ACK)
		vfe_isr_reset_ack(vfe);

	vfe480_isr_camif_error(vfe, status, status2);

	if (status & IRQ_MASK_0_BUS_TOP_IRQ) {
		u32 bus_status = readl_relaxed(vfe->base + VFE_BUS_IRQ_STATUS(0));

		writel_relaxed(bus_status, vfe->base + VFE_BUS_IRQ_CLEAR(0));
		writel_relaxed(1, vfe->base + VFE_BUS_IRQ_CLEAR_GLOBAL);
		if (pix || (bus_status & (BUS_IRQ_MASK_0_RDI_RUP(vfe, VFE_LINE_PIX) |
						 BUS_IRQ_MASK_0_COMP_DONE(vfe, VFE480_COMP_GROUP_FULL) |
						 BUS_IRQ_MASK_0_COMP_DONE(vfe, VFE480_COMP_GROUP_RAW_DUMP))))
			dev_dbg(vfe->camss->dev,
				"VFE%u PIX bus irq: status=%#x reg_update=%#x\n",
				vfe->id, bus_status, vfe->reg_update);

		for (i = 0; i < vfe->res->line_num; i++) {
			if (bus_status & BUS_IRQ_MASK_0_RDI_RUP(vfe, i))
				vfe_isr_reg_update(vfe, i);
		}

		vfe480_isr_bus_violation(vfe, bus_status);
		vfe480_isr_comp_done(vfe, bus_status);
	}

	return IRQ_HANDLED;
}

/*
 * vfe_halt - Trigger halt on VFE module and wait to complete
 * @vfe: VFE device
 *
 * Return 0 on success or a negative error code otherwise
 */
static int vfe_halt(struct vfe_device *vfe)
{
	/* rely on vfe_disable_output() to stop the VFE */
	return 0;
}

/*
 * vfe_isr_reg_update - Process reg update interrupt
 * @vfe: VFE Device
 * @line_id: VFE line
 */
static void vfe_isr_reg_update(struct vfe_device *vfe, enum vfe_line_id line_id)
{
	struct vfe_output *output;
	unsigned long flags;

	if (line_id < VFE_LINE_RDI0 || line_id >= vfe->res->line_num)
		return;

	spin_lock_irqsave(&vfe->output_lock, flags);
	vfe_reg_update_clear(vfe, line_id);
	if (!vfe_is_lite(vfe) && line_id == VFE_LINE_PIX)
		dev_dbg(vfe->camss->dev,
			"VFE%u PIX RUP ack: reg_update=%#x\n",
			vfe->id, vfe->reg_update);

	output = &vfe->line[line_id].output;

	if (output->wait_reg_update) {
		output->wait_reg_update = 0;
		complete(&output->reg_update);
	}

	spin_unlock_irqrestore(&vfe->output_lock, flags);
}

static const struct camss_video_ops vfe_video_ops_480 = {
	.queue_buffer = vfe_queue_buffer_v2,
	.flush_buffers = vfe_flush_buffers,
};

static void vfe_subdev_init(struct device *dev, struct vfe_device *vfe)
{
	vfe->video_ops = vfe_video_ops_480;
}

static void vfe_isr_read(struct vfe_device *vfe, u32 *value0, u32 *value1)
{
	/* nop */
}

static void vfe_violation_read(struct vfe_device *vfe)
{
	/* nop */
}

static void vfe_buf_done_480(struct vfe_device *vfe, int port_id)
{
	/* nop */
}

const struct vfe_hw_ops vfe_ops_480 = {
	.enable_irq = vfe_enable_irq,
	.global_reset = vfe_global_reset,
	.hw_version = vfe_hw_version,
	.isr = vfe_isr,
	.isr_read = vfe_isr_read,
	.reg_update = vfe_reg_update,
	.reg_update_clear = vfe_reg_update_clear,
	.pm_domain_off = vfe_pm_domain_off,
	.pm_domain_on = vfe_pm_domain_on,
	.subdev_init = vfe_subdev_init,
	.vfe_disable = vfe_disable,
	.vfe_enable = vfe_enable_v2,
	.vfe_halt = vfe_halt,
	.violation_read = vfe_violation_read,
	.vfe_wm_start = vfe_wm_start,
	.vfe_wm_stop = vfe_wm_stop,
	.vfe_buf_done = vfe_buf_done_480,
	.vfe_wm_update = vfe_wm_update,
};
