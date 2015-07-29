/* Copyright (c) 2010-2012, Code Aurora Forum. All rights reserved.
 * Copyright (c) 2015, Tom G. <roboter972@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/stat.h>
#include <linux/mutex.h>
#include <mach/socinfo.h>
#include <mach/scm.h>

#include "kgsl.h"
#include "kgsl_pwrscale.h"
#include "kgsl_device.h"

/*
 * Without locking i discovered that conservative switches frequencies
 * randomly at times, meaning that it up/downscales even if the load
 * does not reach/cross the corresponding threshold.
 */
static DEFINE_MUTEX(conservative_policy_mutex);

/*
 * KGSL policy scaling mode.
 * Energy saving locks the active pwrlevel to the highest present.
 * Performance locks the active pwrlevel to the lowest present.
 */ 
static unsigned char mode[] = { 
	'C',	/* Conservative */
	'E',	/* Energy saving */
	'P'	/* Performance */
};

static unsigned char scale_mode;

/*
 * Print conservative stats.
 * default (0): disabled, 1: enabled
 * Warning: Causes additional overhead and microlags on low polling intervals.
 */
static unsigned int g_show_stats;

/*
 * Polling interval in us.
 */
#define MIN_POLL_INTERVAL	10000
#define POLL_INTERVAL		100000
#define MAX_POLL_INTERVAL	1000000

static unsigned long g_polling_interval = POLL_INTERVAL;

/*
 * Total and busytime stats used to calculate the current GPU load.
 */
static unsigned long walltime_total;
static unsigned long busytime_total;

/*
 * Load thresholds.
 * Array position references to the number of the pwrlevel.
 */
static unsigned int up_thresholds[] = {
	110,	/* 400 MHz */
	98,	/* 320 MHz */
	90,	/* 200 MHz */
	75,	/* 128 MHz */
	100	/*  27 MHz */
};

static unsigned int down_thresholds[] = {
	60,	/* 400 MHz */
	45,	/* 320 MHz */
	45,	/* 200 MHz */
	0,	/* 128 MHz */
	0	/*  27 MHz */
};

static void conservative_wake(struct kgsl_device *device,
				struct kgsl_pwrscale *pwrscale)
{
	struct kgsl_power_stats stats;

	if (g_show_stats)
		pr_info("%s: GPU waking up\n", __func__);

	if (device->state != KGSL_STATE_NAP && scale_mode == mode[0]) {
		/* Reset the power stats counters. */
		device->ftbl->power_stats(device, &stats);
		walltime_total = 0;
		busytime_total = 0;
	}
}

static void conservative_idle(struct kgsl_device *device,
				struct kgsl_pwrscale *pwrscale)
{
	struct kgsl_pwrctrl *pwr = &device->pwrctrl;
	struct kgsl_power_stats stats;
	int val = 0;
	unsigned int loadpct;

	device->ftbl->power_stats(device, &stats);

	if (!stats.total_time ||
		scale_mode == mode[1] || scale_mode == mode[2])
		return;

	mutex_lock(&conservative_policy_mutex);

	walltime_total += (unsigned long) stats.total_time;
	busytime_total += (unsigned long) stats.busy_time;

	if (walltime_total > g_polling_interval) {
		if (g_show_stats)
			pr_info("%s: walltime_total = %lu, \
				busytime_total = %lu\n", __func__,
				walltime_total, busytime_total);

		loadpct = (100 * busytime_total) / walltime_total;

		if (g_show_stats)
			pr_info("%s: loadpct = %d\n", __func__, loadpct);

		walltime_total = busytime_total = 0;

		if (loadpct < down_thresholds[pwr->active_pwrlevel])
			val = 1;
		else if (loadpct > up_thresholds[pwr->active_pwrlevel])
			val = -1;

		if (g_show_stats)
			pr_info("%s: active_pwrlevel = %d, change = %d\n",
				__func__, pwr->active_pwrlevel, val);

		if (val)
			kgsl_pwrctrl_pwrlevel_change(device,
			pwr->active_pwrlevel + val);
	}

	mutex_unlock(&conservative_policy_mutex);
}

static void conservative_busy(struct kgsl_device *device,
				struct kgsl_pwrscale *pwrscale)
{
	device->on_time = ktime_to_us(ktime_get());
}

static void conservative_sleep(struct kgsl_device *device,
				struct kgsl_pwrscale *pwrscale)
{
	struct kgsl_pwrctrl *pwr = &device->pwrctrl;

	if (g_show_stats)
		pr_info("%s: GPU going to sleep\n", __func__);

	/* Bring GPU frequency all the way down on sleep */
	if (scale_mode != mode[2] && pwr->active_pwrlevel != pwr->min_pwrlevel)
		kgsl_pwrctrl_pwrlevel_change(device, pwr->min_pwrlevel);
}

static ssize_t conservative_stats_show(struct kgsl_device *device,
				struct kgsl_pwrscale *pwrscale,
				char *buf)
{
	int count;

	count = snprintf(buf, PAGE_SIZE, "%u\n", g_show_stats);

	return count;
}

static ssize_t conservative_stats_store(struct kgsl_device *device,
				struct kgsl_pwrscale *pwrscale,
				const char *buf, size_t count)
{
	unsigned int tmp;
	int err;

	err = kstrtoint(buf, 0, &tmp);
	if (err) {
		pr_err("%s: failed setting stats show!\n", __func__);
		return err;
	}

	g_show_stats = tmp ? 1 : 0;

	return count;
}

PWRSCALE_POLICY_ATTR(print_stats, S_IRUGO | S_IWUSR, conservative_stats_show,
						conservative_stats_store);

static ssize_t conservative_polling_interval_show(struct kgsl_device *device,
				struct kgsl_pwrscale *pwrscale,
				char *buf)
{
	int count;

	count = snprintf(buf, PAGE_SIZE, "%lu\n", g_polling_interval);

	return count;
}

static ssize_t conservative_polling_interval_store(struct kgsl_device *device,
				struct kgsl_pwrscale *pwrscale,
				const char *buf, size_t count)
{
	unsigned long tmp;
	int err;

	err = kstrtoul(buf, 0, &tmp);
	if (err) {
		pr_err("%s: failed setting new polling interval!\n", __func__);
		return err;
	}

	if (tmp < MIN_POLL_INTERVAL)
		tmp = MIN_POLL_INTERVAL;
	else if (tmp > MAX_POLL_INTERVAL)
		tmp = MAX_POLL_INTERVAL;

	g_polling_interval = tmp;

	return count;
}

PWRSCALE_POLICY_ATTR(polling_interval, S_IRUGO | S_IWUSR,
					conservative_polling_interval_show,
					conservative_polling_interval_store);

static ssize_t down_thresholds_show(struct kgsl_device *device,
				struct kgsl_pwrscale *pwrscale,
				char *buf)
{
	int count;

	count = snprintf(buf, PAGE_SIZE, "%u %u %u %u %u\n",
					down_thresholds[0],
					down_thresholds[1],
					down_thresholds[2],
					down_thresholds[3],
					down_thresholds[4]);

	return count;
}

static ssize_t down_thresholds_store(struct kgsl_device *device,
				struct kgsl_pwrscale *pwrscale,
				const char *buf, size_t count)
{
	int ret;

	ret = sscanf(buf, "%u %u %u %u %u",
					&down_thresholds[0],
					&down_thresholds[1],
					&down_thresholds[2],
					&down_thresholds[3],
					&down_thresholds[4]);

	if (ret < 1 || ret > 5)
		return -EINVAL;

	return count;
}

PWRSCALE_POLICY_ATTR(pwrlevel_down_thresholds, S_IRUGO | S_IWUSR,
							down_thresholds_show,
							down_thresholds_store);

static ssize_t up_thresholds_show(struct kgsl_device *device,
				struct kgsl_pwrscale *pwrscale,
				char *buf)
{
	int count;

	count = snprintf(buf, PAGE_SIZE, "%u %u %u %u %u\n",
					up_thresholds[0],
					up_thresholds[1],
					up_thresholds[2],
					up_thresholds[3],
					up_thresholds[4]);

	return count;
}

static ssize_t up_thresholds_store(struct kgsl_device *device,
				struct kgsl_pwrscale *pwrscale,
				const char *buf, size_t count)
{
	int ret;

	ret = sscanf(buf, "%u %u %u %u %u",
					&up_thresholds[0],
					&up_thresholds[1],
					&up_thresholds[2],
					&up_thresholds[3],
					&up_thresholds[4]);

	if (ret < 1 || ret > 5)
		return -EINVAL;

	return count;
}

PWRSCALE_POLICY_ATTR(pwrlevel_up_thresholds, S_IRUGO | S_IWUSR,
							up_thresholds_show,
							up_thresholds_store);

static ssize_t scale_mode_show(struct kgsl_device *device,
				struct kgsl_pwrscale *pwrscale,
				char *buf)
{
	int count;

	count = snprintf(buf, PAGE_SIZE, "%c\n", scale_mode);

	return count;
}

static ssize_t scale_mode_store(struct kgsl_device *device,
				struct kgsl_pwrscale *pwrscale,
				const char *buf, size_t size)
{
	struct kgsl_pwrctrl *pwr = &device->pwrctrl;

	if (sysfs_streq(buf, "C"))
		scale_mode = mode[0];
	else if (sysfs_streq(buf, "E")) {
		scale_mode = mode[1];
		kgsl_pwrctrl_pwrlevel_change(device, pwr->min_pwrlevel);
	} else if (sysfs_streq(buf, "P")) {
		scale_mode = mode[2];
		kgsl_pwrctrl_pwrlevel_change(device, pwr->max_pwrlevel);
	} else
		return -EINVAL;

	return strnlen(buf, PAGE_SIZE);
}

PWRSCALE_POLICY_ATTR(policy_scale_mode, S_IRUGO | S_IWUSR,
							scale_mode_show,
							scale_mode_store);

static struct attribute *conservative_attrs[] = {
	&policy_attr_print_stats.attr,
	&policy_attr_polling_interval.attr,
	&policy_attr_pwrlevel_down_thresholds.attr,
	&policy_attr_pwrlevel_up_thresholds.attr,
	&policy_attr_policy_scale_mode.attr,
	NULL,
};

static struct attribute_group conservative_attr_group = {
	.attrs = conservative_attrs,
	.name = "conservative",
};

static int conservative_init(struct kgsl_device *device,
				struct kgsl_pwrscale *pwrscale)
{
	kgsl_pwrscale_policy_add_files(device, pwrscale,
						&conservative_attr_group);

	scale_mode = mode[0];

	return 0;
}

static void conservative_close(struct kgsl_device *device,
				struct kgsl_pwrscale *pwrscale)
{
	kgsl_pwrscale_policy_remove_files(device, pwrscale,
						&conservative_attr_group);
}

struct kgsl_pwrscale_policy kgsl_pwrscale_policy_conservative = {
	.name = "conservative",
	.init = conservative_init,
	.busy = conservative_busy,
	.idle = conservative_idle,
	.sleep = conservative_sleep,
	.wake = conservative_wake,
	.close = conservative_close
};
EXPORT_SYMBOL(kgsl_pwrscale_policy_conservative);
