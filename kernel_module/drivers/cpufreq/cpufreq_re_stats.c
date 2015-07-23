/*
 *  drivers/cpufreq/cpufreq_re_stats.c
 *
 * Created by Liangzhen Lai @ 09/08/2014
 * Implemented based on existing cpufreq_stats.c 
 *
 */

#include <linux/cpu.h>
#include <linux/io.h>
#include <linux/cpufreq.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/cpuidle.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/time.h> 
#include <linux/timer.h>
#include <linux/delay.h>
#include <linux/export.h>

#include <asm/cputime.h>

#include "cpufreq_re_fit_data.h"

#define LOG_LENGTH 40
#define LOG_FREQ 10
//#define STATIC_POLICY
#define TARGET_FACTOR 50
#define DYN_FREQ 3
#define POLICY_ENABLE

struct cpufreq_re_log;
struct cpufreq_re_stat;
static spinlock_t cpufreq_re_stats_lock;
static int log_thread_init(unsigned int cpu);
static int log_thread_exit(unsigned int cpu);
static int thread_log_fn(void* cpu);
static int cpufreq_re_report_FIT(unsigned int cpu);
static unsigned int mem_addr;
static char log_name[32];
extern int wkup_m3_ping_delay(int iteration);
static int log_state;
static int trace_state;

struct cpufreq_re_log {
	unsigned int cpu;
	struct task_struct *data_logging_thread;
	u64 *pow_log_data;
	u64 *core_fit_data;
	u64 *mem_fit_data;
	u64 *freq_data;
	unsigned int cur_index;
	u64 last_pow;
	u64 last_core_fit;
	u64 last_mem_fit;
};

struct cpufreq_re_stats {
	unsigned int cpu;
	unsigned long long last_time;			// in usec
	unsigned int max_state;			
	unsigned int cpuidle_state_num;		// state count for cpuidle
	unsigned int state_num;			// state count for cpufreq
	unsigned int last_index;
	unsigned int *freq_table;
	unsigned long long *last_idle_state_usage;
	unsigned long long *last_idle_state_time;	// last idle state usage time (us)
	unsigned int location_factor;
	unsigned int cur_core_fit;
	unsigned int cur_mem_fit;
	unsigned int cpuidle_c1_fit;
	unsigned int cpuidle_c2_fit;
	unsigned int cpuidle_mem_ret_fit;
	unsigned int cur_core_pow;
	unsigned int cur_mem_pow;
	unsigned int cur_mem_pow_l2_ret;
	unsigned int cpuidle_c1_pow;
	unsigned int cpuidle_mem_ret_pow;
	unsigned int core_fit_target;
	unsigned int mem_fit_target;
	u64 core_pow_acc;
	u64 mem_pow_acc;
	u64 core_fit_acc;
	u64 mem_fit_acc;
	u64 cycle_max_core_fit;
	u64 cycle_max_mem_fit;
#ifndef STATIC_POLICY
	unsigned long long budget_stop_time;		// in usec
	u64 budget_target_core_fit_acc;
	u64 budget_target_mem_fit_acc;
#endif
};

static DEFINE_PER_CPU(struct cpufreq_re_log *, cpufreq_re_log_table);
static DEFINE_PER_CPU(struct cpufreq_re_stats *, cpufreq_re_stats_table);

/*
 * Making fit_data struct per cpu base for heterogeneous compatibility
 */
static DEFINE_PER_CPU(struct cpufreq_re_fit_data *, cpufreq_re_fit_data_table);

struct cpufreq_re_stats_attribute {
	struct attribute attr;
	ssize_t(*show) (struct cpufreq_re_stats *, char *);
};

/*
 * This function updates all cur_fit with cpufreq level at last_index
 * It should be called within cpufreq_re_stats_lock
 */

static int update_cur_fit(unsigned int cpu) {
	struct cpufreq_re_stats *stat;
	struct cpuidle_device *dev;
	struct cpufreq_re_fit_data *fit_data;
	int index;
	
	stat = per_cpu(cpufreq_re_stats_table, cpu);
	dev = per_cpu(cpuidle_devices, cpu);
	fit_data = per_cpu(cpufreq_re_fit_data_table, cpu);
	
	if (!stat||!dev||!fit_data) {
		return -ENOMEM;
	}

	switch (stat->freq_table[stat->last_index]) {
		case 1000*1000 :
			index = 0;
			break;
		case 800*1000 :
			index = 1;
                        break;
		case 720*1000 :
			index = 2;
                        break;
		case 600*1000 :
			index = 3;
                        break;
		case 300*1000 :
			index = 4;
                        break;
		default :
			printk("cpufreq_re_stats: unknown frequency %d\n", 
				stat->freq_table[stat->last_index]);
			return -EINVAL;
	}
	stat->cur_core_fit = fit_data->core_fit[index]
	                        * stat->location_factor / 100;
	stat->cur_mem_fit = (fit_data->L1_mem_fit[index] + fit_data->L2_mem_fit)
	                        * stat->location_factor / 100;
	stat->cpuidle_c1_fit = fit_data->core_fit_c1[index]
	                        * stat->location_factor / 100;
	stat->cpuidle_c2_fit = fit_data->core_fit_c2
	                        * stat->location_factor / 100;
	stat->cpuidle_mem_ret_fit = (fit_data->L1_mem_fit_ret[index] + fit_data->L2_mem_fit_ret)
	                        * stat->location_factor / 100;
	stat->cur_core_pow = fit_data->core_pow[index];
	stat->cur_mem_pow = fit_data->L1_mem_pow[index] + fit_data->L2_mem_pow;
	stat->cur_mem_pow_l2_ret = fit_data->L1_mem_pow[index] + fit_data->L2_mem_fit_ret;
	stat->cpuidle_c1_pow = fit_data->core_pow_c1[index];
	stat->cpuidle_mem_ret_pow = fit_data->L1_mem_pow_ret[index]
	                        + fit_data->L2_mem_pow_ret;
	return 0;
}

/* 
 * This function updates all related data in struct cpufreq_re_stats
 * using all cur_###_fit. It should be called before cur_fit update.
 */ 
static int cpufreq_re_stats_update(unsigned int cpu)
{
	struct cpufreq_re_stats *stat;
        struct cpuidle_device *dev;
	unsigned int cur_time;
	int idle_time_diff[3], time_diff;

	stat = per_cpu(cpufreq_re_stats_table, cpu);
	dev = per_cpu(cpuidle_devices, cpu);
	cur_time = jiffies_to_usecs(get_jiffies_64());
	spin_lock(&cpufreq_re_stats_lock);

	if (!dev || !stat) {
		printk("cpufreq_re_stats_update: error retrieving stat or dev\n");
		return -1;
	}
	
	// do necessary update here
	time_diff = cur_time - stat->last_time;
        idle_time_diff[1] = dev->states_usage[1].time
                         - stat->last_idle_state_time[1];
        idle_time_diff[2] = dev->states_usage[2].time
                         - stat->last_idle_state_time[2];
        idle_time_diff[3] = dev->states_usage[3].time
                         - stat->last_idle_state_time[3];
	idle_time_diff[0] = time_diff - idle_time_diff[1]
			- idle_time_diff[2] - idle_time_diff[3];
	if (idle_time_diff[0] < 0) {
		idle_time_diff[0] = 0;
	}
#ifdef STATIC_POLICY
	if (stat->cur_core_fit > stat->cycle_max_core_fit) {
		stat->cycle_max_core_fit = stat->cur_core_fit;
	}
	if (stat->cur_mem_fit > stat->cycle_max_mem_fit) {
		stat->cycle_max_mem_fit = stat->cur_mem_fit;
	}
	if (idle_time_diff[1] != 0) {
		if (stat->cpuidle_c1_fit > stat->cycle_max_core_fit)
			stat->cycle_max_core_fit = stat->cpuidle_c1_fit; 
	}
	if (idle_time_diff[2] != 0) {
		if (stat->cpuidle_c2_fit > stat->cycle_max_core_fit)
			stat->cycle_max_core_fit = stat->cpuidle_c2_fit;
	}
	if (idle_time_diff[3] != 0) {
		if (stat->cpuidle_c2_fit > stat->cycle_max_core_fit)
                        stat->cycle_max_core_fit = stat->cpuidle_c2_fit;
		if (stat->cpuidle_mem_ret_fit > stat->cycle_max_core_fit)
			stat->cycle_max_core_fit = stat->cpuidle_mem_ret_fit;
	}
#endif

	stat->core_fit_acc += idle_time_diff[0] * stat->cur_core_fit
			+ idle_time_diff[1] * stat->cpuidle_c1_fit
			+ idle_time_diff[2] * stat->cpuidle_c2_fit
			+ idle_time_diff[3] * stat->cpuidle_c2_fit;
	stat->mem_fit_acc += idle_time_diff[0] * stat->cur_mem_fit
			+ idle_time_diff[1] * stat->cur_mem_fit
			+ idle_time_diff[2] * stat->cur_mem_fit
			+ idle_time_diff[3] * stat->cpuidle_mem_ret_fit;

	stat->core_pow_acc += idle_time_diff[0] * stat->cur_core_pow
			+ idle_time_diff[1] * stat->cpuidle_c1_pow;
	stat->mem_pow_acc += idle_time_diff[0] * stat->cur_mem_pow
			+ idle_time_diff[1] * stat->cur_mem_pow
			+ idle_time_diff[2] * stat->cur_mem_pow_l2_ret
			+ idle_time_diff[3] * stat->cpuidle_mem_ret_pow;

	// C0 has no effect
	stat->last_idle_state_time[0] = dev->states_usage[0].time;

	// CORE FIT: cpuidle_c1_fit
	// MEM FIT: cur_mem_fit
	// CORE POW: cpuidle_c1_pow
	// MEM POW: cur_mem_pow
	stat->last_idle_state_time[1] = dev->states_usage[1].time;

	// CORE FIT: 0
	// MEM FIT: cur_mem_fit
	// CORE POW: 0
	// MEM POW: cur_mem_pow
	stat->last_idle_state_time[2] = dev->states_usage[2].time;

        // CORE FIT: 0
        // MEM FIT: cpuidle_mem_ret_fit
        // CORE POW: 0
        // MEM POW: cpuidle_mem_ret_pow
	stat->last_idle_state_time[3] = dev->states_usage[3].time;

	stat->last_time = cur_time;
	spin_unlock(&cpufreq_re_stats_lock);
	return 0;
}

static ssize_t show_location_factor(struct cpufreq_policy *policy, char *buf)
{
        struct cpufreq_re_stats *stat = per_cpu(cpufreq_re_stats_table, policy->cpu);
        if (!stat)
                return 0;
        return sprintf(buf, "%d\n", stat->location_factor);
}

static ssize_t store_location_factor(struct cpufreq_policy *policy,
                                        const char *buf, size_t count)
{
	unsigned int ret;
	unsigned int new_location_factor;
        struct cpufreq_re_stats *stat = per_cpu(cpufreq_re_stats_table, policy->cpu);
        if (!stat)
                return 0;
        ret = sscanf(buf, "%u", &new_location_factor);
	cpufreq_re_stats_update(stat->cpu);
	stat->location_factor = new_location_factor;
	spin_lock(&cpufreq_re_stats_lock);
	update_cur_fit(stat->cpu);
	cpufreq_re_report_FIT(stat->cpu);
        spin_unlock(&cpufreq_re_stats_lock);
	return count;
}

static ssize_t show_cur_core_fit(struct cpufreq_policy *policy, char *buf)
{
        struct cpufreq_re_stats *stat = per_cpu(cpufreq_re_stats_table, policy->cpu);
        if (!stat)
                return 0;
        return sprintf(buf, "%d\n", stat->cur_core_fit);
}

static ssize_t show_cur_mem_fit(struct cpufreq_policy *policy, char *buf)
{
        struct cpufreq_re_stats *stat = per_cpu(cpufreq_re_stats_table, policy->cpu);
        if (!stat)
                return 0;
        return sprintf(buf, "%d\n", stat->cur_mem_fit);
}

static ssize_t show_core_fit_acc(struct cpufreq_policy *policy, char *buf)
{
        struct cpufreq_re_stats *stat = per_cpu(cpufreq_re_stats_table, policy->cpu);
        if (!stat)
                return 0;
	cpufreq_re_stats_update(stat->cpu);
        return sprintf(buf, "%llu\n", stat->core_fit_acc);
}

static ssize_t show_mem_fit_acc(struct cpufreq_policy *policy, char *buf)
{
        struct cpufreq_re_stats *stat = per_cpu(cpufreq_re_stats_table, policy->cpu);
        if (!stat)
                return 0;
	cpufreq_re_stats_update(stat->cpu);
        return sprintf(buf, "%llu\n", stat->mem_fit_acc);
}

static ssize_t show_core_pow_acc(struct cpufreq_policy *policy, char *buf)
{
        struct cpufreq_re_stats *stat = per_cpu(cpufreq_re_stats_table, policy->cpu);
        if (!stat)
                return 0;
        cpufreq_re_stats_update(stat->cpu);
        return sprintf(buf, "%llu\n", stat->core_pow_acc);
}

static ssize_t show_mem_pow_acc(struct cpufreq_policy *policy, char *buf)
{
        struct cpufreq_re_stats *stat = per_cpu(cpufreq_re_stats_table, policy->cpu);
        if (!stat)
                return 0;
        cpufreq_re_stats_update(stat->cpu);
        return sprintf(buf, "%llu\n", stat->mem_pow_acc);
}

static ssize_t show_cycle_max_core_fit(struct cpufreq_policy *policy, char *buf)
{
        struct cpufreq_re_stats *stat = per_cpu(cpufreq_re_stats_table, policy->cpu);
        if (!stat)
                return 0;
        return sprintf(buf, "%llu\n", stat->cycle_max_core_fit);
}

static ssize_t store_cycle_max_core_fit(struct cpufreq_policy *policy,
                                        const char *buf, size_t count)
{
        int ret;
        struct cpufreq_re_stats *stat = per_cpu(cpufreq_re_stats_table, policy->cpu);
        ret = sscanf(buf, "%llu", &stat->cycle_max_core_fit);
        return count;
}

static ssize_t show_cycle_max_mem_fit(struct cpufreq_policy *policy, char *buf)
{
        struct cpufreq_re_stats *stat = per_cpu(cpufreq_re_stats_table, policy->cpu);
        if (!stat)
                return 0;
        return sprintf(buf, "%llu\n", stat->cycle_max_mem_fit);
}

static ssize_t store_cycle_max_mem_fit(struct cpufreq_policy *policy,
                                        const char *buf, size_t count)
{
        int ret;
        struct cpufreq_re_stats *stat = per_cpu(cpufreq_re_stats_table, policy->cpu);
        ret = sscanf(buf, "%llu", &stat->cycle_max_mem_fit);
        return count;
}

static ssize_t show_last_residency(struct cpufreq_policy *policy, char *buf)
{
        struct cpuidle_device *dev = per_cpu(cpuidle_devices, policy->cpu);;
        if (!dev)
                return 0;
	return sprintf(buf, "%d\n", dev->last_residency);
}

static ssize_t show_m3_iteration(struct cpufreq_policy *policy, char *buf)
{
	return sprintf(buf, "%d\n", mem_addr);
}

static ssize_t store_m3_iteration(struct cpufreq_policy *policy,
                                        const char *buf, size_t count)
{
	int ret;
        ret = sscanf(buf, "%d", &mem_addr);
	return count;
}

static ssize_t show_m3_delay(struct cpufreq_policy *policy, char *buf)
{
	int ret;
	ret = wkup_m3_ping_delay(mem_addr);
	return sprintf(buf, "%d\n", ret);
}

static ssize_t store_logging_state(struct cpufreq_policy *policy,
                                        const char *buf, size_t count)
{
        int ret, new_log_state;
        ret = sscanf(buf, "%d", &new_log_state);
	/*
	if (new_log_state)
	{
		re_status_init(policy->cpu);
	}
	*/
	log_state = new_log_state;
        return count;
}

static ssize_t store_tracing_state(struct cpufreq_policy *policy,
                                        const char *buf, size_t count)
{
        int ret, new_trace_state;
        ret = sscanf(buf, "%d", &new_trace_state);
        /*
        if (new_log_state)
        {
                re_status_init(policy->cpu);
        }
        */
        trace_state = new_trace_state;
        return count;
}

static ssize_t show_logging_state(struct cpufreq_policy *policy, char *buf)
{
        int ret;
        ret = sprintf(buf, "%d\n", log_state);
	return ret;
}

static ssize_t show_tracing_state(struct cpufreq_policy *policy, char *buf)
{
        int ret;
        ret = sprintf(buf, "%d\n", trace_state);
        return ret;
}

static ssize_t store_logging_name(struct cpufreq_policy *policy,
                                        const char *buf, size_t count)
{
        int ret;
        ret = sscanf(buf, "%s", log_name);
        return count;
}

static ssize_t show_logging_name(struct cpufreq_policy *policy, char *buf)
{
        int ret;
        ret = sprintf(buf, "%s\n", log_name);
        return ret;
}

cpufreq_freq_attr_rw(location_factor);
cpufreq_freq_attr_ro(cur_core_fit);
cpufreq_freq_attr_ro(cur_mem_fit);
cpufreq_freq_attr_ro(core_fit_acc);
cpufreq_freq_attr_ro(mem_fit_acc);
cpufreq_freq_attr_ro(core_pow_acc);
cpufreq_freq_attr_ro(mem_pow_acc);
cpufreq_freq_attr_ro(last_residency);
cpufreq_freq_attr_rw(cycle_max_core_fit);
cpufreq_freq_attr_rw(cycle_max_mem_fit);
cpufreq_freq_attr_rw(m3_iteration);
cpufreq_freq_attr_ro(m3_delay);
cpufreq_freq_attr_rw(logging_state);
cpufreq_freq_attr_rw(tracing_state);
cpufreq_freq_attr_rw(logging_name);

static struct attribute *default_attrs[] = {
	&location_factor.attr,
	&cur_core_fit.attr,
	&cur_mem_fit.attr,
	&core_fit_acc.attr,
	&mem_fit_acc.attr,
        &core_pow_acc.attr,
        &mem_pow_acc.attr,
	&cycle_max_core_fit.attr,
	&cycle_max_mem_fit.attr,
	&last_residency.attr,
	&m3_iteration.attr,
	&m3_delay.attr,
	&logging_state.attr,
	&tracing_state.attr,
	&logging_name.attr,
	NULL
};
static struct attribute_group stats_attr_group = {
	.attrs = default_attrs,
	.name = "re_stats"
};

static int freq_table_get_index(struct cpufreq_re_stats *stat, unsigned int freq)
{
	int index;
	for (index = 0; index < stat->max_state; index++)
		if (stat->freq_table[index] == freq)
			return index;
	return -1;
}

/* should be called late in the CPU removal sequence so that the stats
 * memory is still available in case someone tries to use it.
 */
static void cpufreq_re_stats_free_table(unsigned int cpu)
{
	struct cpufreq_re_fit_data * fit_data = per_cpu(cpufreq_re_fit_data_table, cpu);
        struct cpufreq_re_stats *stat = per_cpu(cpufreq_re_stats_table, cpu);

        if (stat) {
                pr_debug("%s: Free stat table\n", __func__);
                kfree(stat->freq_table);
                kfree(stat);
                per_cpu(cpufreq_re_stats_table, cpu) = NULL;
        }

	if (fit_data) {
		pr_debug("%s: Free fit data table\n", __func__);
		kfree(fit_data);
		per_cpu(cpufreq_re_fit_data_table, cpu) = NULL;
	}
}

/* must be called early in the CPU removal sequence (before
 * cpufreq_remove_dev) so that policy is still valid.
 */
static void cpufreq_re_stats_free_sysfs(unsigned int cpu)
{
	struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);

	if (!policy)
		return;

	if (!cpufreq_frequency_get_table(cpu))
		goto put_ref;

	if (!policy_is_shared(policy)) {
		pr_debug("%s: Free sysfs stat\n", __func__);
		sysfs_remove_group(&policy->kobj, &stats_attr_group);
	}

put_ref:
	cpufreq_cpu_put(policy);
}

static int cpufreq_re_stats_create_table(struct cpufreq_policy *policy,
		struct cpufreq_frequency_table *table)
{
	unsigned int i, j, count = 0, ret = 0, idle_state_count = 0;
	struct cpufreq_re_stats *stat;
	struct cpuidle_device *dev;
	struct cpufreq_policy *current_policy;
	struct cpufreq_re_fit_data * fit_data;
	unsigned int alloc_size;
	unsigned int cpu = policy->cpu;

	// first obtain the fit rate data
	if (per_cpu(cpufreq_re_fit_data_table, cpu))
		return -EBUSY;
	fit_data = kzalloc(sizeof(*fit_data), GFP_KERNEL);
	if ((fit_data) == NULL)
		return -ENOMEM;
	import_fit_data(fit_data, cpu);
	per_cpu(cpufreq_re_fit_data_table, cpu) = fit_data;

	if (per_cpu(cpufreq_re_stats_table, cpu))
		return -EBUSY;
	stat = kzalloc(sizeof(*stat), GFP_KERNEL);
	if ((stat) == NULL)
		return -ENOMEM;

	current_policy = cpufreq_cpu_get(cpu);
	if (current_policy == NULL) {
		ret = -EINVAL;
		goto error_get_fail;
	}

	ret = sysfs_create_group(&current_policy->kobj, &stats_attr_group);
	if (ret)
		goto error_out;

	stat->cpu = cpu;
	per_cpu(cpufreq_re_stats_table, cpu) = stat;
	dev = per_cpu(cpuidle_devices, cpu);

	for (i = 0; table[i].frequency != CPUFREQ_TABLE_END; i++) {
		unsigned int freq = table[i].frequency;
		if (freq == CPUFREQ_ENTRY_INVALID)
			continue;
		count++;
	}

	if (dev) {
		idle_state_count = dev->state_count;
		stat->cpuidle_state_num = idle_state_count;
	}

	alloc_size = count * sizeof(int) + 
			idle_state_count * sizeof(unsigned long long) + 
			idle_state_count * sizeof(unsigned long long);
	stat->max_state = count;
	stat->freq_table = kzalloc(alloc_size, GFP_KERNEL);
	if (!stat->freq_table) {
		ret = -ENOMEM;
		goto error_out;
	}
	stat->last_idle_state_usage = (unsigned long long*)(stat->freq_table + count);
        stat->last_idle_state_time = (unsigned long long *)(stat->last_idle_state_usage + count);

	j = 0;
	for (i = 0; table[i].frequency != CPUFREQ_TABLE_END; i++) {
		unsigned int freq = table[i].frequency;
		if (freq == CPUFREQ_ENTRY_INVALID)
			continue;
		if (freq_table_get_index(stat, freq) == -1)
			stat->freq_table[j++] = freq;
	}
	stat->state_num = j;
	spin_lock(&cpufreq_re_stats_lock);
        for (i = 0; i < stat->cpuidle_state_num; i++) {
                stat->last_idle_state_time[i] = dev->states_usage[i].time;
		stat->last_idle_state_usage[i] = dev->states_usage[i].usage;
        }
	stat->last_time = jiffies_to_usecs(get_jiffies_64());
	stat->last_index = freq_table_get_index(stat, policy->cur);
	stat->location_factor = 100;
	stat->core_fit_acc = 0;
	stat->mem_fit_acc = 0;
        stat->core_pow_acc = 0;
        stat->mem_pow_acc = 0;
	update_cur_fit(stat->cpu);
	if (fit_data->core_fit_c2 > fit_data->core_fit[4])
		stat->core_fit_target = fit_data->core_fit_c2 * TARGET_FACTOR / 10;
	else
		stat->core_fit_target = fit_data->core_fit[4] * TARGET_FACTOR / 10;
        stat->mem_fit_target = (fit_data->L1_mem_fit_ret[4] + fit_data->L2_mem_fit_ret) 
				* TARGET_FACTOR / 10;
	printk("fit_target are: %d %d\n", stat->core_fit_target, stat->mem_fit_target);
#ifndef STATIC_POLICY
        stat->budget_stop_time = jiffies_to_usecs(get_jiffies_64()) + ((int)(1000000/DYN_FREQ));
	stat->budget_target_core_fit_acc = stat->core_fit_acc 
			+ stat->core_fit_target * ((int)(1000000/DYN_FREQ));
	stat->budget_target_mem_fit_acc = stat->mem_fit_acc
			+ stat->mem_fit_target * ((int)(1000000/DYN_FREQ)); 
#endif
	spin_unlock(&cpufreq_re_stats_lock);
	log_thread_init(stat->cpu);

	if (stat->last_index == -1) {
		pr_err("%s: No match for current freq %u in table. Disabled!\n",
		       __func__, policy->cur);
		ret = -EINVAL;
		goto error_out;
	}

	cpufreq_cpu_put(current_policy);
	return 0;
error_out:
	cpufreq_cpu_put(current_policy);
error_get_fail:
	kfree(stat);
	per_cpu(cpufreq_re_stats_table, cpu) = NULL;
	return ret;
}

static void cpufreq_re_stats_update_policy_cpu(struct cpufreq_policy *policy)
{
	struct cpufreq_re_stats *stat = per_cpu(cpufreq_re_stats_table,
			policy->last_cpu);

	pr_debug("Updating stats_table for new_cpu %u from last_cpu %u\n",
			policy->cpu, policy->last_cpu);
	per_cpu(cpufreq_re_stats_table, policy->cpu) = per_cpu(cpufreq_re_stats_table,
			policy->last_cpu);
	per_cpu(cpufreq_re_stats_table, policy->last_cpu) = NULL;
	stat->cpu = policy->cpu;
}

static int cpufreq_re_stat_notifier_policy(struct notifier_block *nb,
		unsigned long val, void *data)
{
	int ret;
	struct cpufreq_policy *policy = data;
	struct cpufreq_frequency_table *table;
	unsigned int cpu = policy->cpu;

	if (val == CPUFREQ_UPDATE_POLICY_CPU) {
		cpufreq_re_stats_update_policy_cpu(policy);
		return 0;
	}

	if (val != CPUFREQ_NOTIFY)
		return 0;
	table = cpufreq_frequency_get_table(cpu);
	if (!table)
		return 0;
	ret = cpufreq_re_stats_create_table(policy, table);
	if (ret)
		return ret;
	return 0;
}

static int cpufreq_re_stat_notifier_trans(struct notifier_block *nb,
		unsigned long val, void *data)
{
	struct cpufreq_freqs *freq = data;
	struct cpufreq_re_stats *stat;
	int old_index, new_index;

	if (val != CPUFREQ_POSTCHANGE)
		return 0;

	stat = per_cpu(cpufreq_re_stats_table, freq->cpu);
	if (!stat)
		return 0;

	old_index = stat->last_index;
	new_index = freq_table_get_index(stat, freq->new);

	/* We can't do stat->time_in_state[-1]= .. */
	if (old_index == -1 || new_index == -1)
		return 0;

	cpufreq_re_stats_update(freq->cpu);

	if (old_index == new_index)
		return 0;

	spin_lock(&cpufreq_re_stats_lock);
	stat->last_index = new_index;
	update_cur_fit(freq->cpu);
	spin_unlock(&cpufreq_re_stats_lock);
	return 0;
}

static int cpufreq_re_stat_cpu_callback(struct notifier_block *nfb,
					       unsigned long action,
					       void *hcpu)
{
	unsigned int cpu = (unsigned long)hcpu;

	switch (action) {
	case CPU_DOWN_PREPARE:
		cpufreq_re_stats_free_sysfs(cpu);
		break;
	case CPU_DEAD:
		cpufreq_re_stats_free_table(cpu);
		break;
	}
	return NOTIFY_OK;
}

/* priority=1 so this will get called before cpufreq_remove_dev */
static struct notifier_block cpufreq_re_stat_cpu_notifier __refdata = {
	.notifier_call = cpufreq_re_stat_cpu_callback,
	.priority = 1,
};

static struct notifier_block notifier_policy_block = {
	.notifier_call = cpufreq_re_stat_notifier_policy
};

static struct notifier_block notifier_trans_block = {
	.notifier_call = cpufreq_re_stat_notifier_trans
};

static int __init cpufreq_re_stats_init(void)
{
	int ret;
	unsigned int cpu;

	spin_lock_init(&cpufreq_re_stats_lock);
	ret = cpufreq_register_notifier(&notifier_policy_block,
				CPUFREQ_POLICY_NOTIFIER);
	if (ret)
		return ret;

	register_hotcpu_notifier(&cpufreq_re_stat_cpu_notifier);

	ret = cpufreq_register_notifier(&notifier_trans_block,
				CPUFREQ_TRANSITION_NOTIFIER);
	if (ret) {
		cpufreq_unregister_notifier(&notifier_policy_block,
				CPUFREQ_POLICY_NOTIFIER);
		unregister_hotcpu_notifier(&cpufreq_re_stat_cpu_notifier);
		for_each_online_cpu(cpu)
			cpufreq_re_stats_free_table(cpu);
		return ret;
	}

	return 0;
}
static void __exit cpufreq_re_stats_exit(void)
{
	unsigned int cpu;

	cpufreq_unregister_notifier(&notifier_policy_block,
			CPUFREQ_POLICY_NOTIFIER);
	cpufreq_unregister_notifier(&notifier_trans_block,
			CPUFREQ_TRANSITION_NOTIFIER);
	unregister_hotcpu_notifier(&cpufreq_re_stat_cpu_notifier);
	for_each_online_cpu(cpu) {
		log_thread_exit(cpu);
		cpufreq_re_stats_free_table(cpu);
		cpufreq_re_stats_free_sysfs(cpu);
	}
}

static int log_thread_init(unsigned int cpu)
{
	struct cpufreq_re_log * log;
	unsigned int alloc_size;
	char thread_name[16] = "cpufreq_re_log";

        if (per_cpu(cpufreq_re_log_table, cpu))
                return -EBUSY;
        log = kzalloc(sizeof(*log), GFP_KERNEL);

	log->cpu = cpu;
	log->cur_index = 0;
	log->last_pow = 0;
	log->last_core_fit = 0;
	log->last_mem_fit = 0;

	alloc_size = 4 * LOG_LENGTH * sizeof(u64);
	log->pow_log_data = kzalloc(alloc_size, GFP_KERNEL);
	log->core_fit_data = (u64 *)(log->pow_log_data + LOG_LENGTH);
	log->mem_fit_data = (u64 *)(log->core_fit_data + LOG_LENGTH);
	log->freq_data = (u64 *)(log->mem_fit_data + LOG_LENGTH);

	per_cpu(cpufreq_re_log_table, cpu) = log;

	log_state = 0;
	trace_state = 0;

	log->data_logging_thread = kthread_create(thread_log_fn, 
					(void*)&cpu, thread_name);
	if (log->data_logging_thread)
	{
		printk("re_log_thread created on cpu %d\n", cpu);
		wake_up_process(log->data_logging_thread);
	}
	return 0;
}

static int log_thread_exit(unsigned int cpu)
{
	struct cpufreq_re_log *log = per_cpu(cpufreq_re_log_table, cpu);
	if (log)
	{
		kfree(log->pow_log_data);
		kfree(log);
	}
	return 0;
}

static void thread_collect_data(unsigned int cpu_id)
{
	int i;
	u64 cur_pow, cur_core_fit, cur_mem_fit;
	struct cpufreq_re_stats *stat = per_cpu(cpufreq_re_stats_table, cpu_id);
	struct cpufreq_re_log *log = per_cpu(cpufreq_re_log_table, cpu_id);
	if (!log)
		return;
	cpufreq_re_stats_update(log->cpu);
	cur_pow = stat->core_pow_acc + stat->mem_pow_acc;
	cur_core_fit = stat->core_fit_acc;
	cur_mem_fit = stat->mem_fit_acc;

	log->pow_log_data[log->cur_index] = cur_pow - log->last_pow;
	log->core_fit_data[log->cur_index] = cur_core_fit - log->last_core_fit;
	log->mem_fit_data[log->cur_index] = cur_mem_fit - log->last_mem_fit;
	log->freq_data[log->cur_index] = stat->freq_table[stat->last_index]/1000;

	log->last_pow = cur_pow;
	log->last_core_fit = cur_core_fit;
	log->last_mem_fit = cur_mem_fit;
	
	log->cur_index++;
	if (log->cur_index>=LOG_LENGTH)
	{
		for(i=0;i<LOG_LENGTH;i++)
		{
			if (log_state)
				pr_info("RE_LOG %s: %d %llu %llu %llu %llu\n",
					log_name, i,
					log->freq_data[i],
					log->pow_log_data[i],
					log->core_fit_data[i], 
					log->mem_fit_data[i]);
		}
		log->cur_index = 0;
	}
	return;
}

static int thread_log_fn(void* cpu)
{
	unsigned int cpu_id = *(int *)cpu;

	while(1) 
	{
		msleep(1000/LOG_FREQ);
		thread_collect_data(cpu_id);
	}

	return 0;
}

int cpufreq_re_get_C_states(unsigned int cpu)
{
	struct cpufreq_re_stats *stat;
	unsigned int core_fit_target, mem_fit_target;
#ifndef STATIC_POLICY
	unsigned long long cur_wall_time;
	u64 cycle_core_fit;
	u64 cycle_mem_fit;
	int delta;
#endif
	stat = per_cpu(cpufreq_re_stats_table, cpu);	
	if (!stat)
		return 3;
#ifndef POLICY_ENABLE
	return 3;
#endif
#ifdef STATIC_POLICY
	core_fit_target = stat->core_fit_target;
	mem_fit_target = stat->mem_fit_target;
#else
	cpufreq_re_stats_update(cpu);
	cur_wall_time = jiffies_to_usecs(get_jiffies_64());
	if (cur_wall_time >= stat->budget_stop_time) {
		// new control cycle, adjust values accordingly
		if (stat->budget_target_core_fit_acc < stat->core_fit_acc) {
			//printk("FIT overflow detected: %llu\n", 
			//stat->core_fit_acc - stat->budget_target_core_fit_acc);
		} else {
			//printk("FIT budget leftover: %llu\n",
			//stat->budget_target_core_fit_acc - stat->core_fit_acc);
		}
#ifndef STATIC_POLICY
                delta = (int)(cur_wall_time - stat->budget_stop_time) / 1000;
                if (delta<0)
                        delta = 0;
		cycle_core_fit = stat->core_fit_acc + 
			(unsigned long long)stat->core_fit_target 
			* (int)(1000000/DYN_FREQ) - stat->budget_target_core_fit_acc;
                cycle_core_fit = cycle_core_fit 
                        * ( 1000 - delta );
		if (cycle_core_fit > stat->cycle_max_core_fit)
			stat->cycle_max_core_fit = cycle_core_fit;
		cycle_mem_fit = stat->mem_fit_acc +
			(unsigned long long)stat->mem_fit_target 
			* (int)(1000000/DYN_FREQ) - stat->budget_target_mem_fit_acc;
                cycle_mem_fit = cycle_mem_fit 
                        * ( 1000 - delta );
		if (cycle_mem_fit > stat->cycle_max_mem_fit)
			stat->cycle_max_mem_fit = cycle_mem_fit;
                if (trace_state) {
        	        pr_info("TR_LOG C_STATE TIME %s: %llu %llu %llu %llu\n",
	                        log_name,
                        	stat->last_idle_state_time[0],
                	        stat->last_idle_state_time[1],
        	                stat->last_idle_state_time[2],
	                        stat->last_idle_state_time[3]
	                        );
                        pr_info("TR_LOG CYCLE %s: %llu %llu %llu %llu %llu %llu %llu %u %llu\n",
                                log_name,
                                stat->core_fit_acc>>6,
                                stat->budget_target_core_fit_acc>>6,
                                (unsigned long long)stat->core_fit_target * (int)(1000000/DYN_FREQ)>>6,
                                stat->mem_fit_acc>>6, 
				stat->budget_target_mem_fit_acc>>6,
                                (unsigned long long)stat->mem_fit_target * (int)(1000000/DYN_FREQ)>>6,
				cur_wall_time - stat->budget_stop_time,
				jiffies_to_usecs(get_jiffies_64()),
				stat->core_pow_acc + stat->mem_pow_acc
                                );
                }
#endif
		stat->budget_stop_time = jiffies_to_usecs(get_jiffies_64()) + (int)(1000000/DYN_FREQ);
	        stat->budget_target_core_fit_acc = stat->core_fit_acc 
        	                + stat->core_fit_target * (int)(1000000/DYN_FREQ);
	        stat->budget_target_mem_fit_acc = stat->mem_fit_acc
        	                + stat->mem_fit_target * (int)(1000000/DYN_FREQ); 
	}
	// first calculate the fit_budget
	core_fit_target = (unsigned int)((stat->budget_target_core_fit_acc - stat->core_fit_acc)>>8)
			/ (unsigned int)((stat->budget_stop_time - cur_wall_time)>>8);
	mem_fit_target = (unsigned int)((stat->budget_target_mem_fit_acc - stat->mem_fit_acc)>>8)
			/ (unsigned int)((stat->budget_stop_time - cur_wall_time)>>8);
#endif
        if (core_fit_target < stat->cpuidle_c2_fit) {
                // C1 only
                return 1;
        } else if (mem_fit_target < stat->cpuidle_mem_ret_fit) {
                // C1 or C2
                return 2;
        } else {
                return 3;
        }
}
EXPORT_SYMBOL(cpufreq_re_get_C_states);

int cpufreq_re_get_P_states(unsigned int cpu)
{
	struct cpufreq_re_stats *stat;
	struct cpufreq_re_fit_data * fit_data;
        unsigned int core_fit_target, mem_fit_target;
	unsigned int ret;
#ifndef STATIC_POLICY
        unsigned long long cur_wall_time;
	u64 cycle_core_fit, cycle_mem_fit;
	int delta;
#endif
#ifndef POLICY_ENABLE
	return 0;
#endif 
	stat = per_cpu(cpufreq_re_stats_table, cpu);
	fit_data = per_cpu(cpufreq_re_fit_data_table, cpu);
	ret = 0x1F;
	/*
	printk("%d %d %d %d %d %d\n", stat->core_fit_target, 
				fit_data->core_fit[4]* stat->location_factor / 100,
				fit_data->core_fit[3]* stat->location_factor / 100,
				fit_data->core_fit[2]* stat->location_factor / 100,
				fit_data->core_fit[1]* stat->location_factor / 100,
				fit_data->core_fit[0]* stat->location_factor / 100
		);
	*/
	if (!stat || !fit_data)
		return 0;
#ifdef STATIC_POLICY
	core_fit_target = stat->core_fit_target;
	mem_fit_target = stat->mem_fit_target;
#else
        cpufreq_re_stats_update(cpu);
        cur_wall_time = jiffies_to_usecs(get_jiffies_64());
        if (cur_wall_time >= stat->budget_stop_time) {
                // new control cycle, adjust values accordingly
                if (stat->budget_target_core_fit_acc < stat->core_fit_acc) {
                        //printk("FIT overflow detected: %llu\n",
                        //stat->core_fit_acc - stat->budget_target_core_fit_acc);
                } else {
                        //printk("FIT budget leftover: %llu\n",
                        //stat->budget_target_core_fit_acc - stat->core_fit_acc);
                }
#ifndef STATIC_POLICY
		delta = (int)(cur_wall_time - stat->budget_stop_time) / 1000;
		if (delta<0) 
			delta = 0;
                cycle_core_fit = stat->core_fit_acc + 
                        (unsigned long long)stat->core_fit_target 
                        * (int)(1000000/DYN_FREQ) - stat->budget_target_core_fit_acc;
		cycle_core_fit = cycle_core_fit 
			* ( 1000 - delta );
                if (cycle_core_fit > stat->cycle_max_core_fit)
                        stat->cycle_max_core_fit = cycle_core_fit;
                cycle_mem_fit = stat->mem_fit_acc +
                        (unsigned long long)stat->mem_fit_target 
                        * (int)(1000000/DYN_FREQ) - stat->budget_target_mem_fit_acc;
		cycle_mem_fit = cycle_mem_fit 
			* ( 1000 - delta );
                if (cycle_mem_fit > stat->cycle_max_mem_fit)
                        stat->cycle_max_mem_fit = cycle_mem_fit;
                if (trace_state) {
	                pr_info("TR_LOG C_STATE TIME %s: %llu %llu %llu %llu\n",
        	                log_name,
                	        stat->last_idle_state_time[0],
                        	stat->last_idle_state_time[1],
	                        stat->last_idle_state_time[2],
        	                stat->last_idle_state_time[3]
                	);
                        pr_info("TR_LOG CYCLE %s: %llu %llu %llu %llu %llu %llu %llu %u %llu\n",
                                log_name,
                                stat->core_fit_acc>>6,
                                stat->budget_target_core_fit_acc>>6,
                                (unsigned long long)stat->core_fit_target * (int)(1000000/DYN_FREQ)>>6,
                                stat->mem_fit_acc>>6,
                                stat->budget_target_mem_fit_acc>>6,
                                (unsigned long long)stat->mem_fit_target * (int)(1000000/DYN_FREQ)>>6,
                                cur_wall_time - stat->budget_stop_time,
				jiffies_to_usecs(get_jiffies_64()),
				stat->mem_pow_acc + stat->core_pow_acc
                                );
                }
#endif
                stat->budget_stop_time = jiffies_to_usecs(get_jiffies_64()) + (int)(1000000/DYN_FREQ);
                stat->budget_target_core_fit_acc = stat->core_fit_acc
                                + stat->core_fit_target * (int)(1000000/DYN_FREQ);
                stat->budget_target_mem_fit_acc = stat->mem_fit_acc
                                + stat->mem_fit_target * (int)(1000000/DYN_FREQ);
        }
        if (trace_state) {
                pr_info("TR_LOG C_STATE TIME %s: %llu %llu %llu %llu\n",
                        log_name,
                        stat->last_idle_state_time[0],
                        stat->last_idle_state_time[1],
                        stat->last_idle_state_time[2],
                        stat->last_idle_state_time[3]
                        );
        }
        // first calculate the fit_budget
        core_fit_target = (unsigned int)((stat->budget_target_core_fit_acc - stat->core_fit_acc)>>8)
                        / (unsigned int)((stat->budget_stop_time - cur_wall_time)>>8);
        mem_fit_target = (unsigned int)((stat->budget_target_mem_fit_acc - stat->mem_fit_acc)>>8)
                        / (unsigned int)((stat->budget_stop_time - cur_wall_time)>>8);
#endif
	if (core_fit_target >= fit_data->core_fit[4]
                                * stat->location_factor / 100)
		ret &= 0x1F;
	else if (core_fit_target >= fit_data->core_fit[3]
                                * stat->location_factor / 100)
		ret &= 0xF;
        else if (core_fit_target >= fit_data->core_fit[2]
                                * stat->location_factor / 100)
                ret &= 0x7;
        else if (core_fit_target >= fit_data->core_fit[1]
                                * stat->location_factor / 100)
                ret &= 0x3;
        else if (core_fit_target >= fit_data->core_fit[0]
                                * stat->location_factor / 100)
                ret &= 0x1;
	else
		ret &= 0x0;
	if (mem_fit_target >= (fit_data->L1_mem_fit[4] + fit_data->L2_mem_fit)
                                * stat->location_factor / 100)
                ret &= 0x1F;
        else if (mem_fit_target >= (fit_data->L1_mem_fit[3] + fit_data->L2_mem_fit)
                                * stat->location_factor / 100)
		ret &= 0xF;
        else if (mem_fit_target >= (fit_data->L1_mem_fit[2] + fit_data->L2_mem_fit)
                                * stat->location_factor / 100)
                ret &= 0x7;
        else if (mem_fit_target >= (fit_data->L1_mem_fit[1] + fit_data->L2_mem_fit)
                                * stat->location_factor / 100)
                ret &= 0x3;
        else if (mem_fit_target >= (fit_data->L1_mem_fit[0] + fit_data->L2_mem_fit)
                                * stat->location_factor / 100)
                ret &= 0x1;
	else
		ret &= 0x0;

	switch(ret) {
		case 0x1F:
			return 0;
			break;
		case 0xF:
			return 1;
			break;
		case 0x7:
			return 2;
			break;
		case 0x3:
			return 3;
			break;
		case 0x1:
			return 4;
			break;
		default:
			return 0;
	}
}
EXPORT_SYMBOL_GPL(cpufreq_re_get_P_states);

int cpufreq_re_report_C_states(int entered_state, int C_state_flag, 
				int residency) {
	if (trace_state) {
		pr_info("TR_LOG C %s: %d %d %d\n", log_name,
		entered_state, C_state_flag, residency);
	}
	return 0;
}
EXPORT_SYMBOL_GPL(cpufreq_re_report_C_states);

int cpufreq_re_report_P_states(int actual_state, int ideal_state)
{
        if (trace_state) {
                pr_info("TR_LOG P %s: %d %d %u\n", log_name,
                actual_state, ideal_state, jiffies_to_usecs(get_jiffies_64()));
        }
        return 0;
}
EXPORT_SYMBOL_GPL(cpufreq_re_report_P_states);

static int cpufreq_re_report_FIT(unsigned int cpu)
{
        struct cpufreq_re_stats *stat;
        struct cpuidle_device *dev;
        struct cpufreq_re_fit_data *fit_data;
        int i;
	int core_fit[20],mem_fit[20], pow[20];

        stat = per_cpu(cpufreq_re_stats_table, cpu);
        dev = per_cpu(cpuidle_devices, cpu);
        fit_data = per_cpu(cpufreq_re_fit_data_table, cpu);

        if (!stat||!dev||!fit_data) {
                return -ENOMEM;
        }

	for (i=0;i<5;i++) {
		core_fit[i] = fit_data->core_fit[i]
				* stat->location_factor / 100;
		mem_fit[i] = (fit_data->L1_mem_fit[i] + fit_data->L2_mem_fit)
				* stat->location_factor / 100;
		pow[i] = fit_data->core_pow[i] + fit_data->L1_mem_pow[i] + fit_data->L2_mem_pow;

		core_fit[i+5] = fit_data->core_fit_c1[i]
				* stat->location_factor / 100;
		mem_fit[i+5] = (fit_data->L1_mem_fit[i] + fit_data->L2_mem_fit)
                                * stat->location_factor / 100;
		pow[i+5] = fit_data->core_pow_c1[i] + fit_data->L1_mem_pow[i] + fit_data->L2_mem_pow;

		core_fit[i+10] = 0;
		mem_fit[i+10] = (fit_data->L1_mem_fit[i] + fit_data->L2_mem_fit)
                                * stat->location_factor / 100;
		pow[i+10] = fit_data->L1_mem_pow[i] + fit_data->L2_mem_pow;

		core_fit[i+15] = 0;
		mem_fit[i+15] = (fit_data->L1_mem_fit_ret[i] + fit_data->L2_mem_fit_ret)
                                * stat->location_factor / 100;
		pow[i+15] = fit_data->L1_mem_pow_ret[i]
                                + fit_data->L2_mem_pow_ret;
	}
	for (i=0;i<10;i++) {
		pr_info("DATA_LOG: %d %d %d %d %d %d\n", core_fit[2*i], mem_fit[2*i], 
			pow[2*i], core_fit[2*i+1], mem_fit[2*i+1], pow[2*i+1]);
	}
	return 0;
}



MODULE_AUTHOR("Liangzhen Lai <liangzhen@ucla.edu>");
MODULE_DESCRIPTION("'cpufreq_re_stats' - A driver to export cpu reliability stats "
				"through sysfs filesystem");
MODULE_LICENSE("GPL");

module_init(cpufreq_re_stats_init);
module_exit(cpufreq_re_stats_exit);
