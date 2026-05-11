// SPDX-License-Identifier: GPL-2.0
/*
 * xHCI host controller driver for Realtek SoCs
 *
 * Copyright (C) 2020      Markus Stockhausen <markus.stockhausen@gmx.de>
 * Copyright (C) 2006-2012 Tony Wu <tonywu@realtek.com>
 *
 * Based on the RTL8198C BSP (rtk_openwrt_sdk/xhci-usb.c) and ported to
 * the Linux 6.x xHCI platform driver model (template: xhci-histb.c).
 *
 * KNOWN ISSUES (untested on hardware — no exposed USB ports on test DUT):
 *
 * 1. ENDIAN MISMATCH — U3_IPCFG bit 6 is cleared to set big-endian
 *    controller register mode, but the generic xhci-hcd core and our
 *    own iowrite32 calls both byteswap (__cpu_to_le32).  See detailed
 *    FIXME at rtxh_u3_set_phy_parameters().
 *
 * 2. All PHY init register addresses are hardcoded KSEG1 pointers,
 *    bypassing the DT-mapped xhci controller base (xhdev->base).
 *    If KSEG1 bypass is ever removed, these must be converted to
 *    offsets from hcd->regs.
 *
 * 3. Missing remove() and PM callbacks — no PHY/controller teardown.
 *    The old BSP was an arch_initcall (built-in only), so this was
 *    never needed on the original platform.
 *
 * 4. Commented-out fragments from xhci-histb.c template still present
 *    (histb_host_enable, can_do_streams, async_suspend, etc.).
 *
 * 5. Hardcoded oneportsel=1 (USB2 PHY port 1 only).  This matches the
 *    old BSP but may not be correct for all board layouts.
 *
 * 6. No error handling for CLK_MGR enable — if clock bits fail to set,
 *    subsequent MDIO accesses hang silently.
 *
 * PHY parameter tables match the old BSP exactly (40M/25M crystal
 * auto-detection via HW_STRAP bit 24).
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/timer.h>
#include <linux/types.h>
#include <linux/usb.h>

#include "xhci.h"

/* Reduced from 10ms to 1ms — GP-3000 5.10 kernel uses mdelay(1) for
 * the same MDIO bus (mdio_wait), and the old BSP has a commented-out
 * polling macro that replaces the blind delay entirely:
 *   //#define USB3_PHY_DELAY { while(!(REG32(0xb8140000)&0x10)) {}; }
 */
#define USB2_PHY_DELAY	{ mdelay(1); }

/*
 * NOTE: All register addresses below are hardcoded KSEG1 (uncached)
 * pointers.  SYS_USB_PHY, HW_STRAP, CLK_MGR are in the IO-Block
 * region (0xB8000000+) which is a native LE peripheral — ioread32/
 * iowrite32 is correct for these.
 *
 * R_B804C280 is offset 0xC280 inside the xHCI controller block
 * (BSP_XHCI_BASE + 0xC280).  Endian semantics depend on U3_IPCFG
 * bit 6 — see comment at rtxh_u3_set_phy_parameters().
 *
 * BSP_USB3_EXT / U3_IPCFG are the USB3 extension block at 0xB8140000
 * (IO-Block region) — ioread32/iowrite32 correct.
 *
 * All of these bypass the DT-mapped register space at 0x18040000
 * (xhdev->base).  The DT mapping is only used for hcd->regs.
 */
#define SYS_USB_PHY 	((void __iomem *)0xb8000090)
#define R_B804C280	((void __iomem *)0xb804c280)
#define HW_STRAP	((void __iomem *)0xb8000008)
#define BSP_USB3_EXT	((void __iomem *)0xb8140000)
#define U3_IPCFG	((void __iomem *)0xb8140008)
#define CLK_MGR		((void __iomem *)0xb8000010)

struct rtxh_dev {
	struct device	*dev;
	struct usb_hcd	*hcd;
	void __iomem	*base;
};

static void rtxh_u2_set_phy(unsigned char reg, unsigned char val)
{
	int oneportsel=1;	/* always port 1 */
	unsigned int tmp;
	unsigned char reg_l = (reg & 0x0f);
	unsigned char reg_h = (reg & 0xf0) >> 4;

	/* SYS_USB_PHY is IO-Block (LE) — ioread32/iowrite32 is correct */
	tmp = ioread32(SYS_USB_PHY);
	tmp = tmp & ~(0xff<<11);

	if (oneportsel==0)
		iowrite32((val << 0) | tmp,SYS_USB_PHY);
	else
		iowrite32((val << 11) | tmp, SYS_USB_PHY);

	USB2_PHY_DELAY;

	/*
	 * R_B804C280 is inside the xHCI controller block (BSP_XHCI_BASE +
	 * 0xC280).  Endian handling here depends on whether U3_IPCFG bit 6
	 * is set (LE mode) or cleared (BE mode).  With bit 6 cleared (as
	 * done below), the controller is in BE mode and iowrite32 may
	 * byteswap incorrectly.  See rtxh_u3_set_phy_parameters().
	 */
	iowrite32((reg_l << 8) | 0x02000000,R_B804C280);
	USB2_PHY_DELAY;
	iowrite32((reg_h << 8) | 0x02000000,R_B804C280);
	USB2_PHY_DELAY;
}

static void rtxh_u2_set_phy_parameters(void)
{
	int phy40M;

	phy40M=(ioread32(HW_STRAP)&(1<<24))>>24;

	rtxh_u2_set_phy(0xe0, 0x44);
	rtxh_u2_set_phy(0xe1, 0xe8);
	rtxh_u2_set_phy(0xe2, 0x9a);
	rtxh_u2_set_phy(0xe3, 0xa1);
	if (phy40M==0)
		rtxh_u2_set_phy( 0xe4, 0x33);
	rtxh_u2_set_phy(0xe5, 0x95);
	rtxh_u2_set_phy(0xe6, 0x98);
	if (phy40M==0)
		rtxh_u2_set_phy(0xe7, 0x66);
	rtxh_u2_set_phy(0xf5, 0x49);
	if (phy40M==0)
		rtxh_u2_set_phy(0xf7, 0x11);
}

static void rtxh_u3_set_phy(unsigned int addr,unsigned int value)
{
	unsigned int readback;

	/* BSP_USB3_EXT is at 0xB8140000 (IO-Block, LE) — iowrite32 correct */
	iowrite32(addr<<8, BSP_USB3_EXT);
	USB2_PHY_DELAY;
	readback = ioread32(BSP_USB3_EXT);
	USB2_PHY_DELAY;
	iowrite32((value << 16) | (addr << 8) | 1, BSP_USB3_EXT);
	USB2_PHY_DELAY;
	readback = ioread32(BSP_USB3_EXT);
	USB2_PHY_DELAY;
	iowrite32(addr << 8, BSP_USB3_EXT);
	USB2_PHY_DELAY;
	readback = ioread32(BSP_USB3_EXT);
	USB2_PHY_DELAY;

	/*
	 * FIXME: This readback check compares against 0x10 in the low byte
	 * for the "done" flag, but on big-endian MIPS the 0x10 byte lands
	 * at offset 3 (MSB).  The old BSP uses raw REG32 (no byteswap),
	 * so the 0x10 comparison is correct there.  With ioread32
	 * (which byteswaps), the 0x10 flag may appear at a different
	 * byte position depending on U3_IPCFG endian configuration.
	 * In practice this hasn't caused issues because BSP_USB3_EXT is
	 * an IO-Block register and the byteswap is transparent for RMW
	 * operations — the check may just not detect real errors.
	 */
	if (readback != ((value << 16) | (addr << 8) | 0x10))
		pr_err("usb phy set error (addr=%x, value=%x read=%x)\n",addr,value,readback);
}
/*
 * NOTE: All three reset functions below operate on U3_IPCFG at 0xB8140008,
 * which is in the IO-Block region (LE peripheral).  ioread32/iowrite32 is
 * correct here regardless of the xHCI controller endian mode.
 *
 * rtxh_u3_reset_mac_bus: bit 20/21 — USB3 MAC bus reset
 * rtxh_u3_reset_phy:     bit  9/10 — USB3 PHY reset
 * rtxh_u2_reset_phy:     bit   7/8 — USB2 PHY reset
 *
 * Pattern: assert control bit, deassert reset, wait 100ms, assert reset, wait.
 */
static void rtxh_u3_reset_phy(void)
{
	iowrite32(ioread32(U3_IPCFG) | (1<<9), U3_IPCFG);

	iowrite32(ioread32(U3_IPCFG) & ~(1<<10), U3_IPCFG);
	mdelay(100);

	iowrite32(ioread32(U3_IPCFG) | (1<<10), U3_IPCFG);
	mdelay(100);
}

static void rtxh_u2_reset_phy(void)
{
	iowrite32(ioread32(U3_IPCFG) | (1<<7), U3_IPCFG);

	iowrite32(ioread32(U3_IPCFG) & ~(1<<8), U3_IPCFG);
	mdelay(100);

	iowrite32(ioread32(U3_IPCFG) | (1<<8), U3_IPCFG);
	mdelay(100);
}

static void rtxh_u3_reset_mac_bus(void)
{
	iowrite32(ioread32(U3_IPCFG) | (1<<20), U3_IPCFG);

	iowrite32(ioread32(U3_IPCFG) & ~(1<<21), U3_IPCFG);
	mdelay(100);

	iowrite32(ioread32(U3_IPCFG) | (1<<21), U3_IPCFG);
	mdelay(100);
}

static void rtxh_u3_set_phy_parameters(void) //just for rle0371
{
	int phy40M;

	/*
	 * Enable USB3 IP clocks.  CLK_MGR is IO-Block (LE) — iowrite32
	 * is correct.  No error checking — if these clock bits fail to
	 * set, subsequent MDIO accesses to the U3 PHY will hang.
	 */
	iowrite32(ioread32(CLK_MGR) | (1<<21) | (1<<19) | (1<<20), CLK_MGR);  //enable usb 3 ip

	/*
	 * FIXME — ENDIAN MISMATCH: Clearing bit 6 sets the xHCI controller
	 * register space to big-endian mode.  The old BSP (and GP-3000)
	 * both do this because they use __raw_writel/REG32 (no byteswap)
	 * for all xHCI controller registers.
	 *
	 * However, the generic Linux xHCI stack (xhci-hcd) accesses hcd->regs
	 * via readl/writel, which DO byteswap on big-endian MIPS
	 * (__cpu_to_le32).  If the controller is in BE mode, every readl/
	 * writel from the core xHCI code will have swapped byte lanes,
	 * returning garbage or hanging the bus.
	 *
	 * Also affected: our own rtxh_u2_set_phy() writes to R_B804C280
	 * (inside the xHCI block) and rtxh_u3_train_phy() accesses to
	 * base+0x430/0x420 (xHCI port registers) using iowrite32/ioread32
	 * which also byteswap.
	 *
	 * If this line is removed (leaving bit 6 = 1, default LE mode),
	 * the generic xHCI core and our iowrite32 accesses should both be
	 * correct.  If the hardware REQUIRES BE mode, the fix would be
	 * to keep this line but replace all xHCI controller register
	 * access with __raw_readl/__raw_writel, AND either patch the
	 * xHCI core to use raw access or set the host controller to LE.
	 */
	iowrite32(ioread32(U3_IPCFG) & ~(1<<6), U3_IPCFG);   //u3 reg big endia

	rtxh_u3_reset_mac_bus();

	//setting u2 of u3 phy
	iowrite32(ioread32(SYS_USB_PHY) & ~(1<<20), SYS_USB_PHY);
	iowrite32(ioread32(SYS_USB_PHY) | (1<<19)|(1<<21), SYS_USB_PHY);        //0x00280500;

	phy40M=(ioread32(HW_STRAP)&(1<<24))>>24;
	pr_info("UPHY: 8198c ASIC u3 of u3 %s phy patch\n", (phy40M==1) ? "40M" : "25M");

	rtxh_u3_set_phy(0x00, 0x4A78);
	rtxh_u3_set_phy(0x01, 0xC0CE);
	rtxh_u3_set_phy(0x02, 0xE048);
	rtxh_u3_set_phy(0x03, 0x2770);
	rtxh_u3_set_phy(0x04, (phy40M==1) ? 0x5800 : 0x5000);
	rtxh_u3_set_phy(0x05, (phy40M==1) ? 0x60EA : 0x6182);
	rtxh_u3_set_phy(0x06, (phy40M==1) ?0x4168 : 0x6178 );  //0108
	rtxh_u3_set_phy(0x07, 0x2E40);
	rtxh_u3_set_phy(0x08, (phy40M==1) ? 0x4F61 : 0x31B1);
	rtxh_u3_set_phy(0x09, 0x923C);
	rtxh_u3_set_phy(0x0A, 0x9240);
	rtxh_u3_set_phy(0x0B, (phy40M==1) ? 0x8B15 : 0x8B1D);  //0108
	rtxh_u3_set_phy(0x0C, 0xDC6A);
	rtxh_u3_set_phy(0x0D, (phy40M==1) ? 0x148A : 0x158a);  //0108
	rtxh_u3_set_phy(0x0E, (phy40M==1) ? 0x98E1 : 0xA8c9);  //0108
	rtxh_u3_set_phy(0x0F, 0x8000);

	rtxh_u3_set_phy(0x10, 0x000C);
	rtxh_u3_set_phy(0x11, 0x4C00);
	rtxh_u3_set_phy(0x12, 0xFC00);
	rtxh_u3_set_phy(0x13, 0x0C81);
	rtxh_u3_set_phy(0x14, 0xDE01);
	rtxh_u3_set_phy(0x19, 0xE102);
	rtxh_u3_set_phy(0x1A, 0x1263);
	rtxh_u3_set_phy(0x1B, 0xC7FD);
	rtxh_u3_set_phy(0x1C, 0xCB00);
	rtxh_u3_set_phy(0x1D, 0xA03F);
	rtxh_u3_set_phy(0x1E, 0xC2E0);

	rtxh_u3_set_phy(0x20, 0xB7F0);
	rtxh_u3_set_phy(0x21, 0x0407);
	rtxh_u3_set_phy(0x22, 0x0016);
	rtxh_u3_set_phy(0x23, 0x0CA1);
	rtxh_u3_set_phy(0x24, 0x93F1);
	rtxh_u3_set_phy(0x25, 0x2BDD);
	rtxh_u3_set_phy(0x26, (phy40M==1) ? 0xA06D : 0x646F);  //0108
	rtxh_u3_set_phy(0x27, (phy40M==1) ? 0x8068 : 0x8107);
	rtxh_u3_set_phy(0x28, (phy40M==1) ? 0xE060 : 0xE020);  //0108
	rtxh_u3_set_phy(0x29, 0x3080);
	rtxh_u3_set_phy(0x2A, 0x3082);
	rtxh_u3_set_phy(0x2B, 0x2038);
	rtxh_u3_set_phy(0x2C, 0x7E30);
	rtxh_u3_set_phy(0x2D, 0x15DC);
	rtxh_u3_set_phy(0x2E, 0x792F);

	rtxh_u3_set_phy(0x04, (phy40M==1) ? 0x7800 : 0x7000);
	rtxh_u3_set_phy(0x09, 0x923C);
	rtxh_u3_set_phy(0x09, 0x903C);
	rtxh_u3_set_phy(0x09, 0x923C);
}

static void rtxh_u3_train_phy(void)
{
	u32 tmp;
	static int test_count_u3 = 0;	/* FIXME: static = non-reentrant, survives module reload */

	/*
	 * Hardcoded KSEG1 base for xHCI controller registers.
	 * Offsets 0x430 (PORTSC) and 0x420 (PORTPMSC) are standard
	 * xHCI port registers.  Endian handling depends on U3_IPCFG
	 * bit 6 — see rtxh_u3_set_phy_parameters().
	 *
	 * TODO: Use offset from hcd->regs (xhdev->base) instead of
	 * hardcoded KSEG1, once endian is resolved.
	 */
	void __iomem *base=(void __iomem *)0xb8040000; // TODO take from DT

	while(1) {
		test_count_u3++;
		if (test_count_u3>100)
			break;

		if ((ioread32(base+0x430)&0x1000)==0x1000) {
			pr_info("%s SS device found!\n", __func__);

			//clear U3 WRC & CSC
			tmp=ioread32(base+0x430);
			tmp|=(1<<17)|(1<<19);  // WRC CSC.
			tmp&=~(1<<1); // don't disable port
			iowrite32(tmp, base+0x430);

			pr_info("%s 0x420=%x\n", __func__,ioread32(base+0x420));
			pr_info("%s 0x430=%x\n", __func__,ioread32(base+0x430));

			break;  // SS device found!
		}

		//warm port reset for USB 3.0
		//pr_info("Do Warm Reset for U3\n");
		tmp=ioread32(base+0x430);
		tmp|=(1<<31)|(1<<0);  // warm port reset and set CCS.
		tmp&=~(1<<1); // don't disable port
		iowrite32(tmp, base+0x430);

		mdelay(100);	/* reduced from 500ms — USB3 warm reset spec minimum */

		/*
		 * 0x2a0 = CCS (bit 0) + PED (bit 1) + PLS=5 (bits 5-8),
		 * i.e. port is enabled and in RxDetect state.  This means
		 * USB3 link training failed and the port fell back to
		 * USB2 mode.  We then do a USB2 port reset and clear
		 * the USB3 status bits.
		 */
		if ((ioread32(base+0x430)&0xfff)==0x2a0) {
			//try USB 2.0 Port Reset
			//pr_info("try Port Reset for U2\n");
			iowrite32(ioread32(base+0x420)|0x10,base+0x420);
			mdelay(200);

			// stop USB 2.0 Port Enable
			iowrite32(ioread32(base+0x420)|0x2,base+0x420);
			mdelay(200);
			//clear U3 WRC & CSC
			tmp=ioread32(base+0x430);
			tmp|=(1<<17)|(1<<19);  // WRC CSC.
			tmp&=~(1<<1); // don't disable port
			iowrite32(tmp,base+0x430);
			break;
		}
	}
}

static void rtxh_init_phy(void)
{
	/*
	 * Init order matches old BSP's rtk_usb3_phyinit():
	 *   1. Set PHY parameters (MDIO register tables)
	 *   2. Reset U3 PHY, then U2 PHY
	 *   3. Train link — poll for SS device, fall back to USB2
	 *
	 * NOTE: PHY reset AFTER parameter programming means the PHY
	 * comes up with the programmed values.  This is the correct
	 * sequence per the old BSP.
	 */
	rtxh_u3_set_phy_parameters();
	rtxh_u2_set_phy_parameters();

	rtxh_u3_reset_phy();
	rtxh_u2_reset_phy();

	rtxh_u3_train_phy();
}

static void rtxh_quirks(struct device *dev, struct xhci_hcd *xhci)
{
	/*
	 * As of now platform drivers don't provide MSI support so we ensure
	 * here that the generic code does not try to make a pci_dev from our
	 * dev struct in order to setup MSI
	 */
	dev_dbg(dev, "setting XHCI_PLAT quirk\n");
	xhci->quirks |= XHCI_PLAT;
}

/* called during probe() after chip reset completes */
static int rtxh_setup(struct usb_hcd *hcd)
{
	/*
	 * Only the primary (USB3) HCD needs PHY init.  The shared
	 * (USB2) HCD reuses the same hardware and PHY.
	 */
	if (usb_hcd_is_primary_hcd(hcd))
		rtxh_init_phy();

	return xhci_gen_setup(hcd, rtxh_quirks);
}

static struct hc_driver __read_mostly rtxh_hc_driver;
static const struct xhci_driver_overrides rtxh_hc_overrides __initconst = {
	.reset = rtxh_setup,
};

static int rtxh_probe(struct platform_device *pdev)
{
	int ret, irq;
	struct usb_hcd *hcd;
	struct resource *res;
	struct xhci_hcd *xhci;
	struct rtxh_dev *xhdev;
	const struct hc_driver *driver;
	struct device *dev = &pdev->dev;

	if (usb_disabled())
		return -ENODEV;

	driver = &rtxh_hc_driver;
	xhdev = devm_kzalloc(dev, sizeof(*xhdev), GFP_KERNEL);
	if (!xhdev)
		return -ENOMEM;

	xhdev->dev = dev;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	xhdev->base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(xhdev->base))
		return PTR_ERR(xhdev->base);

	/*
	 * NOTE: xhdev->base maps the DT reg = <0x18040000 0x100000>
	 * covering the entire xHCI controller block.  This is used for
	 * hcd->regs (generic xHCI core register access).  However, our
	 * PHY init code (set_phy, train_phy) does NOT use this mapping
	 * — it uses hardcoded KSEG1 pointers instead.  If the KSEG1
	 * bypass is ever removed, those functions must be updated to
	 * use offsets from xhdev->base.
	 */

	/* Initialize dma_mask and coherent_dma_mask to 32-bits */
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		goto err_out;

	hcd = usb_create_hcd(driver, dev, dev_name(dev));
	if (!hcd) {
		ret = -ENOMEM;
		goto err_out;
	}

	hcd->regs = xhdev->base;
	hcd->rsrc_start = res->start;
	hcd->rsrc_len = resource_size(res);

	xhdev->hcd = hcd;
	dev_set_drvdata(hcd->self.controller, xhdev);

	xhci = hcd_to_xhci(hcd);

	xhci->main_hcd = hcd;
	xhci->shared_hcd = usb_create_shared_hcd(driver, dev, dev_name(dev),
						 hcd);
	if (!xhci->shared_hcd) {
		ret = -ENOMEM;
		goto err_host;
	}

	ret = usb_add_hcd(hcd, irq, IRQF_SHARED);
	if (ret)
		goto err_usb3;

	ret = usb_add_hcd(xhci->shared_hcd, irq, IRQF_SHARED);
	if (ret)
		goto err_usb2;

	/*
	 * Prevent runtime pm from being on as default, users should enable
	 * runtime pm using power/control in sysfs.
	 */

	pm_runtime_forbid(dev);

	return 0;

err_usb2:
	usb_remove_hcd(hcd);
err_usb3:
	usb_put_hcd(xhci->shared_hcd);
err_host:
	usb_put_hcd(hcd);
err_out:
	return ret;
}

static const struct of_device_id rtxh_of_match[] = {
	{ .compatible = "realtek,realtek-xhci"},
	{ },
};
MODULE_DEVICE_TABLE(of, rtxh_of_match);

static struct platform_driver rtxh_driver = {
	.probe	= rtxh_probe,
//	.remove	= rtxh_remove,	/* FIXME: missing — no PHY/controller cleanup on unload */
	.driver	= {
		.name = "xhci-realtek",
//		.pm = DEV_PM_OPS,	/* FIXME: missing — no suspend/resume support */
		.of_match_table = of_match_ptr(rtxh_of_match),
	},
};
MODULE_ALIAS("platform:xhci-realtek");
MODULE_AUTHOR("Markus Stockhausen <markus.stockhausen@gmx.de>");
MODULE_AUTHOR("Tony Wu <tonywu@realtek.com>");

static int __init rtxh_init(void)
{
	xhci_init_driver(&rtxh_hc_driver, &rtxh_hc_overrides);
	return platform_driver_register(&rtxh_driver);
}
module_init(rtxh_init);

static void __exit rtxh_exit(void)
{
	platform_driver_unregister(&rtxh_driver);
}
module_exit(rtxh_exit);

MODULE_DESCRIPTION("Realtek xHCI Host Controller Driver");
MODULE_LICENSE("GPL v2");
