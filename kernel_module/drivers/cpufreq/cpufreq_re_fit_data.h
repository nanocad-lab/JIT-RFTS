/*
 *  drivers/cpufreq/cpufreq_re_fit_data.h
 *
 * Created by Liangzhen Lai @ 10/09/2014
 * cpufreq_re_fit_data.h : interface for initializing 
 * fit rate for different power states and configurations
 *
 */

#ifndef _CPUFREQ_RE_FIR_DATA_H
#define _CPUFREQ_RE_FIR_DATA_H

struct cpufreq_re_fit_data;


struct cpufreq_re_fit_data {
        unsigned int core_fit[5];
        unsigned int core_fit_c1[5];
        unsigned int core_fit_c2;
        unsigned int core_fit_c3;
        unsigned int L1_mem_fit[5];
        unsigned int L1_mem_fit_ret[5];
        unsigned int L2_mem_fit;
        unsigned int L2_mem_fit_ret;
        unsigned int core_pow[5];
        unsigned int core_pow_c1[5];
        unsigned int core_pow_c2;
        unsigned int core_pow_c3;
        unsigned int L1_mem_pow[5];
        unsigned int L1_mem_pow_ret[5];
        unsigned int L2_mem_pow;
        unsigned int L2_mem_pow_ret;
};

void import_fit_data(struct cpufreq_re_fit_data *fit_data, unsigned int cpu);

#endif
