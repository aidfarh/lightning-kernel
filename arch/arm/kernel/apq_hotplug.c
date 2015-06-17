/*
 * apq_hotplug - multicore hotplug driver
 *
 * Copyright (C) 2015 Tom G. <roboter972@gmail.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * Note: When compiling this driver, you need to add missing __cpuinit's
 * or remove all __cpuinit's from your kernel source.
 *
 * Major Changes:
 * Version 1.0 - 20.03.15: Initial driver release
 * Version 1.1 - 12.06.15: Complete re-write
 * Version 1.2 - 15.06.15: Added sysfs interface
 */

#define pr_fmt(fmt) "apq_hotplug: " fmt

#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/earlysuspend.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kobject.h>
#include <linux/stat.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

#define APQ_HOTPLUG_MAJOR_VERSION	1
#define APQ_HOTPLUG_MINOR_VERSION	2

/*
 * Note: do not release debug builds, as this will flood the kernel log and
 * cause additional overhead.
 */
#define DEBUG				0

/*
 * Boot flag allows direct initialisation of work on the first suspend
 * call.
 */
#define BOOT_FLAG			0

/*
 * SUSPEND_DELAY prevents that CPUs get immediately offlined after a suspend
 * call.
 */
#define SUSPEND_DELAY			(CONFIG_HZ * 2)

/*
 * RESUME_DELAY prevents that CPUs get immediately onlined after a resume
 * call.
 */
#define RESUME_DELAY			(CONFIG_HZ / 10)

static struct workqueue_struct *apq_hotplug_wq;
static struct delayed_work offline_all_work;
static struct delayed_work online_all_work;

static struct kobject *apq_hotplug_kobj;

static unsigned int boot_flag = BOOT_FLAG;
static unsigned int suspend_delay = SUSPEND_DELAY;
static unsigned int resume_delay = RESUME_DELAY;

static inline void offline_all_fn(struct work_struct *work)
{
	unsigned int cpu;

	for_each_online_cpu(cpu) {
		if (cpu != 0) {
			cpu_down(cpu);
#if DEBUG
			pr_info("CPU%u down.\n", cpu);
			pr_info("CPU(s) running: %u\n", num_online_cpus());
#endif
		}
	}
}

static inline void online_all_fn(struct work_struct *work)
{
	unsigned int cpu;

	for_each_cpu_not(cpu, cpu_online_mask) {
		if (cpu == 0)
			continue;
		cpu_up(cpu);
#if DEBUG
		pr_info("CPU%u up.\n", cpu);
		pr_info("CPU(s) running: %u\n", num_online_cpus());
#endif
	}
}

static void apq_hotplug_early_suspend(struct early_suspend *h)
{
	/*
	 * Init new work on the first suspend call,
	 * skip clearing workqueue as no work has been inited yet.
	 */
	if (!boot_flag) {
		cancel_delayed_work_sync(&online_all_work);
		flush_workqueue(apq_hotplug_wq);
	}

	INIT_DELAYED_WORK(&offline_all_work, offline_all_fn);

	/*
	 * Set the boot_flag to zero to allow the clearing of old work
	 * after the first suspend call.
	 */
	if (boot_flag)
		--boot_flag;

	queue_delayed_work(apq_hotplug_wq, &offline_all_work,
					msecs_to_jiffies(suspend_delay));
}

static void apq_hotplug_late_resume(struct early_suspend *h)
{
	/* Clear the workqueue and init new work */
	cancel_delayed_work_sync(&offline_all_work);
	flush_workqueue(apq_hotplug_wq);
	INIT_DELAYED_WORK(&online_all_work, online_all_fn);

	queue_delayed_work(apq_hotplug_wq, &online_all_work,
					msecs_to_jiffies(resume_delay));
}

static struct early_suspend __refdata apq_hotplug_early_suspend_handler = {
	.level = EARLY_SUSPEND_LEVEL_DISABLE_FB,
	.suspend = apq_hotplug_early_suspend,
	.resume = apq_hotplug_late_resume,
};

/******************************** SYSFS START ********************************/
static ssize_t apq_hotplug_version_show(struct kobject *kobj,
					struct kobj_attribute *attr,
					char *buf)
{
	return sprintf(buf, "%u.%u\n", APQ_HOTPLUG_MAJOR_VERSION,
					APQ_HOTPLUG_MINOR_VERSION);
}

static struct kobj_attribute apq_hotplug_version_attribute =
	__ATTR(apq_hotplug_version, S_IRUGO, apq_hotplug_version_show, NULL);

static struct attribute *apq_hotplug_attrs[] = {
	&apq_hotplug_version_attribute.attr,
	NULL,
};

static struct attribute_group apq_hotplug_attr_group = {
	.attrs = apq_hotplug_attrs,
};
/********************************* SYSFS END *********************************/

static int __init apq_hotplug_init(void)
{
	int rc;

	apq_hotplug_wq = alloc_workqueue("apq_hotplug_wq",
					WQ_HIGHPRI | WQ_UNBOUND, 1);
	if (!apq_hotplug_wq) {
		pr_err("Failed to allocate apq_hotplug workqueue!\n");
		return -ENOMEM;
	}

	apq_hotplug_kobj = kobject_create_and_add("apq_hotplug", kernel_kobj);
	if (!apq_hotplug_kobj) {
		pr_err("Failed to create apq_hotplug kobject!\n");
		return -ENOMEM;
	}

	rc = sysfs_create_group(apq_hotplug_kobj, &apq_hotplug_attr_group);
	if (rc) {
		pr_err("Failed to create apq_hotplug sysfs entry!\n");
		kobject_put(apq_hotplug_kobj);
	}

	register_early_suspend(&apq_hotplug_early_suspend_handler);

	/*
	 * Increment boot_flag to allow skipping of clearing work on
	 * the first suspend call.
	 */
	++boot_flag;

	pr_info("initialized!\n");

#if DEBUG
	pr_info("CPUs running: %u\n", num_online_cpus());
#endif

	return 0;
}

static void __exit apq_hotplug_exit(void)
{
	cancel_delayed_work_sync(&offline_all_work);
	cancel_delayed_work_sync(&online_all_work);
	flush_workqueue(apq_hotplug_wq);
	destroy_workqueue(apq_hotplug_wq);

	sysfs_remove_group(apq_hotplug_kobj, &apq_hotplug_attr_group);
	kobject_del(apq_hotplug_kobj);
	kobject_put(apq_hotplug_kobj);

	unregister_early_suspend(&apq_hotplug_early_suspend_handler);
}

late_initcall(apq_hotplug_init);
module_exit(apq_hotplug_exit);
