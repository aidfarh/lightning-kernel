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
 * 20.03.15: Initial driver release
 * 12.06.15: Complete re-write
 */

#define pr_fmt(fmt) "apq_hotplug: " fmt

#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/earlysuspend.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/printk.h>
#include <linux/workqueue.h>

/*
 * Remove #undef DEBUG to enable debugging of this driver.
 * Note: do not release debug builds, as this will flood the kernel log.
*/
#define DEBUG
#undef DEBUG

/* Worker stuff */
static struct workqueue_struct *apq_hotplug_wq;
static struct delayed_work offline_all_work;
static struct delayed_work online_all_work;

/* Variables */
static unsigned int boot_flag = 0;
static unsigned int suspend_delay = 100;
static unsigned int resume_delay = 10;

static inline void offline_all_fn(struct work_struct *work)
{
	unsigned int cpu;

	for_each_online_cpu(cpu) {
		if (cpu != 0) {
			cpu_down(cpu);
#ifdef DEBUG
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
#ifdef DEBUG
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
	if (boot_flag) {
		--boot_flag;
	}

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
	.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN,
	.suspend = apq_hotplug_early_suspend,
	.resume = apq_hotplug_late_resume,
};

static int __init apq_hotplug_init(void)
{
	apq_hotplug_wq = alloc_workqueue("apq_hotplug_wq",
					WQ_HIGHPRI | WQ_UNBOUND, 1);
	if (!apq_hotplug_wq) {
		pr_err("Failed to allocate apq_hotplug workqueue!\n");
		return -ENOMEM;
	}

	register_early_suspend(&apq_hotplug_early_suspend_handler);

	/*
	 * Increment boot_flag to allow skipping of clearing work on
	 * the first suspend call.
	 */
	++boot_flag;

	pr_info("initialized!\n");

#ifdef DEBUG
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

	unregister_early_suspend(&apq_hotplug_early_suspend_handler);
}

late_initcall(apq_hotplug_init);
module_exit(apq_hotplug_exit);
