/* SPDX-License-Identifier: GPL-2.0-or-later */

/***************************************************************************
 *   ESP32-C3 target for OpenOCD                                           *
 *   Copyright (C) 2020 Espressif Systems Ltd.                             *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <helper/command.h>
#include <helper/bits.h>
#include <target/target.h>
#include <target/target_type.h>
#include <target/register.h>
#include <target/semihosting_common.h>
#include <target/riscv/debug_defines.h>

#include "esp_semihosting.h"
#include "esp_riscv_apptrace.h"
#include "esp_riscv.h"

#define ESP32C3_RTCCNTL_BASE                    0x60008000
#define ESP32C3_RTCCNTL_RESET_STATE_OFF         0x0038
#define ESP32C3_RTCCNTL_RESET_STATE_REG         (ESP32C3_RTCCNTL_BASE + ESP32C3_RTCCNTL_RESET_STATE_OFF)

#define ESP32C3_RTCCNTL_RESET_CAUSE_MASK        (BIT(6) - 1)
#define ESP32C3_RESET_CAUSE(reg_val)            ((reg_val) & ESP32C3_RTCCNTL_RESET_CAUSE_MASK)

/* max supported hw breakpoint and watchpoint count */
#define ESP32C3_BP_NUM                          8
#define ESP32C3_WP_NUM                          8

/* ASSIST_DEBUG registers */
#define ESP32C3_ASSIST_DEBUG_CPU0_MON_REG       0x600CE000

#define ESP32C3_IRAM_LOW    0x4037C000
#define ESP32C3_IRAM_HIGH   0x403E0000
#define ESP32C3_DRAM_LOW    0x3FC80000
#define ESP32C3_DRAM_HIGH   0x3FCE0000

enum esp32c3_reset_reason {
	ESP32C3_CHIP_POWER_ON_RESET      = 0x01,	/* Power on reset */
	ESP32C3_CHIP_BROWN_OUT_RESET     = 0x01,	/* VDD voltage is not stable and resets the chip */
	ESP32C3_CHIP_SUPER_WDT_RESET     = 0x01,	/* Super watch dog resets the chip */
	ESP32C3_CORE_SW_RESET            = 0x03,	/* Software resets the digital core by RTC_CNTL_SW_SYS_RST */
	ESP32C3_CORE_DEEP_SLEEP_RESET    = 0x05,	/* Deep sleep reset the digital core */
	ESP32C3_CORE_MWDT0_RESET         = 0x07,	/* Main watch dog 0 resets digital core */
	ESP32C3_CORE_MWDT1_RESET         = 0x08,	/* Main watch dog 1 resets digital core */
	ESP32C3_CORE_RTC_WDT_RESET       = 0x09,	/* RTC watch dog resets digital core */
	ESP32C3_CPU0_MWDT0_RESET         = 0x0B,	/* Main watch dog 0 resets CPU 0 */
	ESP32C3_CPU0_SW_RESET            = 0x0C,	/* Software resets CPU 0 by RTC_CNTL_SW_PROCPU_RST */
	ESP32C3_CPU0_RTC_WDT_RESET       = 0x0D,	/* RTC watch dog resets CPU 0 */
	ESP32C3_SYS_BROWN_OUT_RESET      = 0x0F,	/* VDD voltage is not stable and resets the digital core */
	ESP32C3_SYS_RTC_WDT_RESET        = 0x10,	/* RTC watch dog resets digital core and rtc module */
	ESP32C3_CPU0_MWDT1_RESET         = 0x11,	/* Main watch dog 1 resets CPU 0 */
	ESP32C3_SYS_SUPER_WDT_RESET      = 0x12,	/* Super watch dog resets the digital core and rtc module */
	ESP32C3_SYS_CLK_GLITCH_RESET     = 0x13,	/* Glitch on clock resets the digital core and rtc module */
	ESP32C3_CORE_EFUSE_CRC_RESET     = 0x14,	/* eFuse CRC error resets the digital core */
	ESP32C3_CORE_USB_UART_RESET      = 0x15,	/* USB UART resets the digital core */
	ESP32C3_CORE_USB_JTAG_RESET      = 0x16,	/* USB JTAG resets the digital core */
	ESP32C3_CORE_PWR_GLITCH_RESET    = 0x17,	/* Glitch on power resets the digital core */
};

static const char *esp32c3_get_reset_reason(uint32_t reset_reason_reg_val)
{
	switch (ESP32C3_RESET_CAUSE(reset_reason_reg_val)) {
	case ESP32C3_CHIP_POWER_ON_RESET:
		/* case ESP32C3_CHIP_BROWN_OUT_RESET:
		 * case ESP32C3_CHIP_SUPER_WDT_RESET: */
		return "Chip reset";
	case ESP32C3_CORE_SW_RESET:
		return "Software core reset";
	case ESP32C3_CORE_DEEP_SLEEP_RESET:
		return "Deep-sleep core reset";
	case ESP32C3_CORE_MWDT0_RESET:
		return "Main WDT0 core reset";
	case ESP32C3_CORE_MWDT1_RESET:
		return "Main WDT1 core reset";
	case ESP32C3_CORE_RTC_WDT_RESET:
		return "RTC WDT core reset";
	case ESP32C3_CPU0_MWDT0_RESET:
		return "Main WDT0 CPU reset";
	case ESP32C3_CPU0_SW_RESET:
		return "Software CPU reset";
	case ESP32C3_CPU0_RTC_WDT_RESET:
		return "RTC WDT CPU reset";
	case ESP32C3_SYS_BROWN_OUT_RESET:
		return "Brown-out core reset";
	case ESP32C3_SYS_RTC_WDT_RESET:
		return "RTC WDT core and rtc reset";
	case ESP32C3_CPU0_MWDT1_RESET:
		return "Main WDT1 CPU reset";
	case ESP32C3_SYS_SUPER_WDT_RESET:
		return "Super Watchdog core and rtc";
	case ESP32C3_SYS_CLK_GLITCH_RESET:
		return "CLK GLITCH core and rtc reset";
	case ESP32C3_CORE_EFUSE_CRC_RESET:
		return "eFuse CRC error core reset";
	case ESP32C3_CORE_USB_UART_RESET:
		return "USB (UART) core reset";
	case ESP32C3_CORE_USB_JTAG_RESET:
		return "USB (JTAG) core reset";
	case ESP32C3_CORE_PWR_GLITCH_RESET:
		return "Power glitch core reset";
	}

	return "Unknown reset cause";
}

static void esp32c3_print_reset_reason(struct target *target, uint32_t reset_reason_reg_val)
{
	LOG_TARGET_INFO(target, "Reset cause (%ld) - (%s)",
		ESP32C3_RESET_CAUSE(reset_reason_reg_val),
		esp32c3_get_reset_reason(reset_reason_reg_val));
}

static bool esp32c3_is_iram_address(target_addr_t addr)
{
	return addr >= ESP32C3_IRAM_LOW && addr < ESP32C3_IRAM_HIGH;
}

static bool esp32c3_is_dram_address(target_addr_t addr)
{
	return addr >= ESP32C3_DRAM_LOW && addr < ESP32C3_DRAM_HIGH;
}

static const struct esp_semihost_ops esp32c3_semihost_ops = {
	.prepare = NULL,
	.post_reset = esp_semihosting_post_reset
};

static const struct esp_flash_breakpoint_ops esp32c3_flash_brp_ops = {
	.breakpoint_add = esp_algo_flash_breakpoint_add,
	.breakpoint_remove = esp_algo_flash_breakpoint_remove
};

static int esp32c3_target_create(struct target *target, Jim_Interp *interp)
{
	struct esp_riscv_common *esp_riscv = calloc(1, sizeof(*esp_riscv));
	if (!esp_riscv)
		return ERROR_FAIL;

	target->arch_info = esp_riscv;

	esp_riscv->assist_debug_cpu0_mon_reg = ESP32C3_ASSIST_DEBUG_CPU0_MON_REG;
	esp_riscv->assist_debug_cpu_offset = 0;

	esp_riscv->max_bp_num = ESP32C3_BP_NUM;
	esp_riscv->max_wp_num = ESP32C3_WP_NUM;

	esp_riscv->rtccntl_reset_state_reg = ESP32C3_RTCCNTL_RESET_STATE_REG;
	esp_riscv->print_reset_reason = &esp32c3_print_reset_reason;
	esp_riscv->existent_csrs = NULL;
	esp_riscv->existent_csr_size = 0;
	esp_riscv->is_dram_address = esp32c3_is_dram_address;
	esp_riscv->is_iram_address = esp32c3_is_iram_address;

	if (esp_riscv_alloc_trigger_addr(target) != ERROR_OK)
		return ERROR_FAIL;

	riscv_info_init(target, &esp_riscv->riscv);

	return ERROR_OK;
}

static int esp32c3_init_target(struct command_context *cmd_ctx,
	struct target *target)
{
	int ret = riscv_target.init_target(cmd_ctx, target);
	if (ret != ERROR_OK)
		return ret;

	target->semihosting->user_command_extension = esp_semihosting_common;

	struct esp_riscv_common *esp_riscv = target_to_esp_riscv(target);

	ret = esp_riscv_init_arch_info(cmd_ctx,
		target,
		esp_riscv,
		&esp32c3_flash_brp_ops,
		&esp32c3_semihost_ops);
	if (ret != ERROR_OK)
		return ret;

	return ERROR_OK;
}

static const struct command_registration esp32c3_command_handlers[] = {
	{
		.usage = "",
		.chain = riscv_command_handlers,
	},
	{
		.name = "esp",
		.usage = "",
		.chain = esp_riscv_command_handlers,
	},
	{
		.name = "esp",
		.usage = "",
		.chain = esp32_apptrace_command_handlers,
	},
	COMMAND_REGISTRATION_DONE
};

struct target_type esp32c3_target = {
	.name = "esp32c3",

	.target_create = esp32c3_target_create,
	.init_target = esp32c3_init_target,
	.deinit_target = esp_riscv_deinit_target,
	.examine = esp_riscv_examine,

	/* poll current target status */
	.poll = esp_riscv_poll,

	.halt = riscv_halt,
	.resume = esp_riscv_resume,
	.step = riscv_openocd_step,

	.assert_reset = riscv_assert_reset,
	.deassert_reset = riscv_deassert_reset,

	.read_memory = esp_riscv_read_memory,
	.write_memory = esp_riscv_write_memory,

	.checksum_memory = riscv_checksum_memory,

	.get_gdb_arch = riscv_get_gdb_arch,
	.get_gdb_reg_list = riscv_get_gdb_reg_list,
	.get_gdb_reg_list_noread = riscv_get_gdb_reg_list_noread,

	.add_breakpoint = esp_riscv_breakpoint_add,
	.remove_breakpoint = esp_riscv_breakpoint_remove,

	.add_watchpoint = riscv_add_watchpoint,
	.remove_watchpoint = riscv_remove_watchpoint,
	.hit_watchpoint = esp_riscv_hit_watchpoint,

	.arch_state = riscv_arch_state,

	.run_algorithm = esp_riscv_run_algorithm,
	.start_algorithm = esp_riscv_start_algorithm,
	.wait_algorithm = esp_riscv_wait_algorithm,

	.commands = esp32c3_command_handlers,

	.address_bits = riscv_xlen_nonconst,
};
