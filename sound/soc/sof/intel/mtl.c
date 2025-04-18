// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
//
// Copyright(c) 2022 Intel Corporation
//
// Authors: Ranjani Sridharan <ranjani.sridharan@linux.intel.com>
//

/*
 * Hardware interface for audio DSP on Meteorlake.
 */

#include <linux/debugfs.h>
#include <linux/firmware.h>
#include <linux/string_choices.h>
#include <sound/sof/ipc4/header.h>
#include <trace/events/sof_intel.h>
#include "../ipc4-priv.h"
#include "../ops.h"
#include "hda.h"
#include "hda-ipc.h"
#include "../sof-audio.h"
#include "mtl.h"
#include "telemetry.h"

static const struct snd_sof_debugfs_map mtl_dsp_debugfs[] = {
	{"hda", HDA_DSP_HDA_BAR, 0, 0x4000, SOF_DEBUGFS_ACCESS_ALWAYS},
	{"pp", HDA_DSP_PP_BAR,  0, 0x1000, SOF_DEBUGFS_ACCESS_ALWAYS},
	{"dsp", HDA_DSP_BAR,  0, 0x10000, SOF_DEBUGFS_ACCESS_ALWAYS},
	{"fw_regs", HDA_DSP_BAR,  MTL_SRAM_WINDOW_OFFSET(0), 0x1000, SOF_DEBUGFS_ACCESS_D0_ONLY},
};

static void mtl_ipc_host_done(struct snd_sof_dev *sdev)
{
	/*
	 * clear busy interrupt to tell dsp controller this interrupt has been accepted,
	 * not trigger it again
	 */
	snd_sof_dsp_update_bits_forced(sdev, HDA_DSP_BAR, MTL_DSP_REG_HFIPCXTDR,
				       MTL_DSP_REG_HFIPCXTDR_BUSY, MTL_DSP_REG_HFIPCXTDR_BUSY);
	/*
	 * clear busy bit to ack dsp the msg has been processed and send reply msg to dsp
	 */
	snd_sof_dsp_update_bits_forced(sdev, HDA_DSP_BAR, MTL_DSP_REG_HFIPCXTDA,
				       MTL_DSP_REG_HFIPCXTDA_BUSY, 0);
}

static void mtl_ipc_dsp_done(struct snd_sof_dev *sdev)
{
	/*
	 * set DONE bit - tell DSP we have received the reply msg from DSP, and processed it,
	 * don't send more reply to host
	 */
	snd_sof_dsp_update_bits_forced(sdev, HDA_DSP_BAR, MTL_DSP_REG_HFIPCXIDA,
				       MTL_DSP_REG_HFIPCXIDA_DONE, MTL_DSP_REG_HFIPCXIDA_DONE);

	/* unmask Done interrupt */
	snd_sof_dsp_update_bits(sdev, HDA_DSP_BAR, MTL_DSP_REG_HFIPCXCTL,
				MTL_DSP_REG_HFIPCXCTL_DONE, MTL_DSP_REG_HFIPCXCTL_DONE);
}

/* Check if an IPC IRQ occurred */
bool mtl_dsp_check_ipc_irq(struct snd_sof_dev *sdev)
{
	u32 irq_status;
	u32 hfintipptr;

	if (sdev->dspless_mode_selected)
		return false;

	/* read Interrupt IP Pointer */
	hfintipptr = snd_sof_dsp_read(sdev, HDA_DSP_BAR, MTL_HFINTIPPTR) & MTL_HFINTIPPTR_PTR_MASK;
	irq_status = snd_sof_dsp_read(sdev, HDA_DSP_BAR, hfintipptr + MTL_DSP_IRQSTS);

	trace_sof_intel_hda_irq_ipc_check(sdev, irq_status);

	if (irq_status != U32_MAX && (irq_status & MTL_DSP_IRQSTS_IPC))
		return true;

	return false;
}
EXPORT_SYMBOL_NS(mtl_dsp_check_ipc_irq, "SND_SOC_SOF_INTEL_MTL");

/* Check if an SDW IRQ occurred */
static bool mtl_dsp_check_sdw_irq(struct snd_sof_dev *sdev)
{
	u32 irq_status;
	u32 hfintipptr;

	/* read Interrupt IP Pointer */
	hfintipptr = snd_sof_dsp_read(sdev, HDA_DSP_BAR, MTL_HFINTIPPTR) & MTL_HFINTIPPTR_PTR_MASK;
	irq_status = snd_sof_dsp_read(sdev, HDA_DSP_BAR, hfintipptr + MTL_DSP_IRQSTS);

	if (irq_status != U32_MAX && (irq_status & MTL_DSP_IRQSTS_SDW))
		return true;

	return false;
}

static int mtl_ipc_send_msg(struct snd_sof_dev *sdev, struct snd_sof_ipc_msg *msg)
{
	struct sof_intel_hda_dev *hdev = sdev->pdata->hw_pdata;
	struct sof_ipc4_msg *msg_data = msg->msg_data;

	if (hda_ipc4_tx_is_busy(sdev)) {
		hdev->delayed_ipc_tx_msg = msg;
		return 0;
	}

	hdev->delayed_ipc_tx_msg = NULL;

	/* send the message via mailbox */
	if (msg_data->data_size)
		sof_mailbox_write(sdev, sdev->host_box.offset, msg_data->data_ptr,
				  msg_data->data_size);

	snd_sof_dsp_write(sdev, HDA_DSP_BAR, MTL_DSP_REG_HFIPCXIDDY,
			  msg_data->extension);
	snd_sof_dsp_write(sdev, HDA_DSP_BAR, MTL_DSP_REG_HFIPCXIDR,
			  msg_data->primary | MTL_DSP_REG_HFIPCXIDR_BUSY);

	hda_dsp_ipc4_schedule_d0i3_work(hdev, msg);

	return 0;
}

void mtl_enable_ipc_interrupts(struct snd_sof_dev *sdev)
{
	struct sof_intel_hda_dev *hda = sdev->pdata->hw_pdata;
	const struct sof_intel_dsp_desc *chip = hda->desc;

	if (sdev->dspless_mode_selected)
		return;

	/* enable IPC DONE and BUSY interrupts */
	snd_sof_dsp_update_bits(sdev, HDA_DSP_BAR, chip->ipc_ctl,
				MTL_DSP_REG_HFIPCXCTL_BUSY | MTL_DSP_REG_HFIPCXCTL_DONE,
				MTL_DSP_REG_HFIPCXCTL_BUSY | MTL_DSP_REG_HFIPCXCTL_DONE);
}

void mtl_disable_ipc_interrupts(struct snd_sof_dev *sdev)
{
	struct sof_intel_hda_dev *hda = sdev->pdata->hw_pdata;
	const struct sof_intel_dsp_desc *chip = hda->desc;

	if (sdev->dspless_mode_selected)
		return;

	/* disable IPC DONE and BUSY interrupts */
	snd_sof_dsp_update_bits(sdev, HDA_DSP_BAR, chip->ipc_ctl,
				MTL_DSP_REG_HFIPCXCTL_BUSY | MTL_DSP_REG_HFIPCXCTL_DONE, 0);
}
EXPORT_SYMBOL_NS(mtl_disable_ipc_interrupts, "SND_SOC_SOF_INTEL_MTL");

static void mtl_enable_sdw_irq(struct snd_sof_dev *sdev, bool enable)
{
	u32 hipcie;
	u32 mask;
	u32 val;
	int ret;

	if (sdev->dspless_mode_selected)
		return;

	/* Enable/Disable SoundWire interrupt */
	mask = MTL_DSP_REG_HfSNDWIE_IE_MASK;
	if (enable)
		val = mask;
	else
		val = 0;

	snd_sof_dsp_update_bits(sdev, HDA_DSP_BAR, MTL_DSP_REG_HfSNDWIE, mask, val);

	/* check if operation was successful */
	ret = snd_sof_dsp_read_poll_timeout(sdev, HDA_DSP_BAR, MTL_DSP_REG_HfSNDWIE, hipcie,
					    (hipcie & mask) == val,
					    HDA_DSP_REG_POLL_INTERVAL_US, HDA_DSP_RESET_TIMEOUT_US);
	if (ret < 0)
		dev_err(sdev->dev, "failed to set SoundWire IPC interrupt %s\n",
			str_enable_disable(enable));
}

int mtl_enable_interrupts(struct snd_sof_dev *sdev, bool enable)
{
	u32 hfintipptr;
	u32 irqinten;
	u32 hipcie;
	u32 mask;
	u32 val;
	int ret;

	if (sdev->dspless_mode_selected)
		return 0;

	/* read Interrupt IP Pointer */
	hfintipptr = snd_sof_dsp_read(sdev, HDA_DSP_BAR, MTL_HFINTIPPTR) & MTL_HFINTIPPTR_PTR_MASK;

	/* Enable/Disable Host IPC and SOUNDWIRE */
	mask = MTL_IRQ_INTEN_L_HOST_IPC_MASK | MTL_IRQ_INTEN_L_SOUNDWIRE_MASK;
	if (enable)
		val = mask;
	else
		val = 0;

	snd_sof_dsp_update_bits(sdev, HDA_DSP_BAR, hfintipptr, mask, val);

	/* check if operation was successful */
	ret = snd_sof_dsp_read_poll_timeout(sdev, HDA_DSP_BAR, hfintipptr, irqinten,
					    (irqinten & mask) == val,
					    HDA_DSP_REG_POLL_INTERVAL_US, HDA_DSP_RESET_TIMEOUT_US);
	if (ret < 0) {
		dev_err(sdev->dev, "failed to %s Host IPC and/or SOUNDWIRE\n",
			str_enable_disable(enable));
		return ret;
	}

	/* Enable/Disable Host IPC interrupt*/
	mask = MTL_DSP_REG_HfHIPCIE_IE_MASK;
	if (enable)
		val = mask;
	else
		val = 0;

	snd_sof_dsp_update_bits(sdev, HDA_DSP_BAR, MTL_DSP_REG_HfHIPCIE, mask, val);

	/* check if operation was successful */
	ret = snd_sof_dsp_read_poll_timeout(sdev, HDA_DSP_BAR, MTL_DSP_REG_HfHIPCIE, hipcie,
					    (hipcie & mask) == val,
					    HDA_DSP_REG_POLL_INTERVAL_US, HDA_DSP_RESET_TIMEOUT_US);
	if (ret < 0) {
		dev_err(sdev->dev, "failed to set Host IPC interrupt %s\n",
			str_enable_disable(enable));
		return ret;
	}

	return ret;
}
EXPORT_SYMBOL_NS(mtl_enable_interrupts, "SND_SOC_SOF_INTEL_MTL");

/* pre fw run operations */
static int mtl_dsp_pre_fw_run(struct snd_sof_dev *sdev)
{
	struct sof_intel_hda_dev *hdev = sdev->pdata->hw_pdata;
	u32 dsphfpwrsts;
	u32 dsphfdsscs;
	u32 cpa;
	u32 pgs;
	int ret;
	u32 dsppwrctl;
	u32 dsppwrsts;
	const struct sof_intel_dsp_desc *chip;

	chip = get_chip_info(sdev->pdata);
	if (chip->hw_ip_version > SOF_INTEL_ACE_2_0) {
		dsppwrctl = PTL_HFPWRCTL2;
		dsppwrsts = PTL_HFPWRSTS2;
	} else {
		dsppwrctl = MTL_HFPWRCTL;
		dsppwrsts = MTL_HFPWRSTS;
	}

	/* Set the DSP subsystem power on */
	snd_sof_dsp_update_bits(sdev, HDA_DSP_BAR, MTL_HFDSSCS,
				MTL_HFDSSCS_SPA_MASK, MTL_HFDSSCS_SPA_MASK);

	/* Wait for unstable CPA read (1 then 0 then 1) just after setting SPA bit */
	usleep_range(1000, 1010);

	/* poll with timeout to check if operation successful */
	cpa = MTL_HFDSSCS_CPA_MASK;
	ret = snd_sof_dsp_read_poll_timeout(sdev, HDA_DSP_BAR, MTL_HFDSSCS, dsphfdsscs,
					    (dsphfdsscs & cpa) == cpa, HDA_DSP_REG_POLL_INTERVAL_US,
					    HDA_DSP_RESET_TIMEOUT_US);
	if (ret < 0) {
		dev_err(sdev->dev, "failed to enable DSP subsystem\n");
		return ret;
	}

	/* Power up gated-DSP-0 domain in order to access the DSP shim register block. */
	snd_sof_dsp_update_bits(sdev, HDA_DSP_BAR, dsppwrctl,
				MTL_HFPWRCTL_WPDSPHPXPG, MTL_HFPWRCTL_WPDSPHPXPG);

	usleep_range(1000, 1010);

	/* poll with timeout to check if operation successful */
	pgs = MTL_HFPWRSTS_DSPHPXPGS_MASK;
	ret = snd_sof_dsp_read_poll_timeout(sdev, HDA_DSP_BAR, dsppwrsts, dsphfpwrsts,
					    (dsphfpwrsts & pgs) == pgs,
					    HDA_DSP_REG_POLL_INTERVAL_US,
					    HDA_DSP_RESET_TIMEOUT_US);
	if (ret < 0)
		dev_err(sdev->dev, "failed to power up gated DSP domain\n");

	/* if SoundWire is used, make sure it is not power-gated */
	if (hdev->info.handle && hdev->info.link_mask > 0)
		snd_sof_dsp_update_bits(sdev, HDA_DSP_BAR, MTL_HFPWRCTL,
					MTL_HfPWRCTL_WPIOXPG(1), MTL_HfPWRCTL_WPIOXPG(1));

	return ret;
}

static int mtl_dsp_post_fw_run(struct snd_sof_dev *sdev)
{
	int ret;

	if (sdev->first_boot) {
		struct sof_intel_hda_dev *hdev = sdev->pdata->hw_pdata;

		ret = hda_sdw_startup(sdev);
		if (ret < 0) {
			dev_err(sdev->dev, "could not startup SoundWire links\n");
			return ret;
		}

		/* Check if IMR boot is usable */
		if (!sof_debug_check_flag(SOF_DBG_IGNORE_D3_PERSISTENT)) {
			hdev->imrboot_supported = true;
			debugfs_create_bool("skip_imr_boot",
					    0644, sdev->debugfs_root,
					    &hdev->skip_imr_boot);
		}
	}

	hda_sdw_int_enable(sdev, true);
	return 0;
}

static void mtl_dsp_dump(struct snd_sof_dev *sdev, u32 flags)
{
	char *level = (flags & SOF_DBG_DUMP_OPTIONAL) ? KERN_DEBUG : KERN_ERR;
	u32 fwsts;
	u32 fwlec;

	hda_dsp_get_state(sdev, level);
	fwsts = snd_sof_dsp_read(sdev, HDA_DSP_BAR, MTL_DSP_ROM_STS);
	fwlec = snd_sof_dsp_read(sdev, HDA_DSP_BAR, MTL_DSP_ROM_ERROR);

	if (fwsts != 0xffffffff)
		dev_err(sdev->dev, "Firmware state: %#x, status/error code: %#x\n",
			fwsts, fwlec);

	sof_ipc4_intel_dump_telemetry_state(sdev, flags);
}

static bool mtl_dsp_primary_core_is_enabled(struct snd_sof_dev *sdev)
{
	int val;

	val = snd_sof_dsp_read(sdev, HDA_DSP_BAR, MTL_DSP2CXCTL_PRIMARY_CORE);
	if (val != U32_MAX && val & MTL_DSP2CXCTL_PRIMARY_CORE_CPA_MASK)
		return true;

	return false;
}

static int mtl_dsp_core_power_up(struct snd_sof_dev *sdev, int core)
{
	unsigned int cpa;
	u32 dspcxctl;
	int ret;

	/* Only the primary core can be powered up by the host */
	if (core != SOF_DSP_PRIMARY_CORE || mtl_dsp_primary_core_is_enabled(sdev))
		return 0;

	/* Program the owner of the IP & shim registers (10: Host CPU) */
	snd_sof_dsp_update_bits(sdev, HDA_DSP_BAR, MTL_DSP2CXCTL_PRIMARY_CORE,
				MTL_DSP2CXCTL_PRIMARY_CORE_OSEL,
				0x2 << MTL_DSP2CXCTL_PRIMARY_CORE_OSEL_SHIFT);

	/* enable SPA bit */
	snd_sof_dsp_update_bits(sdev, HDA_DSP_BAR, MTL_DSP2CXCTL_PRIMARY_CORE,
				MTL_DSP2CXCTL_PRIMARY_CORE_SPA_MASK,
				MTL_DSP2CXCTL_PRIMARY_CORE_SPA_MASK);

	/* Wait for unstable CPA read (1 then 0 then 1) just after setting SPA bit */
	usleep_range(1000, 1010);

	/* poll with timeout to check if operation successful */
	cpa = MTL_DSP2CXCTL_PRIMARY_CORE_CPA_MASK;
	ret = snd_sof_dsp_read_poll_timeout(sdev, HDA_DSP_BAR, MTL_DSP2CXCTL_PRIMARY_CORE, dspcxctl,
					    (dspcxctl & cpa) == cpa, HDA_DSP_REG_POLL_INTERVAL_US,
					    HDA_DSP_RESET_TIMEOUT_US);
	if (ret < 0) {
		dev_err(sdev->dev, "%s: timeout on MTL_DSP2CXCTL_PRIMARY_CORE read\n",
			__func__);
		return ret;
	}

	/* set primary core mask and refcount to 1 */
	sdev->enabled_cores_mask = BIT(SOF_DSP_PRIMARY_CORE);
	sdev->dsp_core_ref_count[SOF_DSP_PRIMARY_CORE] = 1;

	return 0;
}

static int mtl_dsp_core_power_down(struct snd_sof_dev *sdev, int core)
{
	u32 dspcxctl;
	int ret;

	/* Only the primary core can be powered down by the host */
	if (core != SOF_DSP_PRIMARY_CORE || !mtl_dsp_primary_core_is_enabled(sdev))
		return 0;

	/* disable SPA bit */
	snd_sof_dsp_update_bits(sdev, HDA_DSP_BAR, MTL_DSP2CXCTL_PRIMARY_CORE,
				MTL_DSP2CXCTL_PRIMARY_CORE_SPA_MASK, 0);

	/* Wait for unstable CPA read (0 then 1 then 0) just after setting SPA bit */
	usleep_range(1000, 1010);

	ret = snd_sof_dsp_read_poll_timeout(sdev, HDA_DSP_BAR, MTL_DSP2CXCTL_PRIMARY_CORE, dspcxctl,
					    !(dspcxctl & MTL_DSP2CXCTL_PRIMARY_CORE_CPA_MASK),
					    HDA_DSP_REG_POLL_INTERVAL_US,
					    HDA_DSP_PD_TIMEOUT * USEC_PER_MSEC);
	if (ret < 0) {
		dev_err(sdev->dev, "failed to power down primary core\n");
		return ret;
	}

	sdev->enabled_cores_mask = 0;
	sdev->dsp_core_ref_count[SOF_DSP_PRIMARY_CORE] = 0;

	return 0;
}

int mtl_power_down_dsp(struct snd_sof_dev *sdev)
{
	u32 dsphfdsscs, cpa;
	int ret;

	/* first power down core */
	ret = mtl_dsp_core_power_down(sdev, SOF_DSP_PRIMARY_CORE);
	if (ret) {
		dev_err(sdev->dev, "mtl dsp power down error, %d\n", ret);
		return ret;
	}

	/* Set the DSP subsystem power down */
	snd_sof_dsp_update_bits(sdev, HDA_DSP_BAR, MTL_HFDSSCS,
				MTL_HFDSSCS_SPA_MASK, 0);

	/* Wait for unstable CPA read (0 then 1 then 0) just after setting SPA bit */
	usleep_range(1000, 1010);

	/* poll with timeout to check if operation successful */
	cpa = MTL_HFDSSCS_CPA_MASK;
	dsphfdsscs = snd_sof_dsp_read(sdev, HDA_DSP_BAR, MTL_HFDSSCS);
	return snd_sof_dsp_read_poll_timeout(sdev, HDA_DSP_BAR, MTL_HFDSSCS, dsphfdsscs,
					     (dsphfdsscs & cpa) == 0, HDA_DSP_REG_POLL_INTERVAL_US,
					     HDA_DSP_RESET_TIMEOUT_US);
}
EXPORT_SYMBOL_NS(mtl_power_down_dsp, "SND_SOC_SOF_INTEL_MTL");

int mtl_dsp_cl_init(struct snd_sof_dev *sdev, int stream_tag, bool imr_boot)
{
	struct sof_intel_hda_dev *hda = sdev->pdata->hw_pdata;
	const struct sof_intel_dsp_desc *chip = hda->desc;
	unsigned int status, target_status;
	u32 ipc_hdr, flags;
	char *dump_msg;
	int ret;

	/* step 1: purge FW request */
	ipc_hdr = chip->ipc_req_mask | HDA_DSP_ROM_IPC_CONTROL;
	if (!imr_boot)
		ipc_hdr |= HDA_DSP_ROM_IPC_PURGE_FW | ((stream_tag - 1) << 9);

	snd_sof_dsp_write(sdev, HDA_DSP_BAR, chip->ipc_req, ipc_hdr);

	/* step 2: power up primary core */
	ret = mtl_dsp_core_power_up(sdev, SOF_DSP_PRIMARY_CORE);
	if (ret < 0) {
		if (hda->boot_iteration == HDA_FW_BOOT_ATTEMPTS)
			dev_err(sdev->dev, "dsp core 0/1 power up failed\n");
		goto err;
	}

	dev_dbg(sdev->dev, "Primary core power up successful\n");

	/* step 3: wait for IPC DONE bit from ROM */
	ret = snd_sof_dsp_read_poll_timeout(sdev, HDA_DSP_BAR, chip->ipc_ack, status,
					    ((status & chip->ipc_ack_mask) == chip->ipc_ack_mask),
					    HDA_DSP_REG_POLL_INTERVAL_US, HDA_DSP_INIT_TIMEOUT_US);
	if (ret < 0) {
		if (hda->boot_iteration == HDA_FW_BOOT_ATTEMPTS)
			dev_err(sdev->dev, "timeout waiting for purge IPC done\n");
		goto err;
	}

	/* set DONE bit to clear the reply IPC message */
	snd_sof_dsp_update_bits_forced(sdev, HDA_DSP_BAR, chip->ipc_ack, chip->ipc_ack_mask,
				       chip->ipc_ack_mask);

	/* step 4: enable interrupts */
	ret = mtl_enable_interrupts(sdev, true);
	if (ret < 0) {
		if (hda->boot_iteration == HDA_FW_BOOT_ATTEMPTS)
			dev_err(sdev->dev, "%s: failed to enable interrupts\n", __func__);
		goto err;
	}

	mtl_enable_ipc_interrupts(sdev);

	if (chip->rom_status_reg == MTL_DSP_ROM_STS) {
		/*
		 * Workaround: when the ROM status register is pointing to
		 * the SRAM window (MTL_DSP_ROM_STS) the platform cannot catch
		 * ROM_INIT_DONE because of a very short timing window.
		 * Follow the recommendations and skip target state waiting.
		 */
		return 0;
	}

	/*
	 * step 7:
	 * - Cold/Full boot: wait for ROM init to proceed to download the firmware
	 * - IMR boot: wait for ROM firmware entered (firmware booted up from IMR)
	 */
	if (imr_boot)
		target_status = FSR_STATE_FW_ENTERED;
	else
		target_status = FSR_STATE_INIT_DONE;

	ret = snd_sof_dsp_read_poll_timeout(sdev, HDA_DSP_BAR,
					chip->rom_status_reg, status,
					(FSR_TO_STATE_CODE(status) == target_status),
					HDA_DSP_REG_POLL_INTERVAL_US,
					chip->rom_init_timeout *
					USEC_PER_MSEC);

	if (!ret)
		return 0;

	if (hda->boot_iteration == HDA_FW_BOOT_ATTEMPTS)
		dev_err(sdev->dev,
			"%s: timeout with rom_status_reg (%#x) read\n",
			__func__, chip->rom_status_reg);

err:
	flags = SOF_DBG_DUMP_PCI | SOF_DBG_DUMP_MBOX | SOF_DBG_DUMP_OPTIONAL;

	/* after max boot attempts make sure that the dump is printed */
	if (hda->boot_iteration == HDA_FW_BOOT_ATTEMPTS)
		flags &= ~SOF_DBG_DUMP_OPTIONAL;

	dump_msg = kasprintf(GFP_KERNEL, "Boot iteration failed: %d/%d",
			     hda->boot_iteration, HDA_FW_BOOT_ATTEMPTS);
	snd_sof_dsp_dbg_dump(sdev, dump_msg, flags);
	mtl_enable_interrupts(sdev, false);
	mtl_dsp_core_power_down(sdev, SOF_DSP_PRIMARY_CORE);

	kfree(dump_msg);
	return ret;
}
EXPORT_SYMBOL_NS(mtl_dsp_cl_init, "SND_SOC_SOF_INTEL_MTL");

static irqreturn_t mtl_ipc_irq_thread(int irq, void *context)
{
	struct sof_ipc4_msg notification_data = {{ 0 }};
	struct snd_sof_dev *sdev = context;
	bool ack_received = false;
	bool ipc_irq = false;
	u32 hipcida;
	u32 hipctdr;

	hipcida = snd_sof_dsp_read(sdev, HDA_DSP_BAR, MTL_DSP_REG_HFIPCXIDA);
	hipctdr = snd_sof_dsp_read(sdev, HDA_DSP_BAR, MTL_DSP_REG_HFIPCXTDR);

	/* reply message from DSP */
	if (hipcida & MTL_DSP_REG_HFIPCXIDA_DONE) {
		/* DSP received the message */
		snd_sof_dsp_update_bits(sdev, HDA_DSP_BAR, MTL_DSP_REG_HFIPCXCTL,
					MTL_DSP_REG_HFIPCXCTL_DONE, 0);

		mtl_ipc_dsp_done(sdev);

		ipc_irq = true;
		ack_received = true;
	}

	if (hipctdr & MTL_DSP_REG_HFIPCXTDR_BUSY) {
		/* Message from DSP (reply or notification) */
		u32 extension = snd_sof_dsp_read(sdev, HDA_DSP_BAR, MTL_DSP_REG_HFIPCXTDDY);
		u32 primary = hipctdr & MTL_DSP_REG_HFIPCXTDR_MSG_MASK;

		/*
		 * ACE fw sends a new fw ipc message to host to
		 * notify the status of the last host ipc message
		 */
		if (primary & SOF_IPC4_MSG_DIR_MASK) {
			/* Reply received */
			if (likely(sdev->fw_state == SOF_FW_BOOT_COMPLETE)) {
				struct sof_ipc4_msg *data = sdev->ipc->msg.reply_data;

				data->primary = primary;
				data->extension = extension;

				spin_lock_irq(&sdev->ipc_lock);

				snd_sof_ipc_get_reply(sdev);
				mtl_ipc_host_done(sdev);
				snd_sof_ipc_reply(sdev, data->primary);

				spin_unlock_irq(&sdev->ipc_lock);
			} else {
				dev_dbg_ratelimited(sdev->dev,
						    "IPC reply before FW_READY: %#x|%#x\n",
						    primary, extension);
			}
		} else {
			/* Notification received */
			notification_data.primary = primary;
			notification_data.extension = extension;

			sdev->ipc->msg.rx_data = &notification_data;
			snd_sof_ipc_msgs_rx(sdev);
			sdev->ipc->msg.rx_data = NULL;

			mtl_ipc_host_done(sdev);
		}

		ipc_irq = true;
	}

	if (!ipc_irq) {
		/* This interrupt is not shared so no need to return IRQ_NONE. */
		dev_dbg_ratelimited(sdev->dev, "nothing to do in IPC IRQ thread\n");
	}

	if (ack_received) {
		struct sof_intel_hda_dev *hdev = sdev->pdata->hw_pdata;

		if (hdev->delayed_ipc_tx_msg)
			mtl_ipc_send_msg(sdev, hdev->delayed_ipc_tx_msg);
	}

	return IRQ_HANDLED;
}

static int mtl_dsp_ipc_get_mailbox_offset(struct snd_sof_dev *sdev)
{
	return MTL_DSP_MBOX_UPLINK_OFFSET;
}

static int mtl_dsp_ipc_get_window_offset(struct snd_sof_dev *sdev, u32 id)
{
	return MTL_SRAM_WINDOW_OFFSET(id);
}

static void mtl_ipc_dump(struct snd_sof_dev *sdev)
{
	u32 hipcidr, hipcidd, hipcida, hipctdr, hipctdd, hipctda, hipcctl;

	hipcidr = snd_sof_dsp_read(sdev, HDA_DSP_BAR, MTL_DSP_REG_HFIPCXIDR);
	hipcidd = snd_sof_dsp_read(sdev, HDA_DSP_BAR, MTL_DSP_REG_HFIPCXIDDY);
	hipcida = snd_sof_dsp_read(sdev, HDA_DSP_BAR, MTL_DSP_REG_HFIPCXIDA);
	hipctdr = snd_sof_dsp_read(sdev, HDA_DSP_BAR, MTL_DSP_REG_HFIPCXTDR);
	hipctdd = snd_sof_dsp_read(sdev, HDA_DSP_BAR, MTL_DSP_REG_HFIPCXTDDY);
	hipctda = snd_sof_dsp_read(sdev, HDA_DSP_BAR, MTL_DSP_REG_HFIPCXTDA);
	hipcctl = snd_sof_dsp_read(sdev, HDA_DSP_BAR, MTL_DSP_REG_HFIPCXCTL);

	dev_err(sdev->dev,
		"Host IPC initiator: %#x|%#x|%#x, target: %#x|%#x|%#x, ctl: %#x\n",
		hipcidr, hipcidd, hipcida, hipctdr, hipctdd, hipctda, hipcctl);
}

static int mtl_dsp_disable_interrupts(struct snd_sof_dev *sdev)
{
	mtl_enable_sdw_irq(sdev, false);
	mtl_disable_ipc_interrupts(sdev);
	return mtl_enable_interrupts(sdev, false);
}

static int mtl_dsp_core_get(struct snd_sof_dev *sdev, int core)
{
	const struct sof_ipc_pm_ops *pm_ops = sdev->ipc->ops->pm;

	if (core == SOF_DSP_PRIMARY_CORE)
		return mtl_dsp_core_power_up(sdev, SOF_DSP_PRIMARY_CORE);

	if (pm_ops->set_core_state)
		return pm_ops->set_core_state(sdev, core, true);

	return 0;
}

static int mtl_dsp_core_put(struct snd_sof_dev *sdev, int core)
{
	const struct sof_ipc_pm_ops *pm_ops = sdev->ipc->ops->pm;
	int ret;

	if (pm_ops->set_core_state) {
		ret = pm_ops->set_core_state(sdev, core, false);
		if (ret < 0)
			return ret;
	}

	if (core == SOF_DSP_PRIMARY_CORE)
		return mtl_dsp_core_power_down(sdev, SOF_DSP_PRIMARY_CORE);

	return 0;
}

int sof_mtl_set_ops(struct snd_sof_dev *sdev, struct snd_sof_dsp_ops *dsp_ops)
{
	struct sof_ipc4_fw_data *ipc4_data;

	/* common defaults */
	memcpy(dsp_ops, &sof_hda_common_ops, sizeof(struct snd_sof_dsp_ops));

	/* shutdown */
	dsp_ops->shutdown = hda_dsp_shutdown;

	/* doorbell */
	dsp_ops->irq_thread = mtl_ipc_irq_thread;

	/* ipc */
	dsp_ops->send_msg = mtl_ipc_send_msg;
	dsp_ops->get_mailbox_offset = mtl_dsp_ipc_get_mailbox_offset;
	dsp_ops->get_window_offset = mtl_dsp_ipc_get_window_offset;

	/* debug */
	dsp_ops->debug_map = mtl_dsp_debugfs;
	dsp_ops->debug_map_count = ARRAY_SIZE(mtl_dsp_debugfs);
	dsp_ops->dbg_dump = mtl_dsp_dump;
	dsp_ops->ipc_dump = mtl_ipc_dump;

	/* pre/post fw run */
	dsp_ops->pre_fw_run = mtl_dsp_pre_fw_run;
	dsp_ops->post_fw_run = mtl_dsp_post_fw_run;

	/* parse platform specific extended manifest */
	dsp_ops->parse_platform_ext_manifest = NULL;

	/* dsp core get/put */
	dsp_ops->core_get = mtl_dsp_core_get;
	dsp_ops->core_put = mtl_dsp_core_put;

	sdev->private = kzalloc(sizeof(struct sof_ipc4_fw_data), GFP_KERNEL);
	if (!sdev->private)
		return -ENOMEM;

	ipc4_data = sdev->private;
	ipc4_data->manifest_fw_hdr_offset = SOF_MAN4_FW_HDR_OFFSET;

	ipc4_data->mtrace_type = SOF_IPC4_MTRACE_INTEL_CAVS_2;

	ipc4_data->fw_context_save = true;

	/* External library loading support */
	ipc4_data->load_library = hda_dsp_ipc4_load_library;

	dsp_ops->set_power_state = hda_dsp_set_power_state_ipc4;

	/* set DAI ops */
	hda_set_dai_drv_ops(sdev, dsp_ops);

	return 0;
}
EXPORT_SYMBOL_NS(sof_mtl_set_ops, "SND_SOC_SOF_INTEL_MTL");

const struct sof_intel_dsp_desc mtl_chip_info = {
	.cores_num = 3,
	.init_core_mask = BIT(0),
	.host_managed_cores_mask = BIT(0),
	.ipc_req = MTL_DSP_REG_HFIPCXIDR,
	.ipc_req_mask = MTL_DSP_REG_HFIPCXIDR_BUSY,
	.ipc_ack = MTL_DSP_REG_HFIPCXIDA,
	.ipc_ack_mask = MTL_DSP_REG_HFIPCXIDA_DONE,
	.ipc_ctl = MTL_DSP_REG_HFIPCXCTL,
	.rom_status_reg = MTL_DSP_REG_HFFLGPXQWY,
	.rom_init_timeout	= 300,
	.ssp_count = MTL_SSP_COUNT,
	.ssp_base_offset = CNL_SSP_BASE_OFFSET,
	.sdw_shim_base = SDW_SHIM_BASE_ACE,
	.sdw_alh_base = SDW_ALH_BASE_ACE,
	.d0i3_offset = MTL_HDA_VS_D0I3C,
	.read_sdw_lcount =  hda_sdw_check_lcount_common,
	.enable_sdw_irq = mtl_enable_sdw_irq,
	.check_sdw_irq = mtl_dsp_check_sdw_irq,
	.check_sdw_wakeen_irq = hda_sdw_check_wakeen_irq_common,
	.sdw_process_wakeen = hda_sdw_process_wakeen_common,
	.check_ipc_irq = mtl_dsp_check_ipc_irq,
	.cl_init = mtl_dsp_cl_init,
	.power_down_dsp = mtl_power_down_dsp,
	.disable_interrupts = mtl_dsp_disable_interrupts,
	.hw_ip_version = SOF_INTEL_ACE_1_0,
};

const struct sof_intel_dsp_desc arl_s_chip_info = {
	.cores_num = 2,
	.init_core_mask = BIT(0),
	.host_managed_cores_mask = BIT(0),
	.ipc_req = MTL_DSP_REG_HFIPCXIDR,
	.ipc_req_mask = MTL_DSP_REG_HFIPCXIDR_BUSY,
	.ipc_ack = MTL_DSP_REG_HFIPCXIDA,
	.ipc_ack_mask = MTL_DSP_REG_HFIPCXIDA_DONE,
	.ipc_ctl = MTL_DSP_REG_HFIPCXCTL,
	.rom_status_reg = MTL_DSP_REG_HFFLGPXQWY,
	.rom_init_timeout	= 300,
	.ssp_count = MTL_SSP_COUNT,
	.ssp_base_offset = CNL_SSP_BASE_OFFSET,
	.sdw_shim_base = SDW_SHIM_BASE_ACE,
	.sdw_alh_base = SDW_ALH_BASE_ACE,
	.d0i3_offset = MTL_HDA_VS_D0I3C,
	.read_sdw_lcount =  hda_sdw_check_lcount_common,
	.enable_sdw_irq = mtl_enable_sdw_irq,
	.check_sdw_irq = mtl_dsp_check_sdw_irq,
	.check_sdw_wakeen_irq = hda_sdw_check_wakeen_irq_common,
	.sdw_process_wakeen = hda_sdw_process_wakeen_common,
	.check_ipc_irq = mtl_dsp_check_ipc_irq,
	.cl_init = mtl_dsp_cl_init,
	.power_down_dsp = mtl_power_down_dsp,
	.disable_interrupts = mtl_dsp_disable_interrupts,
	.hw_ip_version = SOF_INTEL_ACE_1_0,
};
