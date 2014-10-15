/*
 * Copyright (c) 2010-2011 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * EXYNOS - CPU frequency scaling support for EXYNOS series
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
*/

#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/regulator/consumer.h>
#include <linux/cpufreq.h>
#include <linux/suspend.h>
#include <linux/module.h>
#include <linux/reboot.h>
#include <linux/delay.h>
#include <linux/pm_qos.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>

#include <mach/cpufreq.h>
#include <mach/asv-exynos.h>
#include <mach/tmu.h>
#include <mach/sec_debug.h>

#include <plat/cpu.h>

#define VOLT_RANGE_STEP		25000

#ifdef CONFIG_SMP
struct lpj_info {
	unsigned long   ref;
	unsigned int    freq;
};

static struct lpj_info global_lpj_ref;
#endif

/* Use boot_freq when entering sleep mode */
static unsigned int freq_max;
static unsigned int freq_min;

static struct exynos_dvfs_info *exynos_info;

static struct regulator *arm_regulator;
static unsigned int volt_offset;

static struct cpufreq_freqs freqs;

DEFINE_MUTEX(cpufreq_lock);

static bool exynos_cpufreq_disable;
static bool exynos_cpufreq_init_done;

static struct pm_qos_constraints max_cpu_qos_const;

static struct pm_qos_request boot_min_cpu_qos;
static struct pm_qos_request boot_max_cpu_qos;
static struct pm_qos_request min_cpu_qos;
static struct pm_qos_request max_cpu_qos;
static struct pm_qos_request exynos_mif_qos;

static unsigned int get_limit_voltage(unsigned int voltage)
{
	BUG_ON(!voltage);
	return voltage + volt_offset;
}

static unsigned int get_freq_volt(unsigned int target_freq)
{
	struct cpufreq_frequency_table *table = exynos_info->freq_table;
	int index;
	int i;

	for (i = 0; (table[i].frequency != CPUFREQ_TABLE_END); i++) {
		unsigned int freq = table[i].frequency;
		if (freq == CPUFREQ_ENTRY_INVALID)
			continue;

		if (target_freq == freq) {
			index = i;
			break;
		}
	}

	if (table[i].frequency == CPUFREQ_TABLE_END)
		return -EINVAL;

	return exynos_info->volt_table[index];
}

static unsigned int get_boot_freq(void)
{
	if (exynos_info == NULL)
		return 0;

	return exynos_info->boot_freq;
}

static unsigned int get_boot_volt(void)
{
	unsigned int bootfreq = get_boot_freq();

	return get_freq_volt(bootfreq);
}

static int exynos_verify_speed(struct cpufreq_policy *policy)
{
	return cpufreq_frequency_table_verify(policy,
					      exynos_info->freq_table);
}

static unsigned int exynos_getspeed(unsigned int cpu)
{
	return clk_get_rate(exynos_info->cpu_clk) / 1000;
}

static unsigned int exynos_get_safe_armvolt(unsigned int old_index, unsigned int new_index)
{
	unsigned int safe_arm_volt = 0;
	struct cpufreq_frequency_table *freq_table = exynos_info->freq_table;
	unsigned int *volt_table = exynos_info->volt_table;

	/*
	 * ARM clock source will be changed APLL to MPLL temporary
	 * To support this level, need to control regulator for
	 * reguired voltage level
	 */

	if (exynos_info->need_apll_change != NULL) {
		if (exynos_info->need_apll_change(old_index, new_index) &&
			(freq_table[new_index].frequency < exynos_info->mpll_freq_khz) &&
			(freq_table[old_index].frequency < exynos_info->mpll_freq_khz)) {
				safe_arm_volt = volt_table[exynos_info->pll_safe_idx];
			}

	}

	return safe_arm_volt;
}

static int exynos_frequency_table_target(struct cpufreq_policy *policy,
		struct cpufreq_frequency_table *table,
		unsigned int target_freq,
		unsigned int relation,
		unsigned int *index)
{
	unsigned int i;

	if (!cpu_online(policy->cpu))
		return -EINVAL;

	for (i = 0; (table[i].frequency != CPUFREQ_TABLE_END); i++) {
		unsigned int freq = table[i].frequency;
		if (freq == CPUFREQ_ENTRY_INVALID)
			continue;

		if (target_freq == freq) {
			*index = i;
			break;
		}
	}

	if (table[i].frequency == CPUFREQ_TABLE_END)
		return -EINVAL;

	return 0;
}

static int exynos_cpufreq_scale(unsigned int target_freq,
				unsigned int curr_freq, struct cpufreq_policy *policy)
{
	unsigned int *volt_table = exynos_info->volt_table;
	struct cpufreq_frequency_table *freq_table = exynos_info->freq_table;
	unsigned int new_index, old_index;
	unsigned int arm_volt, safe_arm_volt = 0;
	int ret = 0;

	freqs.new = target_freq;

	if (exynos_frequency_table_target(policy, freq_table,
					   curr_freq, CPUFREQ_RELATION_H, &old_index)) {
		ret = -EINVAL;
		goto out;
	}

	if (exynos_frequency_table_target(policy, freq_table,
					   target_freq, CPUFREQ_RELATION_H, &new_index)) {
		ret = -EINVAL;
		goto out;
	}

	if (old_index == new_index)
		goto out;

	/*
	 * ARM clock source will be changed APLL to MPLL temporary
	 * To support this level, need to control regulator for
	 * required voltage level
	 */
	safe_arm_volt = exynos_get_safe_armvolt(old_index, new_index);
	if (safe_arm_volt)
		safe_arm_volt = get_limit_voltage(safe_arm_volt);

	arm_volt = get_limit_voltage(volt_table[new_index]);

#ifdef CONFIG_SEC_DEBUG_AUXILIARY_LOG
	sec_debug_aux_log(SEC_DEBUG_AUXLOG_CPU_BUS_CLOCK_CHANGE,
			"[ARM] volt=%u, safe=%u (Freq)", arm_volt, safe_arm_volt);
#endif

	/* Update policy current frequency */
	for_each_cpu(freqs.cpu, policy->cpus)
		cpufreq_notify_transition(&freqs, CPUFREQ_PRECHANGE);

	/* When the new frequency is higher than current frequency */
	if ((freqs.new > freqs.old) && !safe_arm_volt) {
		/* Firstly, voltage up to increase frequency */
		ret = regulator_set_voltage(arm_regulator, arm_volt, arm_volt + VOLT_RANGE_STEP);
		if (ret) {
			pr_err("%s: failed to set cpu voltage to %d\n",
				__func__, arm_volt);
			return ret;
		}

		if (exynos_info->abb_table)
			exynos_set_abb(ID_ARM, exynos_info->abb_table[new_index]);

		if (exynos_info->set_ema)
			exynos_info->set_ema(arm_volt);
	}

	if (safe_arm_volt) {
		ret = regulator_set_voltage(arm_regulator, safe_arm_volt,
				safe_arm_volt + VOLT_RANGE_STEP);
		if (ret) {
			pr_err("%s: failed to set cpu voltage to %d\n",
				__func__, safe_arm_volt);
			return ret;
		}

		if (exynos_info->abb_table)
			exynos_set_abb(ID_ARM, exynos_info->abb_table[exynos_info->pll_safe_idx]);

		if (exynos_info->set_ema)
			exynos_info->set_ema(safe_arm_volt);
	}

	if (old_index > new_index) {
		if (pm_qos_request_active(&exynos_mif_qos))
			pm_qos_update_request(&exynos_mif_qos, exynos_info->bus_table[new_index]);
	}

	exynos_info->set_freq(old_index, new_index);

	if (old_index < new_index) {
		if (pm_qos_request_active(&exynos_mif_qos))
			pm_qos_update_request(&exynos_mif_qos, exynos_info->bus_table[new_index]);
	}

#ifdef CONFIG_SMP
	if (!global_lpj_ref.freq) {
		global_lpj_ref.ref = loops_per_jiffy;
		global_lpj_ref.freq = freqs.old;
	}
	loops_per_jiffy = cpufreq_scale(global_lpj_ref.ref, global_lpj_ref.freq,
			freqs.new);
#endif

	for_each_cpu(freqs.cpu, policy->cpus)
		cpufreq_notify_transition(&freqs, CPUFREQ_POSTCHANGE);

	/* When the new frequency is lower than current frequency */
	if ((freqs.new < freqs.old) ||
	   ((freqs.new > freqs.old) && safe_arm_volt)) {
		/* down the voltage after frequency change */
		if (exynos_info->set_ema)
			exynos_info->set_ema(arm_volt);

		if (exynos_info->abb_table)
			exynos_set_abb(ID_ARM, exynos_info->abb_table[new_index]);

		ret = regulator_set_voltage(arm_regulator, arm_volt, arm_volt + VOLT_RANGE_STEP);
		if (ret)
			pr_err("%s: failed to set cpu voltage to %d\n",
				__func__, arm_volt);
	}

#ifdef CONFIG_SEC_DEBUG_AUXILIARY_LOG
	sec_debug_aux_log(SEC_DEBUG_AUXLOG_CPU_BUS_CLOCK_CHANGE,
			"[ARM] set volt=%u,", regulator_get_voltage(arm_regulator));
#endif
out:
	return ret;
}

unsigned int g_cpufreq;
/* Set clock frequency */
static int exynos_target(struct cpufreq_policy *policy,
			  unsigned int target_freq,
			  unsigned int relation)
{
	struct cpufreq_frequency_table *freq_table = exynos_info->freq_table;
	unsigned int index;
	int ret = 0;

	mutex_lock(&cpufreq_lock);

	if (exynos_cpufreq_disable)
		goto out;

	if (target_freq == 0)
		target_freq = policy->min;

	/* verify old frequency */
	BUG_ON(freqs.old != exynos_getspeed(0));

	target_freq = max((unsigned int)pm_qos_request(PM_QOS_CPU_FREQ_MIN), target_freq);
	target_freq = min((unsigned int)pm_qos_request(PM_QOS_CPU_FREQ_MAX), target_freq);

	if (cpufreq_frequency_table_target(policy, freq_table,
				   target_freq, relation, &index)) {
		ret = -EINVAL;
		goto out;
	}

	target_freq = freq_table[index].frequency;

	pr_debug("%s: new_freq[%d], index[%d]\n",
				__func__, target_freq, index);

	/* frequency and volt scaling */
	ret = exynos_cpufreq_scale(target_freq, freqs.old, policy);
	if (ret < 0)
		goto out;

	/* save current frequency */
	freqs.old = target_freq;

	g_cpufreq = target_freq;

out:
	mutex_unlock(&cpufreq_lock);

	return ret;
}

#ifdef CONFIG_PM
static int exynos_cpufreq_suspend(struct cpufreq_policy *policy)
{
	return 0;
}

static int exynos_cpufreq_resume(struct cpufreq_policy *policy)
{
	freqs.old = exynos_getspeed(0);
	return 0;
}
#endif

/**
 * exynos_cpufreq_pm_notifier - block CPUFREQ's activities in suspend-resume
 *			context
 * @notifier
 * @pm_event
 * @v
 *
 * While cpufreq_disable == true, target() ignores every frequency but
 * boot_freq. The boot_freq value is the initial frequency,
 * which is set by the bootloader. In order to eliminate possible
 * inconsistency in clock values, we save and restore frequencies during
 * suspend and resume and block CPUFREQ activities. Note that the standard
 * suspend/resume cannot be used as they are too deep (syscore_ops) for
 * regulator actions.
 */
static int exynos_cpufreq_pm_notifier(struct notifier_block *notifier,
				       unsigned long pm_event, void *v)
{
	struct cpufreq_frequency_table *freq_table = exynos_info->freq_table;
	unsigned int cur_freq, bootfreq, abb_freq;
	int volt, i;

	switch (pm_event) {
	case PM_SUSPEND_PREPARE:
		mutex_lock(&cpufreq_lock);
		exynos_cpufreq_disable = true;
		mutex_unlock(&cpufreq_lock);

		bootfreq = get_boot_freq();
		cur_freq = freqs.old;

		volt = max(get_boot_volt(), get_freq_volt(cur_freq));
		BUG_ON(volt <= 0);
		volt = get_limit_voltage(volt);

		if (regulator_set_voltage(arm_regulator, volt, volt + VOLT_RANGE_STEP))
			goto err;

		if (exynos_info->abb_table) {
			abb_freq = max(bootfreq, cur_freq);
			for (i = 0; (freq_table[i].frequency != CPUFREQ_TABLE_END); i++) {
				if (freq_table[i].frequency == CPUFREQ_ENTRY_INVALID)
					continue;
				if (freq_table[i].frequency == abb_freq) {
					exynos_set_abb(ID_ARM, exynos_info->abb_table[i]);
					break;
				}
			}
		}

		if (exynos_info->set_ema)
			exynos_info->set_ema(volt);

		pr_debug("PM_SUSPEND_PREPARE for CPUFREQ\n");
		break;
	case PM_POST_SUSPEND:
		pr_debug("PM_POST_SUSPEND for CPUFREQ\n");

		if (exynos_info->abb_table) {
			bootfreq = get_boot_freq();
			abb_freq = bootfreq;
			for (i = 0; (freq_table[i].frequency != CPUFREQ_TABLE_END); i++) {
				if (freq_table[i].frequency == CPUFREQ_ENTRY_INVALID)
					continue;
				if (freq_table[i].frequency == abb_freq) {
					exynos_set_abb(ID_ARM, exynos_info->abb_table[i]);
					break;
				}
			}
		}

		mutex_lock(&cpufreq_lock);
		exynos_cpufreq_disable = false;
		mutex_unlock(&cpufreq_lock);

		break;
	}

	return NOTIFY_OK;
err:
	pr_err("%s: failed to set voltage\n", __func__);

	return NOTIFY_BAD;
}

static struct notifier_block exynos_cpufreq_nb = {
	.notifier_call = exynos_cpufreq_pm_notifier,
};

#ifdef CONFIG_EXYNOS_THERMAL
static int exynos_cpufreq_tmu_notifier(struct notifier_block *notifier,
				       unsigned long event, void *v)
{
	int volt;
	int *on = v;

	if (event != TMU_COLD)
		return NOTIFY_OK;

	mutex_lock(&cpufreq_lock);
	if (*on) {
		if (volt_offset)
			goto out;
		else
			volt_offset = COLD_VOLT_OFFSET;

		volt = get_limit_voltage(regulator_get_voltage(arm_regulator));
		regulator_set_voltage(arm_regulator, volt, volt + VOLT_RANGE_STEP);
		if (exynos_info->set_ema)
			exynos_info->set_ema(volt);
	} else {
		if (!volt_offset)
			goto out;
		else
			volt_offset = 0;

		volt = get_limit_voltage(regulator_get_voltage(arm_regulator)
							- COLD_VOLT_OFFSET);

		if (volt < get_freq_volt(freqs.old))
			volt = get_freq_volt(freqs.old);

		if (exynos_info->set_ema)
			exynos_info->set_ema(volt);
		regulator_set_voltage(arm_regulator, volt, volt + VOLT_RANGE_STEP);
	}

out:
	mutex_unlock(&cpufreq_lock);

	return NOTIFY_OK;
}

static struct notifier_block exynos_tmu_nb = {
	.notifier_call = exynos_cpufreq_tmu_notifier,
};
#endif

static int exynos_cpufreq_cpu_init(struct cpufreq_policy *policy)
{
	policy->cur = policy->min = policy->max = exynos_getspeed(policy->cpu);

	cpufreq_frequency_table_get_attr(exynos_info->freq_table, policy->cpu);

	/* set the transition latency value */
	policy->cpuinfo.transition_latency = 100000;

	/*
	 * EXYNOS4 multi-core processors has 2 cores
	 * that the frequency cannot be set independently.
	 * Each cpu is bound to the same speed.
	 * So the affected cpu is all of the cpus.
	 */
	if (num_online_cpus() == 1) {
		cpumask_copy(policy->related_cpus, cpu_possible_mask);
		cpumask_copy(policy->cpus, cpu_online_mask);
	} else {
		cpumask_setall(policy->cpus);
	}

	return cpufreq_frequency_table_cpuinfo(policy, exynos_info->freq_table);
}

static struct cpufreq_driver exynos_driver = {
	.flags		= CPUFREQ_STICKY,
	.verify		= exynos_verify_speed,
	.target		= exynos_target,
	.get		= exynos_getspeed,
	.init		= exynos_cpufreq_cpu_init,
	.name		= "exynos_cpufreq",
#ifdef CONFIG_PM
	.suspend	= exynos_cpufreq_suspend,
	.resume		= exynos_cpufreq_resume,
#endif
};

/************************** sysfs interface ************************/

static ssize_t show_freq_table(struct kobject *kobj,
			     struct attribute *attr, char *buf)
{
	int i, count = 0;
	size_t tbl_sz = 0, pr_len;
	struct cpufreq_frequency_table *freq_table = exynos_info->freq_table;

	for (i = 0; freq_table[i].frequency != CPUFREQ_TABLE_END; i++)
		tbl_sz++;

	if (!tbl_sz)
		return -EINVAL;

	pr_len = (size_t)((PAGE_SIZE - 2) / tbl_sz);

	for (i = 0; freq_table[i].frequency != CPUFREQ_TABLE_END; i++) {
		if (freq_table[i].frequency != CPUFREQ_ENTRY_INVALID)
			count += snprintf(&buf[count], pr_len, "%d ",
					freq_table[i].frequency);
	}

	count += snprintf(&buf[count], 2, "\n");
	return count;
}

static ssize_t show_min_freq(struct kobject *kobj,
			     struct attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", (unsigned int)pm_qos_request(PM_QOS_CPU_FREQ_MIN));
}

static ssize_t show_max_freq(struct kobject *kobj,
			     struct attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", (unsigned int)pm_qos_request(PM_QOS_CPU_FREQ_MAX));
}

static ssize_t store_min_freq(struct kobject *kobj, struct attribute *attr,
			      const char *buf, size_t count)
{
	int input;

	if (!sscanf(buf, "%d", &input))
		return -EINVAL;

	if (input > 0)
		input = min(input, (int)freq_max);

	if (input <= (int)freq_min) {
		if (pm_qos_request_active(&min_cpu_qos))
			pm_qos_update_request(&min_cpu_qos, freq_min);
	} else {
		if (pm_qos_request_active(&min_cpu_qos))
			pm_qos_update_request(&min_cpu_qos, input);
		else
			pm_qos_add_request(&min_cpu_qos, PM_QOS_CPU_FREQ_MIN, input);
	}

	return count;
}

static ssize_t store_max_freq(struct kobject *kobj, struct attribute *attr,
			      const char *buf, size_t count)
{
	int input;

	if (!sscanf(buf, "%d", &input))
		return -EINVAL;

	if (input > 0)
		input = max(input, (int)freq_min);

	if (input >= (int)freq_max || input <= 0) {
		if (pm_qos_request_active(&max_cpu_qos))
			pm_qos_update_request(&max_cpu_qos, freq_max);
	} else {
		if (pm_qos_request_active(&max_cpu_qos))
			pm_qos_update_request(&max_cpu_qos, input);
		else
			pm_qos_add_request(&max_cpu_qos, PM_QOS_CPU_FREQ_MAX, input);
	}

	return count;
}

define_one_global_ro(freq_table);
define_one_global_rw(min_freq);
define_one_global_rw(max_freq);

static struct global_attr cpufreq_table =
		__ATTR(cpufreq_table, S_IRUGO, show_freq_table, NULL);
static struct global_attr cpufreq_min_limit =
		__ATTR(cpufreq_min_limit, S_IRUGO | S_IWUSR, show_min_freq, store_min_freq);
static struct global_attr cpufreq_max_limit =
		__ATTR(cpufreq_max_limit, S_IRUGO | S_IWUSR, show_max_freq, store_max_freq);

static struct attribute *cpufreq_attributes[] = {
	&freq_table.attr,
	&min_freq.attr,
	&max_freq.attr,
	NULL
};

static struct attribute_group cpufreq_attr_group = {
	.attrs = cpufreq_attributes,
	.name = "exynos-cpufreq",
};

/************************** sysfs end ************************/
static int exynos_cpufreq_reboot_notifier_call(struct notifier_block *this,
				   unsigned long code, void *_cmd)
{
	struct cpufreq_frequency_table *freq_table = exynos_info->freq_table;
	unsigned int cur_freq, bootfreq, abb_freq;
	int volt, i;

	mutex_lock(&cpufreq_lock);
	exynos_cpufreq_disable = true;
	mutex_unlock(&cpufreq_lock);

	bootfreq = get_boot_freq();
	cur_freq = freqs.old;

	volt = max(get_boot_volt(), get_freq_volt(cur_freq));
	volt = get_limit_voltage(volt);

	if (regulator_set_voltage(arm_regulator, volt, volt + VOLT_RANGE_STEP))
		goto err;

	if (exynos_info->abb_table) {
		abb_freq = max(bootfreq, cur_freq);
		for (i = 0; (freq_table[i].frequency != CPUFREQ_TABLE_END); i++) {
			if (freq_table[i].frequency == CPUFREQ_ENTRY_INVALID)
				continue;
			if (freq_table[i].frequency == abb_freq) {
				exynos_set_abb(ID_ARM, exynos_info->abb_table[i]);
				break;
			}
		}
	}

	if (exynos_info->set_ema)
		exynos_info->set_ema(volt);

	return NOTIFY_OK;
err:
	pr_err("%s: failed to set voltage\n", __func__);

	return NOTIFY_BAD;
}


static int exynos_min_qos_handler(struct notifier_block *b, unsigned long val, void *v)
{
	int ret;
	unsigned long freq;
	struct cpufreq_policy *policy;

	freq = exynos_getspeed(0);
	if (freq >= val)
		goto good;

	policy = cpufreq_cpu_get(0);

	if (!policy)
		goto bad;

	if (!policy->user_policy.governor) {
		cpufreq_cpu_put(policy);
		goto bad;
	}

#if defined(CONFIG_CPU_FREQ_GOV_USERSPACE) || defined(CONFIG_CPU_FREQ_GOV_PERFORMANCE)
	if ((strcmp(policy->governor->name, "userspace") == 0)
			|| strcmp(policy->governor->name, "performance") == 0) {
		cpufreq_cpu_put(policy);
		goto good;
	}
#endif

	ret = __cpufreq_driver_target(policy, val, CPUFREQ_RELATION_H);

	cpufreq_cpu_put(policy);

	if (ret < 0)
		goto bad;

good:
	return NOTIFY_OK;
bad:
	return NOTIFY_BAD;
}

static int exynos_max_qos_handler(struct notifier_block *b, unsigned long val, void *v)
{
	int ret;
	unsigned long freq;
	struct cpufreq_policy *policy;

	freq = exynos_getspeed(0);
	if (freq <= val)
		goto good;

	policy = cpufreq_cpu_get(0);

	if (!policy)
		goto bad;

	if (!policy->user_policy.governor) {
		cpufreq_cpu_put(policy);
		goto bad;
	}

#if defined(CONFIG_CPU_FREQ_GOV_USERSPACE) || defined(CONFIG_CPU_FREQ_GOV_PERFORMANCE)
	if ((strcmp(policy->governor->name, "userspace") == 0)
			|| strcmp(policy->governor->name, "performance") == 0) {
		cpufreq_cpu_put(policy);
		goto good;
	}
#endif

	ret = __cpufreq_driver_target(policy, val, CPUFREQ_RELATION_H);

	cpufreq_cpu_put(policy);

	if (ret < 0)
		goto bad;

good:
	return NOTIFY_OK;
bad:
	return NOTIFY_BAD;
}

static struct notifier_block exynos_min_qos_notifier = {
	.notifier_call = exynos_min_qos_handler,
};

static struct notifier_block exynos_max_qos_notifier = {
	.notifier_call = exynos_max_qos_handler,
};

static struct notifier_block exynos_cpufreq_reboot_notifier = {
	.notifier_call = exynos_cpufreq_reboot_notifier_call,
};

static int __init exynos_cpufreq_init(void)
{
	int ret = -EINVAL;

	exynos_info = kzalloc(sizeof(struct exynos_dvfs_info), GFP_KERNEL);
	if (!exynos_info)
		return -ENOMEM;

	if (soc_is_exynos3470()) {
		ret = exynos3470_cpufreq_init(exynos_info);
	} else if (soc_is_exynos4210()) {
		ret = exynos4210_cpufreq_init(exynos_info);
	} else if (soc_is_exynos4212() || soc_is_exynos4412()) {
		ret = exynos4x12_cpufreq_init(exynos_info);
	} else if (soc_is_exynos5250()) {
		ret = exynos5250_cpufreq_init(exynos_info);
	} else {
		pr_err("%s: CPU type not found\n", __func__);
		goto err_init_cpufreq;
	}

	if (ret)
		goto err_init_cpufreq;

	if (exynos_info->set_freq == NULL) {
		pr_err("%s: No set_freq function (ERR)\n", __func__);
		goto err_init_cpufreq;
	}

	arm_regulator = regulator_get(NULL, "vdd_arm");
	if (IS_ERR(arm_regulator)) {
		pr_err("%s: failed to get resource vdd_arm\n", __func__);
		goto err_vdd_arm;
	}

	freq_max = exynos_info->freq_table[exynos_info->max_support_idx].frequency;
	freq_min = exynos_info->freq_table[exynos_info->min_support_idx].frequency;

	exynos_info->boot_freq = exynos_getspeed(0);

	/* set initial old frequency */
	freqs.old = exynos_info->boot_freq;

	/* For low temperature compensation when boot time */
	volt_offset = COLD_VOLT_OFFSET;

	register_pm_notifier(&exynos_cpufreq_nb);
	register_reboot_notifier(&exynos_cpufreq_reboot_notifier);
#ifdef CONFIG_EXYNOS_THERMAL
	exynos_tmu_add_notifier(&exynos_tmu_nb);
#endif

	/* setup default qos constraints */
	max_cpu_qos_const.target_value = freq_max;
	max_cpu_qos_const.default_value = freq_max;
	pm_qos_update_constraints(PM_QOS_CPU_FREQ_MAX, &max_cpu_qos_const);

	pm_qos_add_notifier(PM_QOS_CPU_FREQ_MIN, &exynos_min_qos_notifier);
	pm_qos_add_notifier(PM_QOS_CPU_FREQ_MAX, &exynos_max_qos_notifier);

	/* blocking frequency scale before acquire boot lock */
	mutex_lock(&cpufreq_lock);
	exynos_cpufreq_disable = true;
	mutex_unlock(&cpufreq_lock);

	if (cpufreq_register_driver(&exynos_driver)) {
		pr_err("%s: failed to register cpufreq driver\n", __func__);
		goto err_cpufreq;
	}

	ret = sysfs_create_group(cpufreq_global_kobject, &cpufreq_attr_group);
	if (ret) {
		pr_err("%s: failed to create iks-cpufreq sysfs interface\n", __func__);
		goto err_cpufreq_attr;
	}

	ret = sysfs_create_file(cpufreq_global_kobject, &cpufreq_table.attr);
	if (ret) {
		pr_err("%s: failed to create cpufreq_table sysfs interface\n", __func__);
		goto err_cpufreq_table;
	}

	ret = sysfs_create_file(cpufreq_global_kobject, &cpufreq_min_limit.attr);
	if (ret) {
		pr_err("%s: failed to create cpufreq_min_limit sysfs interface\n", __func__);
		goto err_cpufreq_min_limit;
	}

	ret = sysfs_create_file(cpufreq_global_kobject, &cpufreq_max_limit.attr);
	if (ret) {
		pr_err("%s: failed to create cpufreq_max_limit sysfs interface\n", __func__);
		goto err_cpufreq_max_limit;
	}

#ifdef CONFIG_PM
	ret = sysfs_create_file(power_kobj, &cpufreq_max_limit.attr);
	if (ret) {
		pr_err("%s: failed to create cpufreq_max_limit sysfs interface\n", __func__);
		goto err_cpufreq_max_limit_power;
	}

	ret = sysfs_create_file(power_kobj, &cpufreq_min_limit.attr);
	if (ret) {
		pr_err("%s: failed to create cpufreq_min_limit sysfs interface\n", __func__);
		goto err_cpufreq_min_limit_power;
	}

	ret = sysfs_create_file(power_kobj, &cpufreq_table.attr);
	if (ret) {
		pr_err("%s: failed to create cpufreq_table sysfs interface\n", __func__);
		goto err_cpufreq_table_power;
	}
#endif

	pm_qos_add_request(&min_cpu_qos, PM_QOS_CPU_FREQ_MIN,
					PM_QOS_CPU_FREQ_MIN_DEFAULT_VALUE);
	pm_qos_add_request(&max_cpu_qos, PM_QOS_CPU_FREQ_MAX,
					max_cpu_qos_const.default_value);

	if (exynos_info->boot_cpu_min_qos) {
		pm_qos_add_request(&boot_min_cpu_qos, PM_QOS_CPU_FREQ_MIN,
					PM_QOS_CPU_FREQ_MIN_DEFAULT_VALUE);
		pm_qos_update_request_timeout(&boot_min_cpu_qos,
					exynos_info->boot_cpu_min_qos, 40000 * 1000);
	}

	if (exynos_info->boot_cpu_max_qos) {
		pm_qos_add_request(&boot_min_cpu_qos, PM_QOS_CPU_FREQ_MAX,
					max_cpu_qos_const.default_value);
		pm_qos_update_request_timeout(&boot_max_cpu_qos,
					exynos_info->boot_cpu_max_qos, 40000 * 1000);
	}

	if (exynos_info->bus_table)
		pm_qos_add_request(&exynos_mif_qos, PM_QOS_BUS_THROUGHPUT, 0);

	/* unblocking frequency scale */
	mutex_lock(&cpufreq_lock);
	exynos_cpufreq_disable = false;
	mutex_unlock(&cpufreq_lock);

	exynos_cpufreq_init_done = true;
#ifdef CONFIG_EXYNOS_DM_CPU_HOTPLUG
	dm_cpu_hotplug_init();
#endif

	return 0;

#ifdef CONFIG_PM
err_cpufreq_table_power:
	sysfs_remove_file(power_kobj, &cpufreq_min_limit.attr);
err_cpufreq_min_limit_power:
	sysfs_remove_file(power_kobj, &cpufreq_max_limit.attr);
err_cpufreq_max_limit_power:
	sysfs_remove_file(cpufreq_global_kobject, &cpufreq_max_limit.attr);
#endif
err_cpufreq_max_limit:
	sysfs_remove_file(cpufreq_global_kobject, &cpufreq_min_limit.attr);
err_cpufreq_min_limit:
	sysfs_remove_file(cpufreq_global_kobject, &cpufreq_table.attr);
err_cpufreq_table:
	sysfs_remove_group(cpufreq_global_kobject, &cpufreq_attr_group);
err_cpufreq_attr:
	cpufreq_unregister_driver(&exynos_driver);
err_cpufreq:
	pm_qos_remove_notifier(PM_QOS_CPU_FREQ_MIN, &exynos_min_qos_notifier);
	pm_qos_remove_notifier(PM_QOS_CPU_FREQ_MAX, &exynos_max_qos_notifier);
	unregister_reboot_notifier(&exynos_cpufreq_reboot_notifier);
	unregister_pm_notifier(&exynos_cpufreq_nb);
	regulator_put(arm_regulator);
err_vdd_arm:
err_init_cpufreq:
	kfree(exynos_info);
	pr_debug("%s: failed initialization\n", __func__);
	return -EINVAL;
}

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_INTERACTIVE
device_initcall(exynos_cpufreq_init);
#else
late_initcall(exynos_cpufreq_init);
#endif
