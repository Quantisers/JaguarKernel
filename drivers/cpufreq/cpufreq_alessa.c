/*
* drivers/cpufreq/cpufreq_alessa.c
*
* Copyright (C) 2011 Samsung Electronics co. ltd
* ByungChang Cha <bc.cha@samsung.com>
*
* Based on ondemand governor
* Copyright (C) 2001 Russell King
* 	    (C) 2003 Venkatesh Pallipadi <venkatesh.pallipadi@intel.com>.
* 		     Jun Nakajima <jun.nakajima@intel.com>
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License version 3 as
* published by the Free Software Foundation. 
*
* Autor: Carlos "Klozz" Jesús <xxx.reptar.rawrr.xxx@gmail.com>
* 	(C) 2014 Klozz Jesús (TeamMEX@XDA-Developers)
* 
* This peace of art are dedicated to Stephanny Marlene...
* 
* v1.0
 */

#include <asm/cputime.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/cpufreq.h>
#include <linux/cpu.h>
#include <linux/jiffies.h>
#include <linux/kernel_stat.h>
#include <linux/mutex.h>
#include <linux/hrtimer.h>
#include <linux/tick.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
/*
 * dbs is used in this file as a shortform for demandbased switching
 * It helps to keep variable names smaller, simpler
 */

/*Declare some timers*/
#define TIMER_DEFERRABLE                0x1LU

//////////////////////////////FINISH DECLARATIONS/////////////         
         
static void do_alessa_timer(struct work_struct *work);

struct cpufreq_alessa_cpuinfo {
	u64 prev_cpu_wall;
	u64 prev_cpu_idle;
	struct cpufreq_frequency_table *freq_table;
	struct delayed_work work;
	struct cpufreq_policy *cur_policy;
#if 0
	ktime_t time_stamp;
#endif
	int cpu;
	int min_index;
	int max_index;
	int pump_inc_step;
	int pump_inc_step_at_min_freq;
	int pump_dec_step;
	unsigned int cur_freq;
	/*
	 * mutex that serializes governor limit change with
	 * do_alessa_timer invocation. We do not want do_alessa_timer to run
	 * when user is changing the governor or limits.
	 */
	struct mutex timer_mutex;
};

static DEFINE_PER_CPU(struct cpufreq_alessa_cpuinfo, od_alessa_cpuinfo);

static inline u64 get_cpu_idle_time_jiffy(unsigned int cpu, u64 *wall)
{
	u64 idle_time;
	u64 cur_wall_time;
	u64 busy_time;

	cur_wall_time = jiffies64_to_cputime64(get_jiffies_64());

	busy_time  = kcpustat_cpu(cpu).cpustat[CPUTIME_USER];
	busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_SYSTEM];
	busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_IRQ];
	busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_SOFTIRQ];
	busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_STEAL];
	busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_NICE];

	idle_time = cur_wall_time - busy_time;
	if (wall)
		*wall = jiffies_to_usecs(cur_wall_time);

	return jiffies_to_usecs(idle_time);
}

static inline cputime64_t get_cpu_idle_time(unsigned int cpu, cputime64_t *wall,int io_busy)
{
	u64 idle_time = get_cpu_idle_time_us(cpu, io_busy ? wall : NULL);

	if (idle_time == -1ULL)
		return get_cpu_idle_time_jiffy(cpu, wall);
	else if (!io_busy)
		idle_time += get_cpu_iowait_time_us(cpu, wall);

	return idle_time;
}

static inline cputime64_t get_cpu_iowait_time(unsigned int cpu,
							cputime64_t *wall)
{
	u64 iowait_time = get_cpu_iowait_time_us(cpu, wall);

	if (iowait_time == -1ULL)
		return 0;

	return iowait_time;
}

static struct workqueue_struct *alessa_wq;

static unsigned int alessa_enable;	/* number of CPUs using this policy */
/*
 * alessa_mutex protects alessa_enable in governor start/stop.
 */
static DEFINE_MUTEX(alessa_mutex);

/* alessa tuners */
static struct alessa_tuners {
	unsigned int sampling_rate;
	int inc_cpu_load_at_min_freq;
	int inc_cpu_load;
	int dec_cpu_load_at_min_freq;
	int dec_cpu_load;
	int freq_responsiveness;
	unsigned int boost_cpus;
	unsigned int io_is_busy;
} alessa_tuners_ins = {
	.sampling_rate = 60000,
	.inc_cpu_load_at_min_freq = 60,
	.inc_cpu_load = 80,
	.dec_cpu_load_at_min_freq = 60,
	.dec_cpu_load = 80,
	.freq_responsiveness = 1134000,
	.boost_cpus = 0,
	.io_is_busy = 0,
};

/**************************                 ************************/
/************************** sysfs interface ************************/
/**************************                 ************************/
/*Configurable interface using xperience kerel tweaker, FauxClock enhacement project
 * trickstermod etc...*
 * cpufreq_alessa Governor Tunables */
#define show_one(file_name, object)					\
static ssize_t show_##file_name						\
(struct kobject *kobj, struct attribute *attr, char *buf)		\
{									\
	return sprintf(buf, "%d\n", alessa_tuners_ins.object);		\
}
show_one(sampling_rate, sampling_rate);/*Sample rate of the cpu work*/
show_one(inc_cpu_load_at_min_freq, inc_cpu_load_at_min_freq);/*load of cpu on min freq*/
show_one(inc_cpu_load, inc_cpu_load);
show_one(dec_cpu_load_at_min_freq, dec_cpu_load_at_min_freq);
show_one(dec_cpu_load, dec_cpu_load);
show_one(freq_responsiveness, freq_responsiveness);
show_one(boost_cpus, boost_cpus);
show_one(io_is_busy, io_is_busy);

#define show_pcpu_param(file_name, num_core)		\
static ssize_t show_##file_name##_##num_core		\
(struct kobject *kobj, struct attribute *attr, char *buf)		\
{									\
	struct cpufreq_alessa_cpuinfo *this_alessa_cpuinfo = &per_cpu(od_alessa_cpuinfo, num_core - 1); \
	return sprintf(buf, "%d\n", \
			this_alessa_cpuinfo->file_name);		\
}

show_pcpu_param(pump_inc_step_at_min_freq, 1);
show_pcpu_param(pump_inc_step_at_min_freq, 2);
show_pcpu_param(pump_inc_step_at_min_freq, 3);
show_pcpu_param(pump_inc_step_at_min_freq, 4);
show_pcpu_param(pump_inc_step, 1);
show_pcpu_param(pump_inc_step, 2);
show_pcpu_param(pump_inc_step, 3);
show_pcpu_param(pump_inc_step, 4);
show_pcpu_param(pump_dec_step, 1);
show_pcpu_param(pump_dec_step, 2);
show_pcpu_param(pump_dec_step, 3);
show_pcpu_param(pump_dec_step, 4);

#define store_pcpu_param(file_name, num_core)		\
static ssize_t store_##file_name##_##num_core		\
(struct kobject *kobj, struct attribute *attr,				\
	const char *buf, size_t count)					\
{									\
	int input;						\
	struct cpufreq_alessa_cpuinfo *this_alessa_cpuinfo; \
	int ret;							\
														\
	ret = sscanf(buf, "%d", &input);					\
	if (ret != 1)											\
		return -EINVAL;										\
														\
	this_alessa_cpuinfo = &per_cpu(od_alessa_cpuinfo, num_core - 1); \
														\
	if (input == this_alessa_cpuinfo->file_name) {		\
		return count;						\
	}								\
										\
	this_alessa_cpuinfo->file_name = input;			\
	return count;							\
}


#define store_pcpu_pump_param(file_name, num_core)		\
static ssize_t store_##file_name##_##num_core		\
(struct kobject *kobj, struct attribute *attr,				\
	const char *buf, size_t count)					\
{									\
	int input;						\
	struct cpufreq_alessa_cpuinfo *this_alessa_cpuinfo; \
	int ret;							\
														\
	ret = sscanf(buf, "%d", &input);					\
	if (ret != 1)											\
		return -EINVAL;										\
														\
	input = min(max(1, input), 3);							\
														\
	this_alessa_cpuinfo = &per_cpu(od_alessa_cpuinfo, num_core - 1); \
														\
	if (input == this_alessa_cpuinfo->file_name) {		\
		return count;						\
	}								\
										\
	this_alessa_cpuinfo->file_name = input;			\
	return count;							\
}

store_pcpu_pump_param(pump_inc_step_at_min_freq, 1);
store_pcpu_pump_param(pump_inc_step_at_min_freq, 2);
store_pcpu_pump_param(pump_inc_step_at_min_freq, 3);
store_pcpu_pump_param(pump_inc_step_at_min_freq, 4);
store_pcpu_pump_param(pump_inc_step, 1);
store_pcpu_pump_param(pump_inc_step, 2);
store_pcpu_pump_param(pump_inc_step, 3);
store_pcpu_pump_param(pump_inc_step, 4);
store_pcpu_pump_param(pump_dec_step, 1);
store_pcpu_pump_param(pump_dec_step, 2);
store_pcpu_pump_param(pump_dec_step, 3);
store_pcpu_pump_param(pump_dec_step, 4);

define_one_global_rw(pump_inc_step_at_min_freq_1);
define_one_global_rw(pump_inc_step_at_min_freq_2);
define_one_global_rw(pump_inc_step_at_min_freq_3);
define_one_global_rw(pump_inc_step_at_min_freq_4);
define_one_global_rw(pump_inc_step_1);
define_one_global_rw(pump_inc_step_2);
define_one_global_rw(pump_inc_step_3);
define_one_global_rw(pump_inc_step_4);
define_one_global_rw(pump_dec_step_1);
define_one_global_rw(pump_dec_step_2);
define_one_global_rw(pump_dec_step_3);
define_one_global_rw(pump_dec_step_4);

/* sampling_rate */
static ssize_t store_sampling_rate(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	input = max(input,10000);

	if (input == alessa_tuners_ins.sampling_rate)
		return count;

	alessa_tuners_ins.sampling_rate = input;

	return count;
}

/* inc_cpu_load_at_min_freq */
static ssize_t store_inc_cpu_load_at_min_freq(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%d", &input);
	if (ret != 1) {
		return -EINVAL;
	}

	input = min(input,alessa_tuners_ins.inc_cpu_load);

	if (input == alessa_tuners_ins.inc_cpu_load_at_min_freq)
		return count;

	alessa_tuners_ins.inc_cpu_load_at_min_freq = input;

	return count;
}

/* inc_cpu_load */
static ssize_t store_inc_cpu_load(struct kobject *a, struct attribute *b,
					const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%d", &input);
	if (ret != 1)
		return -EINVAL;

	input = max(min(input,100),0);

	if (input == alessa_tuners_ins.inc_cpu_load)
		return count;

	alessa_tuners_ins.inc_cpu_load = input;

	return count;
}

/* dec_cpu_load_at_min_freq */
static ssize_t store_dec_cpu_load_at_min_freq(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%d", &input);
	if (ret != 1) {
		return -EINVAL;
	}

	input = min(input,alessa_tuners_ins.dec_cpu_load);

	if (input == alessa_tuners_ins.dec_cpu_load_at_min_freq)
		return count;

	alessa_tuners_ins.dec_cpu_load_at_min_freq = input;

	return count;
}

/* dec_cpu_load */
static ssize_t store_dec_cpu_load(struct kobject *a, struct attribute *b,
					const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%d", &input);
	if (ret != 1)
		return -EINVAL;

	input = max(min(input,95),5);

	if (input == alessa_tuners_ins.dec_cpu_load)
		return count;

	alessa_tuners_ins.dec_cpu_load = input;

	return count;
}

/* freq_responsiveness */
static ssize_t store_freq_responsiveness(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%d", &input);
	if (ret != 1)
		return -EINVAL;

	if (input == alessa_tuners_ins.freq_responsiveness)
		return count;

	alessa_tuners_ins.freq_responsiveness = input;

	return count;
}

/* boost_cpus */
static ssize_t store_boost_cpus(struct kobject *a, struct attribute *b,
					const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%d", &input);
	if (ret != 1)
		return -EINVAL;

	input = max(min(input,2),0);

	if (input == (int)alessa_tuners_ins.boost_cpus)
		return count;

	alessa_tuners_ins.boost_cpus = (unsigned int)input;

	return count;
}
/* io_is_busy */
static ssize_t store_io_is_busy(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	unsigned int input,j;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	if (input > 1)
		input = 1;

	if (input == alessa_tuners_ins.io_is_busy)
		return count;

	alessa_tuners_ins.io_is_busy = !!input;

	/* we need to re-evaluate prev_cpu_idle */
	for_each_online_cpu(j) {
		struct cpufreq_alessa_cpuinfo *j_alessa_cpuinfo;

		j_alessa_cpuinfo = &per_cpu(od_alessa_cpuinfo, j);

		j_alessa_cpuinfo->prev_cpu_idle = get_cpu_idle_time(j,
								    &j_alessa_cpuinfo->prev_cpu_wall, alessa_tuners_ins.io_is_busy);
	}
	return count;
}

define_one_global_rw(sampling_rate);
define_one_global_rw(inc_cpu_load_at_min_freq);
define_one_global_rw(inc_cpu_load);
define_one_global_rw(dec_cpu_load_at_min_freq);
define_one_global_rw(dec_cpu_load);
define_one_global_rw(freq_responsiveness);
define_one_global_rw(boost_cpus);
define_one_global_rw(io_is_busy);

static struct attribute *alessa_attributes[] = {
	&sampling_rate.attr,
	&inc_cpu_load_at_min_freq.attr,
	&inc_cpu_load.attr,
	&dec_cpu_load_at_min_freq.attr,
	&dec_cpu_load.attr,
	&freq_responsiveness.attr,
	&boost_cpus.attr,
	&io_is_busy.attr,
	&pump_inc_step_at_min_freq_1.attr,
	&pump_inc_step_at_min_freq_2.attr,
	&pump_inc_step_at_min_freq_3.attr,
	&pump_inc_step_at_min_freq_4.attr,
	&pump_inc_step_1.attr,
	&pump_inc_step_2.attr,
	&pump_inc_step_3.attr,
	&pump_inc_step_4.attr,
	&pump_dec_step_1.attr,
	&pump_dec_step_2.attr,
	&pump_dec_step_3.attr,
	&pump_dec_step_4.attr,
	NULL
};

static struct attribute_group alessa_attr_group = {
	.attrs = alessa_attributes,
	.name = "alessa",
};

/**************************                     ************************/
/************************** end sysfs interface ************************/
/**************************                     ************************/

#if 0
/* Will return if we need to evaluate cpu load again or not */
static inline bool need_load_eval(struct cpufreq_alessa_cpuinfo *this_alessa_cpuinfo,
		unsigned int sampling_rate)
{
	ktime_t time_now = ktime_get();
	s64 delta_us = ktime_us_delta(time_now, this_alessa_cpuinfo->time_stamp);

	/* Do nothing if we recently have sampled */
	if (delta_us < (s64)(sampling_rate / 2))
		return false;
	else
		this_alessa_cpuinfo->time_stamp = time_now;

	return true;
}
#endif

static void alessa_check_cpu(struct cpufreq_alessa_cpuinfo *this_alessa_cpuinfo)
{
	struct cpufreq_policy *cpu_policy;
	unsigned int freq_responsiveness = alessa_tuners_ins.freq_responsiveness;
	int dec_cpu_load = alessa_tuners_ins.dec_cpu_load;
	int inc_cpu_load = alessa_tuners_ins.inc_cpu_load;
	int pump_inc_step = this_alessa_cpuinfo->pump_inc_step;
	int pump_dec_step = this_alessa_cpuinfo->pump_dec_step;
	u64 cur_wall_time, cur_idle_time;
	unsigned int wall_time, idle_time;
	unsigned int index = 0;
	unsigned int hi_index = 0;
	int cur_load = -1;
	int j;
	int onlines = 0;
	unsigned int cpu;
	unsigned int avg_freq = 0;
	unsigned int max_freq = 0;
	unsigned int boost_cpus = alessa_tuners_ins.boost_cpus;
	int io_busy = alessa_tuners_ins.io_is_busy;

	cpu = this_alessa_cpuinfo->cpu;
	cpu_policy = this_alessa_cpuinfo->cur_policy;

	cur_idle_time = get_cpu_idle_time(cpu, &cur_wall_time, io_busy);

	wall_time = (unsigned int)
			(cur_wall_time - this_alessa_cpuinfo->prev_cpu_wall);
	this_alessa_cpuinfo->prev_cpu_wall = cur_wall_time;

	idle_time = (unsigned int)
			(cur_idle_time - this_alessa_cpuinfo->prev_cpu_idle);
	this_alessa_cpuinfo->prev_cpu_idle = cur_idle_time;

	/*printk(KERN_ERR "TIMER CPU[%u], wall[%u], idle[%u]\n",cpu, wall_time, idle_time);*/
	if (wall_time >= idle_time) { /*if wall_time < idle_time, evaluate cpu load next time*/
		cur_load = wall_time > idle_time ? (100 * (wall_time - idle_time)) / wall_time : 1;/*if wall_time is equal to idle_time cpu_load is equal to 1*/

		if (boost_cpus > 0) {
			for_each_cpu(j, cpu_policy->cpus) {
				struct cpufreq_alessa_cpuinfo *j_alessa_cpuinfo;

				j_alessa_cpuinfo = &per_cpu(od_alessa_cpuinfo, j);
				if (j != cpu && j_alessa_cpuinfo->cur_freq > 0) {
					if (j_alessa_cpuinfo->cur_freq > max_freq)
						max_freq = j_alessa_cpuinfo->cur_freq;
					avg_freq += j_alessa_cpuinfo->cur_freq;
					++onlines;
				}
			}
			avg_freq = (avg_freq / onlines);
		}

		cpufreq_notify_utilization(cpu_policy, cur_load);

		/* Maximum increasing frequency possible */
		cpufreq_frequency_table_target(cpu_policy, this_alessa_cpuinfo->freq_table, max(cur_load * (cpu_policy->max / 100), cpu_policy->min),
				CPUFREQ_RELATION_C, &hi_index);

		cpufreq_frequency_table_target(cpu_policy, this_alessa_cpuinfo->freq_table, cpu_policy->cur,
				CPUFREQ_RELATION_C, &index);

		/* CPUs Online Scale Frequency*/
		if (cpu_policy->cur < freq_responsiveness) {
			inc_cpu_load = alessa_tuners_ins.inc_cpu_load_at_min_freq;
			dec_cpu_load = alessa_tuners_ins.dec_cpu_load_at_min_freq;
			pump_inc_step = this_alessa_cpuinfo->pump_inc_step_at_min_freq;
			hi_index = this_alessa_cpuinfo->max_index;
		}
		/* Check for frequency increase or for frequency decrease */
		if (cur_load >= inc_cpu_load && index < hi_index) {
			if ((index + pump_inc_step) >= hi_index)
				index = hi_index;
			else
				index += pump_inc_step;

		} else if (cur_load < dec_cpu_load && index > this_alessa_cpuinfo->min_index) {
			if ((index - pump_dec_step) <= this_alessa_cpuinfo->min_index)
				index = this_alessa_cpuinfo->min_index;
			else
				index -= pump_dec_step;
		}

		if (boost_cpus == 1 &&	avg_freq > this_alessa_cpuinfo->freq_table[index].frequency) {
			cpufreq_frequency_table_target(cpu_policy, this_alessa_cpuinfo->freq_table, avg_freq,
				CPUFREQ_RELATION_C, &index);
			this_alessa_cpuinfo->cur_freq = this_alessa_cpuinfo->freq_table[index].frequency;
		} else if (boost_cpus == 2 && max_freq > this_alessa_cpuinfo->freq_table[index].frequency) {
			this_alessa_cpuinfo->cur_freq = max_freq;
		} else {
			this_alessa_cpuinfo->cur_freq = this_alessa_cpuinfo->freq_table[index].frequency;
		}
		/*printk(KERN_ERR "FREQ CALC.: CPU[%u], load[%d], target freq[%u], cur freq[%u], min freq[%u], max_freq[%u]\n",cpu, cur_load, this_alessa_cpuinfo->freq_table[index].frequency, cpu_policy->cur, cpu_policy->min, this_alessa_cpuinfo->freq_table[hi_index].frequency);*/
		if (this_alessa_cpuinfo->cur_freq != cpu_policy->cur) {
			__cpufreq_driver_target(cpu_policy, this_alessa_cpuinfo->cur_freq, CPUFREQ_RELATION_C);
		}
	}
}

static void do_alessa_timer(struct work_struct *work)
{
	struct cpufreq_alessa_cpuinfo *alessa_cpuinfo;
	unsigned int sampling_rate;
	int delay;
	unsigned int cpu;

	alessa_cpuinfo = container_of(work, struct cpufreq_alessa_cpuinfo, work.work);
	cpu = alessa_cpuinfo->cpu;

	mutex_lock(&alessa_cpuinfo->timer_mutex);

	sampling_rate = alessa_tuners_ins.sampling_rate;
	delay = usecs_to_jiffies(sampling_rate);
	/* We want all CPUs to do sampling nearly on
	 * same jiffy
	 */
	if (num_online_cpus() > 1) {
		delay -= jiffies % delay;
	}

#if 0
	if (need_load_eval(alessa_cpuinfo, sampling_rate))
#endif
		alessa_check_cpu(alessa_cpuinfo);

	queue_delayed_work_on(cpu, alessa_wq, &alessa_cpuinfo->work, delay);
	mutex_unlock(&alessa_cpuinfo->timer_mutex);
}

static int cpufreq_governor_alessa(struct cpufreq_policy *policy,
				unsigned int event)
{
	unsigned int cpu;
	struct cpufreq_alessa_cpuinfo *this_alessa_cpuinfo;
	int rc, delay;
	int io_busy;

	cpu = policy->cpu;
	io_busy = alessa_tuners_ins.io_is_busy;
	this_alessa_cpuinfo = &per_cpu(od_alessa_cpuinfo, cpu);

	switch (event) {
	case CPUFREQ_GOV_START:
		if ((!cpu_online(cpu)) || (!policy->cur))
			return -EINVAL;

		mutex_lock(&alessa_mutex);

		this_alessa_cpuinfo->cpu = cpu;
		this_alessa_cpuinfo->cur_policy = policy;

		this_alessa_cpuinfo->prev_cpu_idle = get_cpu_idle_time(cpu, &this_alessa_cpuinfo->prev_cpu_wall, io_busy);

		cpufreq_frequency_table_target(policy, this_alessa_cpuinfo->freq_table, policy->min,
			CPUFREQ_RELATION_L, &this_alessa_cpuinfo->min_index);

		cpufreq_frequency_table_target(policy, this_alessa_cpuinfo->freq_table, policy->max,
			CPUFREQ_RELATION_H, &this_alessa_cpuinfo->max_index);

		this_alessa_cpuinfo->cur_freq = policy->cur;

		alessa_enable++;
		/*
		 * Start the timerschedule work, when this governor
		 * is used for first time
		 */
		if (alessa_enable == 1) {
			rc = sysfs_create_group(cpufreq_global_kobject,
						&alessa_attr_group);
			if (rc) {
				alessa_enable--;
				mutex_unlock(&alessa_mutex);
				return rc;
			}
		}
		mutex_unlock(&alessa_mutex);

		mutex_init(&this_alessa_cpuinfo->timer_mutex);

#if 0
		/* Initiate timer time stamp */
		this_alessa_cpuinfo->time_stamp = ktime_get();
#endif
		delay=usecs_to_jiffies(alessa_tuners_ins.sampling_rate);
		if (num_online_cpus() > 1) {
			delay -= jiffies % delay;
		}

		INIT_DELAYED_WORK_DEFERRABLE(&this_alessa_cpuinfo->work, do_alessa_timer);
		queue_delayed_work_on(this_alessa_cpuinfo->cpu, alessa_wq, &this_alessa_cpuinfo->work, delay);

		break;

	case CPUFREQ_GOV_STOP:
		cancel_delayed_work_sync(&this_alessa_cpuinfo->work);

		mutex_lock(&alessa_mutex);
		mutex_destroy(&this_alessa_cpuinfo->timer_mutex);

		alessa_enable--;
		if (!alessa_enable) {
			sysfs_remove_group(cpufreq_global_kobject,
					   &alessa_attr_group);
		}
		this_alessa_cpuinfo->cur_freq = 0;
		mutex_unlock(&alessa_mutex);

		break;

	case CPUFREQ_GOV_LIMITS:
		if (!this_alessa_cpuinfo->cur_policy) {
			pr_debug("Unable to limit cpu freq due to cur_policy == NULL\n");
			return -EPERM;
		}
		mutex_lock(&this_alessa_cpuinfo->timer_mutex);
		cpufreq_frequency_table_target(policy, this_alessa_cpuinfo->freq_table, policy->min,
			CPUFREQ_RELATION_L, &this_alessa_cpuinfo->min_index);

		cpufreq_frequency_table_target(policy, this_alessa_cpuinfo->freq_table, policy->max,
			CPUFREQ_RELATION_H, &this_alessa_cpuinfo->max_index);

		if (policy->max < this_alessa_cpuinfo->cur_policy->cur)
			__cpufreq_driver_target(this_alessa_cpuinfo->cur_policy,
				policy->max, CPUFREQ_RELATION_H);
		else if (policy->min > this_alessa_cpuinfo->cur_policy->cur)
			__cpufreq_driver_target(this_alessa_cpuinfo->cur_policy,
				policy->min, CPUFREQ_RELATION_L);

		this_alessa_cpuinfo->cur_freq = policy->cur;
		mutex_unlock(&this_alessa_cpuinfo->timer_mutex);

		break;
	}
	return 0;
}

#ifndef CONFIG_CPU_FREQ_DEFAULT_GOV_ALESSA
static
#endif
struct cpufreq_governor cpufreq_gov_alessa = {
	.name                   = "alessa",
	.governor               = cpufreq_governor_alessa,
	.owner                  = THIS_MODULE,
};


static int __init cpufreq_gov_alessa_init(void)
{
	unsigned int cpu;

	alessa_wq = alloc_workqueue("alessa_wq", WQ_HIGHPRI, 0);

	if (!alessa_wq) {
		printk(KERN_ERR "Failed to create alessa workqueue\n");
		return -EFAULT;
	}

	for_each_possible_cpu(cpu) {
		struct cpufreq_alessa_cpuinfo *this_alessa_cpuinfo = &per_cpu(od_alessa_cpuinfo, cpu);

		this_alessa_cpuinfo->freq_table = cpufreq_frequency_get_table(cpu);

		this_alessa_cpuinfo->pump_inc_step_at_min_freq = 2;

		if (cpu < 2)
			this_alessa_cpuinfo->pump_inc_step = 2;
		else
			this_alessa_cpuinfo->pump_inc_step = 1;

		this_alessa_cpuinfo->pump_dec_step = 1;
	}

	return cpufreq_register_governor(&cpufreq_gov_alessa);
}

static void __exit cpufreq_gov_alessa_exit(void)
{
	destroy_workqueue(alessa_wq);
	cpufreq_unregister_governor(&cpufreq_gov_alessa);
}

MODULE_AUTHOR("Carlos "klozz" Jesus TeamMEX@XDA-Develpers");
MODULE_DESCRIPTION("'cpufreq_alessa' - A dynamic cpufreq governor for msm devices");
MODULE_LICENSE("GPL");

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_ALESSA
fs_initcall(cpufreq_gov_alessa_init);
#else
module_init(cpufreq_gov_alessa_init);
#endif
module_exit(cpufreq_gov_alessa_exit);
 
