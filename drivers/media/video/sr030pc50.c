/* drivers/media/video/sr030pc50.c
 *
 * Copyright (c) 2010, Samsung Electronics. All rights reserved
 * Author: dongseong.lim
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * - change date: 2012.06.28
 */
#include "sr030pc50.h"
#include <linux/gpio.h>
#include <linux/regulator/consumer.h>

#define sr030pc50_readb(sd, addr, data) sr030pc50_i2c_read(sd, addr, data)
#define sr030pc50_writeb(sd, addr, data) sr030pc50_i2c_write(sd, addr, data)

static u32 dbg_level = CAMDBG_LEVEL_DEFAULT;
static u32 stats_power;
static u32 stats_init;
static u32 stats_reset;
static u32 stats_i2c_err;

#ifdef CONFIG_MACH_GARDA

static struct regulator *vt_cam_sensor_a2v8_regulator = NULL;
static struct regulator *vt_cam_core_1v8_regulator = NULL;
static struct regulator *vt_cam_io_1v8_regulator = NULL;
static struct regulator *main_cam_core_1v2_regulator = NULL;
#endif
static const struct sr030pc50_fps sr030pc50_framerates[] = {
	{ I_FPS_0,	FRAME_RATE_AUTO },
	{ I_FPS_7,	FRAME_RATE_7},
	{ I_FPS_15,	FRAME_RATE_15 },
	{ I_FPS_20,	FRAME_RATE_20 },
	{ I_FPS_25,	FRAME_RATE_25 },
};

static const struct sr030pc50_framesize sr030pc50_preview_frmsizes[] = {
#if defined(CONFIG_MACH_GARDA)
	{ PREVIEW_SZ_528x432,	528,  432 },
#endif
	{ PREVIEW_SZ_VGA,	640,  480 },
};

static const struct sr030pc50_framesize sr030pc50_capture_frmsizes[] = {
/*	{ CAPTURE_SZ_VGA,	640,  480 },*/
	{ CAPTURE_SZ_1MP,	1280, 960 },
};

static struct sr030pc50_control sr030pc50_ctrls[] = {
	SR030PC50_INIT_CONTROL(V4L2_CID_CAMERA_FLASH_MODE, \
					FLASH_MODE_OFF),

	SR030PC50_INIT_CONTROL(V4L2_CID_CAM_BRIGHTNESS, \
					EV_DEFAULT),

	SR030PC50_INIT_CONTROL(V4L2_CID_CAM_METERING, \
					METERING_MATRIX),

	SR030PC50_INIT_CONTROL(V4L2_CID_WHITE_BALANCE_PRESET, \
					WHITE_BALANCE_AUTO),

	SR030PC50_INIT_CONTROL(V4L2_CID_IMAGE_EFFECT, \
					IMAGE_EFFECT_NONE),

	SR030PC50_INIT_CONTROL(V4L2_CID_CAMERA_FRAME_RATE, \
					FRAME_RATE_AUTO),
};

static const struct sr030pc50_regs reg_datas = {
	.ev = {
		SR030PC50_REGSET(GET_EV_INDEX(EV_MINUS_4),
				sr030pc50_ev_minus_4_regs, 0),
		SR030PC50_REGSET(GET_EV_INDEX(EV_MINUS_3),
				sr030pc50_ev_minus_3_regs, 0),
		SR030PC50_REGSET(GET_EV_INDEX(EV_MINUS_2),
				sr030pc50_ev_minus_2_regs, 0),
		SR030PC50_REGSET(GET_EV_INDEX(EV_MINUS_1),
				sr030pc50_ev_minus_1_regs, 0),
		SR030PC50_REGSET(GET_EV_INDEX(EV_DEFAULT),
				sr030pc50_ev_default_regs, 0),
		SR030PC50_REGSET(GET_EV_INDEX(EV_PLUS_1),
				sr030pc50_ev_plus_1_regs, 0),
		SR030PC50_REGSET(GET_EV_INDEX(EV_PLUS_2),
				sr030pc50_ev_plus_2_regs, 0),
		SR030PC50_REGSET(GET_EV_INDEX(EV_PLUS_3),
				sr030pc50_ev_plus_3_regs, 0),
		SR030PC50_REGSET(GET_EV_INDEX(EV_PLUS_4),
				sr030pc50_ev_plus_4_regs, 0),
	},
	.metering = {
		SR030PC50_REGSET(METERING_MATRIX, sr030pc50_metering_matrix_regs, 0),
		SR030PC50_REGSET(METERING_CENTER, sr030pc50_metering_center_regs, 0),
		SR030PC50_REGSET(METERING_SPOT, sr030pc50_metering_spot_regs, 0),
	},
	.iso = {
		/*SR030PC50_REGSET(ISO_AUTO, sr030pc50_iso_auto_regs, 0),*/
	},
	.effect = {
		SR030PC50_REGSET(IMAGE_EFFECT_NONE, sr030pc50_effect_normal_regs, 0),
		SR030PC50_REGSET(IMAGE_EFFECT_BNW, sr030pc50_effect_mono_regs, 0),
		SR030PC50_REGSET(IMAGE_EFFECT_SEPIA, sr030pc50_effect_sepia_regs, 0),
		SR030PC50_REGSET(IMAGE_EFFECT_NEGATIVE,
				sr030pc50_effect_negative_regs, 0),
		/*SR030PC50_REGSET(IMAGE_EFFECT_SOLARIZE, sr030pc50_Effect_Solar, 0),
		SR030PC50_REGSET(IMAGE_EFFECT_SKETCH, sr030pc50_Effect_Sketch, 0),
		SR030PC50_REGSET(IMAGE_EFFECT_POINT_COLOR_3,
				sr030pc50_Effect_Pastel, 0),*/
	},
	.white_balance = {
		SR030PC50_REGSET(WHITE_BALANCE_AUTO, sr030pc50_wb_auto_regs, 0),
		SR030PC50_REGSET(WHITE_BALANCE_SUNNY, sr030pc50_wb_daylight_regs, 0),
		SR030PC50_REGSET(WHITE_BALANCE_CLOUDY, sr030pc50_wb_cloudy_regs, 0),
		SR030PC50_REGSET(WHITE_BALANCE_TUNGSTEN, sr030pc50_wb_incandescent_regs, 0),
		SR030PC50_REGSET(WHITE_BALANCE_FLUORESCENT,
				sr030pc50_wb_fluorescent_regs, 0),
	},
	.fps = {
		SR030PC50_REGSET(I_FPS_0, sr030pc50_fps_auto_regs, 0),
		SR030PC50_REGSET(I_FPS_7, sr030pc50_fps_7_regs, 0),
		SR030PC50_REGSET(I_FPS_15, sr030pc50_fps_15_regs, 0),
		SR030PC50_REGSET(I_FPS_20, sr030pc50_fps_20_regs, 0),
		SR030PC50_REGSET(I_FPS_25, sr030pc50_fps_25_regs, 0),
		SR030PC50_REGSET(I_FPS_30, sr030pc50_fps_30_regs, 0),
	},
	.preview_size = {
#if defined(CONFIG_MACH_GARDA)
		SR030PC50_REGSET(PREVIEW_SZ_VGA, sr030pc50_640_480_size_regs, 0),
		SR030PC50_REGSET(PREVIEW_SZ_528x432, sr030pc50_528_432_size_regs, 0),
#endif
	},
	.capture_size = {
		/*SR030PC50_REGSET(CAPTURE_SZ_VGA, sr030pc50_VGA_Capture, 0),*/
	},

	.init_reg = SR030PC50_REGSET_TABLE(sr030pc50_set_init_regs, 0),
	.VT_init_reg = SR030PC50_REGSET_TABLE(sr030pc50_vt_mode_regs, 0),
	.SS_init_reg = SR030PC50_REGSET_TABLE(sr030pc50_init_regs_smart_stay, 0),
	/* Camera mode */
	/*
	.preview_mode = SR030PC50_REGSET_TABLE(SR130PC20_preview_mode, 0),
	.capture_mode = SR030PC50_REGSET_TABLE(SR130PC20_capture_mode, 0),
	.capture_mode_night =
		SR030PC50_REGSET_TABLE(SR130PC20_Lowlux_Night_Capture_Mode, 0),
		*/
	.stream_stop = SR030PC50_REGSET_TABLE(sr030pc50_stream_off, 0),
	.stream_start = SR030PC50_REGSET_TABLE(sr030pc50_stream_on, 0),
};

static const struct v4l2_mbus_framefmt capture_fmts[] = {
	{
		.code		= V4L2_MBUS_FMT_FIXED,
		.colorspace	= V4L2_COLORSPACE_JPEG,
	},
};

#ifdef FIND_OPRMODE_ENABLE
/**
 * __find_oprmode - Lookup SR030PC50 resolution type according to pixel code
 * @code: pixel code
 */
static enum sr030pc50_oprmode __find_oprmode(enum v4l2_mbus_pixelcode code)
{
	enum sr030pc50_oprmode type = SR030PC50_OPRMODE_VIDEO;

	do {
		if (code == default_fmt[type].code)
			return type;
	} while (type++ != SIZE_DEFAULT_FFMT);

	return 0;
}
#endif


/**
 * __find_resolution - Lookup preset and type of M-5MOLS's resolution
 * @mf: pixel format to find/negotiate the resolution preset for
 * @type: M-5MOLS resolution type
 * @resolution:	M-5MOLS resolution preset register value
 *
 * Find nearest resolution matching resolution preset and adjust mf
 * to supported values.
 */
static int __find_resolution(struct v4l2_subdev *sd,
			     struct v4l2_mbus_framefmt *mf,
			     enum sr030pc50_oprmode *type,
			     u32 *resolution)
{
	struct sr030pc50_state *state =
		container_of(sd, struct sr030pc50_state, sd);
	const struct sr030pc50_resolution *fsize = &sr030pc50_resolutions[0];
	const struct sr030pc50_resolution *match = NULL;
#ifdef FIND_OPRMODE_ENABLE
	enum sr030pc50_oprmode stype = __find_oprmode(mf->code);
#else
	enum sr030pc50_oprmode stype = state->oprmode;
#endif
	int i = ARRAY_SIZE(sr030pc50_resolutions);
	unsigned int min_err = ~0;
	int err;

	while (i--) {
		if (stype == fsize->type) {
			err = abs(fsize->width - mf->width)
				+ abs(fsize->height - mf->height);

			if (err < min_err) {
				min_err = err;
				match = fsize;
				stype = fsize->type;
			}
		}
		fsize++;
	}
	pr_debug("LINE(%d): mf width: %d, mf height: %d, mf code: %d\n", __LINE__,
		mf->width, mf->height, stype);
	pr_debug("LINE(%d): match width: %d, match height: %d, match code: %d\n", __LINE__,
		match->width, match->height, stype);
	if (match) {
		mf->width  = match->width;
		mf->height = match->height;
		*resolution = match->value;
		*type = stype;
		return 0;
	}
	pr_debug("LINE(%d): mf width: %d, mf height: %d, mf code: %d\n", __LINE__,
		mf->width, mf->height, stype);

	return -EINVAL;
}

static struct v4l2_mbus_framefmt *__find_format(struct sr030pc50_state *state,
				struct v4l2_subdev_fh *fh,
				enum v4l2_subdev_format_whence which,
				enum sr030pc50_oprmode type)
{
	if (which == V4L2_SUBDEV_FORMAT_TRY)
		return fh ? v4l2_subdev_get_try_format(fh, 0) : NULL;

	return &state->ffmt[type];
}

/**
 * msleep_debug: wrapper function calling proper sleep()
 * @msecs: time to be sleep (in milli-seconds unit)
 * @dbg_on: whether enable log or not.
 */
static void msleep_debug(u32 msecs, bool dbg_on)
{
	u32 delta_halfrange; /* in us unit */

	if (unlikely(!msecs))
		return;

	if (dbg_on)
		cam_dbg("delay for %dms\n", msecs);

	if (msecs <= 7)
		delta_halfrange = 100;
	else
		delta_halfrange = 300;

	if (msecs <= 20)
		usleep_range((msecs * 1000 - delta_halfrange),
			(msecs * 1000 + delta_halfrange));
	else
		msleep(msecs);
}

#ifdef CONFIG_LOAD_FILE
#define TABLE_MAX_NUM 500
static char *sr030pc50_regs_table;
static int sr030pc50_regs_table_size;
static int gtable_buf[TABLE_MAX_NUM];
static int sr030pc50_i2c_write(struct v4l2_subdev *sd,
		u8 subaddr, u8 data);

int sr030pc50_regs_table_init(void)
{
	struct file *filp;
	char *dp;
	long size;
	loff_t pos;
	int ret;
	mm_segment_t fs = get_fs();

	/*cam_info("%s %d\n", __func__, __LINE__);*/

	set_fs(get_ds());

	filp = filp_open(TUNING_FILE_PATH, O_RDONLY, 0);

	if (IS_ERR_OR_NULL(filp)) {
		cam_err("file open error\n");
		return PTR_ERR(filp);
	}

	size = filp->f_path.dentry->d_inode->i_size;
	cam_dbg("size = %ld\n", size);
	//dp = kmalloc(size, GFP_KERNEL);
	dp = vmalloc(size);
	if (unlikely(!dp)) {
		cam_err("Out of Memory\n");
		filp_close(filp, current->files);
		return -ENOMEM;
	}

	pos = 0;
	memset(dp, 0, size);
	ret = vfs_read(filp, (char __user *)dp, size, &pos);

	if (unlikely(ret != size)) {
		cam_err("Failed to read file ret = %d\n", ret);
		/*kfree(dp);*/
		vfree(dp);
		filp_close(filp, current->files);
		return -EINVAL;
	}

	filp_close(filp, current->files);

	set_fs(fs);

	sr030pc50_regs_table = dp;

	sr030pc50_regs_table_size = size;

	*((sr030pc50_regs_table + sr030pc50_regs_table_size) - 1) = '\0';

	cam_info("sr030pc50_reg_table_init end\n");
	return 0;
}

void sr030pc50_regs_table_exit(void)
{
	printk(KERN_DEBUG "%s %d\n", __func__, __LINE__);

	if (sr030pc50_regs_table) {
		vfree(sr030pc50_regs_table);
		sr030pc50_regs_table = NULL;
	}
}

static bool sr030pc50_is_hexnum(char *num)
{
	int i = 0;

	for (i = 2; num[i] != '\0'; i++) {
		if (!(((num[i] >= '0') && (num[5] <= '9'))
		    || ((num[5] >= 'a') && (num[5] <= 'f'))
		    || ((num[5] >= 'A') && (num[5] <= 'F'))))
			return false;
	}

	return true;
}

static int sr030pc50_write_regs_from_sd(struct v4l2_subdev *sd,
	const char *name)
{
	char *start = NULL, *end = NULL, *reg = NULL, *temp = NULL;
	u8 addr = 0, value = 0;
	u16 data = 0;
	char data_buf[7] = {0, };
	u32 len = 0;
	int err = 0;

	cam_dbg("Enter!!\n");

	addr = value = 0;

	*(data_buf + 6) = '\0';

	start = strnstr(sr030pc50_regs_table, name, sr030pc50_regs_table_size);
	CHECK_ERR_COND_MSG(start == NULL, -ENODATA, "start is NULL\n");

	end = strnstr(start, "};", sr030pc50_regs_table_size);
	CHECK_ERR_COND_MSG(start == NULL, -ENODATA, "end is NULL\n");

	while (1) {
		len = end -start;

		/* Find Address */
		reg = strnstr(start, "0x", len);
		if (!reg || (reg > end)) {
			cam_info("write end of %s\n", name);
			break;
		}

		start = (reg + 6);

		/* Write Value to Address */
		memcpy(data_buf, reg, 6);

		if (!sr030pc50_is_hexnum(data_buf)) {
			cam_err("Hex number not found %s\n", data_buf);
			return -EINVAL;
		}

		err = kstrtou16(data_buf, 16, &data);
		CHECK_ERR_MSG(err, "kstrtou16 failed\n");

		addr = (data >> 8);
		value = (data & 0xff);

		if (unlikely(DELAY_SEQ == addr)) {
			if (value != 0xFF)
				msleep_debug(value * 10, true);
		} else {
			err = sr030pc50_writeb(sd, addr, value);
			CHECK_ERR_MSG(err, "register set failed\n");
		}
	}

	cam_dbg("Exit!!\n");

	return err;
}
#endif

/**
 * sr030pc50_read: read data from sensor with I2C
 * Note the data-store way(Big or Little)
 */
static int sr030pc50_i2c_read(struct v4l2_subdev *sd,
			u8 subaddr, u8 *data)
{
	int err = -EIO;
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct i2c_msg msg[2];
	u8 buf[16] = {0,};
	int retry = 5;


	CHECK_ERR_COND_MSG(!client->adapter, -ENODEV,
		"can't search i2c client adapter\n");

	msg[0].addr = client->addr;
	msg[0].flags = 0;
	msg[0].len = sizeof(subaddr);
	msg[0].buf = &subaddr;

	msg[1].addr = client->addr;
	msg[1].flags = I2C_M_RD;
	msg[1].len = 1;
	msg[1].buf = buf;

	while (retry-- > 0) {
		err = i2c_transfer(client->adapter, msg, 2);
		if (likely(err == 2))
			break;
		cam_err("i2c read: error, read register(0x%X). cnt %d\n",
			subaddr, retry);
		msleep_debug(POLL_TIME_MS, false);
		stats_i2c_err++;
	}

	CHECK_ERR_COND_MSG(err != 2, -EIO, "I2C does not work\n");

	*data = buf[0];

	return 0;
}

/**
 * sr030pc50_write: write data with I2C
 * Note the data-store way(Big or Little)
 */
static inline int  sr030pc50_i2c_write(struct v4l2_subdev *sd,
					u8 subaddr, u8 data)
{
	u8 buf[2];
	int err = 0, retry = 5;
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct i2c_msg msg = {
		.addr	= client->addr,
		.flags	= 0,
		.buf	= buf,
		.len	= 2,
	};

	CHECK_ERR_COND_MSG(!client->adapter, -ENODEV,
		"can't search i2c client adapter\n");

	buf[0] = subaddr;
	buf[1] = data;

	while (retry-- > 0) {
		err = i2c_transfer(client->adapter, &msg, 1);
		if (likely(err == 1))
			break;
		cam_err("i2c write: error %d, write 0x%04X, retry %d\n",
			err, ((subaddr << 8) | data), retry);
		msleep_debug(POLL_TIME_MS, false);
		stats_i2c_err++;
	}

	CHECK_ERR_COND_MSG(err != 1, -EIO, "I2C does not work\n");
	return 0;
}

static int sr030pc50_i2c_burst_write_list(struct v4l2_subdev *sd,
		const sr030pc50_regset_t regs[], int size, const char *name)
{

	cam_err("burst write: not implemented\n");

	return 0;
}

static inline int sr030pc50_write_regs(struct v4l2_subdev *sd,
			const sr030pc50_regset_t regs[], int size)
{
	int err = 0, i;
	u8 subaddr, value;

	cam_trace("size %d\n", size);

	for (i = 0; i < size; i++) {
		subaddr = (u8)(regs[i] >> 8);
		value = (u8)(regs[i]);
		if (unlikely(DELAY_SEQ == subaddr)) {
			if (value != 0xFF)
				msleep_debug(value * 10, true);
		} else {
			err = sr030pc50_writeb(sd, subaddr, value);
			CHECK_ERR_MSG(err, "register set failed\n")
                }
	}

	return 0;
}

/* PX: */
static int sr030pc50_set_from_table(struct v4l2_subdev *sd,
				const char *setting_name,
				const struct regset_table *table,
				u32 table_size, s32 index)
{
	int err = 0;

	cam_trace("set %s index %d\n", setting_name, index);

	CHECK_ERR_COND_MSG(((index < 0) || (index >= table_size)),
		-EINVAL, "index(%d) out of range[0:%d] for table for %s\n",
		index, table_size, setting_name);

	table += index;

#ifdef CONFIG_LOAD_FILE
	cam_dbg("%s: \"%s\", reg_name=%s\n", __func__,
			setting_name, table->name);
	return sr030pc50_write_regs_from_sd(sd, table->name);

#else /* !CONFIG_LOAD_FILE */
	CHECK_ERR_COND_MSG(!table->reg, -EFAULT, \
		"table=%s, index=%d, reg = NULL\n", setting_name, index);
# ifdef DEBUG_WRITE_REGS
	cam_dbg("write_regtable: \"%s\", reg_name=%s\n", setting_name,
		table->name);
# endif /* DEBUG_WRITE_REGS */

	if (table->burst) {
		err = sr030pc50_i2c_burst_write_list(sd,
			table->reg, table->array_size, setting_name);
	} else
		err = sr030pc50_write_regs(sd, table->reg, table->array_size);

	CHECK_ERR_MSG(err, "write regs(%s), err=%d\n", setting_name, err);

	return 0;
#endif /* CONFIG_LOAD_FILE */
}

static inline int sr030pc50_transit_preview_mode(struct v4l2_subdev *sd)
{
	struct sr030pc50_state *state = to_state(sd);
	int err = 0;

	if (state->exposure.ae_lock || state->wb.awb_lock)
		cam_info("Restore user ae(awb)-lock...\n");

	err = sr030pc50_set_from_table(sd, "preview_mode",
		&state->regs->preview_mode, 1, 0);

	return err;
}

static inline int sr030pc50_transit_capture_mode(struct v4l2_subdev *sd)
{
	struct sr030pc50_state *state = to_state(sd);
	int err = -EIO;

	if (state->capture.lowlux_night) {
		cam_info("capture_mode: night lowlux\n");
		err = sr030pc50_set_from_table(sd, "capture_mode_night",
			&state->regs->capture_mode_night, 1, 0);
	} else
		err = sr030pc50_set_from_table(sd, "capture_mode",
			&state->regs->capture_mode, 1, 0);

	return err;
}

/**
 * sr030pc50_transit_movie_mode: switch camera mode if needed.
 * Note that this fuction should be called from start_preview().
 */
static inline int sr030pc50_transit_movie_mode(struct v4l2_subdev *sd)
{
	struct sr030pc50_state *state = to_state(sd);

	/* we'll go from the below modes to RUNNING or RECORDING */
	switch (state->runmode) {
	case RUNMODE_INIT:
		/* case of entering camcorder firstly */
		break;
	case RUNMODE_RUNNING_STOP:
		/* case of switching from camera to camcorder */
		break;
	case RUNMODE_RECORDING_STOP:
		/* case of switching from camcorder to camera */
		break;

	default:
		break;
	}

	return 0;
}

/**
 * sr030pc50_is_hwflash_on - check whether flash device is on
 *
 * Refer to state->flash.on to check whether flash is in use in driver.
 */
static inline int sr030pc50_is_hwflash_on(struct v4l2_subdev *sd)
{
#ifdef SR030PC50_SUPPORT_FLASH
	struct sr030pc50_state *state = to_state(sd);

	return state->pdata->is_flash_on();
#else
	return 0;
#endif
}

/**
 * sr030pc50_flash_en - contro Flash LED
 * @mode: SR030PC50_FLASH_MODE_NORMAL or SR030PC50_FLASH_MODE_MOVIE
 * @onoff: SR030PC50_FLASH_ON or SR030PC50_FLASH_OFF
 */
static int sr030pc50_flash_en(struct v4l2_subdev *sd, s32 mode, s32 onoff)
{
	struct sr030pc50_state *state = to_state(sd);

	if (unlikely(state->flash.ignore_flash)) {
		cam_warn("WARNING, we ignore flash command.\n");
		return 0;
	}

#ifdef SR030PC50_SUPPORT_FLASH
	return state->pdata->flash_en(mode, onoff);
#else
	return 0;
#endif
}

/**
 * sr030pc50_flash_torch - turn flash on/off as torch for preflash, recording
 * @onoff: SR030PC50_FLASH_ON or SR030PC50_FLASH_OFF
 *
 * This func set state->flash.on properly.
 */
static inline int sr030pc50_flash_torch(struct v4l2_subdev *sd, s32 onoff)
{
	struct sr030pc50_state *state = to_state(sd);
	int err = 0;

	err = sr030pc50_flash_en(sd, SR030PC50_FLASH_MODE_MOVIE, onoff);
	state->flash.on = (onoff == SR030PC50_FLASH_ON) ? 1 : 0;

	return err;
}

/**
 * sr030pc50_flash_oneshot - turn main flash on for capture
 * @onoff: SR030PC50_FLASH_ON or SR030PC50_FLASH_OFF
 *
 * Main flash is turn off automatically in some milliseconds.
 */
static inline int sr030pc50_flash_oneshot(struct v4l2_subdev *sd, s32 onoff)
{
	struct sr030pc50_state *state = to_state(sd);
	int err = 0;

	err = sr030pc50_flash_en(sd, SR030PC50_FLASH_MODE_NORMAL, onoff);
	state->flash.on = (onoff == SR030PC50_FLASH_ON) ? 1 : 0;

	return err;
}

static const struct sr030pc50_framesize *sr030pc50_get_framesize
	(const struct sr030pc50_framesize *frmsizes,
	u32 frmsize_count, u32 index)
{
	int i = 0;

	for (i = 0; i < frmsize_count; i++) {
		if (frmsizes[i].index == index)
			return &frmsizes[i];
	}

	return NULL;
}

/* This function is called from the g_ctrl api
 *
 * This function should be called only after the s_fmt call,
 * which sets the required width/height value.
 *
 * It checks a list of available frame sizes and sets the
 * most appropriate frame size.
 *
 * The list is stored in an increasing order (as far as possible).
 * Hence the first entry (searching from the beginning) where both the
 * width and height is more than the required value is returned.
 * In case of no perfect match, we set the last entry (which is supposed
 * to be the largest resolution supported.)
 */
static void sr030pc50_set_framesize(struct v4l2_subdev *sd,
				const struct sr030pc50_framesize *frmsizes,
				u32 num_frmsize, bool preview)
{
	struct sr030pc50_state *state = to_state(sd);
	const struct sr030pc50_framesize **found_frmsize = NULL;
	u32 width = state->req_fmt.width;
	u32 height = state->req_fmt.height;
	int i = 0;

	cam_dbg("%s: Requested Res %dx%d\n", __func__,
			width, height);

	found_frmsize = preview ?
		&state->preview.frmsize : &state->capture.frmsize;

	for (i = 0; i < num_frmsize; i++) {
		if ((frmsizes[i].width == width) &&
			(frmsizes[i].height == height)) {
			*found_frmsize = &frmsizes[i];
			break;
		}
	}

	if (*found_frmsize == NULL) {
		cam_err("%s: error, invalid frame size %dx%d\n",
			__func__, width, height);
		*found_frmsize = preview ?
			sr030pc50_get_framesize(frmsizes, num_frmsize,
					PREVIEW_SZ_VGA) :
			sr030pc50_get_framesize(frmsizes, num_frmsize,
					CAPTURE_SZ_1MP);
		BUG_ON(!(*found_frmsize));
	}

	if (preview)
		cam_info("Preview Res Set: %dx%d, index %d\n",
			(*found_frmsize)->width, (*found_frmsize)->height,
			(*found_frmsize)->index);
	else
		cam_info("Capture Res Set: %dx%d, index %d\n",
			(*found_frmsize)->width, (*found_frmsize)->height,
			(*found_frmsize)->index);
}

/* PX: Set scene mode */
static int sr030pc50_set_scene_mode(struct v4l2_subdev *sd, s32 val)
{
	struct sr030pc50_state *state = to_state(sd);

	cam_trace("E, value %d\n", val);

retry:
	switch (val) {
	case SCENE_MODE_NONE:
	case SCENE_MODE_PORTRAIT:
	case SCENE_MODE_NIGHTSHOT:
	case SCENE_MODE_BACK_LIGHT:
	case SCENE_MODE_LANDSCAPE:
	case SCENE_MODE_SPORTS:
	case SCENE_MODE_PARTY_INDOOR:
	case SCENE_MODE_BEACH_SNOW:
	case SCENE_MODE_SUNSET:
	case SCENE_MODE_DUSK_DAWN:
	case SCENE_MODE_FALL_COLOR:
	case SCENE_MODE_FIREWORKS:
	case SCENE_MODE_TEXT:
	case SCENE_MODE_CANDLE_LIGHT:
		sr030pc50_set_from_table(sd, "scene_mode",
			state->regs->scene_mode,
			ARRAY_SIZE(state->regs->scene_mode), val);
		break;

	default:
		cam_err("set_scene: error, not supported (%d)\n", val);
		val = SCENE_MODE_NONE;
		goto retry;
	}

	state->scene_mode = val;

	cam_trace("X\n");
	return 0;
}

/* PX: Set brightness */
static int sr030pc50_set_exposure(struct v4l2_subdev *sd, s32 val)
{
	struct sr030pc50_state *state = to_state(sd);
	int err = 0;

	if ((val < EV_MINUS_4) || (val > EV_PLUS_4)) {
		cam_err("%s: error, invalid value(%d)\n", __func__, val);
		return -EINVAL;
	}

	cam_info("%s exposure:%d(%d)\n",__func__,val,GET_EV_INDEX(val));

	sr030pc50_set_from_table(sd, "brightness", state->regs->ev,
		ARRAY_SIZE(state->regs->ev), GET_EV_INDEX(val));

	state->exposure.val = val;

	return err;
}

static int sr030pc50_set_vt_mode(struct v4l2_subdev *sd, s32 val)
{
	struct sr030pc50_state *state = to_state(sd);
	int err = 0;

	state->vt_mode = val;

	return err;
}

/* PX(NEW) */
static int sr030pc50_set_capture_size(struct v4l2_subdev *sd)
{
	struct sr030pc50_state *state = to_state(sd);
	u32 width, height;
	int err = 0;

	if (unlikely(!state->capture.frmsize)) {
		cam_warn("warning, capture resolution not set\n");
		state->capture.frmsize = sr030pc50_get_framesize(
					sr030pc50_capture_frmsizes,
					ARRAY_SIZE(sr030pc50_capture_frmsizes),
					CAPTURE_SZ_1MP);
	}

	width = state->capture.frmsize->width;
	height = state->capture.frmsize->height;

	/* cam_dbg("set capture size(%dx%d)\n", width, height);
	err = sr030pc50_set_from_table(sd, "capture_size",
			state->regs->capture_size,
			ARRAY_SIZE(state->regs->capture_size),
			state->capture.frmsize->index);
	CHECK_ERR_MSG(err, "fail to set capture size\n"); */

	state->preview.update_frmsize = 1;

	return err;
}

/* PX: Set sensor mode */
static int sr030pc50_set_sensor_mode(struct v4l2_subdev *sd, s32 val)
{
	struct sr030pc50_state *state = to_state(sd);

	cam_trace("mode=%d\n", val);

	switch (val) {
	case SENSOR_MOVIE:
		/* We does not support movie mode when in VT. */
		if (state->vt_mode) {
			state->sensor_mode = SENSOR_CAMERA;
			cam_err("%s: error, Not support movie\n", __func__);
			break;
		}
		/* We do not break. */

	case SENSOR_CAMERA:
		state->sensor_mode = val;
		break;

	default:
		cam_err("%s: error, Not support.(%d)\n", __func__, val);
		state->sensor_mode = SENSOR_CAMERA;
		WARN_ON(1);
		break;
	}

	return 0;
}

/* PX: Set framerate */
static int sr030pc50_set_frame_rate(struct v4l2_subdev *sd, s32 fps)
{
	struct sr030pc50_state *state = to_state(sd);
	int i = 0, fps_index = -1;
	int min = FRAME_RATE_AUTO;
	int max = FRAME_RATE_25;
	int err = -EIO;

	cam_info("set frame rate %d\n", fps);

#if defined(CONFIG_MACH_DELOSLTE_KOR_SKT) || defined(CONFIG_MACH_DELOSLTE_KOR_LGT)
	if ((state->vt_initialized == 1) && (fps == FRAME_RATE_7)) {
		cam_info("Return set frame rate 7fps for KOR SKT");
		return 0;
	}
#endif

	if ((fps < min) || (fps > max)) {
		cam_err("set_frame_rate: error, invalid frame rate %d\n", fps);
		fps = (fps < min) ? min : max;
	}

	if (unlikely(!state->initialized)) {
		cam_dbg("pending fps %d\n", fps);
        	state->req_fps = fps;
		return 0;
	}

	for (i = 0; i < ARRAY_SIZE(sr030pc50_framerates); i++) {
		if (fps == sr030pc50_framerates[i].fps) {
			fps_index = sr030pc50_framerates[i].index;
			state->fps = fps;
			state->req_fps = -1;
			break;
		}
	}

	if (unlikely(fps_index < 0)) {
		cam_err("set_fps: warning, not supported fps %d\n", fps);
		return 0;
	}

	err = sr030pc50_set_from_table(sd, "fps", state->regs->fps,
			ARRAY_SIZE(state->regs->fps), fps_index);
	CHECK_ERR_MSG(err, "fail to set framerate\n");

	return 0;
}

static int sr030pc50_control_stream(struct v4l2_subdev *sd, u32 cmd)
{
	struct sr030pc50_state *state = to_state(sd);
	int i, err = -EINVAL;

	if (cmd == STREAM_STOP) {
		cam_info("STREAM STOP!!\n");
		err = sr030pc50_set_from_table(sd, "stream_stop",
			&state->regs->stream_stop, 1, 0);
		CHECK_ERR_MSG(err, "failed to stop stream\n");
	} else {
		cam_info("STREAM START\n");
		err = sr030pc50_set_from_table(sd, "stream_start",
			&state->regs->stream_start, 1, 0);
		CHECK_ERR_MSG(err, "failed to start stream\n");
		return 0;
	}

	switch (state->runmode) {
	case RUNMODE_CAPTURING:
		cam_dbg("Capture Stop!\n");
		state->runmode = RUNMODE_CAPTURING_STOP;
		state->capture.ready = 0;
		state->capture.lowlux_night = 0;
		break;

	case RUNMODE_RUNNING:
		cam_dbg("Preview Stop!\n");
		state->runmode = RUNMODE_RUNNING_STOP;
		break;

	case RUNMODE_RECORDING:
		state->runmode = RUNMODE_RECORDING_STOP;

		for (i = 0; i < ARRAY_SIZE(sr030pc50_ctrls); i++) {
			if (V4L2_CID_CAMERA_FRAME_RATE == sr030pc50_ctrls[i].id) {
				sr030pc50_ctrls[i].value = sr030pc50_ctrls[i].default_value;
				break;
			}
		}
		sr030pc50_init(sd, 2);
		break;

	default:
		break;
	}

	return 0;
}

#ifdef SR030PC50_SUPPORT_FLASH
/* PX: Set flash mode */
static int sr030pc50_set_flash_mode(struct v4l2_subdev *sd, s32 val)
{
	struct sr030pc50_state *state = to_state(sd);

	/* movie flash mode should be set when recording is started */
/*	if (state->sensor_mode == SENSOR_MOVIE && !state->recording)
		return 0;*/

	if (state->flash.mode == val) {
		cam_dbg("the same flash mode=%d\n", val);
		return 0;
	}

	if (val == FLASH_MODE_TORCH)
		sr030pc50_flash_torch(sd, SR030PC50_FLASH_ON);

	if ((state->flash.mode == FLASH_MODE_TORCH)
	    && (val == FLASH_MODE_OFF))
		sr030pc50_flash_torch(sd, SR030PC50_FLASH_OFF);

	state->flash.mode = val;
	cam_dbg("Flash mode = %d\n", val);
	return 0;
}
#endif

static int sr030pc50_check_esd(struct v4l2_subdev *sd, s32 val)
{
#ifdef TODO_ENABLE
	u32 data = 0, size_h = 0, size_v = 0;
#endif
/* To do */
	return 0;
#ifdef TODO_ENABLE
esd_out:
	cam_err("Check ESD(%d): ESD Shock detected! val=0x%X\n\n", data, val);
	return -ERESTART;
#endif
}

/* returns the real iso currently used by sensor due to lighting
 * conditions, not the requested iso we sent using s_ctrl.
 */
static inline int sr030pc50_get_exif_iso(struct v4l2_subdev *sd, u16 *iso)
{
	int err = 0;
	u8 read_value = 0;
	unsigned short gain_value = 0;

	err = sr030pc50_writeb(sd, 0x03, 0x20);
	CHECK_ERR_COND(err < 0, -ENODEV);
	sr030pc50_readb(sd, 0xb0, &read_value);

	gain_value = ((read_value * 100) / 32) + 50;
	cam_dbg("gain_value=%d, read_value=%d\n", gain_value, read_value);

	if (gain_value < 114)
		*iso = 50;
	else if (gain_value < 214)
		*iso = 100;
	else if (gain_value < 264)
		*iso = 200;
	else if (gain_value < 825)
		*iso = 400;
	else
		*iso = 800;

	cam_dbg("gain_value=%d, ISO=%d\n", gain_value, *iso);
	return 0;
}

/* PX: Set ISO */
static int __used sr030pc50_set_iso(struct v4l2_subdev *sd, s32 val)
{
	struct sr030pc50_state *state = to_state(sd);

	sr030pc50_set_from_table(sd, "iso", state->regs->iso,
		ARRAY_SIZE(state->regs->iso), val);

	state->iso = val;

	cam_trace("X\n");
	return 0;
}

/* PX: Return exposure time (ms) */
static inline int sr030pc50_get_exif_exptime(struct v4l2_subdev *sd,
						u32 *exp_time)
{
	int err = 0;
	u8 read_value1 = 0;
	u8 read_value2 = 0;
	u8 read_value3 = 0;

	err = sr030pc50_writeb(sd, 0x03, 0x20);
	CHECK_ERR_COND(err < 0, -ENODEV);

	sr030pc50_readb(sd, 0x80, &read_value1);
	sr030pc50_readb(sd, 0x81, &read_value2);
	sr030pc50_readb(sd, 0x82, &read_value3);

	cam_dbg("exposure time read_value %d, %d, %d\n",
		read_value1, read_value2, read_value3);
	*exp_time = (read_value1 << 19)
		+ (read_value2 << 11) + (read_value3 << 3);

	cam_dbg("exposure time %dus\n", *exp_time);
	return 0;
}

static inline void sr030pc50_get_exif_flash(struct v4l2_subdev *sd,
					u16 *flash)
{
	struct sr030pc50_state *state = to_state(sd);

	*flash = 0;

	switch (state->flash.mode) {
	case FLASH_MODE_OFF:
		*flash |= EXIF_FLASH_MODE_SUPPRESSION;
		break;

	case FLASH_MODE_AUTO:
		*flash |= EXIF_FLASH_MODE_AUTO;
		break;

	case FLASH_MODE_ON:
	case FLASH_MODE_TORCH:
		*flash |= EXIF_FLASH_MODE_FIRING;
		break;

	default:
		break;
	}

	if (state->flash.on)
		*flash |= EXIF_FLASH_FIRED;
}

/* PX: */
static int sr030pc50_get_exif(struct v4l2_subdev *sd)
{
	struct sr030pc50_state *state = to_state(sd);
	u32 exposure_time = 0;
	int OPCLK = 24000000;

	/* exposure time */
	state->exif.exp_time_den = 0;
	sr030pc50_get_exif_exptime(sd, &exposure_time);
	if (exposure_time) {
		state->exif.exp_time_den = OPCLK / exposure_time;
	} else {
		state->exif.exp_time_den = 0;
	}

	/* iso */
	state->exif.iso = 0;
	sr030pc50_get_exif_iso(sd, &state->exif.iso);

	/* flash */
	sr030pc50_get_exif_flash(sd, &state->exif.flash);

	cam_dbg("EXIF: ex_time_den=%d, iso=%d, flash=0x%02X\n",
		state->exif.exp_time_den, state->exif.iso, state->exif.flash);

	return 0;
}

static int sr030pc50_set_preview_size(struct v4l2_subdev *sd)
{
	struct sr030pc50_state *state = to_state(sd);
	u32 width, height;
	int err = -EINVAL;

	if (!state->preview.update_frmsize)
		return 0;

	if (unlikely(!state->preview.frmsize)) {
		cam_warn("warning, preview resolution not set\n");
		state->preview.frmsize = sr030pc50_get_framesize(
					sr030pc50_preview_frmsizes,
					ARRAY_SIZE(sr030pc50_preview_frmsizes),
					PREVIEW_SZ_VGA);
	}

	width = state->preview.frmsize->width;
	height = state->preview.frmsize->height;

#if defined(CONFIG_MACH_GARDA)
	cam_dbg("set preview size(%dx%d)\n", width, height);
	err = sr030pc50_set_from_table(sd, "preview_size",
			state->regs->preview_size,
			ARRAY_SIZE(state->regs->preview_size),
			state->preview.frmsize->index);
	CHECK_ERR_MSG(err, "fail to set preview size\n");
#endif

	state->preview.update_frmsize = 0;

	return 0;
}

static int sr030pc50_start_preview(struct v4l2_subdev *sd)
{
	struct sr030pc50_state *state = to_state(sd);
	int err = 0;

	cam_info("Camera Preview start, runmode = %d\n", state->runmode);

	if ((state->runmode == RUNMODE_NOTREADY) ||
	    (state->runmode == RUNMODE_CAPTURING)) {
		cam_err("%s: error - Invalid runmode\n", __func__);
		return -EPERM;
	}

	/* Check pending fps */
	if (state->req_fps >= 0) {
		err = sr030pc50_set_frame_rate(sd, state->req_fps);
		CHECK_ERR(err);
	}

	/* Set preview size */
	err = sr030pc50_set_preview_size(sd);
	CHECK_ERR_MSG(err, "failed to set preview size(%d)\n", err);

	err = sr030pc50_control_stream(sd, STREAM_START);
	CHECK_ERR(err);

	if (RUNMODE_INIT == state->runmode)
		msleep_debug(200, true);

	state->runmode = (state->sensor_mode == SENSOR_CAMERA) ?
			RUNMODE_RUNNING : RUNMODE_RECORDING;
	return 0;
}

static int sr030pc50_set_capture(struct v4l2_subdev *sd)
{
	int err = 0;

	cam_info("set_capture\n");

	/* Set capture size */
	sr030pc50_set_capture_size(sd);

	/* Transit to capture mode */
	err = sr030pc50_transit_capture_mode(sd);
	CHECK_ERR_MSG(err, "fail to capture_mode (%d)\n", err);
	return 0;
}

#if 0
static int sr030pc50_prepare_fast_capture(struct v4l2_subdev *sd)
{
	struct sr030pc50_state *state = to_state(sd);
	int err = 0;

	cam_info("prepare_fast_capture\n");

	state->req_fmt.width = (state->capture.pre_req >> 16);
	state->req_fmt.height = (state->capture.pre_req & 0xFFFF);
	sr030pc50_set_framesize(sd, sr030pc50_capture_frmsizes,
		ARRAY_SIZE(sr030pc50_capture_frmsizes), false);

	err = sr030pc50_set_capture(sd);
	CHECK_ERR(err);

	state->capture.ready = 1;

	return 0;
}
#endif

static int sr030pc50_start_capture(struct v4l2_subdev *sd)
{
	struct sr030pc50_state *state = to_state(sd);
	int err = -ENODEV;
	u32 night_delay;

	cam_info("start_capture\n");

	if (!state->capture.ready) {
		err = sr030pc50_set_capture(sd);
		CHECK_ERR(err);

		sr030pc50_control_stream(sd, STREAM_START);
		night_delay = 500;
	} else
		night_delay = 700; /* for completely skipping 1 frame. */

	state->runmode = RUNMODE_CAPTURING;

	if (state->capture.lowlux_night)
		msleep_debug(night_delay, true);

	/* Get EXIF */
	sr030pc50_get_exif(sd);

	return 0;
}

/**
 * sr200pc20_init_regs: Indentify chip and get pointer to reg table
 * @
 */
static int sr030pc50_check_sensor(struct v4l2_subdev *sd)
{
	/* struct sr030pc50_state *state = to_state(sd);*/
	int err = -ENODEV;
	u8 read_value = 0;

	err = sr030pc50_writeb(sd, 0x03, 0x00);
	err |= sr030pc50_readb(sd, 0x04, &read_value);
	CHECK_ERR_COND(err < 0, -ENODEV);

	if (SR030PC50_CHIP_ID == read_value)
		cam_info("Sensor ChipID: 0x%02X\n", SR030PC50_CHIP_ID);
	else
		cam_info("Sensor ChipID: 0x%02X, unknown chipID\n", read_value);

	return 0;
}

/* PX(NEW) */
static int sr030pc50_s_mbus_fmt(struct v4l2_subdev *sd,
				struct v4l2_mbus_framefmt *fmt)
{
	struct sr030pc50_state *state = to_state(sd);
	s32 previous_index = 0;

	cam_dbg("%s: pixelformat = 0x%x, colorspace = 0x%x, width = %d, height = %d\n",
		__func__, fmt->code, fmt->colorspace, fmt->width, fmt->height);

	v4l2_fill_pix_format(&state->req_fmt, fmt);
	if ((IS_MODE_CAPTURE_STILL == fmt->field)
	    && (SENSOR_CAMERA == state->sensor_mode))
		state->format_mode = V4L2_PIX_FMT_MODE_CAPTURE;
	else
		state->format_mode = V4L2_PIX_FMT_MODE_PREVIEW;

	if (state->format_mode != V4L2_PIX_FMT_MODE_CAPTURE) {
		previous_index = state->preview.frmsize ?
				state->preview.frmsize->index : -1;
		sr030pc50_set_framesize(sd, sr030pc50_preview_frmsizes,
			ARRAY_SIZE(sr030pc50_preview_frmsizes), true);

		if (previous_index != state->preview.frmsize->index)
			state->preview.update_frmsize = 1;
	} else {
		sr030pc50_set_framesize(sd, sr030pc50_capture_frmsizes,
			ARRAY_SIZE(sr030pc50_capture_frmsizes), false);

		/* for maket app.
		 * Samsung camera app does not use unmatched ratio.*/
		if (unlikely(!state->preview.frmsize)) {
			cam_warn("warning, capture without preview\n");
		} else if (unlikely(FRM_RATIO(state->preview.frmsize)
		    != FRM_RATIO(state->capture.frmsize))) {
			cam_warn("warning, preview, capture ratio not matched\n\n");
		}
	}

	return 0;
}

static int sr030pc50_enum_mbus_fmt(struct v4l2_subdev *sd, unsigned int index,
					enum v4l2_mbus_pixelcode *code)
{
	cam_dbg("%s: index = %d\n", __func__, index);

	if (index >= ARRAY_SIZE(capture_fmts))
		return -EINVAL;

	*code = capture_fmts[index].code;

	return 0;
}

static int sr030pc50_try_mbus_fmt(struct v4l2_subdev *sd,
				struct v4l2_mbus_framefmt *fmt)
{
	int num_entries;
	int i;

	num_entries = ARRAY_SIZE(capture_fmts);

	cam_dbg("%s: code = 0x%x , colorspace = 0x%x, num_entries = %d\n",
		__func__, fmt->code, fmt->colorspace, num_entries);

	for (i = 0; i < num_entries; i++) {
		if (capture_fmts[i].code == fmt->code &&
		    capture_fmts[i].colorspace == fmt->colorspace) {
			cam_info("%s: match found, returning 0\n", __func__);
			return 0;
		}
	}

	cam_err("%s: no match found, returning -EINVAL\n", __func__);
	return -EINVAL;
}


static int sr030pc50_enum_framesizes(struct v4l2_subdev *sd,
				  struct v4l2_frmsizeenum *fsize)
{
	struct sr030pc50_state *state = to_state(sd);

	/*
	* The camera interface should read this value, this is the resolution
	* at which the sensor would provide framedata to the camera i/f
	* In case of image capture,
	* this returns the default camera resolution (VGA)
	*/
	if (state->format_mode != V4L2_PIX_FMT_MODE_CAPTURE) {
		if (unlikely(state->preview.frmsize == NULL)) {
			cam_err("%s: error\n", __func__);
			return -EFAULT;
		}

		fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
		fsize->discrete.width = state->preview.frmsize->width;
		fsize->discrete.height = state->preview.frmsize->height;
	} else {
		if (unlikely(state->capture.frmsize == NULL)) {
			cam_err("%s: error\n", __func__);
			return -EFAULT;
		}

		fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
		fsize->discrete.width = state->capture.frmsize->width;
		fsize->discrete.height = state->capture.frmsize->height;
	}

	return 0;
}

static int sr030pc50_g_parm(struct v4l2_subdev *sd,
			struct v4l2_streamparm *param)
{
	return 0;
}

static int sr030pc50_s_parm(struct v4l2_subdev *sd,
			struct v4l2_streamparm *param)
{
	struct sr030pc50_state *state = to_state(sd);
	s32 req_fps;

	req_fps = param->parm.capture.timeperframe.denominator /
			param->parm.capture.timeperframe.numerator;

	cam_dbg("s_parm state->fps=%d, state->req_fps=%d\n",
		state->fps, req_fps);

	return sr030pc50_set_frame_rate(sd, req_fps);
}

static inline bool sr030pc50_is_clear_ctrl(struct v4l2_control *ctrl)
{
	switch (ctrl->id) {
	case V4L2_CID_CAMERA_BRIGHTNESS:
	case V4L2_CID_CAM_BRIGHTNESS:
		break;

	default:
		if (ctrl->value < 0) {
			/*cam_dbg("ctrl ID 0x%08X skipped (%d)\n",
				ctrl->id, ctrl->value);*/
			return true;
		}
	}

	return false;
}

static int sr030pc50_g_ctrl(struct v4l2_subdev *sd, struct v4l2_control *ctrl)
{
	struct sr030pc50_state *state = to_state(sd);
	int err = 0;

	if (!state->initialized) {
		cam_err("%s: WARNING, camera not initialized\n", __func__);
		return 0;
	}

	mutex_lock(&state->ctrl_lock);

	switch (ctrl->id) {
	case V4L2_CID_CAMERA_EXIF_EXPTIME:
		if (state->sensor_mode == SENSOR_CAMERA) {
			sr030pc50_get_exif_exptime(sd, &state->exif.exp_time_den);
			state->exif.exp_time_den = (24000000 / 2) / state->exif.exp_time_den;
			ctrl->value = state->exif.exp_time_den;
			cam_dbg("exp_time_den: %u\n", state->exif.exp_time_den);
		} else
			ctrl->value = 24;
		break;

	case V4L2_CID_CAMERA_EXIF_ISO:
		if (state->sensor_mode == SENSOR_CAMERA) {
			sr030pc50_get_exif_iso(sd, &state->exif.iso);
			ctrl->value = state->exif.iso;
		} else
			ctrl->value = 100;
		break;

	case V4L2_CID_CAMERA_EXIF_FLASH:
		if (state->sensor_mode == SENSOR_CAMERA)
			ctrl->value = state->exif.flash;
		else
			sr030pc50_get_exif_flash(sd, (u16 *)ctrl->value);
		break;

	case V4L2_CID_CAMERA_AUTO_FOCUS_RESULT:
		ctrl->value = state->focus.status;
		break;

	case V4L2_CID_CAMERA_WHITE_BALANCE:
	case V4L2_CID_CAMERA_EFFECT:
	case V4L2_CID_CAMERA_CONTRAST:
	case V4L2_CID_CAMERA_SATURATION:
	case V4L2_CID_CAMERA_SHARPNESS:
	case V4L2_CID_CAMERA_OBJ_TRACKING_STATUS:
	case V4L2_CID_CAMERA_SMART_AUTO_STATUS:
	default:
		cam_err("%s: WARNING, unknown Ctrl-ID 0x%x\n",
					__func__, ctrl->id);
		err = 0; /* we return no error. */
		break;
	}

	mutex_unlock(&state->ctrl_lock);

	return err;
}

static int sr030pc50_s_ctrl(struct v4l2_subdev *sd, struct v4l2_control *ctrl)
{
	struct sr030pc50_state *state = to_state(sd);
	int err = 0;

	if (!state->initialized && ctrl->id != V4L2_CID_CAMERA_SENSOR_MODE
		&& ctrl->id != V4L2_CID_CAMERA_VT_MODE) {
		cam_warn("s_ctrl: warning, camera not initialized. ID %d(0x%X)\n",
			ctrl->id & 0xFF, ctrl->id);
		return 0;
	}

	cam_dbg("s_ctrl: ID =0x%08X, val = %d\n", ctrl->id, ctrl->value);

	mutex_lock(&state->ctrl_lock);

	switch (ctrl->id) {
	case V4L2_CID_CAMERA_SENSOR_MODE:
		err = sr030pc50_set_sensor_mode(sd, ctrl->value);
		break;

	case V4L2_CID_CAM_BRIGHTNESS:
	case V4L2_CID_CAMERA_BRIGHTNESS:
		err = sr030pc50_set_exposure(sd, ctrl->value);
		break;

	case V4L2_CID_WHITE_BALANCE_PRESET:
	case V4L2_CID_CAMERA_WHITE_BALANCE:
		err = sr030pc50_set_from_table(sd, "white balance",
			state->regs->white_balance,
			ARRAY_SIZE(state->regs->white_balance), ctrl->value);
		state->wb.mode = ctrl->value;
		break;

	case V4L2_CID_IMAGE_EFFECT:
	case V4L2_CID_CAMERA_EFFECT:
		cam_info("%s effect:%d\n",__func__,ctrl->value);
		err = sr030pc50_set_from_table(sd, "effects",
			state->regs->effect,
			ARRAY_SIZE(state->regs->effect), ctrl->value);
                break;

	case V4L2_CID_CAM_METERING:
	case V4L2_CID_CAMERA_METERING:
		err = sr030pc50_set_from_table(sd, "metering",
			state->regs->metering,
			ARRAY_SIZE(state->regs->metering), ctrl->value);
		break;

	case V4L2_CID_CAMERA_SCENE_MODE:
		err = sr030pc50_set_scene_mode(sd, ctrl->value);
		break;

	case V4L2_CID_CAMERA_CHECK_ESD:
		err = sr030pc50_check_esd(sd, ctrl->value);
		break;

	case V4L2_CID_CAMERA_ISO:
		err = sr030pc50_set_iso(sd, ctrl->value);
		break;

	/*case V4L2_CID_CAMERA_CAPTURE_MODE:
		if (RUNMODE_RUNNING == state->runmode)
			state->capture.pre_req = ctrl->value;
		break;*/

	case V4L2_CID_CAMERA_VT_MODE:
		err = sr030pc50_set_vt_mode(sd, ctrl->value);
		break;

	case V4L2_CID_CAMERA_ANTI_BANDING:
		break;

	case V4L2_CID_CAMERA_FRAME_RATE:
		err = sr030pc50_set_frame_rate(sd, ctrl->value);
		break;

	case V4L2_CID_CAMERA_OBJECT_POSITION_X:
	case V4L2_CID_CAMERA_OBJECT_POSITION_Y:
	case V4L2_CID_CAMERA_TOUCH_AF_START_STOP:
	case V4L2_CID_CAMERA_FOCUS_MODE:
	case V4L2_CID_CAMERA_SET_AUTO_FOCUS:
	case V4L2_CID_CAMERA_FLASH_MODE:
	case V4L2_CID_CAMERA_CONTRAST:
	case V4L2_CID_CAMERA_SATURATION:
	case V4L2_CID_CAMERA_SHARPNESS:
/*	case V4L2_CID_CAMERA_AE_LOCK_UNLOCK:
	case V4L2_CID_CAMERA_AWB_LOCK_UNLOCK:*/
	default:
		cam_err("s_ctrl: warning, unknown Ctrl-ID %d (0x%08X)\n",
			ctrl->id & 0xFF, ctrl->id );
		/* we return no error. */
		break;
	}

	mutex_unlock(&state->ctrl_lock);
	CHECK_ERR_MSG(err, "s_ctrl failed %d\n", err)

	return 0;
}

static inline int sr030pc50_save_ctrl(struct v4l2_subdev *sd,
					struct v4l2_control *ctrl)
{
	int i, ctrl_cnt = ARRAY_SIZE(sr030pc50_ctrls);

	cam_trace("ID =0x%08X, val = %d\n", ctrl->id, ctrl->value);

	for (i = 0; i < ctrl_cnt; i++) {
		if (ctrl->id == sr030pc50_ctrls[i].id) {
			sr030pc50_ctrls[i].value = ctrl->value;
			return 0;
		}
	}

	cam_trace("not saved, ID %d(0x%X)\n", ctrl->id & 0xFF, ctrl->id);
	return 0;
}

static int sr030pc50_restore_ctrl(struct v4l2_subdev *sd)
{
	struct v4l2_control ctrl;
	int i;

	cam_trace("EX\n");

	for (i = 0; i < ARRAY_SIZE(sr030pc50_ctrls); i++) {
		if (sr030pc50_ctrls[i].value !=
		    sr030pc50_ctrls[i].default_value) {
			ctrl.id = sr030pc50_ctrls[i].id;
			ctrl.value = sr030pc50_ctrls[i].value;
			cam_dbg("restore_ctrl: ID 0x%08X, val %d\n",
					ctrl.id, ctrl.value);

			sr030pc50_s_ctrl(sd, &ctrl);
		}
	}

	return 0;
}

static int sr030pc50_pre_s_ctrl(struct v4l2_subdev *sd, struct v4l2_control *ctrl)
{
	struct sr030pc50_state *state = to_state(sd);

	if (sr030pc50_is_clear_ctrl(ctrl))
		return 0;

	if (!state->initialized);
		sr030pc50_save_ctrl(sd, ctrl);

	return sr030pc50_s_ctrl(sd, ctrl);
}

static int sr030pc50_s_ext_ctrl(struct v4l2_subdev *sd,
			      struct v4l2_ext_control *ctrl)
{
	return 0;
}

static int sr030pc50_s_ext_ctrls(struct v4l2_subdev *sd,
				struct v4l2_ext_controls *ctrls)
{
	struct v4l2_ext_control *ctrl = ctrls->controls;
	int ret;
	int i;

	for (i = 0; i < ctrls->count; i++, ctrl++) {
		ret = sr030pc50_s_ext_ctrl(sd, ctrl);

		if (ret) {
			ctrls->error_idx = i;
			break;
		}
	}

	return ret;
}

static int sr030pc50_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct sr030pc50_state *state = to_state(sd);
	int tries = 2;
	int err = 0;

	cam_info("stream mode = %d\n", enable);

	switch (enable) {
	case STREAM_MODE_CAM_OFF:
		if (state->pdata->is_mipi)
			err = sr030pc50_control_stream(sd, STREAM_STOP);
		break;

	case STREAM_MODE_CAM_ON:
		if (!state->initialized) {
retry:
			err = sr030pc50_init(sd, 0);
			if (err && --tries) {
				cam_err("s_stream: retry to init...\n");
				err = sr030pc50_reset(sd, 1);
				CHECK_ERR_GOTO_MSG(err, out_fail,
					"s_stream: power-on failed\n");
				goto retry;
			} else
				CHECK_ERR_GOTO_COND_MSG(!tries, out_fail,
					"s_stream: error, init failed\n");
		}

		if (state->format_mode == V4L2_PIX_FMT_MODE_CAPTURE)
			err = sr030pc50_start_capture(sd);
		else
			err = sr030pc50_start_preview(sd);
		break;

	case STREAM_MODE_MOVIE_OFF:
		cam_info("movie off");
		state->recording = 0;
		break;

	case STREAM_MODE_MOVIE_ON:
		cam_info("movie on");
		state->recording = 1;
		break;

	case STREAM_MODE_WAIT_OFF:
		cam_dbg("do nothing\n");
		break;

	default:
		cam_err("%s: error - Invalid stream mode\n", __func__);
		break;
	}
	CHECK_ERR_MSG(err, "failed\n");

	return 0;

out_fail:
	sr030pc50_s_power(sd, 0);
	return err;
}

static inline int sr030pc50_check_i2c(struct v4l2_subdev *sd, u16 data)
{
	return 0;
}

static void sr030pc50_init_parameter(struct v4l2_subdev *sd)
{
	struct sr030pc50_state *state = to_state(sd);

	state->runmode = RUNMODE_INIT;

	/* Default state values */
	state->scene_mode = SCENE_MODE_NONE;
	state->wb.mode = WHITE_BALANCE_AUTO;
	state->light_level = LUX_LEVEL_MAX;

	/* Set update_frmsize to 1 for case of power reset */
	state->preview.update_frmsize = 1;

	/* Initialize focus field for case of init after power reset. */
	memset(&state->focus, 0, sizeof(state->focus));

	state->lux_level_flash = LUX_LEVEL_FLASH_ON;
	state->shutter_level_flash = 0x0;
	state->vt_mode = 0; /* dslim */
}

static int sr030pc50_put_power(struct v4l2_subdev *sd)
{
#ifdef CONFIG_MACH_GARDA
	if (!IS_ERR_OR_NULL(vt_cam_sensor_a2v8_regulator))
		regulator_put(vt_cam_sensor_a2v8_regulator);

	if (!IS_ERR_OR_NULL(vt_cam_core_1v8_regulator))
		regulator_put(vt_cam_core_1v8_regulator);

	if (!IS_ERR_OR_NULL(vt_cam_io_1v8_regulator))
		regulator_put(vt_cam_io_1v8_regulator);

	if (!IS_ERR_OR_NULL(main_cam_core_1v2_regulator))
		regulator_put(main_cam_core_1v2_regulator);

	vt_cam_sensor_a2v8_regulator = NULL;
	vt_cam_core_1v8_regulator = NULL;
	vt_cam_io_1v8_regulator = NULL;
	main_cam_core_1v2_regulator = NULL;
#endif

	return 0;
}

static int sr030pc50_get_power(struct v4l2_subdev *sd)
{
	int ret = 0;

#ifdef CONFIG_MACH_GARDA
	vt_cam_sensor_a2v8_regulator = regulator_get(NULL, "vt_cam_sensor_a2v8");
	if (IS_ERR(vt_cam_sensor_a2v8_regulator)) {
		pr_info("%s: failed to get %s\n", __func__, "vt_cam_sensor_a2v8");
		ret = -ENODEV;
		goto err_regulator;
	}
	vt_cam_core_1v8_regulator = regulator_get(NULL, "vt_cam_core_1v8");
	if (IS_ERR(vt_cam_core_1v8_regulator)) {
		pr_info("%s: failed to get %s\n", __func__, "vt_cam_core_1v8");
		ret = -ENODEV;
		goto err_regulator;
	}
	vt_cam_io_1v8_regulator = regulator_get(NULL, "vt_cam_io_1v8");
	if (IS_ERR(vt_cam_io_1v8_regulator)) {
		pr_info("%s: failed to get %s\n", __func__, "vt_cam_io_1v8");
		ret = -ENODEV;
		goto err_regulator;
	}
	main_cam_core_1v2_regulator = regulator_get(NULL, "main_cam_core_1v2");
	if (IS_ERR(main_cam_core_1v2_regulator)) {
		pr_info("%s: failed to get %s\n", __func__, "main_cam_core_1v2");
		ret = -ENODEV;
		goto err_regulator;
	}

#endif
	/*state->power_on =S5K4ECGX_HW_POWER_READY;*/

	return 0;

err_regulator:
	sr030pc50_put_power(sd);
	return ret;
}

#if 1
static int sr030pc50_power(struct v4l2_subdev *sd, int flag)
{
	struct sr030pc50_state *state = to_state(sd);

	cam_info("power %s\n",flag?"on":"off");

	/* Camera B */
	if (flag) {
		//powerdown
		if (gpio_request(GPIO_VT_CAM_MEGA_nRST, "GPM1_2") < 0)
			pr_err("failed gpio_request(GPM1_2) for camera control\n");

		if (gpio_request(GPIO_VT_CAM_MEGA_EN, "GPM1_3") < 0)
			pr_err("failed gpio_request(GPM1_3) for camera control\n");

#ifdef CONFIG_MACH_GARDA
		regulator_enable(vt_cam_sensor_a2v8_regulator);
		udelay(10);
		regulator_enable(vt_cam_core_1v8_regulator);
		udelay(10);
		regulator_enable(vt_cam_io_1v8_regulator);
		udelay(10);
		regulator_enable(main_cam_core_1v2_regulator);
		usleep_range(1050,1150);
		regulator_disable(main_cam_core_1v2_regulator);
		usleep_range(1050,1150);
#endif
#if 0
		s3c_gpio_cfgpin(GPIO_CAM_MCLK, S3C_GPIO_SFN(3));//MCLK
		//mdelay(1);
		udelay(20);
#else
		mdelay(5);
		clk_enable(state->mclk);
		mdelay(5);
#endif


		// STBYN high
		gpio_direction_output(GPIO_VT_CAM_MEGA_EN, 1);
		msleep_debug(8, true);

		// RSTN high
		gpio_direction_output(GPIO_VT_CAM_MEGA_nRST, 1);
		msleep_debug(57, true);

		gpio_free(GPIO_VT_CAM_MEGA_nRST);
		gpio_free(GPIO_VT_CAM_MEGA_EN);

		state->power_on = SR030PC50_HW_POWER_ON;
	} else {
		if (gpio_request(GPIO_VT_CAM_MEGA_nRST, "GPM1_2") < 0)
			pr_err("failed gpio_request(GPF1_4) for camera control\n");

		if (gpio_request(GPIO_VT_CAM_MEGA_EN, "GPM1_3") < 0)
			pr_err("failed gpio_request(GPF1_5) for camera control\n");
		// nRST low
		gpio_direction_output(GPIO_VT_CAM_MEGA_nRST, 0);
		udelay(15);

		// STBYN low
		gpio_direction_output(GPIO_VT_CAM_MEGA_EN, 0);
		udelay(60);

#if 0
		s3c_gpio_cfgpin(GPIO_CAM_MCLK, S3C_GPIO_SFN(1));//MCLK
		udelay(10);
#else
		clk_disable(state->mclk);
		udelay(30);
#endif


#ifdef CONFIG_MACH_GARDA
		regulator_disable(vt_cam_io_1v8_regulator);
		udelay(10);
		regulator_disable(vt_cam_core_1v8_regulator);
		udelay(10);
		regulator_disable(vt_cam_sensor_a2v8_regulator);
		udelay(10);
#endif
		gpio_free(GPIO_VT_CAM_MEGA_nRST);
		gpio_free(GPIO_VT_CAM_MEGA_EN);

		state->power_on = SR030PC50_HW_POWER_OFF;
	}
	return 0;
}
#else
static int sr030pc50_power(int flag)
{
	pr_info("Robin: sensor is power %s\n",flag == 1 ?"on":"off");

	/* Camera B */
	if(flag == 1)
	{
		//powerdown
		if (gpio_request(GPIO_VT_CAM_MEGA_nRST, "GPM1_2") < 0)
			pr_err("failed gpio_request(GPM1_2) for camera control\n");

		gpio_direction_output(GPIO_VT_CAM_MEGA_nRST, 0);
		s3c_gpio_setpull(GPIO_VT_CAM_MEGA_nRST, S3C_GPIO_PULL_NONE);

		if (gpio_request(GPIO_VT_CAM_MEGA_EN, "GPM1_3") < 0)
			pr_err("failed gpio_request(GPM1_3) for camera control\n");

		gpio_direction_output(GPIO_VT_CAM_MEGA_EN, 0);
		s3c_gpio_setpull(GPIO_VT_CAM_MEGA_EN, S3C_GPIO_PULL_NONE);

#ifdef CONFIG_MACH_GARDA
		regulator_enable(vt_cam_io_1v8_regulator);
		udelay(10);
		regulator_enable(vt_cam_sensor_a2v8_regulator);
		udelay(10);
		regulator_enable(vt_cam_core_1v8_regulator);
		udelay(10);
#endif

		s3c_gpio_cfgpin(GPIO_CAM_MCLK, S3C_GPIO_SFN(3));//MCLK
		//mdelay(1);
		udelay(20);

		// STBYN high
		gpio_direction_output(GPIO_VT_CAM_MEGA_EN, 1);
		mdelay(3);

		// RSTN high
		gpio_direction_output(GPIO_VT_CAM_MEGA_nRST, 1);
		mdelay(51);

		gpio_free(GPIO_VT_CAM_MEGA_nRST);
		gpio_free(GPIO_VT_CAM_MEGA_EN);

	} else {
		// nRST low
		if (gpio_request(GPIO_VT_CAM_MEGA_nRST, "GPM1_2") < 0)
			pr_err("failed gpio_request(GPF1_4) for camera control\n");
		gpio_direction_output(GPIO_VT_CAM_MEGA_nRST, 0);
		udelay(20);

		s3c_gpio_cfgpin(GPIO_CAM_MCLK, S3C_GPIO_SFN(1));//MCLK
		udelay(10);

		// STBYN low
		if (gpio_request(GPIO_VT_CAM_MEGA_EN, "GPM1_3") < 0)
			pr_err("failed gpio_request(GPF1_5) for camera control\n");
		gpio_direction_output(GPIO_VT_CAM_MEGA_EN, 0);
		udelay(10);

		gpio_free(GPIO_VT_CAM_MEGA_nRST);
		gpio_free(GPIO_VT_CAM_MEGA_EN);

#ifdef CONFIG_MACH_GARDA
		regulator_disable(vt_cam_core_1v8_regulator);
		udelay(10);
		regulator_disable(vt_cam_sensor_a2v8_regulator);
		udelay(10);
		regulator_disable(vt_cam_io_1v8_regulator);
		udelay(10);
#endif
	}

	return 0;

}
#endif

/**
 * sr030pc50_reset: reset the sensor device
 * @val: 0 - reset parameter.
 *      1 - power reset
 */
static int sr030pc50_reset(struct v4l2_subdev *sd, u32 val)
{
	struct sr030pc50_state *state = to_state(sd);
	int err = -EINVAL;

	cam_info("reset camera sub-device\n");

	if (state->wq)
		flush_workqueue(state->wq);

#if defined(CONFIG_MACH_DELOSLTE_KOR_SKT) || defined(CONFIG_MACH_DELOSLTE_KOR_LGT)
	state->vt_initialized = 0;
#endif
	state->initialized = 0;
	state->need_wait_streamoff = 0;
	state->runmode = RUNMODE_NOTREADY;

	if (val) {
		if (SR030PC50_HW_POWER_ON == state->power_on) {
			err = sr030pc50_power(sd, 0);
			CHECK_ERR(err);
			msleep_debug(50, true);
		} else
			cam_err("reset: sensor is not powered\n");

		err = sr030pc50_power(sd, 1);
		CHECK_ERR(err);
	}

	state->reset_done = 1;
	stats_reset++;

	return 0;
}

static int sr030pc50_init(struct v4l2_subdev *sd, u32 val)
{
	struct sr030pc50_state *state = to_state(sd);
	int err = -EINVAL;

	cam_info("init: start (%s). power %u, init %u, rst %u, i2c %u\n", __DATE__,
		stats_power, stats_init, stats_reset, stats_i2c_err);

	if (state->power_on != SR030PC50_HW_POWER_ON) {
		cam_err("init: sensor is not powered\n");
		return -EPERM;
	}

#ifdef CONFIG_LOAD_FILE
	err = sr030pc50_regs_table_init();
	CHECK_ERR_MSG(err, "loading setfile fail!\n");
#endif

#if defined(CONFIG_MACH_DELOSLTE_KOR_SKT) || defined(CONFIG_MACH_DELOSLTE_KOR_LGT)
	if (state->vt_mode == PREVIEW_CAMERA) {
		err = sr030pc50_set_from_table(sd, "init_reg",
				&state->regs->init_reg, 1, 0);
		cam_info("Normal Mode\n");
	} else if (state->vt_mode == PREVIEW_VIDEOCALL_3G) {
		err = sr030pc50_set_from_table(sd, "VT_init_reg",
				&state->regs->VT_init_reg, 1, 0);
		state->vt_initialized = 1;
		cam_info("VT Mode\n");
	}
#else
	if (state->vt_mode == PREVIEW_VIDEOCALL) {
		err = sr030pc50_set_from_table(sd, "VT_init_reg",
				&state->regs->VT_init_reg, 1, 0);
		cam_info("VT Mode\n");
	}
#endif
	else if (state->vt_mode == PREVIEW_SMARTSTAY) {
		err = sr030pc50_set_from_table(sd, "SS_init_reg",
				&state->regs->SS_init_reg, 1, 0);
		cam_info("SMART STAY Mode\n");
	} else {
		err = sr030pc50_set_from_table(sd, "init_reg",
				&state->regs->init_reg, 1, 0);
	}
	CHECK_ERR_MSG(err, "failed to initialize camera device\n");

	sr030pc50_init_parameter(sd);
	state->initialized = 1;

	if (val < 2)
		stats_init++;

	sr030pc50_restore_ctrl(sd);

	return 0;
}

static int sr030pc50_s_power(struct v4l2_subdev *sd, int on)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct sr030pc50_state *state =
		container_of(sd, struct sr030pc50_state, sd);
	int tries = 3;
	int err = 0;

	dev_dbg(&client->dev, "%s: %d\n", __func__, __LINE__);
        if (on) {
		stats_power++;
		err = sr030pc50_s_config(sd, 0, client->dev.platform_data); /* W/A*/
		CHECK_ERR_MSG(err, "fail to s_config\n");

		err = sr030pc50_get_power(sd);
		CHECK_ERR(err);

		err = sr030pc50_power(sd, 1);
		CHECK_ERR_GOTO(err, out_fail);
retry:
		err = sr030pc50_check_sensor(sd);
		if (err && --tries) {
			cam_err("fail to indentify sensor chip. retry (%d)", tries);
			err = sr030pc50_reset(sd, 1);
			CHECK_ERR_GOTO_MSG(err, out_fail,
				"s_stream: power-on failed\n");
			goto retry;
		} else if (!tries)
			goto out_fail;

		state->runmode = RUNMODE_NOTREADY;
	} else {
		state->initialized = 0;
#if defined(CONFIG_MACH_DELOSLTE_KOR_SKT) || defined(CONFIG_MACH_DELOSLTE_KOR_LGT)
		state->vt_initialized = 0;
#endif
		err = sr030pc50_power(sd, 0);
		CHECK_ERR_MSG(err, "s_power: fail to power off. %d\n", err);

		err = sr030pc50_put_power(sd);
		CHECK_ERR(err);

		cam_info("stats: power %u, init %u, rst %u, i2c %u\n",
			stats_power, stats_init, stats_reset, stats_i2c_err);
	}

	return 0;

out_fail:
	cam_err("s_power: error, couldn't init device");
	sr030pc50_s_power(sd, 0);
	return err;
}

static int sr030pc50_foo(struct v4l2_subdev *sd, u32 val)
{
	cam_info("init: dummy function\n");
	return 0;
}

static int sr030pc50_link_setup(struct media_entity *entity,
			    const struct media_pad *local,
			    const struct media_pad *remote, u32 flags)
{
	pr_debug("%s\n", __func__);
	return 0;
}

static const struct media_entity_operations sr030pc50_media_ops = {
	.link_setup = sr030pc50_link_setup,
};

/*
 * s_config subdev ops
 * With camera device, we need to re-initialize
 * every single opening time therefor,
 * it is not necessary to be initialized on probe time.
 * except for version checking
 * NOTE: version checking is optional
 */
static int sr030pc50_s_config(struct v4l2_subdev *sd,
			int irq, void *platform_data)
{
	struct sr030pc50_state *state = to_state(sd);
	int i;

	if (!platform_data) {
		cam_err("%s: error, no platform data\n", __func__);
		return -ENODEV;
	}
	state->pdata = platform_data;

	state->pdata->is_mipi = 1;
	state->dbg_level = &state->pdata->dbg_level;

	/*
	 * Assign default format and resolution
	 * Use configured default information in platform data
	 * or without them, use default information in driver
	 */
	state->req_fmt.width = state->pdata->default_width;
	state->req_fmt.height = state->pdata->default_height;

	if (!state->pdata->pixelformat)
		state->req_fmt.pixelformat = DEFAULT_PIX_FMT;
	else
		state->req_fmt.pixelformat = state->pdata->pixelformat;

	if (!state->pdata->freq)
		state->freq = DEFAULT_MCLK;	/* 24MHz default */
	else
		state->freq = state->pdata->freq;

	state->preview.frmsize = state->capture.frmsize = NULL;
	state->sensor_mode = SENSOR_CAMERA;
	state->format_mode = V4L2_PIX_FMT_MODE_PREVIEW;
	state->fps = 0;
	state->req_fps = -1;
	state->write_fps = 0;

	/* Initialize the independant HW module like flash here */
	state->flash.mode = FLASH_MODE_OFF;
	state->flash.on = 0;

	for (i = 0; i < ARRAY_SIZE(sr030pc50_ctrls); i++)
		sr030pc50_ctrls[i].value = sr030pc50_ctrls[i].default_value;

#ifdef SR030PC50_SUPPORT_FLASH
	if (sr030pc50_is_hwflash_on(sd)) {
		state->flash.ignore_flash = 1;
	}
#endif

	state->regs = &reg_datas;

	return 0;
}

static const struct v4l2_subdev_core_ops sr030pc50_core_ops = {
	.s_power = sr030pc50_s_power,
	.init = sr030pc50_foo,	/* initializing API */
	.g_ctrl = sr030pc50_g_ctrl,
	.s_ctrl = sr030pc50_pre_s_ctrl,
	.s_ext_ctrls = sr030pc50_s_ext_ctrls,
	.reset = sr030pc50_reset,
};

static const struct v4l2_subdev_video_ops sr030pc50_video_ops = {
	.s_mbus_fmt = sr030pc50_s_mbus_fmt,
	.enum_framesizes = sr030pc50_enum_framesizes,
	.enum_mbus_fmt = sr030pc50_enum_mbus_fmt,
	.try_mbus_fmt = sr030pc50_try_mbus_fmt,
	.g_parm = sr030pc50_g_parm,
	.s_parm = sr030pc50_s_parm,
	.s_stream = sr030pc50_s_stream,
};

/* get format by flite video device command */
static int sr030pc50_get_fmt(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh,
			  struct v4l2_subdev_format *fmt)
{
	struct sr030pc50_state *state =
		container_of(sd, struct sr030pc50_state, sd);
	struct v4l2_mbus_framefmt *format;

	if (fmt->pad != 0)
		return -EINVAL;

	format = __find_format(state, fh, fmt->which, state->res_type);
	if (!format)
		return -EINVAL;

	fmt->format = *format;

	return 0;
}

/* set format by flite video device command */
static int sr030pc50_set_fmt(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh,
			  struct v4l2_subdev_format *fmt)
{
	struct sr030pc50_state *state =
		container_of(sd, struct sr030pc50_state, sd);
	struct v4l2_mbus_framefmt *format = &fmt->format;
	struct v4l2_mbus_framefmt *sfmt;
	enum sr030pc50_oprmode type;
	u32 resolution = 0;
	int ret;

	if (fmt->pad != 0)
		return -EINVAL;

	ret = __find_resolution(sd, format, &type, &resolution);
	if (ret < 0)
		return ret;

	sfmt = __find_format(state, fh, fmt->which, type);
	if (!sfmt)
		return 0;

	sfmt		= &default_fmt[type];
	sfmt->width	= format->width;
	sfmt->height	= format->height;

	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE) {
		/* for enum size of entity by flite */
		state->ffmt[type].width 	= format->width;
		state->ffmt[type].height 	= format->height;
#ifndef CONFIG_VIDEO_SR030PC50_SENSOR_JPEG
		state->ffmt[type].code 		= V4L2_MBUS_FMT_YUYV8_2X8;
#else
		state->ffmt[type].code 		= format->code;
#endif

		/* find adaptable resolution */
		state->resolution 		= resolution;
#ifndef CONFIG_VIDEO_SR030PC50_SENSOR_JPEG
		state->code 			= V4L2_MBUS_FMT_YUYV8_2X8;
#else
		state->code 			= format->code;
#endif
		state->res_type 		= type;

		/* for set foramat */
		state->req_fmt.width 		= format->width;
		state->req_fmt.height 		= format->height;

		if ((state->power_on == SR030PC50_HW_POWER_ON)
		    && (state->runmode != RUNMODE_CAPTURING))
			sr030pc50_s_mbus_fmt(sd, sfmt);  /* set format */
	}

	return 0;
}

/* enum code by flite video device command */
static int sr030pc50_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_fh *fh,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (!code || code->index >= SIZE_DEFAULT_FFMT)
		return -EINVAL;

	code->code = default_fmt[code->index].code;

	return 0;
}

static struct v4l2_subdev_pad_ops sr030pc50_pad_ops = {
	.enum_mbus_code	= sr030pc50_enum_mbus_code,
	.get_fmt	= sr030pc50_get_fmt,
	.set_fmt	= sr030pc50_set_fmt,
};

static const struct v4l2_subdev_ops sr030pc50_ops = {
	.core = &sr030pc50_core_ops,
	.pad	= &sr030pc50_pad_ops,
	.video = &sr030pc50_video_ops,
};

/* internal ops for media controller */
static int sr030pc50_init_formats(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct v4l2_subdev_format format;
	struct i2c_client *client = v4l2_get_subdevdata(sd);

	dev_err(&client->dev, "%s: \n", __func__);
	memset(&format, 0, sizeof(format));
	format.pad = 0;
	format.which = fh ? V4L2_SUBDEV_FORMAT_TRY : V4L2_SUBDEV_FORMAT_ACTIVE;
	format.format.code = DEFAULT_SENSOR_CODE;
	format.format.width = DEFAULT_SENSOR_WIDTH;
	format.format.height = DEFAULT_SENSOR_HEIGHT;

#ifdef ENABLE
	sr030pc50_set_fmt(sd, fh, &format);
	sr030pc50_s_parm(sd, &state->strm);
#endif

	return 0;
}

static int sr030pc50_subdev_close(struct v4l2_subdev *sd,
			      struct v4l2_subdev_fh *fh)
{
	pr_debug("%s", __func__);
	pr_info("%s", __func__);
	return 0;
}

static int sr030pc50_subdev_registered(struct v4l2_subdev *sd)
{
	pr_debug("%s", __func__);
	return 0;
}

static void sr030pc50_subdev_unregistered(struct v4l2_subdev *sd)
{
	pr_debug("%s", __func__);
}

static const struct v4l2_subdev_internal_ops sr030pc50_v4l2_internal_ops = {
	.open = sr030pc50_init_formats,
	.close = sr030pc50_subdev_close,
	.registered = sr030pc50_subdev_registered,
	.unregistered = sr030pc50_subdev_unregistered,
};

/*
 * sr030pc50_probe
 * Fetching platform data is being done with s_config subdev call.
 * In probe routine, we just register subdev device
 */
 #ifdef CONFIG_CAM_EARYLY_PROBE
static int sr030pc50_late_probe(struct v4l2_subdev *sd)
{
	struct sr030pc50_state *state = to_state(sd);
	struct sr030pc50_core_state *c_state = to_c_state(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int err = -EINVAL;

retry:
	if (unlikely(!c_state || !state)) {
		dev_err(&client->dev, "late_probe, fail to get memory."
			" c_state = 0x%X, state = 0x%X\n", (u32)c_state, (u32)state);
		return -ENOMEM;
	}

	memset(state, 0, sizeof(struct sr030pc50_state));
	state->c_state = c_state;
	state->sd = sd;
	state->wq = c_state->wq;
	strcpy(state->s_name, "sr030pc50_state"); /* for debugging */

	mutex_init(&state->ctrl_lock);
	mutex_init(&state->af_lock);

	state->runmode = RUNMODE_NOTREADY;

	err = sr030pc50_s_config(sd, 0, client->dev.platform_data);
	CHECK_ERR_MSG(err, "probe: fail to s_config\n");

	if (IS_AF_SUPPORTED()) {
		INIT_WORK(&state->af_work, sr030pc50_af_worker);
		INIT_WORK(&state->af_win_work, sr030pc50_af_win_worker);
	}

#if defined(CONFIG_VIDEO_FAST_MODECHANGE) \
    || defined(CONFIG_VIDEO_FAST_MODECHANGE_V2)
	INIT_WORK(&state->streamoff_work, sr030pc50_streamoff_checker);
#endif
#ifdef CONFIG_VIDEO_FAST_CAPTURE
	INIT_WORK(&state->capmode_work, sr030pc50_capmode_checker);
#endif

	state->mclk = clk_get(NULL, "cam1");
	if (IS_ERR_OR_NULL(state->mclk)) {
		pr_err("failed to get cam1 clk (mclk)");
		return -ENXIO;
	}

	err = sr030pc50_get_power(sd);
	CHECK_ERR_GOTO_MSG(err, err_out, "probe: fail to get power\n");

	printk(KERN_DEBUG "%s %s: driver late probed!\n",
		dev_driver_string(&client->dev), dev_name(&client->dev));

	return 0;

err_out:
	sr030pc50_put_power(sd);
	return -ENOMEM;
}

static int sr030pc50_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct v4l2_subdev *sd;
	struct sr030pc50_core_state *c_state;
	struct sr030pc50_state *state;
	int err = -EINVAL;

	c_state = kzalloc(sizeof(struct sr030pc50_core_state), GFP_KERNEL);
	if (unlikely(!c_state)) {
		dev_err(&client->dev, "early_probe, fail to get memory\n");
		return -ENOMEM;
	}

	state = kzalloc(sizeof(struct sr030pc50_state), GFP_KERNEL);
	if (unlikely(!state)) {
		dev_err(&client->dev, "early_probe, fail to get memory\n");
		goto err_out2;
	}

	c_state->data = (u32)state;
	sd = &c_state->sd;
	strcpy(sd->name, driver_name);
	strcpy(c_state->c_name, "sr030pc50_core_state"); /* for debugging */

	/* Registering subdev */
	v4l2_i2c_subdev_init(sd, client, &sr030pc50_ops);

#ifdef CONFIG_MEDIA_CONTROLLER
	c_state->pad.flags = MEDIA_PAD_FL_SOURCE;
	err = media_entity_init(&sd->entity, 1, &c_state->pad, 0);
	if (unlikely(err)) {
		dev_err(&client->dev, "probe: fail to init media entity\n");
		goto err_out1;
	}

	sd->entity.type = MEDIA_ENT_T_V4L2_SUBDEV_SENSOR;
	sd->entity.ops = &sr030pc50_media_ops;
#endif

	sd->flags = V4L2_SUBDEV_FL_HAS_DEVNODE;
	sd->internal_ops = &sr030pc50_v4l2_internal_ops;

	c_state->wq = create_workqueue("cam_wq");
	if (unlikely(!c_state->wq)) {
		dev_err(&client->dev,
			"early_probe: fail to create workqueue\n");
		goto err_out1;
	}

	printk(KERN_DEBUG "%s %s: driver probed!\n",
		dev_driver_string(&client->dev), dev_name(&client->dev));

	return 0;

err_out1:
	kfree(state);
err_out2:
	kfree(c_state);
	return -ENOMEM;
}

static int sr030pc50_early_remove(struct v4l2_subdev *sd)
{
	struct sr030pc50_state *state = to_state(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int err = 0, ret = 0;

	if (state->wq)
		flush_workqueue(state->wq);

	/* Check whether flash is on when unlolading driver,
	 * to preventing Market App from controlling improperly flash.
	 * It isn't necessary in case that you power flash down
	 * in power routine to turn camera off.*/
	if (unlikely(state->flash.on && !state->flash.ignore_flash))
		sr030pc50_flash_torch(sd, SR030PC50_FLASH_OFF);

	err = sr030pc50_power(sd, 0);
	if (unlikely(err)) {
		cam_info("remove: power off failed. %d\n", err);
		ret = err;
	}

	state->power_on = SR030PC50_HW_POWER_OFF;

	err = sr030pc50_put_power(sd);
	if (unlikely(err)) {
		cam_info("remove: put power failed. %d\n", err);
		ret = err;
	}

	cam_info("stats: power %u, init %u, rst %u, i2c %u\n",
		stats_power, stats_init, stats_reset, stats_i2c_err);

	printk(KERN_DEBUG "%s %s: driver early removed!!\n",
		dev_driver_string(&client->dev), dev_name(&client->dev));
	return ret;
}

static int sr030pc50_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct sr030pc50_state *state = to_state(sd);

#ifdef CONFIG_MEDIA_CONTROLLER
	media_entity_cleanup(&sd->entity);
#endif

	v4l2_device_unregister_subdev(sd);
	mutex_destroy(&state->ctrl_lock);
	kfree(state);

	printk(KERN_DEBUG "%s %s: driver removed!!\n",
		dev_driver_string(&client->dev), dev_name(&client->dev));
	return 0;
}
#else
static int sr030pc50_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct v4l2_subdev *sd;
	struct sr030pc50_state *state;
	int err = -EINVAL;
	int ret;

	state = kzalloc(sizeof(struct sr030pc50_state), GFP_KERNEL);
	if (unlikely(!state)) {
		dev_err(&client->dev, "probe, fail to get memory\n");
		return -ENOMEM;
	}

	mutex_init(&state->ctrl_lock);

	state->runmode = RUNMODE_NOTREADY;
	sd = &state->sd;
	strcpy(sd->name, SR030PC50_DRIVER_NAME);

	/* Registering subdev */
	v4l2_i2c_subdev_init(sd, client, &sr030pc50_ops);

	state->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_init(&sd->entity, 1, &state->pad, 0);
	if (ret < 0)
		goto err_out;

	state->wq = create_workqueue("cam_workqueue");
	if (unlikely(!state->wq)) {
		dev_err(&client->dev, "probe, fail to create wq\n");
		goto err_out;
	}

	err = sr030pc50_s_config(sd, 0, client->dev.platform_data);
	CHECK_ERR_MSG(err, "fail to s_config\n");

	sr030pc50_init_formats(sd, NULL);

	sd->entity.type = MEDIA_ENT_T_V4L2_SUBDEV_SENSOR;
	sd->flags = V4L2_SUBDEV_FL_HAS_DEVNODE;
	sd->internal_ops = &sr030pc50_v4l2_internal_ops;
	sd->entity.ops = &sr030pc50_media_ops;

	state->mclk = clk_get(NULL, "cam1");
	if (IS_ERR_OR_NULL(state->mclk)) {
		pr_err("failed to get cam1 clk (mclk)");
		return -ENXIO;
	}

	printk(KERN_DEBUG "%s %s: driver probed!!\n",
		dev_driver_string(&client->dev), dev_name(&client->dev));

	return 0;

err_out:
	kfree(state);
	return -ENOMEM;
}

static int sr030pc50_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct sr030pc50_state *state = to_state(sd);

	if (state->wq)
		flush_workqueue(state->wq);

	/* Check whether flash is on when unlolading driver,
	 * to preventing Market App from controlling improperly flash.
	 * It isn't necessary in case that you power flash down
	 * in power routine to turn camera off.*/
	if (unlikely(state->flash.on && !state->flash.ignore_flash))
		sr030pc50_flash_torch(sd, SR030PC50_FLASH_OFF);

	v4l2_device_unregister_subdev(sd);
	mutex_destroy(&state->ctrl_lock);
	kfree(state);

	printk(KERN_DEBUG "%s %s: driver removed!!\n",
		dev_driver_string(&client->dev), dev_name(&client->dev));
	return 0;
}
#endif

static ssize_t camtype_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	/*pr_info("%s\n", __func__);*/
	return sprintf(buf, "%s_%s\n", "SF", "SR030PC50");
}
static DEVICE_ATTR(front_camtype, S_IRUGO, camtype_show, NULL);

static ssize_t camfw_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%s %s\n", "SR030PC50", "SR030PC50");

}
static DEVICE_ATTR(front_camfw, S_IRUGO, camfw_show, NULL);

static int is_sysdev(struct device *dev, void *str)
{
	return !strcmp(dev_name(dev), (char *)str) ? 1 : 0;
}

static ssize_t cam_loglevel_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	char temp_buf[60] = {0,};

	sprintf(buf, "Log Level: ");
	if (dbg_level & CAMDBG_LEVEL_TRACE) {
		sprintf(temp_buf, "trace ");
		strcat(buf, temp_buf);
	}

	if (dbg_level & CAMDBG_LEVEL_DEBUG) {
		sprintf(temp_buf, "debug ");
		strcat(buf, temp_buf);
	}

	if (dbg_level & CAMDBG_LEVEL_INFO) {
		sprintf(temp_buf, "info ");
		strcat(buf, temp_buf);
	}

	sprintf(temp_buf, "\n - warn and error level is always on\n\n");
	strcat(buf, temp_buf);

	return strlen(buf);
}

static ssize_t cam_loglevel_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count)
{
	printk(KERN_DEBUG "CAM buf=%s, count=%d\n", buf, count);

	if (strstr(buf, "trace"))
		dbg_level |= CAMDBG_LEVEL_TRACE;
	else
		dbg_level &= ~CAMDBG_LEVEL_TRACE;

	if (strstr(buf, "debug"))
		dbg_level |= CAMDBG_LEVEL_DEBUG;
	else
		dbg_level &= ~CAMDBG_LEVEL_DEBUG;

	if (strstr(buf, "info"))
		dbg_level |= CAMDBG_LEVEL_INFO;

	return count;
}

static DEVICE_ATTR(loglevel, 0664, cam_loglevel_show, cam_loglevel_store);

static int sr030pc50_create_dbglogfile(struct class *cls)
{
	struct device *dev;
	int err;

	dbg_level |= CAMDBG_LEVEL_DEFAULT;

	dev = class_find_device(cls, NULL, "front", is_sysdev);
	if (unlikely(!dev)) {
		pr_info("[SR030PC50] can not find front device\n");
		return 0;
	}

	err = device_create_file(dev, &dev_attr_loglevel);
	if (unlikely(err < 0)) {
		pr_err("cam_init: failed to create device file, %s\n",
			dev_attr_loglevel.attr.name);
	}

	return 0;
}

int sr030pc50_create_sysfs(struct class *cls)
{
	struct device *dev = NULL;
	int err = -ENODEV;

	dev = device_create(cls, NULL, 0, NULL, "front");
	if (IS_ERR(dev)) {
		pr_err("cam_init: failed to create device(frontcam_dev)\n");
		return -ENODEV;
	}

	err = device_create_file(dev, &dev_attr_front_camtype);
	if (unlikely(err < 0)) {
		pr_err("cam_init: failed to create device file, %s\n",
			dev_attr_front_camtype.attr.name);
	}

	err = device_create_file(dev, &dev_attr_front_camfw);
	if (unlikely(err < 0)) {
		pr_err("cam_init: failed to create device file, %s\n",
			dev_attr_front_camtype.attr.name);
	}

	return 0;
}

static const struct i2c_device_id sr030pc50_id[] = {
	{ SR030PC50_DRIVER_NAME, 0 },
	{}
};

MODULE_DEVICE_TABLE(i2c, sr030pc50_id);

static struct i2c_driver v4l2_i2c_driver = {
	.driver.name	= SR030PC50_DRIVER_NAME,
	.probe		= sr030pc50_probe,
	.remove		= sr030pc50_remove,
	.id_table	= sr030pc50_id,
};

static int __init v4l2_i2c_drv_init(void)
{
	pr_info("%s: %s called\n", __func__, SR030PC50_DRIVER_NAME); /* dslim*/
	sr030pc50_create_sysfs(camera_class);
	sr030pc50_create_dbglogfile(camera_class);
	return i2c_add_driver(&v4l2_i2c_driver);
}

static void __exit v4l2_i2c_drv_cleanup(void)
{
	pr_info("%s: %s called\n", __func__, SR030PC50_DRIVER_NAME); /* dslim*/
	i2c_del_driver(&v4l2_i2c_driver);
}

module_init(v4l2_i2c_drv_init);
module_exit(v4l2_i2c_drv_cleanup);

MODULE_DESCRIPTION("SILICONFILE SR030PC50 1.3MP SOC camera driver");
MODULE_AUTHOR("Dong-Seong Lim <dongseong.lim@samsung.com>");
MODULE_LICENSE("GPL");
