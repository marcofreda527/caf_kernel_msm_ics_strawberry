/* drivers/misc/lowmemorykiller.c
 *
 * The lowmemorykiller driver lets user-space specify a set of memory thresholds
 * where processes with a range of oom_adj values will get killed. Specify the
 * minimum oom_adj values in /sys/module/lowmemorykiller/parameters/adj and the
 * number of free pages in /sys/module/lowmemorykiller/parameters/minfree. Both
 * files take a comma separated list of numbers in ascending order.
 *
 * For example, write "0,8" to /sys/module/lowmemorykiller/parameters/adj and
 * "1024,4096" to /sys/module/lowmemorykiller/parameters/minfree to kill processes
 * with a oom_adj value of 8 or higher when the free memory drops below 4096 pages
 * and kill processes with a oom_adj value of 0 or higher when the free memory
 * drops below 1024 pages.
 *
 * The driver considers memory used for caches to be free, but if a large
 * percentage of the cached memory is locked this can be very inaccurate
 * and processes may not get killed until the normal oom killer is triggered.
 *
 * Copyright (C) 2007-2008 Google, Inc.
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
 */

#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/memory.h>
#include <linux/memory_hotplug.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/oom.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/sysfs.h>

static uint32_t lowmem_debug_level = 2;
static int lowmem_adj[6] = {
	0,
	1,
	6,
	12,
};
static int lowmem_adj_size = 4;
static size_t lowmem_minfree[6] = {
	3 * 512,	/* 6MB */
	2 * 1024,	/* 8MB */
	4 * 1024,	/* 16MB */
	16 * 1024,	/* 64MB */
};
static int lowmem_minfree_size = 4;

static size_t lowmem_minfree_notif_trigger;

static unsigned int offlining;
static struct task_struct *lowmem_deathpending;
static unsigned long lowmem_deathpending_timeout;
static struct kobject *lowmem_kobj;

#define lowmem_print(level, x...)			\
	do {						\
		if (lowmem_debug_level >= (level))	\
			printk(x);			\
	} while (0)

static int
task_notify_func(struct notifier_block *self, unsigned long val, void *data);

static struct notifier_block task_nb = {
	.notifier_call	= task_notify_func,
};

static int
task_notify_func(struct notifier_block *self, unsigned long val, void *data)
{
	struct task_struct *task = data;

	if (task == lowmem_deathpending)
		lowmem_deathpending = NULL;

	return NOTIFY_OK;
}

#ifdef CONFIG_MEMORY_HOTPLUG
static int lmk_hotplug_callback(struct notifier_block *self,
				unsigned long cmd, void *data)
{
	switch (cmd) {
	/* Don't care LMK cases */
	case MEM_ONLINE:
	case MEM_OFFLINE:
	case MEM_CANCEL_ONLINE:
	case MEM_CANCEL_OFFLINE:
	case MEM_GOING_ONLINE:
		offlining = 0;
		lowmem_print(4, "lmk in normal mode\n");
		break;
	/* LMK should account for movable zone */
	case MEM_GOING_OFFLINE:
		offlining = 1;
		lowmem_print(4, "lmk in hotplug mode\n");
		break;
	}
	return NOTIFY_DONE;
}
#endif



static void lowmem_notify_killzone_approach(void);

static inline void get_free_ram(int *other_free, int *other_file)
{
	struct zone *zone;
	*other_free = global_page_state(NR_FREE_PAGES);
	*other_file = global_page_state(NR_FILE_PAGES) -
						global_page_state(NR_SHMEM);

	if (offlining) {
		/* Discount all free space in the section being offlined */
		for_each_zone(zone) {
			 if (zone_idx(zone) == ZONE_MOVABLE) {
				*other_free -= zone_page_state(zone,
						NR_FREE_PAGES);
				lowmem_print(4, "lowmem_shrink discounted "
					"%lu pages in movable zone\n",
					zone_page_state(zone, NR_FREE_PAGES));
			}
		}
	}
}

static int lowmem_shrink(struct shrinker *s, struct shrink_control *sc)
{
	struct task_struct *p;
	struct task_struct *selected = NULL;
	int rem = 0;
	int tasksize;
	int i;
	int min_adj = OOM_ADJUST_MAX + 1;
	int selected_tasksize = 0;
	int selected_oom_adj;
	int array_size = ARRAY_SIZE(lowmem_adj);
	int other_free;
	int other_file;
	/*
	 * If we already have a death outstanding, then
	 * bail out right away; indicating to vmscan
	 * that we have nothing further to offer on
	 * this pass.
	 *
	 */
	if (lowmem_deathpending &&
	    time_before_eq(jiffies, lowmem_deathpending_timeout))
		return 0;

	get_free_ram(&other_free, &other_file);

	if (other_free < lowmem_minfree_notif_trigger &&
			other_file < lowmem_minfree_notif_trigger) {
		lowmem_notify_killzone_approach();
	}

	if (lowmem_adj_size < array_size)
		array_size = lowmem_adj_size;
	if (lowmem_minfree_size < array_size)
		array_size = lowmem_minfree_size;
	for (i = 0; i < array_size; i++) {
		if (other_free < lowmem_minfree[i] &&
		    other_file < lowmem_minfree[i]) {
			min_adj = lowmem_adj[i];
			break;
		}
	}
	if (sc->nr_to_scan > 0)
		lowmem_print(3, "lowmem_shrink %lu, %x, ofree %d %d, ma %d\n",
			     sc->nr_to_scan, sc->gfp_mask, other_free, other_file,
			     min_adj);
	rem = global_page_state(NR_ACTIVE_ANON) +
		global_page_state(NR_ACTIVE_FILE) +
		global_page_state(NR_INACTIVE_ANON) +
		global_page_state(NR_INACTIVE_FILE);
	if (sc->nr_to_scan <= 0 || min_adj == OOM_ADJUST_MAX + 1) {
		lowmem_print(5, "lowmem_shrink %lu, %x, return %d\n",
			     sc->nr_to_scan, sc->gfp_mask, rem);
		return rem;
	}
	selected_oom_adj = min_adj;

	read_lock(&tasklist_lock);
	for_each_process(p) {
		struct mm_struct *mm;
		struct signal_struct *sig;
		int oom_adj;

		task_lock(p);
		mm = p->mm;
		sig = p->signal;
		if (!mm || !sig) {
			task_unlock(p);
			continue;
		}
		oom_adj = sig->oom_adj;
		if (oom_adj < min_adj) {
			task_unlock(p);
			continue;
		}
		tasksize = get_mm_rss(mm);
		task_unlock(p);
		if (tasksize <= 0)
			continue;
		if (selected) {
			if (oom_adj < selected_oom_adj)
				continue;
			if (oom_adj == selected_oom_adj &&
			    tasksize <= selected_tasksize)
				continue;
		}
		selected = p;
		selected_tasksize = tasksize;
		selected_oom_adj = oom_adj;
		lowmem_print(2, "select %d (%s), adj %d, size %d, to kill\n",
			     p->pid, p->comm, oom_adj, tasksize);
	}
	if (selected) {
		lowmem_print(1, "send sigkill to %d (%s), adj %d, size %d\n",
			     selected->pid, selected->comm,
			     selected_oom_adj, selected_tasksize);
		lowmem_deathpending = selected;
		lowmem_deathpending_timeout = jiffies + HZ;
		force_sig(SIGKILL, selected);
		rem -= selected_tasksize;
	}
	lowmem_print(4, "lowmem_shrink %lu, %x, return %d\n",
		     sc->nr_to_scan, sc->gfp_mask, rem);
	read_unlock(&tasklist_lock);
	return rem;
}

static struct shrinker lowmem_shrinker = {
	.shrink = lowmem_shrink,
	.seeks = DEFAULT_SEEKS * 16
};

static void lowmem_notify_killzone_approach(void)
{
	lowmem_print(3, "notification trigger activated\n");
	sysfs_notify(lowmem_kobj, NULL, "notify_trigger_active");
}

static ssize_t lowmem_notify_trigger_active_show(struct kobject *k,
		struct kobj_attribute *attr, char *buf)
{
	int other_free, other_file;
	get_free_ram(&other_free, &other_file);
	if (other_free < lowmem_minfree_notif_trigger &&
			other_file < lowmem_minfree_notif_trigger)
		return snprintf(buf, 3, "1\n");
	else
		return snprintf(buf, 3, "0\n");
}

static struct kobj_attribute lowmem_notify_trigger_active_attr =
	__ATTR(notify_trigger_active, S_IRUGO,
			lowmem_notify_trigger_active_show, NULL);

static struct attribute *lowmem_default_attrs[] = {
	&lowmem_notify_trigger_active_attr.attr,
	NULL,
};

static ssize_t lowmem_show(struct kobject *k, struct attribute *attr, char *buf)
{
	struct kobj_attribute *kobj_attr;
	kobj_attr = container_of(attr, struct kobj_attribute, attr);
	return kobj_attr->show(k, kobj_attr, buf);
}

static const struct sysfs_ops lowmem_ops = {
	.show = lowmem_show,
};

static void lowmem_kobj_release(struct kobject *kobj)
{
	/* Nothing to be done here */
}

static struct kobj_type lowmem_kobj_type = {
	.release = lowmem_kobj_release,
	.sysfs_ops = &lowmem_ops,
	.default_attrs = lowmem_default_attrs,
};

static int __init lowmem_init(void)
{
	int rc;
	task_free_register(&task_nb);
	register_shrinker(&lowmem_shrinker);
#ifdef CONFIG_MEMORY_HOTPLUG
	hotplug_memory_notifier(lmk_hotplug_callback, 0);
#endif

	lowmem_kobj = kzalloc(sizeof(*lowmem_kobj), GFP_KERNEL);
	if (!lowmem_kobj) {
		rc = -ENOMEM;
		goto err;
	}

	rc = kobject_init_and_add(lowmem_kobj, &lowmem_kobj_type,
			mm_kobj, "lowmemkiller");
	if (rc)
		goto err_kobj;

	return 0;

err_kobj:
	kfree(lowmem_kobj);

err:
	unregister_shrinker(&lowmem_shrinker);
	task_free_unregister(&task_nb);

	return rc;
}

static void __exit lowmem_exit(void)
{
	kobject_put(lowmem_kobj);
	kfree(lowmem_kobj);
	unregister_shrinker(&lowmem_shrinker);
	task_free_unregister(&task_nb);
}

module_param_named(cost, lowmem_shrinker.seeks, int, S_IRUGO | S_IWUSR);
module_param_array_named(adj, lowmem_adj, int, &lowmem_adj_size,
			 S_IRUGO | S_IWUSR);
module_param_array_named(minfree, lowmem_minfree, uint, &lowmem_minfree_size,
			 S_IRUGO | S_IWUSR);
module_param_named(debug_level, lowmem_debug_level, uint, S_IRUGO | S_IWUSR);
module_param_named(notify_trigger, lowmem_minfree_notif_trigger, uint,
			 S_IRUGO | S_IWUSR);

module_init(lowmem_init);
module_exit(lowmem_exit);

MODULE_LICENSE("GPL");

