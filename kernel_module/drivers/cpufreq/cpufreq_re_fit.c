/*
 *  drivers/cpufreq/cpufreq_re_fit.c
 *
 * Created by Liangzhen Lai @ 10/09/2014
 * Sample values are given based on 45nm data. 
 * All values are normalized.
 *
 */

#include "cpufreq_re_fit_data.h"

// Select the targeting hardware configuration cases
// here
#define CASE1
//#define CASE2
//#define CASE3
//#define CASE4

#ifdef CASE1
//#define L1_ECC        // L1 with ECC or not
#define L1_VS // L1 with voltage scaling or not
//#define RET_FLOP
#endif

#ifdef CASE2
//#define L1_ECC        // L1 with ECC or not
//#define L1_VS // L1 with voltage scaling or not
//#define RET_FLOP
#endif

#ifdef CASE3
//#define L1_ECC        // L1 with ECC or not
#define L1_VS // L1 with voltage scaling or not
#define RET_FLOP
#endif

#ifdef CASE4
#define L1_ECC        // L1 with ECC or not
#define L1_VS // L1 with voltage scaling or not
//#define RET_FLOP
#endif

#define L2_ECC	// L2 with ECC or not
#define FF_COUNT 12000
#define NORM_FACTOR 1000000

void import_fit_data(struct cpufreq_re_fit_data *fit_data, 
			unsigned int cpu)
{
	// every power is normalized to fix point, 100 -> ~1mw
	unsigned int SRAM_32KB_base_noECC[5] = {15864, 10669, 8818, 6879, 3634};
	unsigned int SRAM_32KB_base_ECC[5] = {20339, 13587, 11129, 8743, 4645};
	unsigned int SRAM_32KB_base_noECC_ret[5] = {1479, 1329, 1327, 1236, 1131};
	unsigned int SRAM_32KB_base_ECC_ret[5] = {1938, 1701, 1658, 1564, 1452};
	unsigned int CORE_base[5] = {46800, 30824, 26352, 19740, 8820};
	unsigned int CORE_c1_base[5] = {37440, 24659, 21081, 15792, 7056};
	unsigned int CORE_c2 = 10;
	unsigned int CORE_c3 = 10;

	// FIT normalization takes 1000000 FIT -> ~1
	unsigned int SRAM_cell_base_fit[5] = {500, 545, 650, 745, 859};
	unsigned int SRAM_cell_ret_fit[5] = {2000, 2180, 2600, 2980, 3436};
	unsigned int FF_cell_base_fit[5] = {153, 167, 199, 228, 263};
#ifdef RET_FLOP
	unsigned int RET_LATCH_base_fit = 500;
#endif
	unsigned int *l1_base_pointer, *l1_base_ret_pointer;
	unsigned int *l2_base_pointer, *l2_base_ret_pointer;
	int i;

#ifdef L1_ECC
	l1_base_pointer = SRAM_32KB_base_ECC;
	l1_base_ret_pointer = SRAM_32KB_base_ECC_ret;
#else
        l1_base_pointer = SRAM_32KB_base_noECC;
        l1_base_ret_pointer = SRAM_32KB_base_noECC_ret;
#endif

#ifdef L2_ECC
        l2_base_pointer = SRAM_32KB_base_ECC;
        l2_base_ret_pointer = SRAM_32KB_base_ECC_ret;
#else
        l2_base_pointer = SRAM_32KB_base_noECC;
        l2_base_ret_pointer = SRAM_32KB_base_noECC_ret;
#endif

	for (i=0;i<5;i++) {
		// Power part
#ifdef L1_VS
		fit_data->L1_mem_pow[i] = 2 * l1_base_pointer[i];
		fit_data->L1_mem_pow_ret[i] = 2 * l1_base_ret_pointer[i];
#else
	        fit_data->L1_mem_pow[i] = 2 * l1_base_pointer[0];
        	fit_data->L1_mem_pow_ret[i] = 2 * l1_base_ret_pointer[0];
#endif
		fit_data->L2_mem_pow = 8 * l2_base_pointer[0];
		fit_data->L2_mem_pow_ret = 8 * l2_base_ret_pointer[0];
	        fit_data->core_pow[i] = CORE_base[i];
	        fit_data->core_pow_c1[i] = CORE_c1_base[i];

		// SER part
	        fit_data->core_fit[i] = FF_COUNT * FF_cell_base_fit[i]/ NORM_FACTOR;
        	fit_data->core_fit_c1[i] = FF_COUNT * FF_cell_base_fit[i]/ NORM_FACTOR;
#ifdef L1_VS
	        fit_data->L1_mem_fit[i] = 2 * 32 * 1024 * SRAM_cell_base_fit[i] / NORM_FACTOR;
	        fit_data->L1_mem_fit_ret[i] = 2 * 32 * 1024 * SRAM_cell_ret_fit[i] / NORM_FACTOR;
#else
		fit_data->L1_mem_fit[i] = 2 * 32 * 1024 * SRAM_cell_base_fit[0] / NORM_FACTOR;
	        fit_data->L1_mem_fit_ret[i] = 2 * 32 * 1024 * SRAM_cell_ret_fit[0] / NORM_FACTOR;
#endif
#ifdef L1_ECC
		fit_data->L1_mem_fit[i] = 0;
		fit_data->L1_mem_fit_ret[i] = 0;
#endif
	}
#ifdef RET_FLOP
	fit_data->core_fit_c2 = FF_COUNT * RET_LATCH_base_fit / NORM_FACTOR;
	fit_data->core_fit_c3 = FF_COUNT *RET_LATCH_base_fit / NORM_FACTOR;
#else
	fit_data->core_fit_c2 = 0;
	fit_data->core_fit_c3 = 0;
#endif
#ifdef L2_ECC
	fit_data->L2_mem_fit = 0;
	fit_data->L2_mem_fit_ret = 0;
#else
	fit_data->L2_mem_fit = 256 * 1024 * SRAM_cell_base_fit[0] / NORM_FACTOR;
	fit_data->L2_mem_fit_ret = 256 * 1024 * SRAM_cell_ret_fit[0] / NORM_FACTOR;
#endif
	fit_data->core_pow_c2 = CORE_c2; 
	fit_data->core_pow_c3 = CORE_c3;
	return;
}
