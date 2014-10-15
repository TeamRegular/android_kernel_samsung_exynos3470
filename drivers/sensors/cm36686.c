/* driver/sensor/cm36686.c
 * Copyright (c) 2011 SAMSUNG
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA  02110-1301, USA.
 */

#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/i2c.h>
#include <linux/errno.h>
#include <linux/device.h>
#include <linux/gpio.h>
#include <linux/wakelock.h>
#include <linux/input.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/sensor/cm36686.h>
#include <linux/sensor/sensors_core.h>

/* For debugging */
#undef	cm36686_DEBUG

#define	VENDOR		"CAPELLA"
#define	CHIP_ID		"CM36686"

#define I2C_M_WR	0 /* for i2c Write */
#define I2c_M_RD	1 /* for i2c Read */

/* register addresses */
/* Ambient light sensor */
#define REG_CS_CONF1	0x00
#define REG_ALS_DATA	0x09
#define REG_WHITE_DATA	0x0A

/* Proximity sensor */
#define REG_PS_CONF1		0x03
#define REG_PS_CONF3		0x04
#define REG_PS_CANC		0x05
#define REG_PS_THD_LOW		0x06
#define REG_PS_THD_HIGH		0x07
#define REG_PS_DATA		0x08

#define ALS_REG_NUM		2
#define PS_REG_NUM		5

#define MSK_L(x)		(x & 0xff)
#define MSK_H(x)		((x & 0xff00) >> 8)

/* Intelligent Cancelation*/
#define CM36686_CANCELATION
#ifdef CM36686_CANCELATION
#define CANCELATION_FILE_PATH	"/efs/prox_cal"
#endif

#define PROX_READ_NUM		40
 /* proximity sensor threshold */
#define DEFUALT_HI_THD		0x0022
#define DEFUALT_LOW_THD		0x001E
#define CANCEL_HI_THD		0x0022
#define CANCEL_LOW_THD		0x001E

 /*lightsnesor log time 6SEC 200mec X 30*/
#define LIGHT_LOG_TIME		30
#define LIGHT_ADD_STARTTIME		300000000
enum {
	LIGHT_ENABLED = BIT(0),
	PROXIMITY_ENABLED = BIT(1),
};

/* register settings */
static u16 als_reg_setting[ALS_REG_NUM][2] = {
	{REG_CS_CONF1, 0x0000},	/* enable */
	{REG_CS_CONF1, 0x0001},	/* disable */
};

/* Change threshold value on the midas-sensor.c */
enum {
	PS_CONF1 = 0,
	PS_CONF3,
	PS_THD_LOW,
	PS_THD_HIGH,
	PS_CANCEL,
};
enum {
	REG_ADDR = 0,
	CMD,
};

static u16 ps_reg_init_setting[PS_REG_NUM][2] = {
	{REG_PS_CONF1, 0x0300},	/* REG_PS_CONF1 */
	{REG_PS_CONF3, 0x4200},	/* REG_PS_CONF3 */
	{REG_PS_THD_LOW, DEFUALT_LOW_THD},	/* REG_PS_THD_LOW */
	{REG_PS_THD_HIGH, DEFUALT_HI_THD},	/* REG_PS_THD_HIGH */
	{REG_PS_CANC, 0x0000},	/* REG_PS_CANC */
};

/* driver data */
struct cm36686_data {
	struct i2c_client *i2c_client;
	struct wake_lock prx_wake_lock;
	struct input_dev *proximity_input_dev;
	struct input_dev *light_input_dev;
	struct cm36686_platform_data *pdata;
	struct mutex power_lock;
	struct mutex read_lock;
	struct hrtimer light_timer;
	struct hrtimer prox_timer;
	struct workqueue_struct *light_wq;
	struct workqueue_struct *prox_wq;
	struct work_struct work_light;
	struct work_struct work_prox;
	struct device *proximity_dev;
	struct device *light_dev;
	ktime_t light_poll_delay;
	ktime_t prox_poll_delay;
	int irq;
	u8 power_state;
	int avg[3];
	u16 als_data;
	u16 white_data;
	int count_log_time;

	void (*cm36686_light_vddpower)(bool);
	void (*cm36686_proxi_vddpower)(bool);
};

int cm36686_i2c_read_word(struct cm36686_data *cm36686, u8 command,
			  u16 *val)
{
	int err = 0;
	int retry = 3;
	struct i2c_client *client = cm36686->i2c_client;
	struct i2c_msg msg[2];
	unsigned char data[2] = {0,};
	u16 value = 0;

	if ((client == NULL) || (!client->adapter))
		return -ENODEV;

	while (retry--) {
		/* send slave address & command */
		msg[0].addr = client->addr;
		msg[0].flags = I2C_M_WR;
		msg[0].len = 1;
		msg[0].buf = &command;

		/* read word data */
		msg[1].addr = client->addr;
		msg[1].flags = I2C_M_RD;
		msg[1].len = 2;
		msg[1].buf = data;

		err = i2c_transfer(client->adapter, msg, 2);

		if (err >= 0) {
			value = (u16)data[1];
			*val = (value << 8) | (u16)data[0];
			return err;
		}
	}
	pr_err("%s, i2c transfer error ret=%d\n", __func__, err);
	return err;
}

int cm36686_i2c_write_word(struct cm36686_data *cm36686, u8 command,
			   u16 val)
{
	int err = 0;
	struct i2c_client *client = cm36686->i2c_client;
	int retry = 3;

	if ((client == NULL) || (!client->adapter))
		return -ENODEV;

	while (retry--) {
		err = i2c_smbus_write_word_data(client, command, val);
		if (err >= 0)
			return 0;
	}
	pr_err("%s, i2c transfer error(%d)\n", __func__, err);
	return err;
}

static void cm36686_light_enable(struct cm36686_data *cm36686)
{
	/* enable setting */
	cm36686_i2c_write_word(cm36686, REG_CS_CONF1,
		als_reg_setting[0][1]);
	hrtimer_start(&cm36686->light_timer, ns_to_ktime(200 * NSEC_PER_MSEC),
		HRTIMER_MODE_REL);
}

static void cm36686_light_disable(struct cm36686_data *cm36686)
{
	/* disable setting */
	cm36686_i2c_write_word(cm36686, REG_CS_CONF1,
		als_reg_setting[1][1]);
	hrtimer_cancel(&cm36686->light_timer);
	cancel_work_sync(&cm36686->work_light);
}

/* sysfs */
static ssize_t cm36686_poll_delay_show(struct device *dev,
					struct device_attribute *attr, char *buf)
{
	struct cm36686_data *cm36686 = dev_get_drvdata(dev);
	return sprintf(buf, "%lld\n", ktime_to_ns(cm36686->light_poll_delay));
}

static ssize_t cm36686_poll_delay_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t size)
{
	struct cm36686_data *cm36686 = dev_get_drvdata(dev);
	int64_t new_delay;
	int err;

	err = strict_strtoll(buf, 10, &new_delay);
	if (err < 0)
		return err;

	mutex_lock(&cm36686->power_lock);
	if (new_delay != ktime_to_ns(cm36686->light_poll_delay)) {
		cm36686->light_poll_delay = ns_to_ktime(new_delay);
		if (cm36686->power_state & LIGHT_ENABLED) {
			cm36686_light_disable(cm36686);
			cm36686_light_enable(cm36686);
		}
		pr_info("%s, poll_delay = %lld\n", __func__, new_delay);
	}
	mutex_unlock(&cm36686->power_lock);

	return size;
}

static ssize_t light_enable_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t size)
{
	struct cm36686_data *cm36686 = dev_get_drvdata(dev);
	bool new_value;

	if (sysfs_streq(buf, "1"))
		new_value = true;
	else if (sysfs_streq(buf, "0"))
		new_value = false;
	else {
		pr_err("%s: invalid value %d\n", __func__, *buf);
		return -EINVAL;
	}

	mutex_lock(&cm36686->power_lock);
	pr_info("%s,new_value=%d\n", __func__, new_value);
	if (new_value && !(cm36686->power_state & LIGHT_ENABLED)) {

		if (cm36686->cm36686_light_vddpower)
			cm36686->cm36686_light_vddpower(true);

		cm36686->power_state |= LIGHT_ENABLED;
		cm36686_light_enable(cm36686);
	} else if (!new_value && (cm36686->power_state & LIGHT_ENABLED)) {
		cm36686_light_disable(cm36686);
		cm36686->power_state &= ~LIGHT_ENABLED;

		if (cm36686->cm36686_light_vddpower)
			cm36686->cm36686_light_vddpower(false);
	}
	mutex_unlock(&cm36686->power_lock);
	return size;
}

static ssize_t light_enable_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct cm36686_data *cm36686 = dev_get_drvdata(dev);
	return sprintf(buf, "%d\n",
		(cm36686->power_state & LIGHT_ENABLED) ? 1 : 0);
}

#ifdef CM36686_CANCELATION
static int proximity_open_cancelation(struct cm36686_data *data)
{
	struct file *cancel_filp = NULL;
	int err = 0;
	mm_segment_t old_fs;

	old_fs = get_fs();
	set_fs(KERNEL_DS);

	cancel_filp = filp_open(CANCELATION_FILE_PATH, O_RDONLY, 0666);
	if (IS_ERR(cancel_filp)) {
		err = PTR_ERR(cancel_filp);
		if (err != -ENOENT)
			pr_err("%s: Can't open cancelation file\n", __func__);
		set_fs(old_fs);
		return err;
	}

	err = cancel_filp->f_op->read(cancel_filp,
		(char *)&ps_reg_init_setting[PS_CANCEL][CMD],
		sizeof(u16), &cancel_filp->f_pos);
	if (err != sizeof(u16)) {
		pr_err("%s: Can't read the cancel data from file\n", __func__);
		err = -EIO;
	}

	/*If there is an offset cal data. */
	if (ps_reg_init_setting[PS_CANCEL][CMD] != 0) {
		if (data->pdata->cancel_hi_thd) {
			ps_reg_init_setting[PS_THD_HIGH][CMD] =
				data->pdata->cancel_hi_thd;
		} else
			ps_reg_init_setting[PS_THD_HIGH][CMD] = CANCEL_HI_THD;

		if (data->pdata->cancel_low_thd) {
			ps_reg_init_setting[PS_THD_LOW][CMD] =
				data->pdata->cancel_low_thd;
		} else
			ps_reg_init_setting[PS_THD_LOW][CMD] = CANCEL_LOW_THD;
	}

	pr_info("%s: prox_cal = 0x%x, ps_high_thresh = 0x%x, ps_low_thresh = 0x%x\n",
		__func__, ps_reg_init_setting[PS_CANCEL][CMD],
		ps_reg_init_setting[PS_THD_HIGH][CMD], ps_reg_init_setting[PS_THD_LOW][CMD]);

	filp_close(cancel_filp, current->files);
	set_fs(old_fs);

	return err;
}

static int proximity_store_cancelation(struct device *dev, bool do_calib)
{
	struct cm36686_data *cm36686 = dev_get_drvdata(dev);
	struct file *cancel_filp = NULL;
	mm_segment_t old_fs;
	int err = 0;
	u16 ps_data = 0;

	if (do_calib) {
		mutex_lock(&cm36686->read_lock);
		cm36686_i2c_read_word(cm36686,
			REG_PS_DATA, &ps_data);
		ps_reg_init_setting[PS_CANCEL][CMD] = ps_data;
		mutex_unlock(&cm36686->read_lock);

		if (cm36686->pdata->cancel_hi_thd) {
			ps_reg_init_setting[PS_THD_HIGH][CMD] =
				cm36686->pdata->cancel_hi_thd;
		} else
			ps_reg_init_setting[PS_THD_HIGH][CMD] = CANCEL_HI_THD;

		if (cm36686->pdata->cancel_low_thd) {
			ps_reg_init_setting[PS_THD_LOW][CMD] =
				cm36686->pdata->cancel_low_thd;
		} else
			ps_reg_init_setting[PS_THD_LOW][CMD] = DEFUALT_LOW_THD;
	} else { /* reset */
		ps_reg_init_setting[PS_CANCEL][CMD] = 0;
		if (cm36686->pdata->default_hi_thd) {
			ps_reg_init_setting[PS_THD_HIGH][CMD] =
				cm36686->pdata->default_hi_thd;
		} else
			ps_reg_init_setting[PS_THD_HIGH][CMD] = DEFUALT_HI_THD;

		if (cm36686->pdata->default_low_thd) {
			ps_reg_init_setting[PS_THD_LOW][CMD] =
				cm36686->pdata->default_low_thd;
		} else
			ps_reg_init_setting[PS_THD_LOW][CMD] = DEFUALT_LOW_THD;
	}

	err = cm36686_i2c_write_word(cm36686, REG_PS_CANC,
		ps_reg_init_setting[PS_CANCEL][CMD]);
	if (err < 0)
		pr_err("%s: cm36686_ps_canc_reg is failed. %d\n", __func__,
			err);
	err = cm36686_i2c_write_word(cm36686, REG_PS_THD_HIGH,
		ps_reg_init_setting[PS_THD_HIGH][CMD]);
	if (err < 0)
		pr_err("%s: cm36686_ps_high_reg is failed. %d\n", __func__,
			err);
	err = cm36686_i2c_write_word(cm36686, REG_PS_THD_LOW,
		ps_reg_init_setting[PS_THD_LOW][CMD]);
	if (err < 0)
		pr_err("%s: cm36686_ps_low_reg is failed. %d\n", __func__,
			err);

	pr_info("%s: prox_cal = 0x%x, ps_high_thresh = 0x%x, ps_low_thresh = 0x%x\n",
		__func__, ps_reg_init_setting[PS_CANCEL][CMD],
		ps_reg_init_setting[PS_THD_HIGH][CMD], ps_reg_init_setting[PS_THD_LOW][CMD]);

	old_fs = get_fs();
	set_fs(KERNEL_DS);

	cancel_filp = filp_open(CANCELATION_FILE_PATH,
			O_CREAT | O_TRUNC | O_WRONLY | O_SYNC, 0666);
	if (IS_ERR(cancel_filp)) {
		pr_err("%s: Can't open cancelation file\n", __func__);
		set_fs(old_fs);
		err = PTR_ERR(cancel_filp);
		return err;
	}

	err = cancel_filp->f_op->write(cancel_filp,
		(char *)&ps_reg_init_setting[PS_CANCEL][CMD],
		sizeof(u16), &cancel_filp->f_pos);
	if (err != sizeof(u16)) {
		pr_err("%s: Can't write the cancel data to file\n", __func__);
		err = -EIO;
	}

	filp_close(cancel_filp, current->files);
	set_fs(old_fs);

	if (!do_calib) /* delay for clearing */
		msleep(150);

	return err;
}

static ssize_t proximity_cancel_store(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf, size_t size)
{
	bool do_calib;
	int err;

	if (sysfs_streq(buf, "1")) /* calibrate cancelation value */
		do_calib = true;
	else if (sysfs_streq(buf, "0")) /* reset cancelation value */
		do_calib = false;
	else {
		pr_debug("%s: invalid value %d\n", __func__, *buf);
		return -EINVAL;
	}

	err = proximity_store_cancelation(dev, do_calib);
	if (err < 0) {
		pr_err("%s: proximity_store_cancelation() failed\n", __func__);
		return err;
	}

	return size;
}

static ssize_t proximity_cancel_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%u,%u,%u\n", ps_reg_init_setting[PS_CANCEL][CMD],
		ps_reg_init_setting[PS_THD_HIGH][CMD],ps_reg_init_setting[PS_THD_LOW][CMD]);
}
#endif

static ssize_t proximity_enable_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t size)
{
	struct cm36686_data *cm36686 = dev_get_drvdata(dev);
	bool new_value;

	if (sysfs_streq(buf, "1"))
		new_value = true;
	else if (sysfs_streq(buf, "0"))
		new_value = false;
	else {
		pr_err("%s: invalid value %d\n", __func__, *buf);
		return -EINVAL;
	}

	mutex_lock(&cm36686->power_lock);
	pr_info("%s, new_value = %d\n", __func__, new_value);
	if (new_value && !(cm36686->power_state & PROXIMITY_ENABLED)) {
		u8 val = 1;
		int i;
		int err = 0;

		cm36686->power_state |= PROXIMITY_ENABLED;

		if (cm36686->cm36686_proxi_vddpower)
			cm36686->cm36686_proxi_vddpower(true);

		if (cm36686->pdata->cm36686_led_on) {
			cm36686->pdata->cm36686_led_on(true);
			msleep(20);
		}
#ifdef CM36686_CANCELATION
		/* open cancelation data */
		err = proximity_open_cancelation(cm36686);
		if (err < 0 && err != -ENOENT)
			pr_err("%s: proximity_open_cancelation() failed\n",
				__func__);
#endif
		/* enable settings */
		for (i = 0; i < PS_REG_NUM; i++) {
			cm36686_i2c_write_word(cm36686,
				ps_reg_init_setting[i][REG_ADDR],
				ps_reg_init_setting[i][CMD]);
		}

		val = gpio_get_value(cm36686->pdata->irq);
		/* 0 is close, 1 is far */
		input_report_abs(cm36686->proximity_input_dev,
			ABS_DISTANCE, val);
		input_sync(cm36686->proximity_input_dev);

		enable_irq(cm36686->irq);
		enable_irq_wake(cm36686->irq);
	} else if (!new_value && (cm36686->power_state & PROXIMITY_ENABLED)) {
		cm36686->power_state &= ~PROXIMITY_ENABLED;

		disable_irq_wake(cm36686->irq);
		disable_irq(cm36686->irq);
		/* disable settings */
		cm36686_i2c_write_word(cm36686, REG_PS_CONF1, 0x0001);

		if (cm36686->pdata->cm36686_led_on)
			cm36686->pdata->cm36686_led_on(false);

		if (cm36686->cm36686_proxi_vddpower)
			cm36686->cm36686_proxi_vddpower(false);
	}
	mutex_unlock(&cm36686->power_lock);
	return size;
}

static ssize_t proximity_enable_show(struct device *dev,
					struct device_attribute *attr, char *buf)
{
	struct cm36686_data *cm36686 = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n",
			(cm36686->power_state & PROXIMITY_ENABLED) ? 1 : 0);
}

static DEVICE_ATTR(poll_delay, S_IRUGO | S_IWUSR | S_IWGRP,
			cm36686_poll_delay_show, cm36686_poll_delay_store);

static struct device_attribute dev_attr_light_enable =
__ATTR(enable, S_IRUGO | S_IWUSR | S_IWGRP,
	light_enable_show, light_enable_store);

static struct device_attribute dev_attr_proximity_enable =
__ATTR(enable, S_IRUGO | S_IWUSR | S_IWGRP,
	proximity_enable_show, proximity_enable_store);

static struct attribute *light_sysfs_attrs[] = {
	&dev_attr_light_enable.attr,
	&dev_attr_poll_delay.attr,
	NULL
};

static struct attribute_group light_attribute_group = {
	.attrs = light_sysfs_attrs,
};

static struct attribute *proximity_sysfs_attrs[] = {
	&dev_attr_proximity_enable.attr,
	NULL
};

static struct attribute_group proximity_attribute_group = {
	.attrs = proximity_sysfs_attrs,
};

/* proximity sysfs */
static ssize_t proximity_avg_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct cm36686_data *cm36686 = dev_get_drvdata(dev);

	return sprintf(buf, "%d,%d,%d\n", cm36686->avg[0],
		cm36686->avg[1], cm36686->avg[2]);
}

static ssize_t proximity_avg_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	struct cm36686_data *cm36686 = dev_get_drvdata(dev);
	bool new_value = false;

	if (sysfs_streq(buf, "1"))
		new_value = true;
	else if (sysfs_streq(buf, "0"))
		new_value = false;
	else {
		pr_err("%s, invalid value %d\n", __func__, *buf);
		return -EINVAL;
	}

	pr_info("%s, average enable = %d\n", __func__, new_value);
	mutex_lock(&cm36686->power_lock);
	if (new_value) {
		if (!(cm36686->power_state & PROXIMITY_ENABLED)) {
			if (cm36686->cm36686_proxi_vddpower)
				cm36686->cm36686_proxi_vddpower(true);

			if (cm36686->pdata->cm36686_led_on) {
				cm36686->pdata->cm36686_led_on(true);
				msleep(20);
			}
			cm36686_i2c_write_word(cm36686, REG_PS_CONF1,
				ps_reg_init_setting[PS_CONF1][CMD]);
		}
		hrtimer_start(&cm36686->prox_timer, cm36686->prox_poll_delay,
			HRTIMER_MODE_REL);
	} else if (!new_value) {
		hrtimer_cancel(&cm36686->prox_timer);
		cancel_work_sync(&cm36686->work_prox);
		if (!(cm36686->power_state & PROXIMITY_ENABLED)) {
			cm36686_i2c_write_word(cm36686, REG_PS_CONF1,
				0x0001);
			if (cm36686->pdata->cm36686_led_on)
				cm36686->pdata->cm36686_led_on(false);

			if (cm36686->cm36686_proxi_vddpower)
				cm36686->cm36686_proxi_vddpower(false);
		}
	}
	mutex_unlock(&cm36686->power_lock);

	return size;
}

static ssize_t proximity_state_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct cm36686_data *cm36686 = dev_get_drvdata(dev);
	u16 ps_data;

	mutex_lock(&cm36686->power_lock);
	if (!(cm36686->power_state & PROXIMITY_ENABLED)) {
		if (cm36686->cm36686_proxi_vddpower)
			cm36686->cm36686_proxi_vddpower(true);

		if (cm36686->pdata->cm36686_led_on) {
			cm36686->pdata->cm36686_led_on(true);
			msleep(20);
		}
		cm36686_i2c_write_word(cm36686, REG_PS_CONF1,
			ps_reg_init_setting[PS_CONF1][CMD]);
	}

	mutex_lock(&cm36686->read_lock);
	cm36686_i2c_read_word(cm36686, REG_PS_DATA,
		&ps_data);
	mutex_unlock(&cm36686->read_lock);

	if (!(cm36686->power_state & PROXIMITY_ENABLED)) {
		cm36686_i2c_write_word(cm36686, REG_PS_CONF1,
				0x0001);
		if (cm36686->pdata->cm36686_led_on)
			cm36686->pdata->cm36686_led_on(false);

		if (cm36686->cm36686_proxi_vddpower)
			cm36686->cm36686_proxi_vddpower(false);
	}
	mutex_unlock(&cm36686->power_lock);

	return sprintf(buf, "%u\n", ps_data);
}

static ssize_t proximity_thresh_high_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	pr_info("%s = %u,%u\n",__func__,
		ps_reg_init_setting[PS_THD_HIGH][CMD],ps_reg_init_setting[PS_THD_LOW][CMD]);
	return sprintf(buf, "%u,%u\n",
		ps_reg_init_setting[PS_THD_HIGH][CMD],ps_reg_init_setting[PS_THD_LOW][CMD]);
}

static ssize_t proximity_thresh_high_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	struct cm36686_data *cm36686 = dev_get_drvdata(dev);
	u16 thresh_value = ps_reg_init_setting[PS_THD_HIGH][CMD];
	int err;

	err = kstrtou16(buf, 10, &thresh_value);
	if (err < 0)
		pr_err("%s, kstrtoint failed.", __func__);

	if (thresh_value > 2) {
		ps_reg_init_setting[PS_THD_HIGH][CMD] = thresh_value;
		err = cm36686_i2c_write_word(cm36686, REG_PS_THD_HIGH,
			ps_reg_init_setting[PS_THD_HIGH][CMD]);
		if (err < 0)
			pr_err("%s: cm36686_ps_high_reg is failed. %d\n", __func__,
				err);
		pr_info("%s, new high threshold = 0x%x\n",
			__func__, thresh_value);
		msleep(150);
	} else
		pr_err("%s, wrong high threshold value(0x%x)!!\n",
			__func__, thresh_value);

	return size;
}

static ssize_t proximity_thresh_low_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	pr_info("%s = %u,%u\n",__func__,
		ps_reg_init_setting[PS_THD_HIGH][CMD],ps_reg_init_setting[PS_THD_LOW][CMD]);

	return sprintf(buf, "%u,%u\n",
		ps_reg_init_setting[PS_THD_HIGH][CMD],ps_reg_init_setting[PS_THD_LOW][CMD]);
}

static ssize_t proximity_thresh_low_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	struct cm36686_data *cm36686 = dev_get_drvdata(dev);
	u16 thresh_value = ps_reg_init_setting[PS_THD_LOW][CMD];
	int err;

	err = kstrtou16(buf, 10, &thresh_value);
	if (err < 0)
		pr_err("%s, kstrtoint failed.", __func__);

	if (thresh_value > 2) {
		ps_reg_init_setting[PS_THD_LOW][CMD] = thresh_value;
		err = cm36686_i2c_write_word(cm36686, REG_PS_THD_LOW,
			ps_reg_init_setting[PS_THD_LOW][CMD]);
		if (err < 0)
			pr_err("%s: cm36686_ps_low_reg is failed. %d\n", __func__,
				err);
		pr_info("%s, new low threshold = 0x%x\n",
			__func__, thresh_value);
		msleep(150);
	} else
		pr_err("%s, wrong low threshold value(0x%x)!!\n",
			__func__, thresh_value);

	return size;
}

#ifdef CM36686_CANCELATION
static DEVICE_ATTR(prox_cal, S_IRUGO | S_IWUSR | S_IWGRP,
	proximity_cancel_show, proximity_cancel_store);
#endif
static DEVICE_ATTR(prox_avg, S_IRUGO | S_IWUSR | S_IWGRP,
	proximity_avg_show, proximity_avg_store);
static DEVICE_ATTR(state, S_IRUGO,
	proximity_state_show, NULL);
static struct device_attribute attr_prox_raw = __ATTR(raw_data,
	S_IRUGO, proximity_state_show, NULL);
static DEVICE_ATTR(thresh_high, S_IRUGO | S_IWUSR | S_IWGRP,
	proximity_thresh_high_show, proximity_thresh_high_store);
static DEVICE_ATTR(thresh_low, S_IRUGO | S_IWUSR | S_IWGRP,
	proximity_thresh_low_show, proximity_thresh_low_store);


/* light sysfs */
static ssize_t light_lux_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct cm36686_data *cm36686 = dev_get_drvdata(dev);

	return sprintf(buf, "%u,%u\n", cm36686->als_data, cm36686->white_data);
}

static ssize_t light_data_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct cm36686_data *cm36686 = dev_get_drvdata(dev);
#ifdef cm36686_DEBUG
    pr_info("%s = %u,%u\n",__func__, cm36686->als_data, cm36686->white_data);
#endif
	return sprintf(buf, "%u,%u\n", cm36686->als_data, cm36686->white_data);
}

static DEVICE_ATTR(lux, S_IRUGO, light_lux_show, NULL);
static DEVICE_ATTR(raw_data, S_IRUGO, light_data_show, NULL);

/* sysfs for vendor & name */
static ssize_t cm36686_vendor_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%s\n", VENDOR);
}

static ssize_t cm36686_name_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%s\n", CHIP_ID);
}
static DEVICE_ATTR(vendor, S_IRUGO,
	cm36686_vendor_show, NULL);
static DEVICE_ATTR(name, S_IRUGO,
	cm36686_name_show, NULL);

/* interrupt happened due to transition/change of near/far proximity state */
irqreturn_t cm36686_irq_thread_fn(int irq, void *data)
{
	struct cm36686_data *cm36686 = data;
	u8 val = 1;
	u16 ps_data = 0;
#ifdef cm36686_DEBUG
	static int count;
	pr_info("%s\n", __func__);
#endif

	val = gpio_get_value(cm36686->pdata->irq);
	cm36686_i2c_read_word(cm36686, REG_PS_DATA, &ps_data);
#ifdef cm36686_DEBUG
	pr_info("%s: count = %d\n", __func__, count++);
#endif

	if (cm36686->power_state & PROXIMITY_ENABLED) {
		/* 0 is close, 1 is far */
		input_report_abs(cm36686->proximity_input_dev, ABS_DISTANCE, val);
		input_sync(cm36686->proximity_input_dev);
	}

	wake_lock_timeout(&cm36686->prx_wake_lock, 3 * HZ);

	pr_info("%s: val = %u, ps_data = %u (close:0, far:1)\n",
		__func__, val, ps_data);

	return IRQ_HANDLED;
}

static int cm36686_setup_reg(struct cm36686_data *cm36686)
{
	int err = 0, i = 0;
	u16 tmp = 0;

	/* ALS initialization */
	err = cm36686_i2c_write_word(cm36686,
			als_reg_setting[0][0],
			als_reg_setting[0][1]);
	if (err < 0) {
		pr_err("%s: cm36686_als_reg is failed. %d\n", __func__,
			err);
		return err;
	}
	/* PS initialization */

	if (cm36686->pdata != NULL) {
		ps_reg_init_setting[PS_THD_LOW][CMD] =
			cm36686->pdata->default_low_thd;
		ps_reg_init_setting[PS_THD_HIGH][CMD] =
			cm36686->pdata->default_hi_thd;
		pr_info("%s - THD_LOW = %u, THD_HIGH = %u\n", __func__,
			ps_reg_init_setting[PS_THD_LOW][CMD],
			ps_reg_init_setting[PS_THD_HIGH][CMD]);
	}
	for (i = 0; i < PS_REG_NUM; i++) {
		err = cm36686_i2c_write_word(cm36686,
			ps_reg_init_setting[i][REG_ADDR],
			ps_reg_init_setting[i][CMD]);
		if (err < 0) {
			pr_err("%s: cm36686_ps_reg is failed. %d\n", __func__,
				err);
			return err;
		}
	}

	/* printing the inital proximity value with no contact */
	msleep(50);
	mutex_lock(&cm36686->read_lock);
	err = cm36686_i2c_read_word(cm36686, REG_PS_DATA, &tmp);
	mutex_unlock(&cm36686->read_lock);
	if (err < 0) {
		pr_err("%s: read ps_data failed\n", __func__);
		err = -EIO;
	}
	pr_err("%s: initial proximity value = %d\n",
		__func__, tmp);

	/* turn off */
	cm36686_i2c_write_word(cm36686, REG_CS_CONF1, 0x0001);
	cm36686_i2c_write_word(cm36686, REG_PS_CONF1, 0x0001);
	cm36686_i2c_write_word(cm36686, REG_PS_CONF3, 0x0000);

	pr_info("%s is success.", __func__);
	return err;
}

static int cm36686_setup_irq(struct cm36686_data *cm36686)
{
	int rc = -EIO;
	struct cm36686_platform_data *pdata = cm36686->pdata;

	rc = gpio_request(pdata->irq, "gpio_proximity_out");
	if (rc < 0) {
		pr_err("%s: gpio %d request failed (%d)\n",
			__func__, pdata->irq, rc);
		return rc;
	}

	rc = gpio_direction_input(pdata->irq);
	if (rc < 0) {
		pr_err("%s: failed to set gpio %d as input (%d)\n",
			__func__, pdata->irq, rc);
		goto err_gpio_direction_input;
	}

	cm36686->irq = gpio_to_irq(pdata->irq);
	rc = request_threaded_irq(cm36686->irq, NULL,
				cm36686_irq_thread_fn,
				IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
				"proximity_int", cm36686);
	if (rc < 0) {
		pr_err("%s: request_irq(%d) failed for gpio %d (%d)\n",
			__func__, cm36686->irq, pdata->irq, rc);
		goto err_request_irq;
	}

	/* start with interrupts disabled */
	disable_irq(cm36686->irq);

	pr_err("%s, success\n", __func__);

	goto done;

err_request_irq:
err_gpio_direction_input:
	gpio_free(pdata->irq);
done:
	return rc;
}

/* This function is for light sensor.  It operates every a few seconds.
 * It asks for work to be done on a thread because i2c needs a thread
 * context (slow and blocking) and then reschedules the timer to run again.
 */
static enum hrtimer_restart cm36686_light_timer_func(struct hrtimer *timer)
{
	struct cm36686_data *cm36686
		= container_of(timer, struct cm36686_data, light_timer);
	queue_work(cm36686->light_wq, &cm36686->work_light);
	hrtimer_forward_now(&cm36686->light_timer, cm36686->light_poll_delay);
	return HRTIMER_RESTART;
}

static void cm36686_work_func_light(struct work_struct *work)
{
	struct cm36686_data *cm36686 = container_of(work, struct cm36686_data,
						work_light);
	mutex_lock(&cm36686->read_lock);
	cm36686_i2c_read_word(cm36686, REG_ALS_DATA, &cm36686->als_data);
	cm36686_i2c_read_word(cm36686, REG_WHITE_DATA, &cm36686->white_data);
	mutex_unlock(&cm36686->read_lock);

	input_report_rel(cm36686->light_input_dev, REL_DIAL, cm36686->als_data + 1);
	input_report_rel(cm36686->light_input_dev, REL_WHEEL, cm36686->white_data + 1);
	input_sync(cm36686->light_input_dev);

	if (cm36686->count_log_time >= LIGHT_LOG_TIME) {
		pr_info("%s, %u,%u\n", __func__,
			cm36686->als_data, cm36686->white_data);
		cm36686->count_log_time = 0;
	} else
		cm36686->count_log_time++;

#ifdef cm36686_DEBUG
	pr_info("%s, %u,%u\n", __func__,
		cm36686->als_data, cm36686->white_data);
#endif
}

static void proxsensor_get_avg_val(struct cm36686_data *cm36686)
{
	int min = 0, max = 0, avg = 0;
	int i;
	u16 ps_data = 0;

	for (i = 0; i < PROX_READ_NUM; i++) {
		msleep(40);
		cm36686_i2c_read_word(cm36686, REG_PS_DATA,
			&ps_data);
		avg += ps_data;

		if (!i)
			min = ps_data;
		else if (ps_data < min)
			min = ps_data;

		if (ps_data > max)
			max = ps_data;
	}
	avg /= PROX_READ_NUM;

	cm36686->avg[0] = min;
	cm36686->avg[1] = avg;
	cm36686->avg[2] = max;
}

static void cm36686_work_func_prox(struct work_struct *work)
{
	struct cm36686_data *cm36686 = container_of(work, struct cm36686_data,
						  work_prox);
	proxsensor_get_avg_val(cm36686);
}

static enum hrtimer_restart cm36686_prox_timer_func(struct hrtimer *timer)
{
	struct cm36686_data *cm36686
			= container_of(timer, struct cm36686_data, prox_timer);
	queue_work(cm36686->prox_wq, &cm36686->work_prox);
	hrtimer_forward_now(&cm36686->prox_timer, cm36686->prox_poll_delay);
	return HRTIMER_RESTART;
}

static int cm36686_i2c_probe(struct i2c_client *client,
				const struct i2c_device_id *id)
{
	int ret = -ENODEV;
	struct cm36686_data *cm36686 = NULL;

	pr_info("%s is called.\n", __func__);
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		pr_err("%s: i2c functionality check failed!\n", __func__);
		return ret;
	}

	cm36686 = kzalloc(sizeof(struct cm36686_data), GFP_KERNEL);
	if (!cm36686) {
		pr_err("%s: failed to alloc memory for cm36686 module data\n",
			__func__);
		return -ENOMEM;
	}

	cm36686->pdata = client->dev.platform_data;
	cm36686->i2c_client = client;
	i2c_set_clientdata(client, cm36686);
	mutex_init(&cm36686->power_lock);
	mutex_init(&cm36686->read_lock);

	if(cm36686->pdata->cm36686_light_power != NULL) {
		cm36686->cm36686_light_vddpower = cm36686->pdata->cm36686_light_power;
		if (cm36686->cm36686_light_vddpower)
			cm36686->cm36686_light_vddpower(true);
	}

	if(cm36686->pdata->cm36686_proxi_power != NULL) {
		cm36686->cm36686_proxi_vddpower = cm36686->pdata->cm36686_proxi_power;
		if (cm36686->cm36686_proxi_vddpower)
			cm36686->cm36686_proxi_vddpower(true);
	}

	/* wake lock init for proximity sensor */
	wake_lock_init(&cm36686->prx_wake_lock, WAKE_LOCK_SUSPEND,
			"prx_wake_lock");
	if (cm36686->pdata->cm36686_led_on) {
		cm36686->pdata->cm36686_led_on(true);
		msleep(20);
	}
	/* Check if the device is there or not. */
	ret = cm36686_i2c_write_word(cm36686, REG_CS_CONF1, 0x0001);
	if (ret < 0) {
		pr_err("%s: cm36686 is not connected.(%d)\n", __func__, ret);
		goto err_setup_reg;
	}

	/* setup initial registers */
	ret = cm36686_setup_reg(cm36686);
	if (ret < 0) {
		pr_err("%s: could not setup regs\n", __func__);
		goto err_setup_reg;
	}

	if (cm36686->pdata->cm36686_led_on)
		cm36686->pdata->cm36686_led_on(false);

	if (cm36686->cm36686_light_vddpower)
		cm36686->cm36686_light_vddpower(false);

	if (cm36686->cm36686_proxi_vddpower)
		cm36686->cm36686_proxi_vddpower(false);

	/* allocate proximity input_device */
	cm36686->proximity_input_dev = input_allocate_device();
	if (!cm36686->proximity_input_dev) {
		pr_err("%s: could not allocate proximity input device\n",
			__func__);
		goto err_input_allocate_device_proximity;
	}

	input_set_drvdata(cm36686->proximity_input_dev, cm36686);
	cm36686->proximity_input_dev->name = "proximity_sensor";
	input_set_capability(cm36686->proximity_input_dev, EV_ABS,
			ABS_DISTANCE);
	input_set_abs_params(cm36686->proximity_input_dev, ABS_DISTANCE, 0, 1,
			0, 0);

	ret = input_register_device(cm36686->proximity_input_dev);
	if (ret < 0) {
		input_free_device(cm36686->proximity_input_dev);
		pr_err("%s: could not register input device\n", __func__);
		goto err_input_register_device_proximity;
	}

	ret = sysfs_create_group(&cm36686->proximity_input_dev->dev.kobj,
				 &proximity_attribute_group);
	if (ret) {
		pr_err("%s: could not create sysfs group\n", __func__);
		goto err_sysfs_create_group_proximity;
	}
#if defined(CONFIG_SENSOR_USE_SYMLINK)
	ret =  sensors_initialize_symlink(cm36686->proximity_input_dev);
	if (ret < 0) {
		pr_err("%s - proximity_sensors_initialize_symlink error(%d).\n",
                        __func__, ret);
		goto err_setup_irq;
	}
#endif
	/* setup irq */
	ret = cm36686_setup_irq(cm36686);
	if (ret) {
		pr_err("%s: could not setup irq\n", __func__);
		goto err_setup_irq;
	}

	/* For factory test mode, we use timer to get average proximity data. */
	/* prox_timer settings. we poll for light values using a timer. */
	hrtimer_init(&cm36686->prox_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	cm36686->prox_poll_delay = ns_to_ktime(2000 * NSEC_PER_MSEC);/*2 sec*/
	cm36686->prox_timer.function = cm36686_prox_timer_func;

	/* the timer just fires off a work queue request.  we need a thread
	   to read the i2c (can be slow and blocking). */
	cm36686->prox_wq = create_singlethread_workqueue("cm36686_prox_wq");
	if (!cm36686->prox_wq) {
		ret = -ENOMEM;
		pr_err("%s: could not create prox workqueue\n", __func__);
		goto err_create_prox_workqueue;
	}
	/* this is the thread function we run on the work queue */
	INIT_WORK(&cm36686->work_prox, cm36686_work_func_prox);

	/* allocate lightsensor input_device */
	cm36686->light_input_dev = input_allocate_device();
	if (!cm36686->light_input_dev) {
		pr_err("%s: could not allocate light input device\n", __func__);
		goto err_input_allocate_device_light;
	}

	input_set_drvdata(cm36686->light_input_dev, cm36686);
	cm36686->light_input_dev->name = "light_sensor";
	input_set_capability(cm36686->light_input_dev, EV_REL, REL_MISC);
	input_set_capability(cm36686->light_input_dev, EV_REL, REL_DIAL);
	input_set_capability(cm36686->light_input_dev, EV_REL, REL_WHEEL);

	ret = input_register_device(cm36686->light_input_dev);
	if (ret < 0) {
		input_free_device(cm36686->light_input_dev);
		pr_err("%s: could not register input device\n", __func__);
		goto err_input_register_device_light;
	}

	ret = sysfs_create_group(&cm36686->light_input_dev->dev.kobj,
				 &light_attribute_group);
	if (ret) {
		pr_err("%s: could not create sysfs group\n", __func__);
		goto err_sysfs_create_group_light;
	}
#if defined(CONFIG_SENSOR_USE_SYMLINK)
	ret =  sensors_initialize_symlink(cm36686->light_input_dev);
	if (ret < 0) {
		pr_err("%s - light_sensors_initialize_symlink error(%d).\n",
                        __func__, ret);
		goto err_create_light_workqueue;
	}
#endif
	/* light_timer settings. we poll for light values using a timer. */
	hrtimer_init(&cm36686->light_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	cm36686->light_poll_delay = ns_to_ktime(200 * NSEC_PER_MSEC);
	cm36686->light_timer.function = cm36686_light_timer_func;

	/* the timer just fires off a work queue request.  we need a thread
	   to read the i2c (can be slow and blocking). */
	cm36686->light_wq = create_singlethread_workqueue("cm36686_light_wq");
	if (!cm36686->light_wq) {
		ret = -ENOMEM;
		pr_err("%s: could not create light workqueue\n", __func__);
		goto err_create_light_workqueue;
	}

	/* this is the thread function we run on the work queue */
	INIT_WORK(&cm36686->work_light, cm36686_work_func_light);

	/* set sysfs for proximity sensor */
	cm36686->proximity_dev = sensors_classdev_register("proximity_sensor");
	if (IS_ERR(cm36686->proximity_dev)) {
		pr_err("%s: could not create proximity_dev\n", __func__);
		goto err_proximity_device_create;
	}

	if (device_create_file(cm36686->proximity_dev, &dev_attr_state) < 0) {
		pr_err("%s: could not create device file(%s)!\n", __func__,
			dev_attr_state.attr.name);
		goto err_proximity_device_create_file1;
	}

	if (device_create_file(cm36686->proximity_dev, &attr_prox_raw) < 0) {
		pr_err("%s: could not create device file(%s)!\n", __func__,
			attr_prox_raw.attr.name);
		goto err_proximity_device_create_file2;
	}

#ifdef CM36686_CANCELATION
	if (device_create_file(cm36686->proximity_dev,
		&dev_attr_prox_cal) < 0) {
		pr_err("%s: could not create device file(%s)!\n", __func__,
			dev_attr_prox_cal.attr.name);
		goto err_proximity_device_create_file3;
	}
#endif
	if (device_create_file(cm36686->proximity_dev,
		&dev_attr_prox_avg) < 0) {
		pr_err("%s: could not create device file(%s)!\n", __func__,
			dev_attr_prox_avg.attr.name);
		goto err_proximity_device_create_file4;
	}

	if (device_create_file(cm36686->proximity_dev,
		&dev_attr_thresh_high) < 0) {
		pr_err("%s: could not create device file(%s)!\n", __func__,
			dev_attr_thresh_high.attr.name);
		goto err_proximity_device_create_file5;
	}

	if (device_create_file(cm36686->proximity_dev,
		&dev_attr_vendor) < 0) {
		pr_err("%s: could not create device file(%s)!\n", __func__,
			dev_attr_vendor.attr.name);
		goto err_proximity_device_create_file6;
	}

	if (device_create_file(cm36686->proximity_dev,
		&dev_attr_name) < 0) {
		pr_err("%s: could not create device file(%s)!\n", __func__,
			dev_attr_name.attr.name);
		goto err_proximity_device_create_file7;
	}

	if (device_create_file(cm36686->proximity_dev,
		&dev_attr_thresh_low) < 0) {
		pr_err("%s: could not create device file(%s)!\n", __func__,
			dev_attr_thresh_low.attr.name);
		goto err_proximity_device_create_file8;
	}

	dev_set_drvdata(cm36686->proximity_dev, cm36686);

	/* set sysfs for light sensor */
	cm36686->light_dev = sensors_classdev_register("light_sensor");
	if (IS_ERR(cm36686->light_dev)) {
		pr_err("%s: could not create light_dev\n", __func__);
		goto err_light_device_create;
	}

	if (device_create_file(cm36686->light_dev, &dev_attr_lux) < 0) {
		pr_err("%s: could not create device file(%s)!\n", __func__,
			dev_attr_lux.attr.name);
		goto err_light_device_create_file1;
	}

	if (device_create_file(cm36686->light_dev, &dev_attr_raw_data) < 0) {
		pr_err("%s: could not create device file(%s)!\n", __func__,
			dev_attr_raw_data.attr.name);
		goto err_light_device_create_file2;
	}

	if (device_create_file(cm36686->light_dev, &dev_attr_vendor) < 0) {
		pr_err("%s: could not create device file(%s)!\n", __func__,
			dev_attr_vendor.attr.name);
		goto err_light_device_create_file3;
	}

	if (device_create_file(cm36686->light_dev, &dev_attr_name) < 0) {
		pr_err("%s: could not create device file(%s)!\n", __func__,
			dev_attr_name.attr.name);
		goto err_light_device_create_file4;
	}

	dev_set_drvdata(cm36686->light_dev, cm36686);

	pr_info("%s is success.\n", __func__);
	goto done;

/* error, unwind it all */
err_light_device_create_file4:
	device_remove_file(cm36686->light_dev, &dev_attr_vendor);
err_light_device_create_file3:
	device_remove_file(cm36686->light_dev, &dev_attr_raw_data);
err_light_device_create_file2:
	device_remove_file(cm36686->light_dev, &dev_attr_lux);
err_light_device_create_file1:
	sensors_classdev_unregister(cm36686->light_dev);
err_light_device_create:
	device_remove_file(cm36686->proximity_dev, &dev_attr_thresh_low);
err_proximity_device_create_file8:
	device_remove_file(cm36686->proximity_dev, &dev_attr_name);
err_proximity_device_create_file7:
	device_remove_file(cm36686->proximity_dev, &dev_attr_vendor);
err_proximity_device_create_file6:
	device_remove_file(cm36686->proximity_dev, &dev_attr_thresh_high);
err_proximity_device_create_file5:
	device_remove_file(cm36686->proximity_dev, &dev_attr_prox_avg);
err_proximity_device_create_file4:
#ifdef CM36686_CANCELATION
	device_remove_file(cm36686->proximity_dev, &dev_attr_prox_cal);
err_proximity_device_create_file3:
#endif
	device_remove_file(cm36686->proximity_dev, &attr_prox_raw);
err_proximity_device_create_file2:
	device_remove_file(cm36686->proximity_dev, &dev_attr_state);
err_proximity_device_create_file1:
	sensors_classdev_unregister(cm36686->proximity_dev);
err_proximity_device_create:
	destroy_workqueue(cm36686->light_wq);
err_create_light_workqueue:
	sysfs_remove_group(&cm36686->light_input_dev->dev.kobj,
			   &light_attribute_group);
err_sysfs_create_group_light:
	input_unregister_device(cm36686->light_input_dev);
err_input_register_device_light:
err_input_allocate_device_light:
	destroy_workqueue(cm36686->prox_wq);
err_create_prox_workqueue:
	free_irq(cm36686->irq, cm36686);
	gpio_free(cm36686->pdata->irq);
err_setup_irq:
	sysfs_remove_group(&cm36686->proximity_input_dev->dev.kobj,
			   &proximity_attribute_group);
err_sysfs_create_group_proximity:
	input_unregister_device(cm36686->proximity_input_dev);
err_input_register_device_proximity:
err_input_allocate_device_proximity:
err_setup_reg:
	if (cm36686->pdata->cm36686_led_on)
		cm36686->pdata->cm36686_led_on(false);

	if (cm36686->cm36686_light_vddpower)
		cm36686->cm36686_light_vddpower(false);

	if (cm36686->cm36686_proxi_vddpower)
		cm36686->cm36686_proxi_vddpower(false);

	wake_lock_destroy(&cm36686->prx_wake_lock);
	mutex_destroy(&cm36686->read_lock);
	mutex_destroy(&cm36686->power_lock);
	kfree(cm36686);
done:
	return ret;
}

static int cm36686_i2c_remove(struct i2c_client *client)
{
	struct cm36686_data *cm36686 = i2c_get_clientdata(client);

	/* free irq */
	if (cm36686->power_state & PROXIMITY_ENABLED) {
		disable_irq_wake(cm36686->irq);
		disable_irq(cm36686->irq);
	}
	free_irq(cm36686->irq, cm36686);
	gpio_free(cm36686->pdata->irq);

	/* device off */
	if (cm36686->power_state & LIGHT_ENABLED)
		cm36686_light_disable(cm36686);
	if (cm36686->power_state & PROXIMITY_ENABLED) {
		cm36686_i2c_write_word(cm36686, REG_PS_CONF1,
					   0x0001);
		if (cm36686->pdata->cm36686_led_on)
			cm36686->pdata->cm36686_led_on(false);

		if (cm36686->cm36686_light_vddpower)
			cm36686->cm36686_light_vddpower(false);

		if (cm36686->cm36686_proxi_vddpower)
			cm36686->cm36686_proxi_vddpower(false);
	}

	/* destroy workqueue */
	destroy_workqueue(cm36686->light_wq);
	destroy_workqueue(cm36686->prox_wq);

	/* sysfs destroy */
	device_remove_file(cm36686->light_dev, &dev_attr_name);
	device_remove_file(cm36686->light_dev, &dev_attr_vendor);
	device_remove_file(cm36686->light_dev, &dev_attr_raw_data);
	device_remove_file(cm36686->light_dev, &dev_attr_lux);
	sensors_classdev_unregister(cm36686->light_dev);

	device_remove_file(cm36686->proximity_dev, &dev_attr_name);
	device_remove_file(cm36686->proximity_dev, &dev_attr_vendor);
	device_remove_file(cm36686->proximity_dev, &dev_attr_thresh_high);
	device_remove_file(cm36686->proximity_dev, &dev_attr_thresh_low);

	device_remove_file(cm36686->proximity_dev, &dev_attr_prox_avg);
#ifdef CM36686_CANCELATION
	device_remove_file(cm36686->proximity_dev, &dev_attr_prox_cal);
#endif
	device_remove_file(cm36686->proximity_dev, &attr_prox_raw);
	device_remove_file(cm36686->proximity_dev, &dev_attr_state);
	sensors_classdev_unregister(cm36686->proximity_dev);

	/* input device destroy */
	sysfs_remove_group(&cm36686->light_input_dev->dev.kobj,
				&light_attribute_group);
	input_unregister_device(cm36686->light_input_dev);
	sysfs_remove_group(&cm36686->proximity_input_dev->dev.kobj,
				&proximity_attribute_group);
	input_unregister_device(cm36686->proximity_input_dev);

	/* lock destroy */
	mutex_destroy(&cm36686->read_lock);
	mutex_destroy(&cm36686->power_lock);
	wake_lock_destroy(&cm36686->prx_wake_lock);

	kfree(cm36686);

	return 0;
}

static int cm36686_suspend(struct device *dev)
{
	/* We disable power only if proximity is disabled.  If proximity
	   is enabled, we leave power on because proximity is allowed
	   to wake up device.  We remove power without changing
	   cm36686->power_state because we use that state in resume.
	 */
	struct cm36686_data *cm36686 = dev_get_drvdata(dev);

	if (cm36686->power_state & LIGHT_ENABLED)
		cm36686_light_disable(cm36686);

	return 0;
}

static int cm36686_resume(struct device *dev)
{
	struct cm36686_data *cm36686 = dev_get_drvdata(dev);

	if (cm36686->power_state & LIGHT_ENABLED)
		cm36686_light_enable(cm36686);

	return 0;
}

static const struct i2c_device_id cm36686_device_id[] = {
	{"cm36686", 0},
	{}
};

MODULE_DEVICE_TABLE(i2c, cm36686_device_id);

static const struct dev_pm_ops cm36686_pm_ops = {
	.suspend = cm36686_suspend,
	.resume = cm36686_resume
};

static struct i2c_driver cm36686_i2c_driver = {
	.driver = {
		   .name = "cm36686",
		   .owner = THIS_MODULE,
		   .pm = &cm36686_pm_ops
	},
	.probe = cm36686_i2c_probe,
	.remove = cm36686_i2c_remove,
	.id_table = cm36686_device_id,
};

static int __init cm36686_init(void)
{
	return i2c_add_driver(&cm36686_i2c_driver);
}

static void __exit cm36686_exit(void)
{
	i2c_del_driver(&cm36686_i2c_driver);
}

module_init(cm36686_init);
module_exit(cm36686_exit);

MODULE_AUTHOR("Samsung Electronics");
MODULE_DESCRIPTION("RGB Sensor device driver for cm36686");
MODULE_LICENSE("GPL");
