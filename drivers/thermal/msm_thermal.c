/* Copyright (c) 2012-2015, The Linux Foundation. All rights reserved.
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

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/msm_tsens.h>
#include <linux/workqueue.h>
#include <linux/cpu.h>
#include <linux/reboot.h>
#include <linux/cpufreq.h>
#include <linux/msm_tsens.h>
#include <linux/msm_thermal.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <mach/cpufreq.h>

static DEFINE_MUTEX(emergency_shutdown_mutex);

/*
 * Throttling indicator:
 * 0 = not throttled
 * 1 = low
 * 2 = mid
 * 3 = max
 */
unsigned int thermal_throttled;
static unsigned int pre_throttled_max;

static struct msm_thermal_data msm_thermal_info;

static struct delayed_work check_temp_work;
static struct workqueue_struct *check_temp_workq;

static struct kobject *msm_thermal_kobject;

static int update_cpu_max_freq(struct cpufreq_policy *cpu_policy,
				int cpu, int max_freq)
{
	int ret = 0;

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
	unsigned long temp = 0;
	uint32_t max_freq = 0;
	bool update_policy = false, final_cpu = false;
	int cpu = 0, ret = 0;

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
		mutex_lock(&emergency_shutdown_mutex);

		pr_warn("Emergency Shutdown!\n");
		kernel_power_off();

		mutex_unlock(&emergency_shutdown_mutex);
	}

	/*
	 * for_each_possible_cpu() manages throttling for all CPUs,
	 * starting when the driver is initialized during boot.
	 */
	for_each_possible_cpu(cpu) {
		update_policy = false;
		cpu_policy = cpufreq_cpu_get(cpu);

		if (!cpu_policy) {
			pr_debug("NULL policy on cpu %d\n", cpu);
			continue;
		}

		if (!thermal_throttled && cpu == 0)
			pre_throttled_max = cpu_policy->max;

		if (cpu == CONFIG_NR_CPUS - 1)
			final_cpu = true;

		if (temp >= msm_thermal_info.allowed_low_high &&
				temp < msm_thermal_info.allowed_mid_high &&
				!thermal_throttled) {
			update_policy = true;
			max_freq = msm_thermal_info.allowed_low_freq;

			if (final_cpu)
				thermal_throttled = 1;
		} else if (temp < msm_thermal_info.allowed_low_low &&
						thermal_throttled > 0) {
			if (!pre_throttled_max)
				max_freq = CONFIG_MSM_CPU_FREQ_MAX;
			else
				max_freq = pre_throttled_max;

			update_policy = true;

			if (final_cpu)
				thermal_throttled = 0;
		} else if (temp >= msm_thermal_info.allowed_mid_high &&
				temp < msm_thermal_info.allowed_max_high &&
				thermal_throttled < 2) {
			update_policy = true;
			max_freq = msm_thermal_info.allowed_mid_freq;

			if (final_cpu)
				thermal_throttled = 2;
		} else if (temp < msm_thermal_info.allowed_mid_low &&
						thermal_throttled > 1) {
			max_freq = msm_thermal_info.allowed_low_freq;
			update_policy = true;

			if (final_cpu)
				thermal_throttled = 1;
		} else if (temp >= msm_thermal_info.allowed_max_high) {
			update_policy = true;
			max_freq = msm_thermal_info.allowed_max_freq;

			if (final_cpu)
				thermal_throttled = 3;
		} else if (temp < msm_thermal_info.allowed_max_low &&
						thermal_throttled > 2) {
			max_freq = msm_thermal_info.allowed_mid_freq;
			update_policy = true;

			if (final_cpu)
				thermal_throttled = 2;
		}

		if (update_policy)
			update_cpu_max_freq(cpu_policy, cpu, max_freq);

		cpufreq_cpu_put(cpu_policy);
	}

	/*
	 * Dynamic polling - depending on the throttling state the polling
	 * rate increases with rising temperature.
	 */
	switch (thermal_throttled) {
	case 0:
		queue_delayed_work(check_temp_workq, &check_temp_work,
			msecs_to_jiffies(msm_thermal_info.poll_ms));
		break;
	case 1:
		queue_delayed_work(check_temp_workq, &check_temp_work,
			msecs_to_jiffies(msm_thermal_info.low_poll_ms));
		break;
	case 2 ... 3:
		queue_delayed_work(check_temp_workq, &check_temp_work,
			msecs_to_jiffies(msm_thermal_info.mid_max_poll_ms));
		break;
	}

	return;
}

/******************************** SYSFS START ********************************/
#define show_one(file_name, object)				\
static ssize_t show_##file_name					\
(struct kobject *kobj, struct attribute *attr, char *buf)	\
{								\
	return sprintf(buf, "%u\n", msm_thermal_info.object);	\
}

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

static ssize_t store_shutdown_temp(struct kobject *a, struct attribute *b,
						const char *buf, size_t count)
{
	unsigned int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	msm_thermal_info.shutdown_temp = input;

	return count;
}

static ssize_t store_allowed_max_high(struct kobject *a, struct attribute *b,
						const char *buf, size_t count)
{
	unsigned int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	msm_thermal_info.allowed_max_high = input;

	return count;
}

static ssize_t store_allowed_max_low(struct kobject *a, struct attribute *b,
						const char *buf, size_t count)
{
	unsigned int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	msm_thermal_info.allowed_max_low = input;

	return count;
}

static ssize_t store_allowed_max_freq(struct kobject *a, struct attribute *b,
						const char *buf, size_t count)
{
	unsigned int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	msm_thermal_info.allowed_max_freq = input;

	return count;
}

static ssize_t store_allowed_mid_high(struct kobject *a, struct attribute *b,
						const char *buf, size_t count)
{
	unsigned int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	msm_thermal_info.allowed_mid_high = input;

	return count;
}

static ssize_t store_allowed_mid_low(struct kobject *a, struct attribute *b,
						const char *buf, size_t count)
{
	unsigned int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	msm_thermal_info.allowed_mid_low = input;

	return count;
}

static ssize_t store_allowed_mid_freq(struct kobject *a, struct attribute *b,
						const char *buf, size_t count)
{
	unsigned int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	msm_thermal_info.allowed_mid_freq = input;

	return count;
}

static ssize_t store_allowed_low_high(struct kobject *a, struct attribute *b,
						const char *buf, size_t count)
{
	unsigned int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	msm_thermal_info.allowed_low_high = input;

	return count;
}

static ssize_t store_allowed_low_low(struct kobject *a, struct attribute *b,
						const char *buf, size_t count)
{
	unsigned int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	msm_thermal_info.allowed_low_low = input;

	return count;
}

static ssize_t store_allowed_low_freq(struct kobject *a, struct attribute *b,
						const char *buf, size_t count)
{
	unsigned int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	msm_thermal_info.allowed_low_freq = input;

	return count;
}

static ssize_t store_poll_ms(struct kobject *a, struct attribute *b,
						const char *buf, size_t count)
{
	unsigned int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	msm_thermal_info.poll_ms = input;

	return count;
}

static ssize_t store_low_poll_ms(struct kobject *a, struct attribute *b,
						const char *buf, size_t count)
{
	unsigned int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	msm_thermal_info.low_poll_ms = input;

	return count;
}

static ssize_t store_mid_max_poll_ms(struct kobject *a, struct attribute *b,
						const char *buf, size_t count)
{
	unsigned int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	msm_thermal_info.mid_max_poll_ms = input;

	return count;
}

define_one_global_rw(shutdown_temp);
define_one_global_rw(allowed_max_high);
define_one_global_rw(allowed_max_low);
define_one_global_rw(allowed_max_freq);
define_one_global_rw(allowed_mid_high);
define_one_global_rw(allowed_mid_low);
define_one_global_rw(allowed_mid_freq);
define_one_global_rw(allowed_low_high);
define_one_global_rw(allowed_low_low);
define_one_global_rw(allowed_low_freq);
define_one_global_rw(poll_ms);
define_one_global_rw(low_poll_ms);
define_one_global_rw(mid_max_poll_ms);

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
	NULL
};


static struct attribute_group msm_thermal_attr_group = {
	.attrs = msm_thermal_attributes,
	.name = "conf",
};
/********************************* SYSFS END *********************************/

int __devinit msm_thermal_init(struct msm_thermal_data *pdata)
{
	int ret = 0, rc = 0;

	if (!pdata || pdata->sensor_id >= TSENS_MAX_SENSORS)
		return -EINVAL;

	memcpy(&msm_thermal_info, pdata, sizeof(struct msm_thermal_data));

	check_temp_workq = alloc_workqueue("msm_thermal",
						WQ_UNBOUND | WQ_MEM_RECLAIM, 1);
	if (!check_temp_workq) {
		pr_err("workqueue allocation failed!");
		return -ENOMEM;
	}

	INIT_DELAYED_WORK(&check_temp_work, check_temp);
	queue_delayed_work(check_temp_workq, &check_temp_work, 0);

	msm_thermal_kobject = kobject_create_and_add("msm_thermal",
								kernel_kobj);
	if (!msm_thermal_kobject) {
		pr_err("sysfs kobj creation failed!");
		return -ENOMEM;
	}

	rc = sysfs_create_group(msm_thermal_kobject, &msm_thermal_attr_group);
	if (rc) {
		pr_err("sysfs group creation failed!");
		return -ENOMEM;
	}

	return ret;
}

static int __devinit msm_thermal_dev_probe(struct platform_device *pdev)
{
	int ret = 0;
	char *key = NULL;
	struct device_node *node = pdev->dev.of_node;
	struct msm_thermal_data data;

	memset(&data, 0, sizeof(struct msm_thermal_data));

	key = "qcom,sensor-id";
	ret = of_property_read_u32(node, key, &data.sensor_id);
	if (ret)
		goto fail;
	if (data.sensor_id > TSENS_MAX_SENSORS) {
		data.sensor_id = 7;
		pr_warn("tsens sensor-id out of range, defaulting to %u\n",
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

fail:
	if (ret)
		pr_err("%s: Failed reading node=%s, key=%s\n",
					__func__, node->full_name, key);
	else
		ret = msm_thermal_init(&data);

	return ret;
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
