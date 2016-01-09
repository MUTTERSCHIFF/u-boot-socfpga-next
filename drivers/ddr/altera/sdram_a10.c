/*
 * Copyright (C) 2014 Altera Corporation <www.altera.com>
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

#include <common.h>
#include <fdtdec.h>
#include <malloc.h>
#include <mmc.h>
#include <watchdog.h>
#include <ns16550.h>
#include <asm/io.h>
#include <asm/arch/cff.h>
#include <asm/arch/misc.h>
#include <asm/arch/reset_manager.h>
#include <asm/arch/fpga_manager.h>
#include <asm/arch/sdram_a10.h>
#include <asm/arch/system_manager.h>


DECLARE_GLOBAL_DATA_PTR;

/* FAWBANK - Number of Bank of a given device involved in the FAW period. */
#define ARRIA10_SDR_ACTIVATE_FAWBANK	(0x1)

#define ARRIA10_EMIF_RST	31	/* fpga_mgr_fpgamgrregs.gpo.31 */
#define ARRIA10_OCT_CAL_REQ	30	/* fpga_mgr_fpgamgrregs.gpo.30 */
#define ARRIA10_OCT_CAL_ACK	31	/* fpga_mgr_fpgamgrregs.gpi.31 */

#define ARRIA10_NIOS_OCT_DONE	7
#define ARRIA10_NIOS_OCT_ACK	7

#define DDR_EMIF_DANCE_VER	0x00010001

#define DDR_REG_SEQ2CORE        0xFFD0507C
#define DDR_REG_CORE2SEQ        0xFFD05078
#define DDR_REG_GPOUT           0xFFD03010
#define DDR_REG_GPIN            0xFFD03014

static const struct socfpga_ecc_hmc *socfpga_ecc_hmc_base =
		(void *)SOCFPGA_SDR_ADDRESS;
static const struct socfpga_noc_ddr_scheduler *socfpga_noc_ddr_scheduler_base =
		(void *)SOCFPGA_SDR_SCHEDULER_ADDRESS;
static const struct socfpga_noc_fw_ddr_mpu_fpga2sdram
		*socfpga_noc_fw_ddr_mpu_fpga2sdram_base =
		(void *)SOCFPGA_SDR_FIREWALL_MPU_FPGA_ADDRESS;
static const struct socfpga_noc_fw_ddr_l3 *socfpga_noc_fw_ddr_l3_base =
		(void *)SOCFPGA_SDR_FIREWALL_L3_ADDRESS;
static const struct socfpga_system_manager *socfpga_system_mgr =
		(void *)SOCFPGA_SYSMGR_ADDRESS;
static const struct socfpga_io48_mmr *socfpga_io48_mmr_base =
		(void *)SOCFPGA_HMC_MMR_IO48_ADDRESS;

#define ARRIA10_DDR_CONFIG(A, B, C, R)	((A << 24) | (B << 16) | (C << 8) | R)
/* The followring are the supported configurations */
u32 ddr_config[] = {
	0,	/* Dummy element to simplify indexing */
	/* Chip - Row - Bank - Column Style */
	/* All Types */
	ARRIA10_DDR_CONFIG(0, 3, 10, 12),
	ARRIA10_DDR_CONFIG(0, 3, 10, 13),
	ARRIA10_DDR_CONFIG(0, 3, 10, 14),
	ARRIA10_DDR_CONFIG(0, 3, 10, 15),
	ARRIA10_DDR_CONFIG(0, 3, 10, 16),
	ARRIA10_DDR_CONFIG(0, 3, 10, 17),
	/* LPDDR x16 */
	ARRIA10_DDR_CONFIG(0, 3, 11, 14),
	ARRIA10_DDR_CONFIG(0, 3, 11, 15),
	ARRIA10_DDR_CONFIG(0, 3, 11, 16),
	ARRIA10_DDR_CONFIG(0, 3, 12, 15),
	/* DDR4 Only */
	ARRIA10_DDR_CONFIG(0, 4, 10, 14),
	ARRIA10_DDR_CONFIG(0, 4, 10, 15),
	ARRIA10_DDR_CONFIG(0, 4, 10, 16),
	ARRIA10_DDR_CONFIG(0, 4, 10, 17),	/* 14 */
	/* Chip - Bank - Row - Column Style */
	ARRIA10_DDR_CONFIG(1, 3, 10, 12),
	ARRIA10_DDR_CONFIG(1, 3, 10, 13),
	ARRIA10_DDR_CONFIG(1, 3, 10, 14),
	ARRIA10_DDR_CONFIG(1, 3, 10, 15),
	ARRIA10_DDR_CONFIG(1, 3, 10, 16),
	ARRIA10_DDR_CONFIG(1, 3, 10, 17),
	ARRIA10_DDR_CONFIG(1, 3, 11, 14),
	ARRIA10_DDR_CONFIG(1, 3, 11, 15),
	ARRIA10_DDR_CONFIG(1, 3, 11, 16),
	ARRIA10_DDR_CONFIG(1, 3, 12, 15),
	/* DDR4 Only */
	ARRIA10_DDR_CONFIG(1, 4, 10, 14),
	ARRIA10_DDR_CONFIG(1, 4, 10, 15),
	ARRIA10_DDR_CONFIG(1, 4, 10, 16),
	ARRIA10_DDR_CONFIG(1, 4, 10, 17),
};

static int match_ddr_conf(u32 ddr_conf)
{
	int i;

	for (i = 0; i < (sizeof(ddr_config)/4); i++) {
		if (ddr_conf == ddr_config[i])
			return i;
	}
	return 0;
}

/* Check whether SDRAM is successfully Calibrated */
int is_sdram_cal_success(void)
{
	return readl(&socfpga_ecc_hmc_base->ddrcalstat);
}

unsigned char ddr_get_bit(u32 ereg, unsigned char bit)
{
	unsigned int reg = readl(ereg);

	return (reg & (1 << bit)) ? 1 : 0;
}

unsigned char ddr_wait_bit(u32 ereg, u32 bit,
			   u32 expected, u32 timeout_usec)
{
	unsigned int tmr;

	for (tmr = 0; tmr < timeout_usec; tmr += 100) {
		udelay(100);
		WATCHDOG_RESET();
		if (ddr_get_bit(ereg, bit) == expected)
			return 0;
	}

	return 1;
}

void ddr_set_bit(u32 ereg, u32 bit)
{
	unsigned int tmp = readl(ereg);

	tmp |= (1 << bit);
	writel(tmp, ereg);
}

void ddr_clr_bit(u32 ereg, u32 bit)
{
	unsigned int tmp = readl(ereg);

	tmp &= ~(1 << bit);
	writel(tmp, ereg);
}

void ddr_delay(u32 delay)
{
	int tmr;

	for (tmr = 0; tmr < delay; tmr++) {
		udelay(1000);
		WATCHDOG_RESET();
	}
}

/*
 * Diagram of OCT Workaround:
 *
 * EMIF Core                     HPS Processor              OCT FSM
 * =================================================================
 *
 * seq2core      ==============>
 * [0x?????????]   OCT Request   [0xFFD0507C]
 *
 * core2seq
 * [0x?????????] <==============
 *                 OCT Ready     [0xFFD05078]
 *
 *                               [0xFFD03010] ============> Request
 *                                             OCT Request
 *
 *                               [0xFFD03014] <============ Ready
 *                                              OCT Ready
 * Signal definitions:
 *
 * seq2core[7] - OCT calibration request (act-high)
 * core2seq[7] - Signals OCT FSM is ready (active high)
 * gpout[31]   - EMIF Reset override (active low)
 * gpout[30]   - OCT calibration request (act-high)
 * gpin[31]    - OCT calibration ready (act-high)
 */

int ddr_calibration(void)
{
	ddr_delay(500);
	/* Step 1 - Initiating Reset Sequence */
	ddr_clr_bit(DDR_REG_GPOUT, ARRIA10_EMIF_RST);	/* Reset EMIF */
	ddr_delay(10);

	/* Step 2 - Clearing registers to EMIF core */
	writel(0, DDR_REG_CORE2SEQ);	/*Clear the HPS->NIOS COM reg.*/

	/* Step 3 - Clearing registers to OCT core */
	ddr_clr_bit(DDR_REG_GPOUT, ARRIA10_OCT_CAL_REQ); /* OCT Cal Request */
	ddr_delay(5);

	/* Step 4 - Taking EMIF out of reset */
	ddr_set_bit(DDR_REG_GPOUT, ARRIA10_EMIF_RST);
	ddr_delay(10);

	/* Step 5 - Waiting for OCT circuitry to come out of reset */
	if (ddr_wait_bit(DDR_REG_GPIN, ARRIA10_OCT_CAL_ACK, 1, 1000000))
		return -1;

	/* Step 6 - Allowing EMIF to proceed with OCT calibration */
	ddr_set_bit(DDR_REG_CORE2SEQ, ARRIA10_NIOS_OCT_DONE);

	/* Step 7 - Waiting for EMIF request */
	if (ddr_wait_bit(DDR_REG_SEQ2CORE, ARRIA10_NIOS_OCT_ACK, 1, 2000000))
		return -2;

	/* Step 8 - Acknowledging EMIF OCT request */
	ddr_clr_bit(DDR_REG_CORE2SEQ, ARRIA10_NIOS_OCT_DONE);

	/* Step 9 - Waiting for EMIF response */
	if (ddr_wait_bit(DDR_REG_SEQ2CORE, ARRIA10_NIOS_OCT_ACK, 0, 2000000))
		return -3;

	/* Step 10 - Triggering OCT Calibration */
	ddr_set_bit(DDR_REG_GPOUT, ARRIA10_OCT_CAL_REQ);

	/* Step 11 - Waiting for OCT response */
	if (ddr_wait_bit(DDR_REG_GPIN, ARRIA10_OCT_CAL_ACK, 0, 1000))
		return -4;

	/* Step 12 - Clearing OCT Request bit */
	ddr_clr_bit(DDR_REG_GPOUT, ARRIA10_OCT_CAL_REQ);

	/* Step 13 - Waiting for OCT Engine */
	if (ddr_wait_bit(DDR_REG_GPIN, ARRIA10_OCT_CAL_ACK, 1, 200000))
		return -5;

	/* Step 14 - Proceeding with EMIF calibration */
	ddr_set_bit(DDR_REG_CORE2SEQ, ARRIA10_NIOS_OCT_DONE);

	ddr_delay(100);

	return 0;
}

int ddr_setup_workaround(void)
{
	int i, j, retcode, ddr_setup_complete = 0;
	int chip_version = readl(&socfpga_system_mgr->siliconid1);

	/* Version check - only the initial silicon needs this */
	if (chip_version !=  DDR_EMIF_DANCE_VER)
		return 0;

	/* Try 3 times to do a calibration */
	for (i = 0; (i < 3) && !ddr_setup_complete; i++) {
		WATCHDOG_RESET();

		retcode = ddr_calibration();
		if (retcode) {
			printf("DDRCAL: Failure: %d\n", retcode);
			continue;
		}

		/* A delay to wait for calibration bit to set */
		for (j = 0; (j < 10) && !ddr_setup_complete; j++) {
			ddr_delay(500);
			ddr_setup_complete = is_sdram_cal_success();
		}

		if (!ddr_setup_complete)
			puts("DDRCAL: Retry\n");
	}

	if (!ddr_setup_complete) {
		puts("Error: Could Not Calibrate SDRAM\n");
		return -1;
	}

	return 0;
}

unsigned long irq_cnt_ecc_sdram;

/* Enable and disable SDRAM interrupt */
void sdram_enable_interrupt(unsigned enable)
{
	/* Clear the internal counter (write 1 to clear) */
	setbits_le32(&socfpga_ecc_hmc_base->eccctrl,
		     ALT_ECC_HMC_OCP_ECCCTL_CNT_RST_SET_MSK);

	/* clear the ECC prior enable or even disable (write 1 to clear) */
	setbits_le32(&socfpga_ecc_hmc_base->intstat,
		     ALT_ECC_HMC_OCP_INTSTAT_SERRPENA_SET_MSK |
		     ALT_ECC_HMC_OCP_INTSTAT_DERRPENA_SET_MSK);

	/*
	 * We want the serr trigger after a number of count instead of
	 * triggered every single bit error event which cost cpu time
	 */
	writel(ALT_ECC_HMC_OCP_SERRCNTREG_VALUE,
	       &socfpga_ecc_hmc_base->serrcntreg);

	/* Enable the interrupt on compare */
	setbits_le32(&socfpga_ecc_hmc_base->intmode,
		     ALT_ECC_HMC_OCP_INTMOD_INTONCMP_SET_MSK);

	if (enable)
		writel(ALT_ECC_HMC_OCP_ERRINTEN_SERRINTEN_SET_MSK |
		       ALT_ECC_HMC_OCP_ERRINTEN_DERRINTEN_SET_MSK,
		       &socfpga_ecc_hmc_base->errintens);
	else
		writel(ALT_ECC_HMC_OCP_ERRINTEN_SERRINTEN_SET_MSK |
		       ALT_ECC_HMC_OCP_ERRINTEN_DERRINTEN_SET_MSK,
		       &socfpga_ecc_hmc_base->errintenr);
}

/* handler for SDRAM ECC interrupt */
void irq_handler_ecc_sdram(void *arg)
{
	unsigned reg_value;

	/* check whether SBE happen */
	reg_value = readl(&socfpga_ecc_hmc_base->intstat);
	if (reg_value & ALT_ECC_HMC_OCP_INTSTAT_SERRPENA_SET_MSK) {
		printf("Info: SDRAM ECC SBE @ 0x%08x\n",
		       readl(&socfpga_ecc_hmc_base->serraddra));
		irq_cnt_ecc_sdram += readl(&socfpga_ecc_hmc_base->serrcntreg);
		setenv_ulong("sdram_ecc_sbe", irq_cnt_ecc_sdram);
	}

	/* check whether DBE happen */
	if (reg_value & ALT_ECC_HMC_OCP_ERRINTEN_DERRINTEN_SET_MSK) {
		puts("Error: SDRAM ECC DBE occurred\n");
		printf("sbecount = %lu\n", irq_cnt_ecc_sdram);
		printf("erraddr = %08x\n",
		       readl(&socfpga_ecc_hmc_base->derraddra));
	}

	/* Clear the internal counter (write 1 to clear) */
	setbits_le32(&socfpga_ecc_hmc_base->eccctrl,
		     ALT_ECC_HMC_OCP_ECCCTL_CNT_RST_SET_MSK);

	/* clear the ECC prior enable or even disable (write 1 to clear) */
	setbits_le32(&socfpga_ecc_hmc_base->intstat,
		     ALT_ECC_HMC_OCP_INTSTAT_SERRPENA_SET_MSK |
		     ALT_ECC_HMC_OCP_INTSTAT_DERRPENA_SET_MSK);

	/* if DBE, going into hang */
	if (reg_value & ALT_ECC_HMC_OCP_ERRINTEN_DERRINTEN_SET_MSK) {
		sdram_enable_interrupt(0);
		hang();
	}
}

/* Function to startup the SDRAM*/
int sdram_startup(void)
{
	/* Release NOC ddr scheduler from reset */
	reset_deassert_noc_ddr_scheduler();

	/* Bringup Workaround */
	return ddr_setup_workaround();
}

u32 sdram_size_calc(void)
{
	union dramaddrw_reg dramaddrw =
		(union dramaddrw_reg)readl(&socfpga_io48_mmr_base->dramaddrw);

	u32 size = (1 << (dramaddrw.cfg_cs_addr_width +
		    dramaddrw.cfg_bank_group_addr_width +
		    dramaddrw.cfg_bank_addr_width +
		    dramaddrw.cfg_row_addr_width +
		    dramaddrw.cfg_col_addr_width));

	size *= (2 << (readl(&socfpga_ecc_hmc_base->ddrioctrl) &
		       ALT_ECC_HMC_OCP_DDRIOCTRL_IO_SIZE_MSK));

	return size;
}

/* Function to initialize SDRAM MMR and NOC DDR scheduler*/
void sdram_mmr_init(void)
{
	u32 update_value, io48_value;
	u32 ctrlcfg0_reg = readl(&socfpga_io48_mmr_base->ctrlcfg0);
	u32 ctrlcfg1_reg = readl(&socfpga_io48_mmr_base->ctrlcfg1);
	u32 dramaddrw_reg = readl(&socfpga_io48_mmr_base->dramaddrw);
	u32 caltiming0_reg = readl(&socfpga_io48_mmr_base->caltiming0);
	u32 caltiming1_reg = readl(&socfpga_io48_mmr_base->caltiming1);
	u32 caltiming2_reg = readl(&socfpga_io48_mmr_base->caltiming2);
	u32 caltiming3_reg = readl(&socfpga_io48_mmr_base->caltiming3);
	u32 caltiming4_reg = readl(&socfpga_io48_mmr_base->caltiming4);
	u32 caltiming9_reg = readl(&socfpga_io48_mmr_base->caltiming9);
	u32 ddrioctl;

	/*
	 * Configure the DDR IO size [0xFFCFB008]
	 * niosreserve0: Used to indicate DDR width &
	 *	bit[7:0] = Number of data bits (0x20 for 32bit)
	 *	bit[8]   = 1 if user-mode OCT is present
	 *	bit[9]   = 1 if warm reset compiled into EMIF Cal Code
	 *	bit[10]  = 1 if warm reset is on during generation in EMIF Cal
	 * niosreserve1: IP ADCDS version encoded as 16 bit value
	 *	bit[2:0] = Variant (0=not special,1=FAE beta, 2=Customer beta,
	 *			    3=EAP, 4-6 are reserved)
	 *	bit[5:3] = Service Pack # (e.g. 1)
	 *	bit[9:6] = Minor Release #
	 *	bit[14:10] = Major Release #
	 */
	if ((socfpga_io48_mmr_base->niosreserve1 >> 6) & 0x1FF) {
		update_value = readl(&socfpga_io48_mmr_base->niosreserve0);
		writel(((update_value & 0xFF) >> 5),
		       &socfpga_ecc_hmc_base->ddrioctrl);
	}

	ddrioctl = readl(&socfpga_ecc_hmc_base->ddrioctrl);

	/* Enable or disable the SDRAM ECC */
	if (ctrlcfg1_reg & IO48_MMR_CTRLCFG1_CTRL_ENABLE_ECC) {
		setbits_le32(&socfpga_ecc_hmc_base->eccctrl,
			     (ALT_ECC_HMC_OCP_ECCCTL_AWB_CNT_RST_SET_MSK |
			      ALT_ECC_HMC_OCP_ECCCTL_CNT_RST_SET_MSK |
			      ALT_ECC_HMC_OCP_ECCCTL_ECC_EN_SET_MSK));
		clrbits_le32(&socfpga_ecc_hmc_base->eccctrl,
			     (ALT_ECC_HMC_OCP_ECCCTL_AWB_CNT_RST_SET_MSK |
			      ALT_ECC_HMC_OCP_ECCCTL_CNT_RST_SET_MSK));
		setbits_le32(&socfpga_ecc_hmc_base->eccctrl2,
			     (ALT_ECC_HMC_OCP_ECCCTL2_RMW_EN_SET_MSK |
			      ALT_ECC_HMC_OCP_ECCCTL2_AWB_EN_SET_MSK));
	} else {
		clrbits_le32(&socfpga_ecc_hmc_base->eccctrl,
			     (ALT_ECC_HMC_OCP_ECCCTL_AWB_CNT_RST_SET_MSK |
			      ALT_ECC_HMC_OCP_ECCCTL_CNT_RST_SET_MSK |
			      ALT_ECC_HMC_OCP_ECCCTL_ECC_EN_SET_MSK));
		clrbits_le32(&socfpga_ecc_hmc_base->eccctrl2,
			     (ALT_ECC_HMC_OCP_ECCCTL2_RMW_EN_SET_MSK |
			      ALT_ECC_HMC_OCP_ECCCTL2_AWB_EN_SET_MSK));
	}

	/* Set the DDR Configuration [0xFFD12400] */
	cfg_addr_order = (ctrlcfg1_reg & ADDR_ORDER_MASK) >> ADDR_ORDER_SHIFT;
	cfg_bank_addr_width = (dramaddrw_reg & CFG_BANK_ADDR_WIDTH_MASK) >>
			       CFG_BANK_ADDR_WIDTH_SHIFT;
	cfg_bank_group_addr_width = (dramaddrw_reg & CFG_BANK_GROUP_ADDR_WIDTH_MASK) >>
				     CFG_BANK_GROUP_ADDR_WIDTH_SHIFT;
	cfg_col_addr_width = (dramaddrw_reg & CFG_COL_ADDR_WIDTH_MASK);
	cfg_row_addr_width = (dramaddrw_reg & CFG_ROW_ADDR_WIDTH_MASK) >>
			      CFG_ROW_ADDR_WIDTH_SHIFT;

	io48_value = ARRIA10_DDR_CONFIG(cfg_addr_order,
					(cfg_bank_addr_width +
					cfg_bank_group_addr_width),
					cfg_col_addr_width,
					cfg_row_addr_width);

	update_value = match_ddr_conf(io48_value);
	if (update_value)
		writel(update_value,
		&socfpga_noc_ddr_scheduler_base->ddr_t_main_scheduler_ddrconf);

	/*
	 * Configure DDR timing [0xFFD1240C]
	 *  RDTOMISS = tRTP + tRP + tRCD - BL/2/
	 *  WRTOMISS = WL + tWR + tRP + tRCD and
	 *    WL = RL + BL/2 + 2 - rd-to-wr ; tWR = 15ns  so...
	 *  First part of equation is in memory clock units so divide by 2
	 *  for HMC clock units. 1066MHz is close to 1ns so use 15 directly.
	 *  WRTOMISS = ((RL + BL/2 + 2 + tWR) >> 1)- rd-to-wr + tRP + tRCD
	 */
	cfg_rd_to_pch = (caltim2_reg & CFG_RD_TO_PCH_MASK) >>
			 CFG_RD_TO_PCH_SHIFT;
	cfg_pch_to_valid = (caltim4_reg & CFG_PCH_TO_VALID_MASK) >>
			    CFG_PCH_TO_VALID_SHIFT;
	cfg_act_to_rdwr = (caltim0_reg & CFG_ACT_TO_RDWR_MASK) >>
			   CFG_ACT_TO_RDWR_MASK;
	cfg_ctrl_burst_len = (ctrlcfg0_reg & CTRL_BURST_LENGTH_MASK) >>
			      CTRL_BURST_LENGTH_SHIFT;
	update_value = (cfg_rd_to_pch + caltim4.cfg_pch_to_valid +
			cfg_act_to_rdwr - (cfg_ctrl_burst_len >> 2));

	cfg_rd_to_wr = (caltim1_reg & CALTIMING1_CFG_RD_TO_RD_MASK);
	io48_value = ((((socfpga_io48_mmr_base->dramtiming0 &
		      DRAMTIME_MEM_READ_LATENCY_MASK) + 2 + 15 +
		      (cfg_ctrl_burst_len >> 1)) >> 1) -
		      /* Up to here was in memory cycles so divide by 2 */
		      cfg_rd_to_wr + cfg_act_to_rdwr + cfg_pch_to_valid);

	cfg_act_to_act = (caltim0_reg & CFG_ACT_TO_ACT_MASK) >>
			  CFG_ACT_TO_ACT_SHIFT;
	cfg_wr_to_rd = (caltim3_reg & CFG_WR_TO_RD_MASK) >> CFG_WR_TO_RD_SHIFT;
	writel(((caltim0.cfg_act_to_act <<
			ALT_NOC_MPU_DDR_T_SCHED_DDRTIMING_ACTTOACT_LSB) |
		(update_value <<
			ALT_NOC_MPU_DDR_T_SCHED_DDRTIMING_RDTOMISS_LSB) |
		(io48_value <<
			ALT_NOC_MPU_DDR_T_SCHED_DDRTIMING_WRTOMISS_LSB) |
		((cfg_ctrl_burst_len >> 2) <<
			ALT_NOC_MPU_DDR_T_SCHED_DDRTIMING_BURSTLEN_LSB) |
		(cfg_rd_to_wr <<
			ALT_NOC_MPU_DDR_T_SCHED_DDRTIMING_RDTOWR_LSB) |
		(cfg_wr_to_rd <<
			ALT_NOC_MPU_DDR_T_SCHED_DDRTIMING_WRTORD_LSB) |
		(((ddrioctl == 1) ? 1 : 0) <<
			ALT_NOC_MPU_DDR_T_SCHED_DDRTIMING_BWRATIO_LSB)),
		&socfpga_noc_ddr_scheduler_base->
			ddr_t_main_scheduler_ddrtiming);

	/* Configure DDR mode [0xFFD12410] [precharge = 0] */
	writel(((ddrioctl ? 0 : 1) <<
		ALT_NOC_MPU_DDR_T_SCHED_DDRMOD_BWRATIOEXTENDED_LSB),
		&socfpga_noc_ddr_scheduler_base->ddr_t_main_scheduler_ddrmode);

	/* Configure the read latency [0xFFD12414] */
	writel(((socfpga_io48_mmr_base->dramtiming0 &
		ALT_IO48_DRAMTIME_MEM_READ_LATENCY_MASK) >> 1),
		&socfpga_noc_ddr_scheduler_base->
			ddr_t_main_scheduler_readlatency);

	/*
	 * Configuring timing values concerning activate commands
	 * [0xFFD12438] [FAWBANK alway 1 because always 4 bank DDR]
	 */
	cfg_act_to_act_db = (caltim0_reg & CFG_ACT_TO_ACT_DIFF_BG_MASK) >>
			     CFG_ACT_TO_ACT_DIFF_BG_SHIFT;
	cfg_4_act_to_act = (caltim9_reg & CFG_WR_4_ACT_TO_ACT_MASK);
	writel(((cfg_act_to_act_db <<
			ALT_NOC_MPU_DDR_T_SCHED_ACTIVATE_RRD_LSB) |
		(cfg_4_act_to_act <<
			ALT_NOC_MPU_DDR_T_SCHED_ACTIVATE_FAW_LSB) |
		(ARRIA10_SDR_ACTIVATE_FAWBANK <<
			ALT_NOC_MPU_DDR_T_SCHED_ACTIVATE_FAWBANK_LSB)),
		&socfpga_noc_ddr_scheduler_base->ddr_t_main_scheduler_activate);

	/*
	 * Configuring timing values concerning device to device data bus
	 * ownership change [0xFFD1243C]
	 */
	cfg_rd_to_rd_dc = (caltim1_reg & CFG_RD_TO_RD_DC_MASK) >>
			   CFG_RD_TO_RD_DC_SHIFT;
	cfg_rd_to_wr_dc = (caltim1_reg & CFG_RD_TO_WR_DC_MASK) >>
			   CFG_RD_TO_WR_DC_SHIFT;
	cfg_wr_to_rd_dc = (caltim3_reg & CFG_WR_TO_RD_DC_MASK) >>
			   CFG_WR_TO_RD_DC_SHIFT;
	writel(((cfg_rd_to_rd_dc <<
			ALT_NOC_MPU_DDR_T_SCHED_DEVTODEV_BUSRDTORD_LSB) |
		(cfg_rd_to_wr_dc <<
			ALT_NOC_MPU_DDR_T_SCHED_DEVTODEV_BUSRDTOWR_LSB) |
		(cfg_wr_to_rd_dc <<
			ALT_NOC_MPU_DDR_T_SCHED_DEVTODEV_BUSWRTORD_LSB)),
		&socfpga_noc_ddr_scheduler_base->ddr_t_main_scheduler_devtodev);
}

struct firewall_entry {
	const char *prop_name;
	const u32 cfg_addr;
	const u32 en_addr;
	const u32 en_bit;
};
#define FW_MPU_FPGA_ADDRESS \
	((const struct socfpga_noc_fw_ddr_mpu_fpga2sdram *)\
	SOCFPGA_SDR_FIREWALL_MPU_FPGA_ADDRESS)
const struct firewall_entry firewall_table[] = {
	{
		"mpu0",
		SOCFPGA_SDR_FIREWALL_MPU_FPGA_ADDRESS +
		offsetof(struct socfpga_noc_fw_ddr_mpu_fpga2sdram,
			 mpuregion0addr),
		SOCFPGA_SDR_FIREWALL_MPU_FPGA_ADDRESS +
		offsetof(struct socfpga_noc_fw_ddr_mpu_fpga2sdram,
			 enable),
		ALT_NOC_FW_DDR_SCR_EN_MPUREG0EN_SET_MSK
	},
	{
		"mpu1",
		SOCFPGA_SDR_FIREWALL_MPU_FPGA_ADDRESS +
		offsetof(struct socfpga_noc_fw_ddr_mpu_fpga2sdram,
			 mpuregion1addr),
		SOCFPGA_SDR_FIREWALL_MPU_FPGA_ADDRESS +
		offsetof(struct socfpga_noc_fw_ddr_mpu_fpga2sdram,
			 enable),
		ALT_NOC_FW_DDR_SCR_EN_MPUREG1EN_SET_MSK
	},
	{
		"mpu2",
		SOCFPGA_SDR_FIREWALL_MPU_FPGA_ADDRESS +
		offsetof(struct socfpga_noc_fw_ddr_mpu_fpga2sdram,
			 mpuregion2addr),
		SOCFPGA_SDR_FIREWALL_MPU_FPGA_ADDRESS +
		offsetof(struct socfpga_noc_fw_ddr_mpu_fpga2sdram,
			 enable),
		ALT_NOC_FW_DDR_SCR_EN_MPUREG2EN_SET_MSK
	},
	{
		"mpu3",
		SOCFPGA_SDR_FIREWALL_MPU_FPGA_ADDRESS +
		offsetof(struct socfpga_noc_fw_ddr_mpu_fpga2sdram,
			 mpuregion3addr),
		SOCFPGA_SDR_FIREWALL_MPU_FPGA_ADDRESS +
		offsetof(struct socfpga_noc_fw_ddr_mpu_fpga2sdram,
			 enable),
		ALT_NOC_FW_DDR_SCR_EN_MPUREG3EN_SET_MSK
	},
	{
		"l3-0",
		SOCFPGA_SDR_FIREWALL_L3_ADDRESS +
			offsetof(struct socfpga_noc_fw_ddr_l3, hpsregion0addr),
		SOCFPGA_SDR_FIREWALL_L3_ADDRESS +
			offsetof(struct socfpga_noc_fw_ddr_l3, enable),
		ALT_NOC_FW_DDR_SCR_EN_HPSREG0EN_SET_MSK
	},
	{
		"l3-1",
		SOCFPGA_SDR_FIREWALL_L3_ADDRESS +
			offsetof(struct socfpga_noc_fw_ddr_l3, hpsregion1addr),
		SOCFPGA_SDR_FIREWALL_L3_ADDRESS +
			offsetof(struct socfpga_noc_fw_ddr_l3, enable),
		ALT_NOC_FW_DDR_SCR_EN_HPSREG1EN_SET_MSK
	},
	{
		"l3-2",
		SOCFPGA_SDR_FIREWALL_L3_ADDRESS +
			offsetof(struct socfpga_noc_fw_ddr_l3, hpsregion2addr),
		SOCFPGA_SDR_FIREWALL_L3_ADDRESS +
			offsetof(struct socfpga_noc_fw_ddr_l3, enable),
		ALT_NOC_FW_DDR_SCR_EN_HPSREG2EN_SET_MSK
	},
	{
		"l3-3",
		SOCFPGA_SDR_FIREWALL_L3_ADDRESS +
			offsetof(struct socfpga_noc_fw_ddr_l3, hpsregion3addr),
		SOCFPGA_SDR_FIREWALL_L3_ADDRESS +
			offsetof(struct socfpga_noc_fw_ddr_l3, enable),
		ALT_NOC_FW_DDR_SCR_EN_HPSREG3EN_SET_MSK
	},
	{
		"l3-4",
		SOCFPGA_SDR_FIREWALL_L3_ADDRESS +
			offsetof(struct socfpga_noc_fw_ddr_l3, hpsregion4addr),
		SOCFPGA_SDR_FIREWALL_L3_ADDRESS +
			offsetof(struct socfpga_noc_fw_ddr_l3, enable),
		ALT_NOC_FW_DDR_SCR_EN_HPSREG4EN_SET_MSK
	},
	{
		"l3-5",
		SOCFPGA_SDR_FIREWALL_L3_ADDRESS +
			offsetof(struct socfpga_noc_fw_ddr_l3, hpsregion5addr),
		SOCFPGA_SDR_FIREWALL_L3_ADDRESS +
			offsetof(struct socfpga_noc_fw_ddr_l3, enable),
		ALT_NOC_FW_DDR_SCR_EN_HPSREG5EN_SET_MSK
	},
	{
		"l3-6",
		SOCFPGA_SDR_FIREWALL_L3_ADDRESS +
			offsetof(struct socfpga_noc_fw_ddr_l3, hpsregion6addr),
		SOCFPGA_SDR_FIREWALL_L3_ADDRESS +
			offsetof(struct socfpga_noc_fw_ddr_l3, enable),
		ALT_NOC_FW_DDR_SCR_EN_HPSREG6EN_SET_MSK
	},
	{
		"l3-7",
		SOCFPGA_SDR_FIREWALL_L3_ADDRESS +
			offsetof(struct socfpga_noc_fw_ddr_l3, hpsregion7addr),
		SOCFPGA_SDR_FIREWALL_L3_ADDRESS +
			offsetof(struct socfpga_noc_fw_ddr_l3, enable),
		ALT_NOC_FW_DDR_SCR_EN_HPSREG7EN_SET_MSK
	},
	{
		"fpga2sdram0-0",
		SOCFPGA_SDR_FIREWALL_MPU_FPGA_ADDRESS +
			offsetof(struct socfpga_noc_fw_ddr_mpu_fpga2sdram,
				 fpga2sdram0region0addr),
		SOCFPGA_SDR_FIREWALL_MPU_FPGA_ADDRESS +
			offsetof(struct socfpga_noc_fw_ddr_mpu_fpga2sdram,
				 enable),
		ALT_NOC_FW_DDR_SCR_EN_F2SDR0REG0EN_SET_MSK
	},
	{
		"fpga2sdram0-1",
		SOCFPGA_SDR_FIREWALL_MPU_FPGA_ADDRESS +
			offsetof(struct socfpga_noc_fw_ddr_mpu_fpga2sdram,
				 fpga2sdram0region1addr),
		SOCFPGA_SDR_FIREWALL_MPU_FPGA_ADDRESS +
			offsetof(struct socfpga_noc_fw_ddr_mpu_fpga2sdram,
				 enable),
		ALT_NOC_FW_DDR_SCR_EN_F2SDR0REG1EN_SET_MSK
	},
	{
		"fpga2sdram0-2",
		SOCFPGA_SDR_FIREWALL_MPU_FPGA_ADDRESS +
			offsetof(struct socfpga_noc_fw_ddr_mpu_fpga2sdram,
				 fpga2sdram0region2addr),
		SOCFPGA_SDR_FIREWALL_MPU_FPGA_ADDRESS +
			offsetof(struct socfpga_noc_fw_ddr_mpu_fpga2sdram,
				 enable),
		ALT_NOC_FW_DDR_SCR_EN_F2SDR0REG2EN_SET_MSK
	},
	{
		"fpga2sdram0-3",
		SOCFPGA_SDR_FIREWALL_MPU_FPGA_ADDRESS +
			offsetof(struct socfpga_noc_fw_ddr_mpu_fpga2sdram,
				 fpga2sdram0region3addr),
		SOCFPGA_SDR_FIREWALL_MPU_FPGA_ADDRESS +
			offsetof(struct socfpga_noc_fw_ddr_mpu_fpga2sdram,
				 enable),
		ALT_NOC_FW_DDR_SCR_EN_F2SDR0REG3EN_SET_MSK
	},
	{
		"fpga2sdram1-0",
		SOCFPGA_SDR_FIREWALL_MPU_FPGA_ADDRESS +
			offsetof(struct socfpga_noc_fw_ddr_mpu_fpga2sdram,
				 fpga2sdram1region0addr),
		SOCFPGA_SDR_FIREWALL_MPU_FPGA_ADDRESS +
			offsetof(struct socfpga_noc_fw_ddr_mpu_fpga2sdram,
				 enable),
		ALT_NOC_FW_DDR_SCR_EN_F2SDR1REG0EN_SET_MSK
	},
	{
		"fpga2sdram1-1",
		SOCFPGA_SDR_FIREWALL_MPU_FPGA_ADDRESS +
			offsetof(struct socfpga_noc_fw_ddr_mpu_fpga2sdram,
				 fpga2sdram1region1addr),
		SOCFPGA_SDR_FIREWALL_MPU_FPGA_ADDRESS +
			offsetof(struct socfpga_noc_fw_ddr_mpu_fpga2sdram,
				 enable),
		ALT_NOC_FW_DDR_SCR_EN_F2SDR1REG1EN_SET_MSK
	},
	{
		"fpga2sdram1-2",
		SOCFPGA_SDR_FIREWALL_MPU_FPGA_ADDRESS +
			offsetof(struct socfpga_noc_fw_ddr_mpu_fpga2sdram,
				 fpga2sdram1region2addr),
		SOCFPGA_SDR_FIREWALL_MPU_FPGA_ADDRESS +
			offsetof(struct socfpga_noc_fw_ddr_mpu_fpga2sdram,
				 enable),
		ALT_NOC_FW_DDR_SCR_EN_F2SDR1REG2EN_SET_MSK
	},
	{
		"fpga2sdram1-3",
		SOCFPGA_SDR_FIREWALL_MPU_FPGA_ADDRESS +
			offsetof(struct socfpga_noc_fw_ddr_mpu_fpga2sdram,
				 fpga2sdram1region3addr),
		SOCFPGA_SDR_FIREWALL_MPU_FPGA_ADDRESS +
			offsetof(struct socfpga_noc_fw_ddr_mpu_fpga2sdram,
				 enable),
		ALT_NOC_FW_DDR_SCR_EN_F2SDR1REG3EN_SET_MSK
	},	{
		"fpga2sdram2-0",
		SOCFPGA_SDR_FIREWALL_MPU_FPGA_ADDRESS +
			offsetof(struct socfpga_noc_fw_ddr_mpu_fpga2sdram,
				 fpga2sdram2region0addr),
		SOCFPGA_SDR_FIREWALL_MPU_FPGA_ADDRESS +
			offsetof(struct socfpga_noc_fw_ddr_mpu_fpga2sdram,
				 enable),
		ALT_NOC_FW_DDR_SCR_EN_F2SDR2REG0EN_SET_MSK
	},
	{
		"fpga2sdram2-1",
		SOCFPGA_SDR_FIREWALL_MPU_FPGA_ADDRESS +
			offsetof(struct socfpga_noc_fw_ddr_mpu_fpga2sdram,
				 fpga2sdram2region1addr),
		SOCFPGA_SDR_FIREWALL_MPU_FPGA_ADDRESS +
			offsetof(struct socfpga_noc_fw_ddr_mpu_fpga2sdram,
				 enable),
		ALT_NOC_FW_DDR_SCR_EN_F2SDR2REG1EN_SET_MSK
	},
	{
		"fpga2sdram2-2",
		SOCFPGA_SDR_FIREWALL_MPU_FPGA_ADDRESS +
			offsetof(struct socfpga_noc_fw_ddr_mpu_fpga2sdram,
				 fpga2sdram2region2addr),
		SOCFPGA_SDR_FIREWALL_MPU_FPGA_ADDRESS +
			offsetof(struct socfpga_noc_fw_ddr_mpu_fpga2sdram,
				 enable),
		ALT_NOC_FW_DDR_SCR_EN_F2SDR2REG2EN_SET_MSK
	},
	{
		"fpga2sdram2-3",
		SOCFPGA_SDR_FIREWALL_MPU_FPGA_ADDRESS +
			offsetof(struct socfpga_noc_fw_ddr_mpu_fpga2sdram,
				 fpga2sdram2region3addr),
		SOCFPGA_SDR_FIREWALL_MPU_FPGA_ADDRESS +
			offsetof(struct socfpga_noc_fw_ddr_mpu_fpga2sdram,
				 enable),
		ALT_NOC_FW_DDR_SCR_EN_F2SDR2REG3EN_SET_MSK
	},

};

int of_sdram_firewall_setup(const void *blob)
{
	int child, i, node;
	u32 start_end[2];

	node = fdtdec_next_compatible(blob, 0, COMPAT_ARRIA10_NOC);
	if (node < 0)
		return 2;

	child = fdt_first_subnode(blob, node);
	if (child < 0)
		return 1;

	/* set to default state */
	writel(0, &socfpga_noc_fw_ddr_mpu_fpga2sdram_base->enable);
	writel(0, &socfpga_noc_fw_ddr_l3_base->enable);


	for (i = 0; i < ARRAY_SIZE(firewall_table); i++) {
		if (!fdtdec_get_int_array(blob, child,
					  firewall_table[i].prop_name,
					  start_end, 2)) {
			writel((start_end[0] & ALT_NOC_FW_DDR_ADDR_MASK) |
				(start_end[1] << ALT_NOC_FW_DDR_END_ADDR_LSB),
				 firewall_table[i].cfg_addr);
			setbits_le32(firewall_table[i].en_addr,
				     firewall_table[i].en_bit);
		}
	}

	return 0;
}

int ddr_calibration_sequence(void)
{

	if (!is_fpgamgr_user_mode()) {
		printf("fpga not configured!\n");
		return -1;
	}

	WATCHDOG_RESET();

	/* Check to see if SDRAM cal was success */
	if (sdram_startup()) {
		puts("DDRCAL: Failed\n");
		return -1;
	}

	puts("DDRCAL: Success\n");

	WATCHDOG_RESET();

	/* initialize the MMR register */
	sdram_mmr_init();

	/* assigning the SDRAM size */
	gd->ram_size = sdram_size_calc();

	/* If a weird value, use default Config size */
	if (gd->ram_size <= 0)
		gd->ram_size = PHYS_SDRAM_1_SIZE;

	/* setup the dram info within bd */
	dram_init_banksize();

	if (of_sdram_firewall_setup(gd->fdt_blob))
		puts("FW: Error Configuring Firewall\n");

	return 0;
}

/* Initialise the DRAM by telling the DRAM Size */
int dram_init(void)
{
	bd_t *bd;
	unsigned long addr;
	int rval = 0;

	WATCHDOG_RESET();

	/* enable cache as we want to speed up CFF process */
#if !(defined(CONFIG_SYS_ICACHE_OFF) && defined(CONFIG_SYS_DCACHE_OFF))
	/* reserve TLB table */
	gd->arch.tlb_size = 4096 * 4;
	/* page table is located at last 16kB of OCRAM */
	addr = CONFIG_SYS_INIT_SP_ADDR;
	gd->arch.tlb_addr = addr;

	/*
	 * We need to setup the bd for the dram info too. We will use same
	 * memory layout in later setup
	 */
	addr -= (CONFIG_OCRAM_STACK_SIZE + CONFIG_OCRAM_MALLOC_SIZE);

	/*
	 * (permanently) allocate a Board Info struct
	 * and a permanent copy of the "global" data
	 */
	addr -= sizeof(bd_t);
	bd = (bd_t *)addr;
	gd->bd = bd;

	/* enable the cache */
	enable_caches();
#endif

	WATCHDOG_RESET();
	u32 malloc_start = CONFIG_SYS_INIT_SP_ADDR
		- CONFIG_OCRAM_STACK_SIZE - CONFIG_OCRAM_MALLOC_SIZE;
	mem_malloc_init(malloc_start, CONFIG_OCRAM_MALLOC_SIZE);

	if (is_external_fpga_config(gd->fdt_blob)) {
		ddr_calibration_sequence();
	} else {
#if defined(CONFIG_MMC)
		int len = 0;
		const char *cff = get_cff_filename(gd->fdt_blob, &len);
		if (cff && (len > 0)) {
			mmc_initialize(gd->bd);

			rval = cff_from_mmc_fat("0:1", cff, len);
		}
#elif defined(CONFIG_CADENCE_QSPI)
		rval = cff_from_qspi_env();
#else
#error "unsupported config"
#endif
		if (rval > 0) {
			reset_assert_uart();
			config_shared_fpga_pins(gd->fdt_blob);
			reset_deassert_uart();

			reset_deassert_shared_connected_peripherals();
			reset_deassert_fpga_connected_peripherals();
			NS16550_init((NS16550_t)CONFIG_SYS_NS16550_COM1,
				     ns16550_calc_divisor(
					     (NS16550_t)CONFIG_SYS_NS16550_COM1,
					     CONFIG_SYS_NS16550_CLK,
					     CONFIG_BAUDRATE));

			ddr_calibration_sequence();
		}
	}

	/* Skip relocation as U-Boot cannot run on SDRAM for secure boot */
	skip_relocation();
	WATCHDOG_RESET();
	return 0;
}

void dram_bank_mmu_setup(int bank)
{
	bd_t *bd = gd->bd;
	int	i;

	debug("%s: bank: %d\n", __func__, bank);
	for (i = bd->bi_dram[bank].start >> 20;
	     i < (bd->bi_dram[bank].start + bd->bi_dram[bank].size) >> 20;
	     i++) {
#if defined(CONFIG_SYS_ARM_CACHE_WRITETHROUGH)
		set_section_dcache(i, DCACHE_WRITETHROUGH);
#else
		set_section_dcache(i, DCACHE_WRITEBACK);
#endif
	}

	/* same as above but just that we would want cacheable for ocram too */
	i = CONFIG_SYS_INIT_RAM_ADDR >> 20;
#if defined(CONFIG_SYS_ARM_CACHE_WRITETHROUGH)
	set_section_dcache(i, DCACHE_WRITETHROUGH);
#else
	set_section_dcache(i, DCACHE_WRITEBACK);
#endif
}
