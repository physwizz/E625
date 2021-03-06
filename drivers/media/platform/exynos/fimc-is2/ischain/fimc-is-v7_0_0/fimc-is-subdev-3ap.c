/*
 * Samsung Exynos5 SoC series FIMC-IS driver
 *
 * exynos5 fimc-is video functions
 *
 * Copyright (c) 2018 Samsung Electronics Co., Ltd
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include "fimc-is-device-ischain.h"
#include "fimc-is-device-sensor.h"
#include "fimc-is-subdev-ctrl.h"
#include "fimc-is-config.h"
#include "fimc-is-param.h"
#include "fimc-is-video.h"
#include "fimc-is-type.h"

#include "fimc-is-core.h"
#include "fimc-is-dvfs.h"
#include "fimc-is-hw-dvfs.h"

void fimc_is_ischain_3ap_stripe_cfg(struct fimc_is_subdev *subdev,
		struct fimc_is_frame *ldr_frame,
		struct fimc_is_crop *otcrop,
		u32 bitwidth)
{
	struct fimc_is_framemgr *framemgr;
	struct fimc_is_frame *frame;
	unsigned long flags;
	u32 stripe_x, stripe_w, dma_offset = 0;
	u32 region_id = ldr_frame->stripe_info.region_id;

	framemgr = GET_SUBDEV_FRAMEMGR(subdev);
	if (!framemgr)
		return;

	framemgr_e_barrier_irqs(framemgr, FMGR_IDX_24, flags);

	frame = peek_frame(framemgr, ldr_frame->state);
	if (frame) {
		/* Output crop & WDMA offset configuration */
		if (!region_id) {
			/* Left region */
			stripe_x = otcrop->x;
			stripe_w = ldr_frame->stripe_info.out.h_pix_num;

			frame->stripe_info.out.h_pix_num = stripe_w;
			frame->stripe_info.region_base_addr[0] = frame->dvaddr_buffer[0];
		} else if (region_id < ldr_frame->stripe_info.region_num - 1) {
			stripe_x = 0;
			stripe_w = ldr_frame->stripe_info.out.h_pix_num - ldr_frame->stripe_info.out.prev_h_pix_num;
			/**
			 * 3AA writes the right region with stripe margin.
			 * Add horizontal & vertical DMA offset.
			 */
			dma_offset = ldr_frame->stripe_info.out.prev_h_pix_num + (2 * region_id - 1) * STRIPE_MARGIN_WIDTH;
			dma_offset = dma_offset * bitwidth / BITS_PER_BYTE;
			dma_offset *= otcrop->h;

			frame->stripe_info.out.h_pix_num += stripe_w;
			stripe_w += STRIPE_MARGIN_WIDTH;
		} else {
			/* Right region */
			stripe_x = 0;
			stripe_w = ldr_frame->stripe_info.out.h_pix_num - ldr_frame->stripe_info.out.prev_h_pix_num;
			/**
			 * 3AA writes the right region with stripe margin.
			 * Add horizontal & vertical DMA offset.
			 */
			dma_offset = ldr_frame->stripe_info.out.prev_h_pix_num + (2 * region_id - 1) * STRIPE_MARGIN_WIDTH;
			dma_offset = dma_offset * bitwidth / BITS_PER_BYTE;
			dma_offset *= otcrop->h;

			frame->stripe_info.out.h_pix_num += stripe_w;
		}
		stripe_w += STRIPE_MARGIN_WIDTH;

		otcrop->x = stripe_x;
		otcrop->w = stripe_w;

		frame->dvaddr_buffer[0] = frame->stripe_info.region_base_addr[0] + dma_offset;
		frame->stream->stripe_h_pix_nums[region_id] = frame->stripe_info.out.h_pix_num;

		mdbg_pframe("stripe_ot_crop[%d][%d, %d, %d, %d] offset %x\n", subdev, subdev, ldr_frame,
				region_id,
				otcrop->x, otcrop->y, otcrop->w, otcrop->h, dma_offset);
	}

	framemgr_x_barrier_irqr(framemgr, FMGR_IDX_24, flags);
}

static int fimc_is_ischain_3ap_cfg(struct fimc_is_subdev *subdev,
	void *device_data,
	struct fimc_is_frame *frame,
	struct fimc_is_crop *incrop,
	struct fimc_is_crop *otcrop,
	u32 *lindex,
	u32 *hindex,
	u32 *indexes)
{
	return 0;
}

static int fimc_is_ischain_3ap_start(struct fimc_is_device_ischain *device,
	struct fimc_is_subdev *subdev,
	struct fimc_is_frame *frame,
	struct fimc_is_queue *queue,
	struct taa_param *taa_param,
	struct fimc_is_crop *otcrop,
	u32 *lindex,
	u32 *hindex,
	u32 *indexes)
{
	int ret = 0;
	struct fimc_is_group *group;
	struct param_dma_output *dma_output;
	struct fimc_is_module_enum *module;
	u32 hw_format, hw_bitwidth;
	struct fimc_is_crop otcrop_cfg;

	FIMC_BUG(!queue);
	FIMC_BUG(!queue->framecfg.format);

	group = &device->group_3aa;
	otcrop_cfg = *otcrop;

	hw_format = queue->framecfg.format->hw_format;
	hw_bitwidth = queue->framecfg.format->hw_bitwidth; /* memory width per pixel */

	ret = fimc_is_sensor_g_module(device->sensor, &module);
	if (ret) {
		merr("fimc_is_sensor_g_module is fail(%d)", device, ret);
		goto p_err;
	}

	if (IS_ENABLED(CHAIN_USE_STRIPE_PROCESSING) && frame && frame->stripe_info.region_num)
		fimc_is_ischain_3ap_stripe_cfg(subdev,
				frame,
				&otcrop_cfg,
				hw_bitwidth);

	if ((otcrop_cfg.w > taa_param->otf_input.bayer_crop_width) ||
		(otcrop_cfg.h > taa_param->otf_input.bayer_crop_height)) {
		mrerr("bds output size is invalid((%d, %d) > (%d, %d))", device, frame,
			otcrop_cfg.w,
			otcrop_cfg.h,
			taa_param->otf_input.bayer_crop_width,
			taa_param->otf_input.bayer_crop_height);
		ret = -EINVAL;
		goto p_err;
	}

	if (otcrop_cfg.x || otcrop_cfg.y) {
		mwarn("crop pos(%d, %d) is ignored", device, otcrop_cfg.x, otcrop_cfg.y);
		otcrop_cfg.x = 0;
		otcrop_cfg.y = 0;
	}

	/*
	 * 3AA BDS ratio limitation on width, height
	 * ratio = input * 256 / output
	 * real output = input * 256 / ratio
	 * real output &= ~1
	 * real output is same with output crop
	 */
	dma_output = fimc_is_itf_g_param(device, frame, subdev->param_dma_ot);
	dma_output->cmd = DMA_OUTPUT_COMMAND_ENABLE;
	dma_output->format = hw_format;
	dma_output->bitwidth = hw_bitwidth;
	dma_output->msb = MSB_OF_3AA_DMA_OUT;
#ifdef USE_3AA_CROP_AFTER_BDS
	if (test_bit(FIMC_IS_GROUP_OTF_INPUT, &group->state)) {
		dma_output->width = otcrop_cfg.w;
		dma_output->height = otcrop_cfg.h;
		dma_output->crop_enable = 0;
	} else {
		dma_output->width = taa_param->otf_input.bayer_crop_width;
		dma_output->height = taa_param->otf_input.bayer_crop_height;
		dma_output->crop_enable = 1;
	}
#else
	dma_output->width = otcrop_cfg.w;
	dma_output->height = otcrop_cfg.h;
	dma_output->crop_enable = 0;
#endif
	dma_output->dma_crop_offset_x = otcrop_cfg.x;
	dma_output->dma_crop_offset_y = otcrop_cfg.y;
	dma_output->dma_crop_width = otcrop_cfg.w;
	dma_output->dma_crop_height = otcrop_cfg.h;

	dma_output->stride_plane0 = otcrop->w;

	*lindex |= LOWBIT_OF(subdev->param_dma_ot);
	*hindex |= HIGHBIT_OF(subdev->param_dma_ot);
	(*indexes)++;

	subdev->output.crop = *otcrop;

	set_bit(FIMC_IS_SUBDEV_RUN, &subdev->state);

p_err:
	return ret;
}

static int fimc_is_ischain_3ap_stop(struct fimc_is_device_ischain *device,
	struct fimc_is_subdev *subdev,
	struct fimc_is_frame *frame,
	struct taa_param *taa_param,
	struct fimc_is_crop *otcrop,
	u32 *lindex,
	u32 *hindex,
	u32 *indexes)
{
	int ret = 0;
	struct fimc_is_group *group;
	struct param_dma_output *dma_output;

	mdbgd_ischain("%s\n", device, __func__);

	group = &device->group_3aa;

	if ((otcrop->w > taa_param->otf_input.bayer_crop_width) ||
		(otcrop->h > taa_param->otf_input.bayer_crop_height)) {
		mrerr("bds output size is invalid((%d, %d) > (%d, %d))", device, frame,
			otcrop->w,
			otcrop->h,
			taa_param->otf_input.bayer_crop_width,
			taa_param->otf_input.bayer_crop_height);
		ret = -EINVAL;
		goto p_err;
	}

	if (otcrop->x || otcrop->y) {
		mwarn("crop pos(%d, %d) is ignored", device, otcrop->x, otcrop->y);
		otcrop->x = 0;
		otcrop->y = 0;
	}

	dma_output = fimc_is_itf_g_param(device, frame, subdev->param_dma_ot);
	dma_output->cmd = DMA_OUTPUT_COMMAND_DISABLE;
#ifdef USE_3AA_CROP_AFTER_BDS
	if (test_bit(FIMC_IS_GROUP_OTF_INPUT, &group->state)) {
		dma_output->width = otcrop->w;
		dma_output->height = otcrop->h;
		dma_output->crop_enable = 0;
	} else {
		dma_output->width = taa_param->otf_input.bayer_crop_width;
		dma_output->height = taa_param->otf_input.bayer_crop_height;
		dma_output->crop_enable = 1;
	}
#else
	dma_output->width = otcrop->w;
	dma_output->height = otcrop->h;
	dma_output->crop_enable = 0;
#endif
	dma_output->dma_crop_offset_x = otcrop->x;
	dma_output->dma_crop_offset_y = otcrop->y;
	dma_output->dma_crop_width = otcrop->w;
	dma_output->dma_crop_height = otcrop->h;
	*lindex |= LOWBIT_OF(subdev->param_dma_ot);
	*hindex |= HIGHBIT_OF(subdev->param_dma_ot);
	(*indexes)++;

	clear_bit(FIMC_IS_SUBDEV_RUN, &subdev->state);

p_err:
	return ret;
}

static int fimc_is_ischain_3ap_tag(struct fimc_is_subdev *subdev,
	void *device_data,
	struct fimc_is_frame *ldr_frame,
	struct camera2_node *node)
{
	int ret = 0;
	struct fimc_is_subdev *leader;
	struct fimc_is_queue *queue;
	struct taa_param *taa_param;
	struct fimc_is_crop *otcrop, otparm;
	struct fimc_is_device_ischain *device;
	u32 lindex, hindex, indexes;
	u32 pixelformat = 0;
	int scenario_id = -1;

	device = (struct fimc_is_device_ischain *)device_data;

	FIMC_BUG(!device);
	FIMC_BUG(!device->is_region);
	FIMC_BUG(!subdev);
	FIMC_BUG(!GET_SUBDEV_QUEUE(subdev));
	FIMC_BUG(!ldr_frame);
	FIMC_BUG(!ldr_frame->shot);
	FIMC_BUG(!node);

	mdbgs_ischain(4, "3AAP TAG(request %d)\n", device, node->request);

	lindex = hindex = indexes = 0;
	leader = subdev->leader;
	taa_param = &device->is_region->parameter.taa;
	queue = GET_SUBDEV_QUEUE(subdev);
	if (!queue) {
		merr("queue is NULL", device);
		ret = -EINVAL;
		goto p_err;
	}

	if (!queue->framecfg.format) {
		merr("format is NULL", device);
		ret = -EINVAL;
		goto p_err;
	}
#ifdef ENABLE_DVFS
	scenario_id = device->resourcemgr->dvfs_ctrl.static_ctrl->cur_scenario_id;
#endif
	pixelformat = queue->framecfg.format->pixelformat;
	otcrop = (struct fimc_is_crop *)node->output.cropRegion;

	otparm.x = 0;
	otparm.y = 0;
	otparm.w = taa_param->vdma2_output.width;
	otparm.h = taa_param->vdma2_output.height;

	if (IS_NULL_CROP(otcrop))
		*otcrop = otparm;

	if (node->request) {
		if (!COMPARE_CROP(otcrop, &otparm) ||
			CHECK_STRIPE_CFG(&ldr_frame->stripe_info) ||
			!test_bit(FIMC_IS_SUBDEV_RUN, &subdev->state) ||
			test_bit(FIMC_IS_SUBDEV_FORCE_SET, &leader->state)) {
			ret = fimc_is_ischain_3ap_start(device,
				subdev,
				ldr_frame,
				queue,
				taa_param,
				otcrop,
				&lindex,
				&hindex,
				&indexes);
			if (ret) {
				merr("fimc_is_ischain_3ap_start is fail(%d)", device, ret);
				goto p_err;
			}

			if (!FIMC_IS_SKIP_PERFRAME_LOG(scenario_id))
				mdbg_pframe("ot_crop[%d, %d, %d, %d] on\n", device, subdev, ldr_frame,
					otcrop->x, otcrop->y, otcrop->w, otcrop->h);
		}

		ret = fimc_is_ischain_buf_tag(device,
			subdev,
			ldr_frame,
			pixelformat,
			otcrop->w,
			otcrop->h,
			ldr_frame->txpTargetAddress);
		if (ret) {
			mswarn("%d frame is drop", device, subdev, ldr_frame->fcount);
			node->request = 0;
		}
	} else {
		if (!COMPARE_CROP(otcrop, &otparm) ||
			test_bit(FIMC_IS_SUBDEV_RUN, &subdev->state) ||
			test_bit(FIMC_IS_SUBDEV_FORCE_SET, &leader->state)) {
			ret = fimc_is_ischain_3ap_stop(device,
				subdev,
				ldr_frame,
				taa_param,
				otcrop,
				&lindex,
				&hindex,
				&indexes);
			if (ret) {
				merr("fimc_is_ischain_3ap_stop is fail(%d)", device, ret);
				goto p_err;
			}

			if (!FIMC_IS_SKIP_PERFRAME_LOG(scenario_id))
				mdbg_pframe("ot_crop[%d, %d, %d, %d] off\n", device, subdev, ldr_frame,
					otcrop->x, otcrop->y, otcrop->w, otcrop->h);
		}

		ldr_frame->txpTargetAddress[0] = 0;
		ldr_frame->txpTargetAddress[1] = 0;
		ldr_frame->txpTargetAddress[2] = 0;
		node->request = 0;
	}

	ret = fimc_is_itf_s_param(device, ldr_frame, lindex, hindex, indexes);
	if (ret) {
		mrerr("fimc_is_itf_s_param is fail(%d)", device, ldr_frame, ret);
		goto p_err;
	}

p_err:
	return ret;
}

const struct fimc_is_subdev_ops fimc_is_subdev_3ap_ops = {
	.bypass			= NULL,
	.cfg			= fimc_is_ischain_3ap_cfg,
	.tag			= fimc_is_ischain_3ap_tag,
};
