/*
 *  sata_sis.c - Silicon Integrated Systems SATA
 *
 *  Maintained by:  Uwe Koziolek
 *  		    Please ALWAYS copy linux-ide@vger.kernel.org
 *		    on emails.
 *
 *  Copyright 2004 Uwe Koziolek
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *
 *  libata documentation is available via 'make {ps|pdf}docs',
 *  as Documentation/DocBook/libata.*
 *
 *  Hardware documentation available under NDA.
 *
 */

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include "scsi.h"
#include <scsi/scsi_host.h>
#include <linux/libata.h>

#define DRV_NAME	"sata_sis"
#define DRV_VERSION	"0.5"

enum {
	sis_180			= 0,
	SIS_SCR_PCI_BAR		= 5,

	/* PCI configuration registers */
	SIS_GENCTL		= 0x54, /* IDE General Control register */
	SIS_SCR_BASE		= 0xc0, /* sata0 phy SCR registers */
	SIS180_SATA1_OFS	= 0x10, /* offset from sata0->sata1 phy regs */
	SIS182_SATA1_OFS	= 0x20, /* offset from sata0->sata1 phy regs */
	SIS_PMR			= 0x90, /* port mapping register */
	SIS_PMR_COMBINED	= 0x30,

	/* random bits */
	SIS_FLAG_CFGSCR		= (1 << 30), /* host flag: SCRs via PCI cfg */

	GENCTL_IOMAPPED_SCR	= (1 << 26), /* if set, SCRs are in IO space */
};

static int sis_init_one (struct pci_dev *pdev, const struct pci_device_id *ent);
static u32 sis_scr_read (struct ata_port *ap, unsigned int sc_reg);
static void sis_scr_write (struct ata_port *ap, unsigned int sc_reg, u32 val);

static const struct pci_device_id sis_pci_tbl[] = {
	{ PCI_VENDOR_ID_SI, 0x180, PCI_ANY_ID, PCI_ANY_ID, 0, 0, sis_180 },
	{ PCI_VENDOR_ID_SI, 0x181, PCI_ANY_ID, PCI_ANY_ID, 0, 0, sis_180 },
	{ PCI_VENDOR_ID_SI, 0x182, PCI_ANY_ID, PCI_ANY_ID, 0, 0, sis_180 },
	{ }	/* terminate list */
};


static struct pci_driver sis_pci_driver = {
	.name			= DRV_NAME,
	.id_table		= sis_pci_tbl,
	.probe			= sis_init_one,
	.remove			= ata_pci_remove_one,
};

static Scsi_Host_Template sis_sht = {
	.module			= THIS_MODULE,
	.name			= DRV_NAME,
	.detect			= ata_scsi_detect,
	.release		= ata_scsi_release,
	.ioctl			= ata_scsi_ioctl,
	.queuecommand		= ata_scsi_queuecmd,
	.eh_strategy_handler	= ata_scsi_error,
	.can_queue		= ATA_DEF_QUEUE,
	.this_id		= ATA_SHT_THIS_ID,
	.sg_tablesize		= ATA_MAX_PRD,
	.max_sectors		= ATA_MAX_SECTORS,
	.cmd_per_lun		= ATA_SHT_CMD_PER_LUN,
	.use_new_eh_code	= ATA_SHT_NEW_EH_CODE,
	.emulated		= ATA_SHT_EMULATED,
	.use_clustering		= ATA_SHT_USE_CLUSTERING,
	.proc_name		= DRV_NAME,
	.bios_param		= ata_std_bios_param,
};

static const struct ata_port_operations sis_ops = {
	.port_disable		= ata_port_disable,
	.tf_load		= ata_tf_load,
	.tf_read		= ata_tf_read,
	.check_status		= ata_check_status,
	.exec_command		= ata_exec_command,
	.dev_select		= ata_std_dev_select,
	.phy_reset		= sata_phy_reset,
	.bmdma_setup            = ata_bmdma_setup,
	.bmdma_start            = ata_bmdma_start,
	.bmdma_stop		= ata_bmdma_stop,
	.bmdma_status		= ata_bmdma_status,
	.qc_prep		= ata_qc_prep,
	.qc_issue		= ata_qc_issue_prot,
	.eng_timeout		= ata_eng_timeout,
	.irq_handler		= ata_interrupt,
	.irq_clear		= ata_bmdma_irq_clear,
	.scr_read		= sis_scr_read,
	.scr_write		= sis_scr_write,
	.port_start		= ata_port_start,
	.port_stop		= ata_port_stop,
	.host_stop		= ata_host_stop,
};

static struct ata_port_info sis_port_info = {
	.sht		= &sis_sht,
	.host_flags	= ATA_FLAG_SATA | ATA_FLAG_SATA_RESET |
			  ATA_FLAG_NO_LEGACY,
	.pio_mask	= 0x1f,
	.mwdma_mask	= 0x7,
	.udma_mask	= 0x7f,
	.port_ops	= &sis_ops,
};


MODULE_AUTHOR("Uwe Koziolek");
MODULE_DESCRIPTION("low-level driver for Silicon Integratad Systems SATA controller");
MODULE_LICENSE("GPL");
MODULE_DEVICE_TABLE(pci, sis_pci_tbl);
MODULE_VERSION(DRV_VERSION);

static unsigned int get_scr_cfg_addr(unsigned int port_no, unsigned int sc_reg, int device)
{
	unsigned int addr = SIS_SCR_BASE + (4 * sc_reg);

	if (port_no)  {
		if (device == 0x182)
			addr += SIS182_SATA1_OFS;
		else
			addr += SIS180_SATA1_OFS;
	}

	return addr;
}

static u32 sis_scr_cfg_read (struct ata_port *ap, unsigned int sc_reg)
{
	struct pci_dev *pdev = to_pci_dev(ap->host_set->dev);
	unsigned int cfg_addr = get_scr_cfg_addr(ap->port_no, sc_reg, pdev->device);
	u32 val, val2 = 0;
	u8 pmr;

	if (sc_reg == SCR_ERROR) /* doesn't exist in PCI cfg space */
		return 0xffffffff;

	pci_read_config_byte(pdev, SIS_PMR, &pmr);

	pci_read_config_dword(pdev, cfg_addr, &val);

	if ((pdev->device == 0x182) || (pmr & SIS_PMR_COMBINED))
		pci_read_config_dword(pdev, cfg_addr+0x10, &val2);

	return val|val2;
}

static void sis_scr_cfg_write (struct ata_port *ap, unsigned int scr, u32 val)
{
	struct pci_dev *pdev = to_pci_dev(ap->host_set->dev);
	unsigned int cfg_addr = get_scr_cfg_addr(ap->port_no, scr, pdev->device);
	u8 pmr;

	if (scr == SCR_ERROR) /* doesn't exist in PCI cfg space */
		return;

	pci_read_config_byte(pdev, SIS_PMR, &pmr);

	pci_write_config_dword(pdev, cfg_addr, val);

	if ((pdev->device == 0x182) || (pmr & SIS_PMR_COMBINED))
		pci_write_config_dword(pdev, cfg_addr+0x10, val);
}

static u32 sis_scr_read (struct ata_port *ap, unsigned int sc_reg)
{
	struct pci_dev *pdev = to_pci_dev(ap->host_set->dev);
	u32 val, val2 = 0;
	u8 pmr;

	if (sc_reg > SCR_CONTROL)
		return 0xffffffffU;

	if (ap->flags & SIS_FLAG_CFGSCR)
		return sis_scr_cfg_read(ap, sc_reg);

	pci_read_config_byte(pdev, SIS_PMR, &pmr);

	val = inl(ap->ioaddr.scr_addr + (sc_reg * 4));

	if ((pdev->device == 0x182) || (pmr & SIS_PMR_COMBINED))
		val2 = inl(ap->ioaddr.scr_addr + (sc_reg * 4) + 0x10);

	return val | val2;
}

static void sis_scr_write (struct ata_port *ap, unsigned int sc_reg, u32 val)
{
	struct pci_dev *pdev = to_pci_dev(ap->host_set->dev);
	u8 pmr;

	if (sc_reg > SCR_CONTROL)
		return;

	pci_read_config_byte(pdev, SIS_PMR, &pmr);

	if (ap->flags & SIS_FLAG_CFGSCR)
		sis_scr_cfg_write(ap, sc_reg, val);
	else {
		outl(val, ap->ioaddr.scr_addr + (sc_reg * 4));
		if ((pdev->device == 0x182) || (pmr & SIS_PMR_COMBINED))
			outl(val, ap->ioaddr.scr_addr + (sc_reg * 4)+0x10);
	}
}

static int sis_init_one (struct pci_dev *pdev, const struct pci_device_id *ent)
{
	static int printed_version;
	struct ata_probe_ent *probe_ent = NULL;
	int rc;
	u32 genctl;
	struct ata_port_info *ppi;
	int pci_dev_busy = 0;
	u8 pmr;
	u8 port2_start;

	if (!printed_version++)
		pdev_printk(KERN_INFO, pdev, "version " DRV_VERSION "\n");

	rc = pci_enable_device(pdev);
	if (rc)
		return rc;

	rc = pci_request_regions(pdev, DRV_NAME);
	if (rc) {
		pci_dev_busy = 1;
		goto err_out;
	}

	rc = pci_set_dma_mask(pdev, ATA_DMA_MASK);
	if (rc)
		goto err_out_regions;
	rc = pci_set_consistent_dma_mask(pdev, ATA_DMA_MASK);
	if (rc)
		goto err_out_regions;

	ppi = &sis_port_info;
	probe_ent = ata_pci_init_native_mode(pdev, &ppi, ATA_PORT_PRIMARY | ATA_PORT_SECONDARY);
	if (!probe_ent) {
		rc = -ENOMEM;
		goto err_out_regions;
	}

	/* check and see if the SCRs are in IO space or PCI cfg space */
	pci_read_config_dword(pdev, SIS_GENCTL, &genctl);
	if ((genctl & GENCTL_IOMAPPED_SCR) == 0)
		probe_ent->host_flags |= SIS_FLAG_CFGSCR;

	/* if hardware thinks SCRs are in IO space, but there are
	 * no IO resources assigned, change to PCI cfg space.
	 */
	if ((!(probe_ent->host_flags & SIS_FLAG_CFGSCR)) &&
	    ((pci_resource_start(pdev, SIS_SCR_PCI_BAR) == 0) ||
	     (pci_resource_len(pdev, SIS_SCR_PCI_BAR) < 128))) {
		genctl &= ~GENCTL_IOMAPPED_SCR;
		pci_write_config_dword(pdev, SIS_GENCTL, genctl);
		probe_ent->host_flags |= SIS_FLAG_CFGSCR;
	}

	pci_read_config_byte(pdev, SIS_PMR, &pmr);
	if (ent->device != 0x182) {
		if ((pmr & SIS_PMR_COMBINED) == 0) {
			pdev_printk(KERN_INFO, pdev,
				   "Detected SiS 180/181 chipset in SATA mode\n");
			port2_start = 64;
		}
		else {
			pdev_printk(KERN_INFO, pdev,
				   "Detected SiS 180/181 chipset in combined mode\n");
			port2_start=0;
		}
	}
	else {
		pdev_printk(KERN_INFO, pdev, "Detected SiS 182 chipset\n");
		port2_start = 0x20;
	}

	if (!(probe_ent->host_flags & SIS_FLAG_CFGSCR)) {
		probe_ent->port[0].scr_addr =
			pci_resource_start(pdev, SIS_SCR_PCI_BAR);
		probe_ent->port[1].scr_addr =
			pci_resource_start(pdev, SIS_SCR_PCI_BAR) + port2_start;
	}

	pci_set_master(pdev);
	pci_intx(pdev, 1);

	ata_add_to_probe_list(probe_ent);

	return 0;

err_out_regions:
	pci_release_regions(pdev);

err_out:
	if (!pci_dev_busy)
		pci_disable_device(pdev);
	return rc;

}

static int __init sis_init(void)
{
	int rc = pci_module_init(&sis_pci_driver);
	if (rc)
		return rc;

	rc = scsi_register_module(MODULE_SCSI_HA, &sis_sht);
	if (rc) {
		pci_unregister_driver(&sis_pci_driver);
		return -ENODEV;
	}

	return 0;
}

static void __exit sis_exit(void)
{
	scsi_unregister_module(MODULE_SCSI_HA, &sis_sht);
	pci_unregister_driver(&sis_pci_driver);
}

module_init(sis_init);
module_exit(sis_exit);
