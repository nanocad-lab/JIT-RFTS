/*
 * AM33XX Power Management Routines
 *
 * Copyright (C) 2012 Texas Instruments Incorporated - http://www.ti.com/
 * Vaibhav Bedia <vaibhav.bedia@ti.com>
 *
 *  Sept 2014 - Liangzhen Lai <liangzhen@ucla.edu>
 *      Added support for cpufreq_re_stats module
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation version 2.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/cpu.h>
#include <linux/err.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/sched.h>
#include <linux/suspend.h>
#include <linux/completion.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/ti_emif.h>
#include <linux/omap-mailbox.h>

#include <asm/unaligned.h>
#include <asm/suspend.h>
#include <asm/proc-fns.h>
#include <asm/sizes.h>
#include <asm/fncpy.h>
#include <asm/system_misc.h>
#include <asm/smp_scu.h>

#include "pm.h"
#include "cm33xx.h"
#include "pm33xx.h"
#include "common.h"
#include "clockdomain.h"
#include "powerdomain.h"
#include "soc.h"
#include "sram.h"
#include "omap_device.h"
#include "control.h"

#ifdef CONFIG_SUSPEND
static void __iomem *scu_base;
static struct powerdomain *mpu_pwrdm, *per_pwrdm, *gfx_pwrdm;
static struct clockdomain *gfx_l4ls_clkdm;
static struct clockdomain *l3s_clkdm, *l4fw_clkdm, *clk_24mhz_clkdm;

static char *am33xx_i2c_sleep_sequence;
static char *am33xx_i2c_wake_sequence;
static size_t i2c_sleep_sequence_sz;
static size_t i2c_wake_sequence_sz;
#endif /* CONFIG_SUSPEND */

#ifdef CONFIG_CPU_PM
static void __iomem *am33xx_emif_base;
static struct am33xx_pm_context *am33xx_pm;

static DECLARE_COMPLETION(am33xx_pm_sync);

static void (*am33xx_do_wfi_sram)(struct am33xx_suspend_params *);

static struct am33xx_suspend_params susp_params;

int am33xx_do_sram_cpuidle_customized(u32 wfi_flags, u32 m3_flags, u32 state)
{
        struct am33xx_suspend_params params;
        //struct wkup_m3_wakeup_src wakeup_src;
	//ktime_t time_start, time_end;
	//s64 diff;
        int ret;

        /* Start with the default flags */
        memcpy(&params, &susp_params, sizeof(params));

        /* Clear bits configurable through this call */
        params.wfi_flags &= ~(WFI_SELF_REFRESH | WFI_WAKE_M3 | WFI_SAVE_EMIF |
                                                        WFI_DISABLE_EMIF);

        /* Don't enter these states if the M3 isn't available */
        if (am33xx_pm->state != M3_STATE_INITED)
                wfi_flags &= ~WFI_WAKE_M3;

        /* Set bits that have been passed */
        params.wfi_flags |= wfi_flags;

        if (wfi_flags & WFI_WAKE_M3) {
                am33xx_pm->ipc.reg1 = IPC_CMD_IDLE;
                am33xx_pm->ipc.reg2 = DS_IPC_DEFAULT;
                am33xx_pm->ipc.reg3 = m3_flags;
                am33xx_pm->ipc.reg5 = DS_IPC_DEFAULT;
		am33xx_pm->ipc.reg7 = 0xA5A50000 | state;
                wkup_m3_pm_set_cmd(&am33xx_pm->ipc);
                ret = wkup_m3_ping_noirq();
                if (ret < 0)
                        return ret;
        }
        //time_start = ktime_get();
        am33xx_do_wfi_sram(&params);
	//time_end = ktime_get();
	//diff = ktime_to_us(ktime_sub(time_end, time_start));
	/* print the wakeup reason */
	//wakeup_src = wkup_m3_wake_src();

	//pr_info("PM: Wakeup source %s time %d\n", wakeup_src.src, (int)diff);

        return 0;
}

int am33xx_do_sram_cpuidle(u32 wfi_flags, u32 m3_flags)
{
	struct am33xx_suspend_params params;
	int ret;

	/* Start with the default flags */
	memcpy(&params, &susp_params, sizeof(params));

	/* Clear bits configurable through this call */
	params.wfi_flags &= ~(WFI_SELF_REFRESH | WFI_WAKE_M3 | WFI_SAVE_EMIF |
							WFI_DISABLE_EMIF);

	/* Don't enter these states if the M3 isn't available */
	if (am33xx_pm->state != M3_STATE_INITED)
		wfi_flags &= ~WFI_WAKE_M3;

	/* Set bits that have been passed */
	params.wfi_flags |= wfi_flags;

	if (wfi_flags & WFI_WAKE_M3) {
		am33xx_pm->ipc.reg1 = IPC_CMD_IDLE;
		am33xx_pm->ipc.reg2 = DS_IPC_DEFAULT;
		am33xx_pm->ipc.reg3 = m3_flags;
		am33xx_pm->ipc.reg5 = DS_IPC_DEFAULT;
		wkup_m3_pm_set_cmd(&am33xx_pm->ipc);
		ret = wkup_m3_ping_noirq();
		if (ret < 0)
			return ret;
	}

	am33xx_do_wfi_sram(&params);
	return 0;
}

#ifdef CONFIG_SUSPEND
static int am33xx_do_sram_idle(long unsigned int arg)
{
	am33xx_do_wfi_sram((struct am33xx_suspend_params *)arg);
	return 0;
}

static int am33xx_pm_suspend(unsigned int state)
{
	int i, ret = 0;
	struct wkup_m3_wakeup_src wakeup_src;

	omap_set_pwrdm_state(gfx_pwrdm, PWRDM_POWER_OFF);

	am33xx_pm->ops->pre_suspend(state);

	ret = cpu_suspend((long unsigned int) &susp_params,
							am33xx_do_sram_idle);

	am33xx_pm->ops->post_suspend(state);

	if (ret) {
		pr_err("PM: Kernel suspend failure\n");
	} else {
		i = wkup_m3_pm_status();
		switch (i) {
		case 0:
			pr_info("PM: Successfully put all powerdomains to target state\n");

			/*
			 * The PRCM registers on AM335x do not contain
			 * previous state information like those present on
			 * OMAP4 so we must manually indicate transition so
			 * state counters are properly incremented
			 */
			pwrdm_post_transition(mpu_pwrdm);
			pwrdm_post_transition(per_pwrdm);
			break;
		case 1:
			pr_err("PM: Could not transition all powerdomains to target state\n");
			ret = -1;
			break;
		default:
			pr_err("PM: CM3 returned unknown result = %d\n", i);
			ret = -1;
		}
		/* print the wakeup reason */
		wakeup_src = wkup_m3_wake_src();

		pr_info("PM: Wakeup source %s\n", wakeup_src.src);
	}

	return ret;
}

static int am33xx_pm_enter(suspend_state_t suspend_state)
{
	int ret = 0;

	switch (suspend_state) {
	case PM_SUSPEND_STANDBY:
	case PM_SUSPEND_MEM:
		ret = am33xx_pm_suspend(suspend_state);
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}


static void am33xx_m3_state_machine_reset(void)
{
	int i;

	am33xx_pm->ipc.reg1 = IPC_CMD_RESET;

	wkup_m3_pm_set_cmd(&am33xx_pm->ipc);

	am33xx_pm->state = M3_STATE_MSG_FOR_RESET;

	if (!wkup_m3_ping()) {
		i = wait_for_completion_timeout(&am33xx_pm_sync,
					msecs_to_jiffies(500));
		if (!i) {
			WARN(1, "PM: MPU<->CM3 sync failure\n");
			am33xx_pm->state = M3_STATE_UNKNOWN;
		}
	} else {
		pr_warn("PM: Unable to ping CM3\n");
	}
}

static int am33xx_pm_begin(suspend_state_t state)
{
	int i;

	unsigned long param4;
	int pos;

	cpu_idle_poll_ctrl(true);

	param4 = DS_IPC_DEFAULT;

	wkup_m3_reset_data_pos();
	if (am33xx_i2c_sleep_sequence) {
		pos = wkup_m3_copy_data(am33xx_i2c_sleep_sequence,
						i2c_sleep_sequence_sz);
		/* Lower 16 bits stores offset to sleep sequence */
		param4 &= ~0xffff;
		param4 |= pos;
	}

	if (am33xx_i2c_wake_sequence) {
		pos = wkup_m3_copy_data(am33xx_i2c_wake_sequence,
						i2c_wake_sequence_sz);
		/* Upper 16 bits stores offset to wake sequence */
		param4 &= ~0xffff0000;
		param4 |= pos << 16;
	}

	switch (state) {
	case PM_SUSPEND_MEM:
		am33xx_pm->ipc.reg1	= IPC_CMD_DS0;
		break;
	case PM_SUSPEND_STANDBY:
		am33xx_pm->ipc.reg1	= IPC_CMD_STANDBY;
		break;
	}

	am33xx_pm->ipc.reg2		= DS_IPC_DEFAULT;
	am33xx_pm->ipc.reg3		= DS_IPC_DEFAULT;
	am33xx_pm->ipc.reg5		= param4;
	wkup_m3_pm_set_cmd(&am33xx_pm->ipc);

	am33xx_pm->state = M3_STATE_MSG_FOR_LP;

	if (!wkup_m3_ping()) {
		i = wait_for_completion_timeout(&am33xx_pm_sync,
					msecs_to_jiffies(500));
		if (!i) {
			WARN(1, "PM: MPU<->CM3 sync failure\n");
			return -1;
		}
	} else {
		pr_warn("PM: Unable to ping CM3\n");
		return -1;
	}

	return 0;
}

static void am33xx_pm_end(void)
{
	am33xx_m3_state_machine_reset();

	cpu_idle_poll_ctrl(false);

	return;
}

static int am33xx_pm_valid(suspend_state_t state)
{
	switch (state) {
	case PM_SUSPEND_STANDBY:
	case PM_SUSPEND_MEM:
		return 1;
	default:
		return 0;
	}
}

static const struct platform_suspend_ops am33xx_pm_ops = {
	.begin		= am33xx_pm_begin,
	.end		= am33xx_pm_end,
	.enter		= am33xx_pm_enter,
	.valid		= am33xx_pm_valid,
};
#endif /* CONFIG_SUSPEND */

static void am33xx_txev_handler(void)
{
	switch (am33xx_pm->state) {
	case M3_STATE_RESET:
		am33xx_pm->state = M3_STATE_INITED;
		complete(&am33xx_pm_sync);
		break;
	case M3_STATE_MSG_FOR_RESET:
		am33xx_pm->state = M3_STATE_INITED;
		complete(&am33xx_pm_sync);
		break;
	case M3_STATE_MSG_FOR_LP:
		complete(&am33xx_pm_sync);
		break;
	case M3_STATE_UNKNOWN:
		pr_warn("PM: Unknown CM3 State\n");
	}

	return;
}

static void am33xx_m3_fw_ready_cb(void)
{
	int ret = 0;

	ret = wkup_m3_prepare();
	if (ret) {
		pr_err("PM: Could not prepare WKUP_M3\n");
		return;
	}

	ret = wait_for_completion_timeout(&am33xx_pm_sync,
					msecs_to_jiffies(500));

	if (WARN(ret == 0, "PM: MPU<->CM3 sync failure\n"))
		return;

	am33xx_pm->ver = wkup_m3_fw_version_read();

	if (am33xx_pm->ver == M3_VERSION_UNKNOWN ||
		am33xx_pm->ver < M3_BASELINE_VERSION) {
		pr_warn("PM: CM3 Firmware Version %x not supported\n",
					am33xx_pm->ver);
		return;
	} else {
		pr_info("PM: CM3 Firmware Version = 0x%x\n",
					am33xx_pm->ver);
	}

	if (soc_is_am33xx())
		am33xx_idle_init(susp_params.wfi_flags & WFI_MEM_TYPE_DDR3);

#ifdef CONFIG_SUSPEND
	suspend_set_ops(&am33xx_pm_ops);
#endif /* CONFIG_SUSPEND */
}

static struct wkup_m3_ops am33xx_wkup_m3_ops = {
	.txev_handler = am33xx_txev_handler,
	.firmware_loaded = am33xx_m3_fw_ready_cb,
};

/*
 * Push the minimal suspend-resume code to SRAM
 */
void am33xx_push_sram_idle(void)
{
	am33xx_do_wfi_sram = (void *)omap_sram_push
					(am33xx_do_wfi, am33xx_do_wfi_sz);
}

void am43xx_push_sram_idle(void)
{
	am33xx_do_wfi_sram = (void *)omap_sram_push
					(am43xx_do_wfi, am43xx_do_wfi_sz);
}

static int __init am33xx_map_emif(void)
{
	am33xx_emif_base = ioremap(AM33XX_EMIF_BASE, SZ_32K);

	if (!am33xx_emif_base)
		return -ENOMEM;

	return 0;
}

#ifdef CONFIG_SUSPEND
static int __init am43xx_map_scu(void)
{
	scu_base = ioremap(scu_a9_get_base(), SZ_256);

	if (!scu_base)
		return -ENOMEM;

	return 0;
}

static int __init am33xx_setup_sleep_sequence(void)
{
	int ret;
	int sz;
	const void *prop;
	struct device *dev;
	u32 freq_hz = 100000;
	unsigned short freq_khz;

	/*
	 * We put the device tree node in the I2C controller that will
	 * be sending the sequence. i2c1 is the only controller that can
	 * be accessed by the firmware as it is the only controller in the
	 * WKUP domain.
	 */
	dev = omap_device_get_by_hwmod_name("i2c1");
	if (IS_ERR(dev))
		return PTR_ERR(dev);

	of_property_read_u32(dev->of_node, "clock-frequency", &freq_hz);
	freq_khz = freq_hz / 1000;

	prop = of_get_property(dev->of_node, "sleep-sequence", &sz);
	if (prop) {
		/*
		 * Length is sequence length + 2 bytes for freq_khz, and 1
		 * byte for terminator.
		 */
		am33xx_i2c_sleep_sequence = kzalloc(sz + 3, GFP_KERNEL);

		if (!am33xx_i2c_sleep_sequence)
			return -ENOMEM;
		put_unaligned_le16(freq_khz, am33xx_i2c_sleep_sequence);
		memcpy(am33xx_i2c_sleep_sequence + 2, prop, sz);
		i2c_sleep_sequence_sz = sz + 3;
	}

	prop = of_get_property(dev->of_node, "wake-sequence", &sz);
	if (prop) {
		am33xx_i2c_wake_sequence = kzalloc(sz + 3, GFP_KERNEL);
		if (!am33xx_i2c_wake_sequence) {
			ret = -ENOMEM;
			goto cleanup_sleep;
		}
		put_unaligned_le16(freq_khz, am33xx_i2c_wake_sequence);
		memcpy(am33xx_i2c_wake_sequence + 2, prop, sz);
		i2c_wake_sequence_sz = sz + 3;
	}

	return 0;

cleanup_sleep:
	kfree(am33xx_i2c_sleep_sequence);
	am33xx_i2c_sleep_sequence = NULL;
	return ret;
}

static int am33xx_suspend_init(void)
{
	u32 temp;

	gfx_l4ls_clkdm = clkdm_lookup("gfx_l4ls_gfx_clkdm");
	l3s_clkdm = clkdm_lookup("l3s_clkdm");
	l4fw_clkdm = clkdm_lookup("l4fw_clkdm");
	clk_24mhz_clkdm = clkdm_lookup("clk_24mhz_clkdm");

	if ((!gfx_l4ls_clkdm) || (!l3s_clkdm) || (!l4fw_clkdm) ||
	    (!clk_24mhz_clkdm)) {
		pr_err("PM: Cannot lookup clockdomains\n");
		return -ENODEV;
	}

	/* Physical resume address to be used by ROM code */
	am33xx_pm->ipc.reg0 = (AM33XX_OCMC_END -
		am33xx_do_wfi_sz + am33xx_resume_offset + 0x4);

	/*
	 * Save SDRAM config in shadow register.
	 * When the EMIF gets powered back up, its SDRAM_CONFIG register gets
	 * loaded from the SECURE_SDRAM_CONFIG register.
	 */
	temp = readl(am33xx_emif_base + EMIF_SDRAM_CONFIG);
	omap_ctrl_writel(temp, AM33XX_CONTROL_SECURE_SDRAM_CONFIG);

	return 0;
}

static int am43xx_suspend_init(void)
{
	int ret = 0;
	ret = am43xx_map_scu();
	if (ret) {
			pr_err("PM: Could not ioremap SCU\n");
			return ret;
	}

	susp_params.l2_base_virt = omap4_get_l2cache_base();

	susp_params.cke_override_virt =
		ioremap(AM43XX_CTRL_CKE_OVERRIDE, SZ_4);

	if (!susp_params.cke_override_virt) {
		pr_err("PM: Could not ioremap CKE override in Control Module\n");
		return -ENOMEM;
	}

	/* Physical resume address to be used by ROM code */
	am33xx_pm->ipc.reg0 = (AM33XX_OCMC_END -
		am43xx_do_wfi_sz + am43xx_resume_offset + 0x4);

	return ret;
}

static void am33xx_pre_suspend(unsigned int state)
{
	if (state == PM_SUSPEND_STANDBY) {
		clkdm_wakeup(l3s_clkdm);
		clkdm_wakeup(l4fw_clkdm);
		clkdm_wakeup(clk_24mhz_clkdm);
	}
}

static void am43xx_pre_suspend(unsigned int state)
{
	scu_power_mode(scu_base, SCU_PM_POWEROFF);
}

static void am33xx_post_suspend(unsigned int state)
{
	int status = 0;

	status = pwrdm_read_pwrst(gfx_pwrdm);
	if (status != PWRDM_POWER_OFF)
		pr_err("GFX domain did not transition\n");

	/*
	 * BUG: GFX_L4LS clock domain needs to be woken up to
	 * ensure thet L4LS clock domain does not get stuck in
	 * transition. If that happens L3 module does not get
	 * disabled, thereby leading to PER power domain
	 * transition failing
	 */
	clkdm_wakeup(gfx_l4ls_clkdm);
	clkdm_sleep(gfx_l4ls_clkdm);
}

static void am43xx_post_suspend(unsigned int state)
{
	scu_power_mode(scu_base, SCU_PM_NORMAL);
}

static struct am33xx_pm_ops am33xx_ops = {
	.init = am33xx_suspend_init,
	.pre_suspend = am33xx_pre_suspend,
	.post_suspend = am33xx_post_suspend,
};

static struct am33xx_pm_ops am43xx_ops = {
	.init = am43xx_suspend_init,
	.pre_suspend = am43xx_pre_suspend,
	.post_suspend = am43xx_post_suspend,
};
#endif /* CONFIG_SUSPEND */
#endif /* CONFIG_CPU_PM */

int __init am33xx_pm_init(void)
{
	struct powerdomain *cefuse_pwrdm;
#ifdef CONFIG_CPU_PM
	int ret;
	u32 temp;
	struct device_node *np;
#endif /* CONFIG_CPU_PM */

	if (!soc_is_am33xx() && !soc_is_am43xx())
		return -ENODEV;

#ifdef CONFIG_CPU_PM
	am33xx_pm = kzalloc(sizeof(*am33xx_pm), GFP_KERNEL);
	if (!am33xx_pm) {
		pr_err("Memory allocation failed\n");
		ret = -ENOMEM;
		return ret;
	}

	ret = am33xx_map_emif();
	if (ret) {
		pr_err("PM: Could not ioremap EMIF\n");
		goto err;
	}

#ifdef CONFIG_SUSPEND
	gfx_pwrdm = pwrdm_lookup("gfx_pwrdm");
	per_pwrdm = pwrdm_lookup("per_pwrdm");
	mpu_pwrdm = pwrdm_lookup("mpu_pwrdm");

	if ((!gfx_pwrdm) || (!per_pwrdm) || (!mpu_pwrdm)) {
		ret = -ENODEV;
		goto err;
	}

	/*
	 * Code paths for each SoC are nearly the same but set ops
	 * handle differences during init, pre-suspend, and post-suspend
	 */

	if (soc_is_am33xx())
		am33xx_pm->ops = &am33xx_ops;
	else if (soc_is_am43xx())
		am33xx_pm->ops = &am43xx_ops;

	ret = am33xx_pm->ops->init();

	if (ret)
		goto err;
#endif /* CONFIG_SUSPEND */

	/* Determine Memory Type */
	temp = readl(am33xx_emif_base + EMIF_SDRAM_CONFIG);
	temp = (temp & SDRAM_TYPE_MASK) >> SDRAM_TYPE_SHIFT;
	/* Parameters to pass to assembly code */
	susp_params.wfi_flags = 0;
	susp_params.emif_addr_virt = am33xx_emif_base;
	susp_params.dram_sync = am33xx_dram_sync;

	switch (temp) {
	case MEM_TYPE_DDR2:
		susp_params.wfi_flags |= WFI_MEM_TYPE_DDR2;
		break;
	case MEM_TYPE_DDR3:
		susp_params.wfi_flags |= WFI_MEM_TYPE_DDR3;
		break;
	}
	susp_params.wfi_flags |= WFI_SELF_REFRESH;
	susp_params.wfi_flags |= WFI_SAVE_EMIF;
	susp_params.wfi_flags |= WFI_DISABLE_EMIF;
	susp_params.wfi_flags |= WFI_WAKE_M3;

	am33xx_pm->ipc.reg4 = temp & MEM_TYPE_MASK;

	np = of_find_compatible_node(NULL, NULL, "ti,am3353-wkup-m3");
	if (np) {
		if (of_find_property(np, "ti,needs-vtt-toggle", NULL) &&
		    (!(of_property_read_u32(np, "ti,vtt-gpio-pin",
							&temp)))) {
			if (temp >= 0 && temp <= 31)
				am33xx_pm->ipc.reg4 |=
					((1 << VTT_STAT_SHIFT) |
					(temp << VTT_GPIO_PIN_SHIFT));
			else
				pr_warn("PM: Invalid VTT GPIO(%d) pin\n", temp);
		}

		if (of_find_property(np, "ti,set-io-isolation", NULL))
			am33xx_pm->ipc.reg4 |= (1 << IO_ISOLATION_STAT_SHIFT);
	}

#ifdef CONFIG_SUSPEND
	ret = am33xx_setup_sleep_sequence();
	if (ret) {
		pr_err("Error fetching I2C sleep/wake sequence\n");
		goto err;
	}
#endif /* CONFIG_SUSPEND */
#endif /* CONFIG_CPU_PM */

	(void) clkdm_for_each(omap_pm_clkdms_setup, NULL);

	/* CEFUSE domain can be turned off post bootup */
	cefuse_pwrdm = pwrdm_lookup("cefuse_pwrdm");
	if (cefuse_pwrdm)
		omap_set_pwrdm_state(cefuse_pwrdm, PWRDM_POWER_OFF);
	else
		pr_err("PM: Failed to get cefuse_pwrdm\n");

#ifdef CONFIG_CPU_PM
	am33xx_pm->state = M3_STATE_RESET;

	wkup_m3_set_ops(&am33xx_wkup_m3_ops);

	/* m3 may have already loaded but ops were not set yet,
	 * manually invoke */

	if (wkup_m3_is_valid())
		am33xx_m3_fw_ready_cb();
#endif /* CONFIG_CPU_PM */

	return 0;

#ifdef CONFIG_CPU_PM
err:
	kfree(am33xx_pm);
	return ret;
#endif /* CONFIG_CPU_PM */
}
