/*
 * Copyright (c) 2012, The Linux Foundation. All rights reserved.
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

#define pr_fmt(fmt) "msm_thermal: " fmt

#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/msm_thermal.h>
#include <linux/msm_tsens.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/workqueue.h>
#include <mach/cpufreq.h>

static DEFINE_MUTEX(shutdown_mutex);

enum throttling_status tstat;

static struct msm_thermal_data msm_thermal_info;
static struct delayed_work check_temp_work;
static struct workqueue_struct *check_temp_workq;
static struct kobject *msm_thermal_kobject;

static int update_cpu_max_freq(struct cpufreq_policy *cpu_policy,
				int cpu, int max_freq)
{
	int ret;

	if (!cpu_policy)
		return -EINVAL;

	cpufreq_verify_within_limits(cpu_policy, cpu_policy->min, max_freq);
	cpu_policy->user_policy.max = max_freq;

	ret = cpufreq_update_policy(cpu);

	return ret;
}

static void check_temp(struct work_struct *work)
{
	struct cpufreq_policy *cpu_policy = NULL;
	struct tsens_device tsens_dev;
	unsigned long temp;
	uint32_t max_freq = 0;
	unsigned int max_cpu_freq = 0;
	bool update_policy = false, final_cpu = false;
	int cpu, ret;

	tsens_dev.sensor_num = msm_thermal_info.sensor_id;

	ret = tsens_get_temp(&tsens_dev, &temp);
	if (ret) {
		pr_err("Failed to read TSENS sensor data\n");
		queue_delayed_work(check_temp_workq, &check_temp_work,
			msecs_to_jiffies(msm_thermal_info.mid_max_poll_ms));
		return;
	}

	/*
	 * If temp exceeds msm_thermal_info.shutdown_temp, force a system
	 * shutdown.
	 */
	if (temp >= msm_thermal_info.shutdown_temp) {
		mutex_lock(&shutdown_mutex);

		pr_warn("Emergency shutdown!\n");
		kernel_power_off();

		mutex_unlock(&shutdown_mutex);
	}

	for_each_possible_cpu(cpu) {
		update_policy = false;

		cpu_policy = cpufreq_cpu_get(cpu);
		if (cpu_policy == 0)
			continue;

		if (tstat == UNTHROTTLED && cpu == 0)
			max_cpu_freq = cpu_policy->max;

		if (cpu == CONFIG_NR_CPUS - 1)
			final_cpu = true;

		if (temp >= msm_thermal_info.allowed_low_high &&
			temp < msm_thermal_info.allowed_mid_high &&
			tstat == UNTHROTTLED) {
			max_freq = msm_thermal_info.allowed_low_freq;
			update_policy = true;

			if (final_cpu)
				tstat = PHASE1;
		} else if (temp < msm_thermal_info.allowed_low_low &&
			tstat > UNTHROTTLED) {
			if (max_cpu_freq == 0)
				max_freq = CONFIG_MSM_CPU_FREQ_MAX;
			else
				max_freq = max_cpu_freq;

			update_policy = true;

			if (final_cpu)
				tstat = UNTHROTTLED;
		} else if (temp >= msm_thermal_info.allowed_mid_high &&
			temp < msm_thermal_info.allowed_max_high &&
			tstat < PHASE2) {
			max_freq = msm_thermal_info.allowed_mid_freq;
			update_policy = true;

			if (final_cpu)
				tstat = PHASE2;
		} else if (temp < msm_thermal_info.allowed_mid_low &&
						tstat > PHASE1) {
			max_freq = msm_thermal_info.allowed_low_freq;
			update_policy = true;

			if (final_cpu)
				tstat = PHASE1;
		} else if (temp >= msm_thermal_info.allowed_max_high) {
			max_freq = msm_thermal_info.allowed_max_freq;
			update_policy = true;

			if (final_cpu)
				tstat = PHASE3;
		} else if (temp < msm_thermal_info.allowed_max_low &&
						tstat > PHASE2) {
			max_freq = msm_thermal_info.allowed_mid_freq;
			update_policy = true;

			if (final_cpu)
				tstat = PHASE2;
		}

		if (update_policy)
			update_cpu_max_freq(cpu_policy, cpu, max_freq);

		cpufreq_cpu_put(cpu_policy);
	}

	/*
	 * Dynamic polling - depending on the throttling state the polling
	 * rate increases with rising temperature.
	 */
	switch (tstat) {
	case UNTHROTTLED:
		queue_delayed_work(check_temp_workq, &check_temp_work,
			msecs_to_jiffies(msm_thermal_info.poll_ms));
		break;
	case PHASE1:
		queue_delayed_work(check_temp_workq, &check_temp_work,
			msecs_to_jiffies(msm_thermal_info.low_poll_ms));
		break;
	case PHASE2 ... PHASE3:
		queue_delayed_work(check_temp_workq, &check_temp_work,
			msecs_to_jiffies(msm_thermal_info.mid_max_poll_ms));
		break;
	}
}

/******************************** SYSFS START ********************************/
#define show_one(file_name, object)						\
static ssize_t show_##file_name							\
(struct kobject *kobj, struct attribute *attr, char *buf)			\
{										\
	return snprintf(buf, PAGE_SIZE, "%u\n", msm_thermal_info.object);	\
}

#define store_one(file_name, object)					  	\
static ssize_t store_##file_name						\
(struct kobject *kobj, struct attribute *attr, const char *buf, size_t count)	\
{										\
	unsigned int input;							\
	int ret;								\
										\
	ret = sscanf(buf, "%u", &input);					\
	if (ret != 1)								\
		return -EINVAL;							\
										\
	msm_thermal_info.object = input;					\
										\
	return count;								\
}

#define global_attr_rw(_name)							\
static struct global_attr _name =						\
__ATTR(_name, S_IRUGO | S_IWUSR, show_##_name, store_##_name)

show_one(shutdown_temp, shutdown_temp);
show_one(allowed_max_high, allowed_max_high);
show_one(allowed_max_low, allowed_max_low);
show_one(allowed_max_freq, allowed_max_freq);
show_one(allowed_mid_high, allowed_mid_high);
show_one(allowed_mid_low, allowed_mid_low);
show_one(allowed_mid_freq, allowed_mid_freq);
show_one(allowed_low_high, allowed_low_high);
show_one(allowed_low_low, allowed_low_low);
show_one(allowed_low_freq, allowed_low_freq);
show_one(poll_ms, poll_ms);
show_one(low_poll_ms, low_poll_ms);
show_one(mid_max_poll_ms, mid_max_poll_ms);

store_one(shutdown_temp, shutdown_temp);
store_one(allowed_max_high, allowed_max_high);
store_one(allowed_max_low, allowed_max_low);
store_one(allowed_max_freq, allowed_max_freq);
store_one(allowed_mid_high, allowed_mid_high);
store_one(allowed_mid_low, allowed_mid_low);
store_one(allowed_mid_freq, allowed_mid_freq);
store_one(allowed_low_high, allowed_low_high);
store_one(allowed_low_low, allowed_low_low);
store_one(allowed_low_freq, allowed_low_freq);
store_one(poll_ms, poll_ms);
store_one(low_poll_ms, low_poll_ms);
store_one(mid_max_poll_ms, mid_max_poll_ms);

global_attr_rw(shutdown_temp);
global_attr_rw(allowed_max_high);
global_attr_rw(allowed_max_low);
global_attr_rw(allowed_max_freq);
global_attr_rw(allowed_mid_high);
global_attr_rw(allowed_mid_low);
global_attr_rw(allowed_mid_freq);
global_attr_rw(allowed_low_high);
global_attr_rw(allowed_low_low);
global_attr_rw(allowed_low_freq);
global_attr_rw(poll_ms);
global_attr_rw(low_poll_ms);
global_attr_rw(mid_max_poll_ms);

static struct attribute *msm_thermal_attributes[] = {
	&shutdown_temp.attr,
	&allowed_max_high.attr,
	&allowed_max_low.attr,
	&allowed_max_freq.attr,
	&allowed_mid_high.attr,
	&allowed_mid_low.attr,
	&allowed_mid_freq.attr,
	&allowed_low_high.attr,
	&allowed_low_low.attr,
	&allowed_low_freq.attr,
	&poll_ms.attr,
	&low_poll_ms.attr,
	&mid_max_poll_ms.attr,
	NULL,
};


static struct attribute_group msm_thermal_attr_group = {
	.attrs = msm_thermal_attributes,
	.name = "conf",
};
/********************************* SYSFS END *********************************/

int __devinit msm_thermal_init(struct msm_thermal_data *pdata)
{
	int rc;

	if (!pdata || pdata->sensor_id >= TSENS_MAX_SENSORS)
		return -EINVAL;

	memcpy(&msm_thermal_info, pdata, sizeof(struct msm_thermal_data));

	check_temp_workq = alloc_workqueue("msm_thermal",
					WQ_UNBOUND | WQ_MEM_RECLAIM, 1);
	if (!check_temp_workq) {
		pr_err("Workqueue allocation failed!");
		return -ENOMEM;
	}

	INIT_DELAYED_WORK(&check_temp_work, check_temp);
	queue_delayed_work(check_temp_workq, &check_temp_work, 0);

	msm_thermal_kobject = kobject_create_and_add("msm_thermal",
							kernel_kobj);
	if (!msm_thermal_kobject) {
		pr_err("Sysfs kobj creation failed!");
		return -ENOMEM;
	}

	rc = sysfs_create_group(msm_thermal_kobject, &msm_thermal_attr_group);
	if (rc) {
		pr_err("Sysfs group creation failed!");
		kobject_put(msm_thermal_kobject);
		return rc;
	}

	return 0;
}

static int __devinit msm_thermal_dev_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct msm_thermal_data data;
	int ret;
	char *key = NULL;

	memset(&data, 0, sizeof(struct msm_thermal_data));

	key = "qcom,sensor-id";
	ret = of_property_read_u32(node, key, &data.sensor_id);
	if (ret)
		goto fail;
	if (data.sensor_id > TSENS_MAX_SENSORS) {
		data.sensor_id = 7;
		pr_warn("Tsens sensor-id out of range, defaulting to %u\n",
							data.sensor_id);
	}

	key = "qcom,poll-ms";
	ret = of_property_read_u32(node, key, &data.poll_ms);
	if (ret)
		goto fail;

	key = "qcom,low_poll-ms";
	ret = of_property_read_u32(node, key, &data.low_poll_ms);
	if (ret)
		goto fail;

	key = "qcom,mid_max_poll-ms";
	ret = of_property_read_u32(node, key, &data.mid_max_poll_ms);
	if (ret)
		goto fail;

	key = "qcom,shutdown_temp";
	ret = of_property_read_u32(node, key, &data.shutdown_temp);
	if (ret)
		goto fail;

	key = "qcom,allowed_max_high";
	ret = of_property_read_u32(node, key, &data.allowed_max_high);
	if (ret)
		goto fail;

	key = "qcom,allowed_max_low";
	ret = of_property_read_u32(node, key, &data.allowed_max_low);
	if (ret)
		goto fail;

	key = "qcom,allowed_max_freq";
	ret = of_property_read_u32(node, key, &data.allowed_max_freq);
	if (ret)
		goto fail;

	key = "qcom,allowed_mid_high";
	ret = of_property_read_u32(node, key, &data.allowed_mid_high);
	if (ret)
		goto fail;

	key = "qcom,allowed_mid_low";
	ret = of_property_read_u32(node, key, &data.allowed_mid_low);
	if (ret)
		goto fail;

	key = "qcom,allowed_mid_freq";
	ret = of_property_read_u32(node, key, &data.allowed_mid_freq);
	if (ret)
		goto fail;

	key = "qcom,allowed_low_high";
	ret = of_property_read_u32(node, key, &data.allowed_low_high);
	if (ret)
		goto fail;

	key = "qcom,allowed_low_low";
	ret = of_property_read_u32(node, key, &data.allowed_low_low);
	if (ret)
		goto fail;

	key = "qcom,allowed_low_freq";
	ret = of_property_read_u32(node, key, &data.allowed_low_freq);
	if (ret)
		goto fail;

	msm_thermal_init(&data);

	return 0;

fail:
	pr_err("%s: Failed reading node=%s, key=%s\n",
			__func__, node->full_name, key);
	return -EINVAL;
}

static struct of_device_id msm_thermal_match_table[] = {
	{.compatible = "qcom,msm-thermal"},
	{},
};

static struct platform_driver msm_thermal_device_driver = {
	.probe = msm_thermal_dev_probe,
	.driver = {
		.name = "msm-thermal",
		.owner = THIS_MODULE,
		.of_match_table = msm_thermal_match_table,
	},
};

int __init msm_thermal_device_init(void)
{
	return platform_driver_register(&msm_thermal_device_driver);
}

fs_initcall(msm_thermal_device_init);
