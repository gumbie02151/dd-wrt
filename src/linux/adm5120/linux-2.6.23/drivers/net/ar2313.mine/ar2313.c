/*
 * ar2313.c: Linux driver for the Atheros AR231x Ethernet device.
 *
 * Copyright (C) 2004 by Sameer Dekate <sdekate@arubanetworks.com>
 * Copyright (C) 2006-2007 Imre Kaloz <kaloz@openwrt.org>
 * Copyright (C) 2006-2007 Felix Fietkau <nbd@openwrt.org>
 * Copyright (C) 2007 Tomas Dlabac <tomas@dlabac.net>
 * Copyright (C) 2008 Sebastian Gottschall <s.gottschall@newmedia-net.de> (vlan and phy fixes)
 *
 * Thanks to Atheros for providing hardware and documentation
 * enabling me to write this driver.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Additional credits:
 * 	This code is taken from John Taylor's Sibyte driver and then 
 * 	modified for the AR2313.
 */

#include <linux/autoconf.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/ioport.h>
#include <linux/pci.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/sockios.h>
#include <linux/pkt_sched.h>
#include <linux/compile.h>
#include <linux/mii.h>
#include <linux/ethtool.h>
#include <linux/ctype.h>
#include <linux/platform_device.h>
#include <linux/if_vlan.h>

#include <net/sock.h>
#include <net/ip.h>

#include <asm/system.h>
#include <asm/io.h>
#include <asm/irq.h>
#include <asm/byteorder.h>
#include <asm/uaccess.h>
#include <asm/bootinfo.h>

#define AR2313_MTU                     1692
#define AR2313_PRIOS                   1
#define AR2313_QUEUES                  (2*AR2313_PRIOS)
#define AR2313_DESCR_ENTRIES           64

#undef INDEX_DEBUG
#define DEBUG     0
#define DEBUG_TX  0
#define DEBUG_RX  0
#define DEBUG_INT 0
#define DEBUG_MC  0
#define DEBUG_ERR 1

#ifndef min
#define min(a,b)	(((a)<(b))?(a):(b))
#endif

#ifndef SMP_CACHE_BYTES
#define SMP_CACHE_BYTES	L1_CACHE_BYTES
#endif

#define AR2313_MBOX_SET_BIT  0x8

#define BOARD_IDX_STATIC	0
#define BOARD_IDX_OVERFLOW	-1

#include "dma.h"
#include "ar2313.h"
#include "mvPhy.h"
#include "ipPhy.h"
#include "admPhy.h"
/*
 * New interrupt handler strategy:
 *
 * An old interrupt handler worked using the traditional method of
 * replacing an skbuff with a new one when a packet arrives. However
 * the rx rings do not need to contain a static number of buffer
 * descriptors, thus it makes sense to move the memory allocation out
 * of the main interrupt handler and do it in a bottom half handler
 * and only allocate new buffers when the number of buffers in the
 * ring is below a certain threshold. In order to avoid starving the
 * NIC under heavy load it is however necessary to force allocation
 * when hitting a minimum threshold. The strategy for alloction is as
 * follows:
 *
 *     RX_LOW_BUF_THRES    - allocate buffers in the bottom half
 *     RX_PANIC_LOW_THRES  - we are very low on buffers, allocate
 *                           the buffers in the interrupt handler
 *     RX_RING_THRES       - maximum number of buffers in the rx ring
 *
 * One advantagous side effect of this allocation approach is that the
 * entire rx processing can be done without holding any spin lock
 * since the rx rings and registers are totally independent of the tx
 * ring and its registers.  This of course includes the kmalloc's of
 * new skb's. Thus start_xmit can run in parallel with rx processing
 * and the memory allocation on SMP systems.
 *
 * Note that running the skb reallocation in a bottom half opens up
 * another can of races which needs to be handled properly. In
 * particular it can happen that the interrupt handler tries to run
 * the reallocation while the bottom half is either running on another
 * CPU or was interrupted on the same CPU. To get around this the
 * driver uses bitops to prevent the reallocation routines from being
 * reentered.
 *
 * TX handling can also be done without holding any spin lock, wheee
 * this is fun! since tx_csm is only written to by the interrupt
 * handler.
 */

/*
 * Threshold values for RX buffer allocation - the low water marks for
 * when to start refilling the rings are set to 75% of the ring
 * sizes. It seems to make sense to refill the rings entirely from the
 * intrrupt handler once it gets below the panic threshold, that way
 * we don't risk that the refilling is moved to another CPU when the
 * one running the interrupt handler just got the slab code hot in its
 * cache.
 */
#define RX_RING_SIZE		AR2313_DESCR_ENTRIES
#define RX_PANIC_THRES	        (RX_RING_SIZE/4)
#define RX_LOW_THRES	        ((3*RX_RING_SIZE)/4)
#define CRC_LEN                 4
#define RX_OFFSET               2


#if defined(CONFIG_AR2313_VLAN)
#define DO_VLAN 1
#define VLAN_HDR			(4)
#else
#define VLAN_HDR			(0)
#endif


#define AR2313_BUFSIZE		(AR2313_MTU + ETH_HLEN + CRC_LEN + RX_OFFSET + VLAN_HDR)

#ifdef MODULE
MODULE_AUTHOR
  ("Sameer Dekate <sdekate@arubanetworks.com>, Imre Kaloz <kaloz@openwrt.org>, Felix Fietkau <nbd@openwrt.org>, Tomas Dlabac <tomas@dlabac.net>, Sebastian Gottschall <brainslayer@dd-wrt.com>");
MODULE_DESCRIPTION ("AR2313 Ethernet driver");
MODULE_LICENSE ("GPL");
#endif

#define virt_to_phys(x) ((u32)(x) & 0x1fffffff)

// prototypes
static short armiiread (struct net_device *dev, short phy, short reg);
static void armiiwrite (struct net_device *dev, short phy, short reg,
			short data);

#ifdef TX_TIMEOUT
static void ar2313_tx_timeout (struct net_device *dev);
#endif
static void ar2313_halt (struct net_device *dev);
static void ar2313_multicast_list (struct net_device *dev);
static void ar2313_tx_int(struct net_device *dev);
static int ar2313_rx(struct net_device *dev, int budget);

#ifndef ERR
#define ERR(fmt, args...) printk("%s: " fmt, __func__, ##args)
#endif


#ifdef DO_VLAN
/*
 * Marvell switches are too braindead to do real VLAN,
 * so we have to deal with their custom crap here...
 */

static int marvell_find_vlan(struct ar2313_private *sp,
							 struct sk_buff *skb)
{
	unsigned char *buf = skb->data + skb->len - 4;
	int ret = -1;

	/* Is this a Marvell trailer? */
//	if (*buf != 0x80)
//		return 0;

	/* FIXME: ugly, ugly hack! */
//	printk(KERN_EMERG "packet is %d\n",buf[1]);
	switch (buf[1]) {
#ifdef CONFIG_MTD_AR531X
	case 0:					/* Packet came from the WAN port */
#else
	case 4:					/* Packet came from the WAN port */
#endif

		ret = 2;
		break;
	default:					/* Packet probably came from LAN */
		ret = 1;
		break;
	}

	return ret;
}

static struct sk_buff *marvell_add_vlan(struct ar2313_private *sp,
										struct sk_buff *skb)
{
	u8 *buf = NULL;
	struct sk_buff *newskb;
	u8 vid;

	if (unlikely(skb->len < 16))
		return skb;

	vid = skb->data[15];

	/* XXX: clean up this garbage! */
	if (skb->len <= 64) {
		newskb = skb_copy_expand(skb, skb_headroom(skb), 68, GFP_ATOMIC);
		if (!newskb) {
			if (net_ratelimit())
				printk("%s: failed to expand skb!\n", sp->dev->name);
			goto done;
		}
		dev_kfree_skb(skb);
		skb = newskb;
		buf = skb->data + 64;
		skb->len = 68;
		goto tag_move;
	}
	
	if (unlikely(skb_tailroom(skb) < 4)) {
		/* not enough tailroom:
		 * remove the vlan tag by closing the gap between the ethernet header
		 * and the rest of the packet */
		memmove(skb->data + 12, skb->data + 16, skb->len - 16);
//		skb->network_header-=4;
		skb_set_network_header(skb,-4);
		buf = skb->data + skb->len - 4;
		goto tag_append;
	}

tag_move:
	/* move the ethernet header 4 bytes forward, overwriting the vlan tag */
	memmove(skb->data + 4, skb->data, 12);
	skb_set_mac_header(skb,4);
//	skb->mac_header+=4;
//	skb->mac.raw += 4;
	skb->data += 4;
	skb->len -= 4;
	
tag_append:
	buf = (buf ?: skb_put(skb, 4));
	if (!buf)
		return skb;

	*((u32 *) buf) = (vid==2) ? cpu_to_be32(
		(0x80 << 24) |
#ifdef CONFIG_MTD_AR531X
		(0x1 << 16)
#else
		(0x10 << 16)
#endif

	) : 0; 

done:
	return skb;
}

static void ar2313_vlan_rx_register(struct net_device *dev,
									struct vlan_group *grp)
{
	struct ar2313_private *sp = (struct ar2313_private *) dev->priv;

	sp->vlgrp = grp;
}

static void ar2313_vlan_rx_kill_vid(struct net_device *dev,
									unsigned short vid)
{
	struct ar2313_private *sp = (struct ar2313_private *) dev->priv;

	if (sp->vlgrp)
	        vlan_group_set_device(sp->vlgrp, vid, NULL);
}
#endif

int
ae2313_poll(struct net_device *dev, int *budget)
{
	struct ar2313_private *sp = dev->priv;
	unsigned long flags;
	int done;
	int orig_budget = *budget;
	int work_done;

	sp->dma_regs->intr_ena &= ~(DMA_STATUS_RI | DMA_STATUS_TI);
	if (sp->unloading) {
		return 1;
	}

	spin_lock_irqsave(&sp->lock, flags);
	ar2313_tx_int(dev);
       	spin_unlock_irqrestore(&sp->lock, flags);

	/* run RX thread, within the bounds set by NAPI.
	 * All RX "locking" is done by ensuring outside
	 * code synchronizes with dev->poll()
	 */
	done = 1;
       	if (orig_budget > dev->quota)
       		orig_budget = dev->quota;

       	work_done = ar2313_rx(dev, orig_budget);

       	*budget -= work_done;
       	dev->quota -= work_done;

       	if (work_done >= orig_budget)
       		done = 0;

	/* if no more work, tell net stack and NIC we're done */
	if (done) {
		spin_lock_irqsave(&sp->lock, flags);
		__netif_rx_complete(dev);
		sp->dma_regs->intr_ena |= (DMA_STATUS_RI | DMA_STATUS_TI);
		spin_unlock_irqrestore(&sp->lock, flags);
	}

	return (done ? 0 : 1);
}


int __init
ar2313_probe (struct platform_device *pdev)
{
  struct net_device *dev;
  struct ar2313_private *sp;
  struct resource *res;
  unsigned long ar_eth_base;
  char buf[64];

  dev = alloc_etherdev (sizeof (struct ar2313_private));

  if (dev == NULL)
    {
      printk (KERN_ERR "ar2313: Unable to allocate net_device structure!\n");
      return -ENOMEM;
    }

  SET_MODULE_OWNER (dev);
  platform_set_drvdata (pdev, dev);

  sp = dev->priv;
  sp->dev = dev;
  sp->cfg = pdev->dev.platform_data;

  sprintf (buf, "eth%d_membase", pdev->id);
  res = platform_get_resource_byname (pdev, IORESOURCE_MEM, buf);
  if (!res)
    return -ENODEV;

  sp->eth_phy = AR2313_EPHY_UNKNOWN;
  sp->link = 0;
  ar_eth_base = res->start;
  sp->phy = sp->cfg->phy;
  sprintf (buf, "eth%d_irq", pdev->id);
  dev->irq = platform_get_irq_byname (pdev, buf);

  spin_lock_init (&sp->lock);

  /* initialize func pointers */
  dev->open = &ar2313_open;
  dev->stop = &ar2313_close;
  dev->hard_start_xmit = &ar2313_start_xmit;

  dev->get_stats = &ar2313_get_stats;
  dev->set_multicast_list = &ar2313_multicast_list;
#ifdef TX_TIMEOUT
  dev->tx_timeout = ar2313_tx_timeout;
  dev->watchdog_timeo = AR2313_TX_TIMEOUT;
#endif
  dev->do_ioctl = &ar2313_ioctl;

  // SAMEER: do we need this?
  dev->features |= NETIF_F_SG | NETIF_F_HIGHDMA;

  dev->poll = ae2313_poll;
  dev->weight = 64;

  sp->eth_regs =
    ioremap_nocache (virt_to_phys (ar_eth_base), sizeof (*sp->eth_regs));
  if (!sp->eth_regs)
    {
      printk ("Can't remap eth registers\n");
      return (-ENXIO);
    }

  /* 
   * When there's only one MAC, PHY regs are typically on ENET0, 
   * even though the MAC might be on ENET1.
   * Needto remap PHY regs separately in this case
   */
  if (virt_to_phys (ar_eth_base) == virt_to_phys (sp->phy_regs))
    sp->phy_regs = sp->eth_regs;
  else
    {
      sp->phy_regs =
	ioremap_nocache (virt_to_phys (sp->cfg->phy_base),
			 sizeof (*sp->phy_regs));
      if (!sp->phy_regs)
	{
	  printk ("Can't remap phy registers\n");
	  return (-ENXIO);
	}
    }

  sp->dma_regs =
    ioremap_nocache (virt_to_phys (ar_eth_base + 0x1000),
		     sizeof (*sp->dma_regs));
  dev->base_addr = (unsigned int) sp->dma_regs;
  if (!sp->dma_regs)
    {
      printk ("Can't remap DMA registers\n");
      return (-ENXIO);
    }

  sp->int_regs = ioremap_nocache (virt_to_phys (sp->cfg->reset_base), 4);
  if (!sp->int_regs)
    {
      printk ("Can't remap INTERRUPT registers\n");
      return (-ENXIO);
    }

  strncpy (sp->name, "Atheros AR231x", sizeof (sp->name) - 1);
  sp->name[sizeof (sp->name) - 1] = '\0';
  memcpy (dev->dev_addr, sp->cfg->macaddr, 6);
  sp->board_idx = BOARD_IDX_STATIC;

  if (ar2313_init (dev))
    {
      /* 
       * ar2313_init() calls ar2313_init_cleanup() on error.
       */
      kfree (dev);
      return -ENODEV;
    }

    printk("PHY ID: %04x:%04x\n", (u16) armiiread(dev, 0x1f, 1),
		   (u16) armiiread(dev, 0x1f, 2));

#ifndef CONFIG_MTD_AR531X
#if DO_VLAN
	if (sp->eth_phy == AR2313_EPHY_MARVELL) {
		dev->features |= NETIF_F_HW_VLAN_RX;
		dev->vlan_rx_register = ar2313_vlan_rx_register;
		dev->vlan_rx_kill_vid = ar2313_vlan_rx_kill_vid;
	}
#endif
#endif

  if (register_netdev (dev))
    {
      printk ("%s: register_netdev failed\n", __func__);
      return -1;
    }

  printk ("%s: %s: %02x:%02x:%02x:%02x:%02x:%02x, irq %d\n",
	  dev->name, sp->name,
	  dev->dev_addr[0], dev->dev_addr[1], dev->dev_addr[2],
	  dev->dev_addr[3], dev->dev_addr[4], dev->dev_addr[5], dev->irq);

	if (sp->eth_phy == AR2313_EPHY_MARVELL){
		int i;

		printk("Initialising Marvell switch... ");

		/* reset chip */
		armiiwrite(dev, 0x1f, 0xa, 0xa130);
		int cnt=0;
		do {
			udelay(1000);
			i = armiiread(dev, sp->phy, 0xa);
		cnt++;
		} while (i & 0x8000 && cnt<2);

		int atuControl  = MV_ATUCTRL_AGE_TIME_DEFAULT << MV_ATUCTRL_AGE_TIME_SHIFT;
		atuControl |= MV_ATUCTRL_ATU_SIZE_DEFAULT << MV_ATUCTRL_ATU_SIZE_SHIFT;
		armiiwrite(dev, MV_SWITCH_GLOBAL_ADDR, MV_ATU_CONTROL, atuControl);
		/* configure MAC address */
		armiiwrite(dev, sp->phy, 0x1,
				   dev->dev_addr[0] << 8 | dev->dev_addr[1]);
		armiiwrite(dev, sp->phy, 0x2,
				   dev->dev_addr[2] << 8 | dev->dev_addr[3]);
		armiiwrite(dev, sp->phy, 0x3,
				   dev->dev_addr[4] << 8 | dev->dev_addr[5]);

		/* set ports to forwarding */
		armiiwrite(dev, 0x18, 0x4, 0x3);	/* port 0 */
		armiiwrite(dev, 0x19, 0x4, 0x3);	/* port 1 */
		armiiwrite(dev, 0x1a, 0x4, 0x3);	/* port 2 */
		armiiwrite(dev, 0x1b, 0x4, 0x3);	/* port 3 */
		armiiwrite(dev, 0x1c, 0x4, 0x3);	/* port 4 - WAN */

		/* put ports into vlans */
#ifdef CONFIG_MTD_AR531X
		armiiwrite(dev, 0x1d, 0x4, 0x3);	/* port 5 - connected to CPU */
//		armiiwrite(dev, 0x1d, 0x4, 0x4103);	/* port 5 - connected to CPU */
//		armiiwrite(dev, 0x18, 0x6, 0x1020);	/* port 0 */
//		armiiwrite(dev, 0x19, 0x6, 0x3c);	/* port 1 */
//		armiiwrite(dev, 0x1a, 0x6, 0x3a);	/* port 2 */
//		armiiwrite(dev, 0x1b, 0x6, 0x36);	/* port 3 */
//		armiiwrite(dev, 0x1c, 0x6, 0x2e);	/* port 4 - WAN */
//		armiiwrite(dev, 0x1d, 0x6, 0x0f);	/* port 5 - connected to CPU */


		armiiwrite(dev, 0x18, 0x6, 0x3e);	/* port 0 */
		armiiwrite(dev, 0x19, 0x6, 0x3d);	/* port 1 */
		armiiwrite(dev, 0x1a, 0x6, 0x3b);	/* port 2 */
		armiiwrite(dev, 0x1b, 0x6, 0x37);	/* port 3 */
		armiiwrite(dev, 0x1c, 0x6, 0x2f);	/* port 4 - WAN */
		armiiwrite(dev, 0x1d, 0x6, 0x1f);	/* port 5 - connected to CPU */

#else
		armiiwrite(dev, 0x1d, 0x4, 0x4103);	/* port 5 - connected to CPU */
		armiiwrite(dev, 0x18, 0x6, 0x2e);	/* port 0 */
		armiiwrite(dev, 0x19, 0x6, 0x2d);	/* port 1 */
		armiiwrite(dev, 0x1a, 0x6, 0x2b);	/* port 2 */
		armiiwrite(dev, 0x1b, 0x6, 0x27);	/* port 3 */
		armiiwrite(dev, 0x1c, 0x6, 0x1020);	/* port 4 - WAN */
		armiiwrite(dev, 0x1d, 0x6, 0x0f);	/* port 5 - connected to CPU */
#endif

		/* hmz */
		for (i = 0; i <= 5; i++)
			armiiwrite(dev, 0x18 + i, 11, 1 << i);

		printk("done.\n");
	}

  /* start link poll timer */
  ar2313_setup_timer (dev);

  return 0;
}

#if 0
static void
ar2313_dump_regs (struct net_device *dev)
{
  unsigned int *ptr, i;
  struct ar2313_private *sp = (struct ar2313_private *) dev->priv;

  ptr = (unsigned int *) sp->eth_regs;
  for (i = 0; i < (sizeof (ETHERNET_STRUCT) / sizeof (unsigned int));
       i++, ptr++)
    {
      printk ("ENET: %08x = %08x\n", (int) ptr, *ptr);
    }

  ptr = (unsigned int *) sp->dma_regs;
  for (i = 0; i < (sizeof (DMA) / sizeof (unsigned int)); i++, ptr++)
    {
      printk ("DMA: %08x = %08x\n", (int) ptr, *ptr);
    }

  ptr = (unsigned int *) sp->int_regs;
  for (i = 0; i < (sizeof (INTERRUPT) / sizeof (unsigned int)); i++, ptr++)
    {
      printk ("INT: %08x = %08x\n", (int) ptr, *ptr);
    }

  for (i = 0; i < AR2313_DESCR_ENTRIES; i++)
    {
      ar2313_descr_t *td = &sp->tx_ring[i];
      printk ("Tx desc %2d: %08x %08x %08x %08x\n", i,
	      td->status, td->devcs, td->addr, td->descr);
    }
}
#endif

#ifdef TX_TIMEOUT
static void
ar2313_tx_timeout (struct net_device *dev)
{
  struct ar2313_private *sp = (struct ar2313_private *) dev->priv;
  unsigned long flags;

#if DEBUG_TX
  printk ("Tx timeout\n");
#endif
  spin_lock_irqsave (&sp->lock, flags);
  ar2313_restart (dev);
  spin_unlock_irqrestore (&sp->lock, flags);
}
#endif

#if DEBUG_MC
static void
printMcList (struct net_device *dev)
{
  struct dev_mc_list *list = dev->mc_list;
  int num = 0, i;
  while (list)
    {
      printk ("%d MC ADDR ", num);
      for (i = 0; i < list->dmi_addrlen; i++)
	{
	  printk (":%02x", list->dmi_addr[i]);
	}
      list = list->next;
      printk ("\n");
    }
}
#endif

/*
 * Set or clear the multicast filter for this adaptor.
 * THIS IS ABSOLUTE CRAP, disabled
 */
static void
ar2313_multicast_list (struct net_device *dev)
{
  /* 
   * Always listen to broadcasts and 
   * treat IFF bits independently 
   */
  struct ar2313_private *sp = (struct ar2313_private *) dev->priv;
  unsigned int recognise;

  recognise = sp->eth_regs->mac_control;

  if (dev->flags & IFF_PROMISC)
    {				/* set promiscuous mode */
      recognise |= MAC_CONTROL_PR;
    }
  else
    {
      recognise &= ~MAC_CONTROL_PR;
    }

  if ((dev->flags & IFF_ALLMULTI) || (dev->mc_count > 15))
    {
#if DEBUG_MC
      printMcList (dev);
      printk ("%s: all MULTICAST mc_count %d\n", __FUNCTION__, dev->mc_count);
#endif
      recognise |= MAC_CONTROL_PM;	/* all multicast */
    }
  else if (dev->mc_count > 0)
    {
#if DEBUG_MC
      printMcList (dev);
      printk ("%s: mc_count %d\n", __FUNCTION__, dev->mc_count);
#endif
      recognise |= MAC_CONTROL_PM;	/* for the time being */
    }
#if DEBUG_MC
  printk ("%s: setting %08x to %08x\n", __FUNCTION__, (int) sp->eth_regs,
	  recognise);
#endif

  sp->eth_regs->mac_control = recognise;
}


static int __exit
ar2313_remove (struct platform_device *pdev)
{
  struct net_device *dev = platform_get_drvdata (pdev);
  ar2313_init_cleanup (dev);
  free_irq(dev->irq, dev);
  unregister_netdev (dev);
  kfree (dev);
  return 0;
}


/*
 * Restart the AR2313 ethernet controller. 
 */
static int
ar2313_restart (struct net_device *dev)
{
  /* disable interrupts */
  disable_irq (dev->irq);

  /* stop mac */
  ar2313_halt (dev);

  /* initialize */
  ar2313_init (dev);

  /* enable interrupts */
  enable_irq (dev->irq);

  return 0;
}

static struct platform_driver ar2313_driver = {
  .driver.name = "ar531x-eth",
  .probe = ar2313_probe,
  .remove = ar2313_remove,
};

int __init
ar2313_module_init (void)
{
  return platform_driver_register (&ar2313_driver);
}

void __exit
ar2313_module_cleanup (void)
{
  platform_driver_unregister (&ar2313_driver);
}

module_init (ar2313_module_init);
module_exit (ar2313_module_cleanup);


static void
ar2313_free_descriptors (struct net_device *dev)
{
  struct ar2313_private *sp = dev->priv;
  if (sp->rx_ring != NULL)
    {
      kfree ((void *) KSEG0ADDR (sp->rx_ring));
      sp->rx_ring = NULL;
      sp->tx_ring = NULL;
    }
}


static int
ar2313_allocate_descriptors (struct net_device *dev)
{
  struct ar2313_private *sp = dev->priv;
  int size;
  int j;
  ar2313_descr_t *space;

  if (sp->rx_ring != NULL)
    {
      printk ("%s: already done.\n", __FUNCTION__);
      return 0;
    }

  size = (sizeof (ar2313_descr_t) * (AR2313_DESCR_ENTRIES * AR2313_QUEUES));
  space = kmalloc (size, GFP_KERNEL);
  if (space == NULL)
    return 1;

  /* invalidate caches */
  dma_cache_inv ((unsigned int) space, size);

  /* now convert pointer to KSEG1 */
  space = (ar2313_descr_t *) KSEG1ADDR (space);

  memset ((void *) space, 0, size);

  sp->rx_ring = space;
  space += AR2313_DESCR_ENTRIES;

  sp->tx_ring = space;
  space += AR2313_DESCR_ENTRIES;

  /* Initialize the transmit Descriptors */
  for (j = 0; j < AR2313_DESCR_ENTRIES; j++)
    {
      ar2313_descr_t *td = &sp->tx_ring[j];
      td->status = 0;
      td->devcs = DMA_TX1_CHAINED;
      td->addr = 0;
      td->descr =
	virt_to_phys (&sp->tx_ring[(j + 1) & (AR2313_DESCR_ENTRIES - 1)]);
    }

  return 0;
}


/*
 * Generic cleanup handling data allocated during init. Used when the
 * module is unloaded or if an error occurs during initialization
 */
static void
ar2313_init_cleanup (struct net_device *dev)
{
  struct ar2313_private *sp = dev->priv;
  struct sk_buff *skb;
  int j;

  ar2313_free_descriptors (dev);

  if (sp->eth_regs)
    iounmap ((void *) sp->eth_regs);
  if (sp->dma_regs)
    iounmap ((void *) sp->dma_regs);

  if (sp->rx_skb)
    {
      for (j = 0; j < AR2313_DESCR_ENTRIES; j++)
	{
	  skb = sp->rx_skb[j];
	  if (skb)
	    {
	      sp->rx_skb[j] = NULL;
	      dev_kfree_skb (skb);
	    }
	}
      kfree (sp->rx_skb);
      sp->rx_skb = NULL;
    }

  if (sp->tx_skb)
    {
      for (j = 0; j < AR2313_DESCR_ENTRIES; j++)
	{
	  skb = sp->tx_skb[j];
	  if (skb)
	    {
	      sp->tx_skb[j] = NULL;
	      dev_kfree_skb (skb);
	    }
	}
      kfree (sp->tx_skb);
      sp->tx_skb = NULL;
    }
}

static int
ar2313_setup_timer (struct net_device *dev)
{
  struct ar2313_private *sp = dev->priv;

//  init_timer (&sp->link_timer);

///  sp->link_timer.function = ar2313_link_timer_fn;
///  sp->link_timer.data = (int) dev;
//  sp->link_timer.expires = jiffies + HZ;

//  add_timer (&sp->link_timer);
  return 0;

}

static void
ar2313_link_timer_fn (unsigned long data)
{
  struct net_device *dev = (struct net_device *) data;
  struct ar2313_private *sp = dev->priv;

  // see if the link status changed
  // This was needed to make sure we set the PHY to the
  // autonegotiated value of half or full duplex.
  ar2313_check_link (dev);

  // Loop faster when we don't have link. 
  // This was needed to speed up the AP bootstrap time.
  if (sp->link == 0)
    {
      mod_timer (&sp->link_timer, jiffies + HZ / 2);
    }
  else
    {
      mod_timer (&sp->link_timer, jiffies + LINK_TIMER);
    }
}

#define ADM_PHY_AN_STATUS_REG	 0x05
#define ADM_PHY_LINK_100FULL	0x0100
#define ADM_PHY_LINK_100HALF	0x0080
#define ADM_PHY_LINK_10FULL		0x0040
#define ADM_PHY_LINK_10HALF		0x0020


unsigned int adm_addrs[] =
  { ADM_PHY0_ADDR, ADM_PHY1_ADDR, ADM_PHY2_ADDR, ADM_PHY3_ADDR,
ADM_PHY4_ADDR };

int
adm_phyIsFullDuplex (struct net_device *dev)
{
  int phyUnit;
  int state;
  int fullduplex = 0;
  unsigned short phyHwStatus;
  static unsigned char states[5] = { -1, -1, -1, -1, -1 };
  for (phyUnit = 0; phyUnit < 5; phyUnit++)
    {
      state = 0;
      phyHwStatus = armiiread (dev, adm_addrs[phyUnit], ADM_PHY_STATUS);
      if ((phyHwStatus & ADM_STATUS_LINK_PASS))
	{
	  state |= 0x10;
	  phyHwStatus =
	    armiiread (dev, adm_addrs[phyUnit], ADM_PHY_AN_STATUS_REG);
	  if ((phyHwStatus & ADM_PHY_LINK_100FULL))
	    {
	      state |= 2;
	      fullduplex = 1;
	    }else
	  if ((phyHwStatus & ADM_PHY_LINK_100HALF))
	    {
	      state |= 1;
	      if (!fullduplex)
	      fullduplex = 2;
	    }else
	  if ((phyHwStatus & ADM_PHY_LINK_10FULL))
	    {
	      state |= 8;
	      fullduplex = 1;
	    }else
	  if ((phyHwStatus & ADM_PHY_LINK_10HALF))
	    {
	      state |= 4;
	      if (!fullduplex)
	      fullduplex = 2;
	    }
	}
      if (states[phyUnit] != state)
	{
	  states[phyUnit] = state;
	  printk (KERN_INFO);
	  if (!(state & 0x10))
	    {
	      printk ("Port %d: down\n", phyUnit);
	    }
	  else
	    {
	      if ((state & 1))
		printk ("Port %d: 100 Mbit Half duplex\n", phyUnit);
	      if ((state & 2))
		printk ("Port %d: 100 Mbit Full duplex\n", phyUnit);
	      if ((state & 4))
		printk ("Port %d: 10 Mbit Half duplex\n", phyUnit);
	      if ((state & 8))
		printk ("Port %d: 10 Mbit Full duplex\n", phyUnit);
	    }
	}
    }
  return fullduplex;
}
unsigned int ip_addrs[] =
  { IP_PHY0_ADDR, IP_PHY1_ADDR, IP_PHY2_ADDR, IP_PHY3_ADDR,
IP_PHY4_ADDR };

int
ip_phyIsFullDuplex (struct net_device *dev)
{
  int phyUnit;
  int fullduplex = 0;
  int state = 0;
  unsigned short phyHwStatus;
  static unsigned char states[5] = { -1, -1, -1, -1, -1 };
  for (phyUnit = 0; phyUnit < 5; phyUnit++)
    {

      state = 0;
      phyHwStatus = armiiread (dev, ip_addrs[phyUnit], IP_PHY_STATUS);
      if ((phyHwStatus & IP_STATUS_LINK_PASS))
	{
	  state |= 0x10;
	  phyHwStatus =
	    armiiread (dev,ip_addrs[phyUnit], IP_LINK_PARTNER_ABILITY);
	  if ((phyHwStatus & IP_LINK_100BASETX_FULL_DUPLEX) && (phyHwStatus & IP_LINK_100BASETX))
	    {
	      state |= 2;
	      fullduplex = 1;
	    }else
	  if ((phyHwStatus & IP_LINK_100BASETX))
	    {
	      state |= 1;
	      if (!fullduplex)
	      fullduplex = 2;
	    }else
	  if ((phyHwStatus & IP_LINK_10BASETX_FULL_DUPLEX) && (phyHwStatus & IP_LINK_10BASETX))
	    {
	      state |= 8;
	      fullduplex = 1;
	    }else
	  if ((phyHwStatus & IP_LINK_10BASETX))
	    {
	      state |= 4;
	      if (!fullduplex)
	      fullduplex = 2;
	    }
	}
      if (states[phyUnit] != state)
	{
	  states[phyUnit] = state;
	  printk (KERN_INFO);
	  if (!(state & 0x10))
	    {
	      printk ("Port %d: down\n", phyUnit);
	    }
	  else
	    {
	      if ((state & 1))
		printk ("Port %d: 100 Mbit Half duplex\n", phyUnit);
	      if ((state & 2))
		printk ("Port %d: 100 Mbit Full duplex\n", phyUnit);
	      if ((state & 4))
		printk ("Port %d: 10 Mbit Half duplex\n", phyUnit);
	      if ((state & 8))
		printk ("Port %d: 10 Mbit Full duplex\n", phyUnit);
	    }
	}
    }
  return fullduplex;
}



unsigned int mv_addrs[] =
  { 0x10, 0x11, 0x12, 0x13,0x14 };

int
mv_phyIsFullDuplex (struct net_device *dev)
{
  int phyUnit;
  int fullduplex = 0;
  int state = 0;
  unsigned short phyHwStatus;
  static unsigned char states[5] = { -1, -1, -1, -1, -1 };
  for (phyUnit = 0; phyUnit < 5; phyUnit++)
    {

      state = 0;
      phyHwStatus = armiiread (dev, mv_addrs[phyUnit],MV_PHY_SPECIFIC_STATUS);
      if ((phyHwStatus & MV_STATUS_REAL_TIME_LINK_UP))
	{
	  state |= 0x10;
	  phyHwStatus = armiiread (dev,mv_addrs[phyUnit], MV_PHY_SPECIFIC_STATUS);
	  if ((phyHwStatus & MV_STATUS_RESOLVED_DUPLEX_FULL) && (phyHwStatus & MV_STATUS_RESOLVED_SPEED_100))
	    {
	      state |= 2;
	      fullduplex = 1;
	    }else
	  if ((phyHwStatus & MV_STATUS_RESOLVED_SPEED_100))
	    {
	      state |= 1;
	      if (!fullduplex)
	      fullduplex = 2;
	    }else
	  if ((phyHwStatus & MV_STATUS_RESOLVED_DUPLEX_FULL))
	    {
	      state |= 8;
	      fullduplex = 1;
	    }else
	    {
	      state |= 4;
	      if (!fullduplex)
	      fullduplex = 2;
	    }
	}
      if (states[phyUnit] != state)
	{
	  states[phyUnit] = state;
	  printk (KERN_INFO);
	  if (!(state & 0x10))
	    {
	      printk ("Port %d: down\n", phyUnit);
	    }
	  else
	    {
	      if ((state & 1))
		printk ("Port %d: 100 Mbit Half duplex\n", phyUnit);
	      if ((state & 2))
		printk ("Port %d: 100 Mbit Full duplex\n", phyUnit);
	      if ((state & 4))
		printk ("Port %d: 10 Mbit Half duplex\n", phyUnit);
	      if ((state & 8))
		printk ("Port %d: 10 Mbit Full duplex\n", phyUnit);
	    }
	}
    }
  return fullduplex;
}

static void
ar2313_check_link (struct net_device *dev)
{
  struct ar2313_private *sp = dev->priv;
  static u16 phyData;
  u16 reg;
  u16 duplex = 4;
  static u16 laststateeth0 = -1;
  static u16 laststateeth1 = -1;
  switch (sp->eth_phy)
    {

    case AR2313_EPHY_MARVELL:
      duplex = mv_phyIsFullDuplex(dev);
      break;
    case AR2313_EPHY_ICSPLUS:
      duplex = ip_phyIsFullDuplex (dev);
      break;
    case AR2313_EPHY_ADMTEK:
      duplex = adm_phyIsFullDuplex (dev);
      break;
    default:
      phyData = armiiread (dev, sp->phy, MII_BMSR);
      if (sp->phyData != phyData)
	{
	  if (phyData & BMSR_LSTATUS)
	    {
	      /* link is present, ready link partner ability to deterine
	         duplexity */
	      duplex = 0;

	      sp->link = 1;
	      reg = armiiread (dev, sp->phy, MII_BMCR);
	      if (reg & BMCR_ANENABLE)
		{
		  /* auto neg enabled */
		  reg = armiiread (dev, sp->phy, MII_LPA);
		  duplex = (reg & (LPA_100FULL | LPA_10FULL)) ? 1 : 0;
		}
	      else
		{
		  /* no auto neg, just read duplex config */
		  duplex = (reg & BMCR_FULLDPLX) ? 1 : 0;
		}

	      printk (KERN_INFO "%s: Configuring MAC for %s duplex\n",
		      dev->name, (duplex) ? "full" : "half");

	      if (duplex)
		{
		  /* full duplex */
		  sp->eth_regs->mac_control =
		    ((sp->eth_regs->
		      mac_control | MAC_CONTROL_F) & ~MAC_CONTROL_DRO);
		}
	      else
		{
		  /* half duplex */
		  sp->eth_regs->mac_control =
		    ((sp->eth_regs->
		      mac_control | MAC_CONTROL_DRO) & ~MAC_CONTROL_F);
		}
	    }
	  else
	    {
	      /* no link */
	      sp->link = 0;
	    }
	  sp->phyData = phyData;
	}
      break;
    }

//      printk(KERN_EMERG "check admtek %d\n",duplex);
if (!strcmp(dev->name,"eth0"))
  if (laststateeth0 != duplex)
    {
      laststateeth0 = duplex;
      switch (duplex)
	{
	case 1:
	  /* FULL DUPLEX */
	  printk ("%s: Full duplex\n", dev->name);
	  sp->eth_regs->mac_control =
	    ((sp->eth_regs->mac_control | MAC_CONTROL_F) & ~MAC_CONTROL_DRO);
	  break;
	case 2:
	  /* HALF DUPLEX */
	  printk ("%s: Half duplex\n", dev->name);
	  sp->eth_regs->mac_control =
	    ((sp->eth_regs->mac_control | MAC_CONTROL_DRO) & ~MAC_CONTROL_F);
	  break;
	case 0:
	  /* no link */
	  printk ("%s: No link\n", dev->name);
	  sp->link = 0;
	  break;
	}
    }
if (!strcmp(dev->name,"eth1"))
  if (laststateeth1 != duplex)
    {
      laststateeth1 = duplex;
      switch (duplex)
	{
	case 1:
	  /* FULL DUPLEX */
	  printk ("%s: Full duplex\n", dev->name);
	  sp->eth_regs->mac_control =
	    ((sp->eth_regs->mac_control | MAC_CONTROL_F) & ~MAC_CONTROL_DRO);
	  break;
	case 2:
	  /* HALF DUPLEX */
	  printk ("%s: Half duplex\n", dev->name);
	  sp->eth_regs->mac_control =
	    ((sp->eth_regs->mac_control | MAC_CONTROL_DRO) & ~MAC_CONTROL_F);
	  break;
	case 0:
	  /* no link */
	  printk ("%s: No link\n", dev->name);
	  sp->link = 0;
	  break;
	}
    }

}
static int
ar2313_reset_reg (struct net_device *dev)
{
  struct ar2313_private *sp = (struct ar2313_private *) dev->priv;
  unsigned int ethsal, ethsah;
  unsigned int flags;
  unsigned int sw_port_addr[6] = { 1, 3, 5, 7, 8, 9 };
  unsigned int phyUnit;
  unsigned short reg = 0;
  int phyID1;
  int phyID2;
  u16 atuControl;
  u16 phyData;
  u16 phyAddr;
  u16 switchPortAddr;
  u16 portAssociationVector;

  *sp->int_regs |= sp->cfg->reset_mac;
  mdelay (10);
  *sp->int_regs &= ~sp->cfg->reset_mac;
  mdelay (10);
  *sp->int_regs |= sp->cfg->reset_phy;
  mdelay (10);
  *sp->int_regs &= ~sp->cfg->reset_phy;
  mdelay (10);

  phyData = armiiread (dev, 0x10, MV_PHY_ID1);
  phyData = armiiread (dev, 0x10, MV_PHY_ID1);
  if (phyData == MV_PHY_ID1_EXPECTATION)
    {
      phyData = armiiread (dev, 0x10, MV_PHY_ID2);
      if ((phyData & MV_OUI_LSB_MASK) != MV_OUI_LSB_EXPECTATION)
	printk ("%s: Invalid PHY ID2 Expected 0x%04x, read 0x%04x\n",
		dev->name, MV_OUI_LSB_EXPECTATION, phyData);
      else
	{
	  printk ("%s: Found PHY MARVELL model 0x%x revision 0x%x\n",
		  dev->name,
		  (phyData & MV_MODEL_NUM_MASK) >> MV_MODEL_NUM_SHIFT,
		  (phyData & MV_REV_NUM_MASK) >> MV_REV_NUM_SHIFT);
	  sp->eth_phy = AR2313_EPHY_MARVELL;
	}
    }

  phyData = armiiread (dev, IP_PHY1_ADDR, IP_PHY_ID1);
  phyData = armiiread (dev, IP_PHY1_ADDR, IP_PHY_ID1);
  if (phyData == IP_PHY_ID1_EXPECTATION)
    {
      phyData = armiiread (dev, IP_PHY1_ADDR, IP_PHY_ID2);
      if ((phyData & IP_OUI_LSB_MASK) != IP_OUI_LSB_EXPECTATION)
	printk ("%s: Invalid PHY ID2 Expected 0x%04x, read 0x%04x\n",
		dev->name, IP_OUI_LSB_EXPECTATION, phyData);
      else
	{
	  printk ("%s: Found PHY ICPLUS model 0x%x revision 0x%x\n",
		  dev->name,
		  (phyData & IP_MODEL_NUM_MASK) >> IP_MODEL_NUM_SHIFT,
		  (phyData & IP_REV_NUM_MASK) >> IP_REV_NUM_SHIFT);
	  sp->eth_phy = AR2313_EPHY_ICSPLUS;
	}
    }

  phyID1 = armiiread (dev, 0x5, 0x0);
  phyID1 = armiiread (dev, 0x5, 0x0);
  phyID2 = armiiread (dev, 0x5, 0x1);
  if (((phyID1 & 0xfff0) == ADM_CHIP_ID1_EXPECTATION)
      && (phyID2 == ADM_CHIP_ID2_EXPECTATION))
    {
      printk (KERN_INFO "ADM6996FC detected!\n");
      sp->eth_phy = AR2313_EPHY_ADMTEK;
    }

  switch (sp->eth_phy)
    {
    case AR2313_EPHY_ADMTEK:
      for (phyUnit = 0; phyUnit < 6; phyUnit++)
	{
	  reg = armiiread (dev, PHY_ADDR_SW_PORT, sw_port_addr[phyUnit]);
	  reg |= ADM_SW_AUTO_MDIX_EN;
	  armiiwrite (dev, PHY_ADDR_SW_PORT, sw_port_addr[phyUnit], reg);
	}
      break;
    case AR2313_EPHY_ICSPLUS:
      /* start auto negogiation on phy */
      armiiwrite (dev, IP_PHY1_ADDR, IP_AUTONEG_ADVERT, IP_ADVERTISE_ALL);
      armiiwrite (dev, IP_PHY1_ADDR, IP_PHY_CONTROL,
		  IP_CTRL_AUTONEGOTIATION_ENABLE |
		  IP_CTRL_START_AUTONEGOTIATION);
      break;
    case AR2313_EPHY_MARVELL:
      /* Initialize global switch settings */
      /*atuControl = MV_ATUCTRL_AGE_TIME_DEFAULT << MV_ATUCTRL_AGE_TIME_SHIFT;
      atuControl |= MV_ATUCTRL_ATU_SIZE_DEFAULT << MV_ATUCTRL_ATU_SIZE_SHIFT;
      armiiwrite (dev, MV_SWITCH_GLOBAL_ADDR, MV_ATU_CONTROL, atuControl);
      for (phyAddr = 0x10; phyAddr < 0x16; phyAddr++)
	armiiwrite (dev, phyAddr, MV_PHY_CONTROL,
		    MV_CTRL_SOFTWARE_RESET | MV_CTRL_AUTONEGOTIATION_ENABLE);
      portAssociationVector = 1;
      for (switchPortAddr = 0x18; switchPortAddr < 0x1e; switchPortAddr++)
	{
	  armiiwrite (dev, switchPortAddr, MV_PORT_CONTROL,
		      MV_PORT_CONTROL_PORT_STATE_FORWARDING);
	  armiiwrite (dev, switchPortAddr, MV_PORT_ASSOCIATION_VECTOR,
		      portAssociationVector);
	  flags = armiiread (dev, switchPortAddr, MV_PORT_BASED_VLAN_MAP);
	  portAssociationVector <<= 1;
	}*/

      //mv_phySetup(dev);
      break;
    default:
      printk ("%s: UNKNOWN PHY - ignore !\n", dev->name);
      break;
    }



  sp->dma_regs->bus_mode = (DMA_BUS_MODE_SWR);
  mdelay (10);
  sp->dma_regs->bus_mode =
    ((32 << DMA_BUS_MODE_PBL_SHIFT) | DMA_BUS_MODE_BLE);

  /* enable interrupts */
  sp->dma_regs->intr_ena = (DMA_STATUS_AIS |
			    DMA_STATUS_NIS |
			    DMA_STATUS_RI | DMA_STATUS_TI | DMA_STATUS_FBE);
  sp->dma_regs->xmt_base = virt_to_phys (sp->tx_ring);
  sp->dma_regs->rcv_base = virt_to_phys (sp->rx_ring);
  sp->dma_regs->control = (DMA_CONTROL_SR | DMA_CONTROL_ST | DMA_CONTROL_SF);

  sp->eth_regs->flow_control = (FLOW_CONTROL_FCE);
  sp->eth_regs->vlan_tag = (0x8100);

  /* Enable Ethernet Interface */
  flags = (MAC_CONTROL_TE |	/* transmit enable */
	   MAC_CONTROL_PM |	/* pass mcast */
	   MAC_CONTROL_F |	/* full duplex */
	   MAC_CONTROL_HBD);	/* heart beat disabled */

  if (dev->flags & IFF_PROMISC)
    {				/* set promiscuous mode */
      flags |= MAC_CONTROL_PR;
    }
  sp->eth_regs->mac_control = flags;

  /* Set all Ethernet station address registers to their initial values */
  ethsah = ((((u_int) (dev->dev_addr[5]) << 8) & (u_int) 0x0000FF00) |
	    (((u_int) (dev->dev_addr[4]) << 0) & (u_int) 0x000000FF));

  ethsal = ((((u_int) (dev->dev_addr[3]) << 24) & (u_int) 0xFF000000) |
	    (((u_int) (dev->dev_addr[2]) << 16) & (u_int) 0x00FF0000) |
	    (((u_int) (dev->dev_addr[1]) << 8) & (u_int) 0x0000FF00) |
	    (((u_int) (dev->dev_addr[0]) << 0) & (u_int) 0x000000FF));

  sp->eth_regs->mac_addr[0] = ethsah;
  sp->eth_regs->mac_addr[1] = ethsal;

  mdelay (10);

  return (0);
}


static int
ar2313_init (struct net_device *dev)
{
  struct ar2313_private *sp = dev->priv;
  int ecode = 0;

  /* 
   * Allocate descriptors
   */
  if (ar2313_allocate_descriptors (dev))
    {
      printk ("%s: %s: ar2313_allocate_descriptors failed\n",
	      dev->name, __FUNCTION__);
      ecode = -EAGAIN;
      goto init_error;
    }

  /* 
   * Get the memory for the skb rings.
   */
  if (sp->rx_skb == NULL)
    {
      sp->rx_skb =
	kmalloc (sizeof (struct sk_buff *) * AR2313_DESCR_ENTRIES,
		 GFP_KERNEL);
      if (!(sp->rx_skb))
	{
	  printk ("%s: %s: rx_skb kmalloc failed\n", dev->name, __FUNCTION__);
	  ecode = -EAGAIN;
	  goto init_error;
	}
    }
  memset (sp->rx_skb, 0, sizeof (struct sk_buff *) * AR2313_DESCR_ENTRIES);

  if (sp->tx_skb == NULL)
    {
      sp->tx_skb =
	kmalloc (sizeof (struct sk_buff *) * AR2313_DESCR_ENTRIES,
		 GFP_KERNEL);
      if (!(sp->tx_skb))
	{
	  printk ("%s: %s: tx_skb kmalloc failed\n", dev->name, __FUNCTION__);
	  ecode = -EAGAIN;
	  goto init_error;
	}
    }
  memset (sp->tx_skb, 0, sizeof (struct sk_buff *) * AR2313_DESCR_ENTRIES);

  /* 
   * Set tx_csm before we start receiving interrupts, otherwise
   * the interrupt handler might think it is supposed to process
   * tx ints before we are up and running, which may cause a null
   * pointer access in the int handler.
   */
  sp->rx_skbprd = 0;
  sp->cur_rx = 0;
  sp->tx_prd = 0;
  sp->tx_csm = 0;

  /* 
   * Zero the stats before starting the interface
   */
  memset (&sp->stats, 0, sizeof (sp->stats));

  /* 
   * We load the ring here as there seem to be no way to tell the
   * firmware to wipe the ring without re-initializing it.
   */
  ar2313_load_rx_ring (dev, RX_RING_SIZE);

  /* 
   * Init hardware
   */
  ar2313_reset_reg (dev);

  /* 
   * Get the IRQ
   */
  ecode =
    request_irq (dev->irq, &ar2313_interrupt,
		 IRQF_SHARED | IRQF_DISABLED | IRQF_SAMPLE_RANDOM,
		 dev->name, dev);
  if (ecode)
    {
      printk (KERN_WARNING "%s: %s: Requested IRQ %d is busy\n",
	      dev->name, __FUNCTION__, dev->irq);
      goto init_error;
    }

  disable_irq(dev->irq);

  return 0;

init_error:
  ar2313_init_cleanup (dev);
  return ecode;
}

/*
 * Load the rx ring.
 *
 * Loading rings is safe without holding the spin lock since this is
 * done only before the device is enabled, thus no interrupts are
 * generated and by the interrupt handler/tasklet handler.
 */
static void
ar2313_load_rx_ring (struct net_device *dev, int nr_bufs)
{

  struct ar2313_private *sp = ((struct net_device *) dev)->priv;
  short i, idx;

  idx = sp->rx_skbprd;

  for (i = 0; i < nr_bufs; i++)
    {
      struct sk_buff *skb;
      ar2313_descr_t *rd;

      if (sp->rx_skb[idx])
	{
#if DEBUG_RX
	  printk (KERN_INFO "ar2313 rx refill full\n");
#endif /* DEBUG */
	  break;
	}
      // partha: create additional room for the second GRE fragment
      skb = alloc_skb (AR2313_BUFSIZE + 128, GFP_ATOMIC);
      if (!skb)
	{
	  printk ("\n\n\n\n %s: No memory in system\n\n\n\n", __FUNCTION__);
	  break;
	}
      // partha: create additional room in the front for tx pkt capture
      skb_reserve (skb, 32);

      /* 
       * Make sure IP header starts on a fresh cache line.
       */
      skb->dev = dev;
      skb_reserve (skb, RX_OFFSET);
      sp->rx_skb[idx] = skb;

      rd = (ar2313_descr_t *) & sp->rx_ring[idx];

      /* initialize dma descriptor */
      rd->devcs = ((AR2313_BUFSIZE << DMA_RX1_BSIZE_SHIFT) | DMA_RX1_CHAINED);
      rd->addr = virt_to_phys (skb->data);
      rd->descr =
	virt_to_phys (&sp->rx_ring[(idx + 1) & (AR2313_DESCR_ENTRIES - 1)]);
      rd->status = DMA_RX_OWN;

      idx = DSC_NEXT (idx);
    }

  if (!i)
    {
#if DEBUG_ERR
      printk (KERN_INFO
	      "Out of memory when allocating standard receive buffers\n");
#endif /* DEBUG */
    }
  else
    {
      sp->rx_skbprd = idx;
    }

  return;
}

#define AR2313_MAX_PKTS_PER_CALL        64

static int
ar2313_rx (struct net_device *dev,int budget)
{
  struct ar2313_private *sp = dev->priv;
  struct sk_buff *skb, *skb_new;
  ar2313_descr_t *rxdesc;
  unsigned int status;
  u32 idx;
  int pkts = 0;
  int received = 0;
#if DO_VLAN
	int vlan_id;
#endif

  idx = sp->cur_rx;

  /* process at most the entire ring and then wait for another interrupt 
   */
  while (budget > 0)
    {

      rxdesc = &sp->rx_ring[idx];
      status = rxdesc->status;
      if (status & DMA_RX_OWN)
	{
	  /* SiByte owns descriptor or descr not yet filled in */
	  break;
	}

      if (++pkts > AR2313_MAX_PKTS_PER_CALL)
	{
	  break;
	}
#if DEBUG_RX
      printk ("index %d\n", idx);
      printk ("RX status %08x\n", rxdesc->status);
      printk ("RX devcs  %08x\n", rxdesc->devcs);
      printk ("RX addr   %08x\n", rxdesc->addr);
      printk ("RX descr  %08x\n", rxdesc->descr);
#endif

      if (status & (DMA_RX_ERROR | DMA_RX_ERR_LENGTH | DMA_RX_ERR_DESC | DMA_RX_ERR_RUNT | DMA_RX_ERR_COL | DMA_RX_ERR_MII | DMA_RX_ERR_CRC))
	{
#if DEBUG_RX
	  printk ("%s: rx ERROR %08x\n", __FUNCTION__, status);
#endif
	  sp->stats.rx_errors++;
	  sp->stats.rx_dropped++;

	  /* add statistics counters */
	  if (status & DMA_RX_ERR_CRC)
	    sp->stats.rx_crc_errors++;
	  if (status & DMA_RX_ERR_COL)
	    sp->stats.rx_over_errors++;
	  if (status & DMA_RX_ERR_LENGTH)
	    sp->stats.rx_length_errors++;
	  if (status & DMA_RX_ERR_RUNT)
	    sp->stats.rx_over_errors++;
	  if (status & DMA_RX_ERR_DESC)
	    sp->stats.rx_over_errors++;

	}
      else if (status & (DMA_RX_LS | DMA_RX_FS))
	{
	  int length = 0;
	  /* alloc new buffer. */
	  skb_new = dev_alloc_skb (AR2313_BUFSIZE + RX_OFFSET + 128);
	  if (skb_new != NULL)
	    {

	      skb = sp->rx_skb[idx];
	      if (status & DMA_RX_LS)
	    	length = ((status >> DMA_RX_LEN_SHIFT) & 0x3fff) - CRC_LEN;
	      else
	        length = AR2313_BUFSIZE;
	      /* set skb */
	      skb_put (skb,length);

	      sp->stats.rx_bytes += skb->len;
	      skb->protocol = eth_type_trans (skb, dev);
	      /* pass the packet to upper layers */
#ifndef CONFIG_MTD_AR531X
#if DO_VLAN
				if ((sp->eth_phy == AR2313_EPHY_MARVELL) && sp->vlgrp) {
					vlan_id = marvell_find_vlan(sp, skb);
					vlan_hwaccel_receive_skb(skb, sp->vlgrp, vlan_id);
				} else
#endif
#endif
	      netif_receive_skb(skb);

	      skb_new->dev = dev;
	      /* 16 bit align */
	      skb_reserve (skb_new, RX_OFFSET + 32);
	      /* reset descriptor's curr_addr */
	      rxdesc->addr = virt_to_phys (skb_new->data);

	      sp->stats.rx_packets++;
	      sp->rx_skb[idx] = skb_new;
	      --budget;
	      ++received;
	    }
	  else
	    {
	      sp->stats.rx_dropped++;
	    }
	}

      rxdesc->devcs = ((AR2313_BUFSIZE << DMA_RX1_BSIZE_SHIFT) |
		       DMA_RX1_CHAINED);
      rxdesc->status = DMA_RX_OWN;

      idx = DSC_NEXT (idx);
    }

  sp->cur_rx = idx;

  return received;
}


static void
ar2313_tx_int (struct net_device *dev)
{
  struct ar2313_private *sp = dev->priv;
  u32 idx;
  struct sk_buff *skb;
  ar2313_descr_t *txdesc;
  unsigned int status = 0;

  idx = sp->tx_csm;

  while (idx != sp->tx_prd)
    {

      txdesc = &sp->tx_ring[idx];

#if DEBUG_TX
      printk
	("%s: TXINT: csm=%d idx=%d prd=%d status=%x devcs=%x addr=%08x descr=%x\n",
	 dev->name, sp->tx_csm, idx, sp->tx_prd, txdesc->status,
	 txdesc->devcs, txdesc->addr, txdesc->descr);
#endif /* DEBUG */

      if ((status = txdesc->status) & DMA_TX_OWN)
	{
	  /* ar2313 dma still owns descr */
	  break;
	}
      /* done with this descriptor */
      dma_unmap_single (NULL, txdesc->addr,
			txdesc->devcs & DMA_TX1_BSIZE_MASK, DMA_TO_DEVICE);
      txdesc->status = 0;

      if (status & DMA_TX_ERROR)
	{
	  sp->stats.tx_errors++;
	  sp->stats.tx_dropped++;
	  if (status & DMA_TX_ERR_UNDER)
	    sp->stats.tx_fifo_errors++;
	  if (status & DMA_TX_ERR_HB)
	    sp->stats.tx_heartbeat_errors++;
	  if (status & (DMA_TX_ERR_LOSS | DMA_TX_ERR_LINK))
	    sp->stats.tx_carrier_errors++;
	  if (status & (DMA_TX_ERR_LATE |
			DMA_TX_ERR_COL |
			DMA_TX_ERR_JABBER | DMA_TX_ERR_DEFER))
	    sp->stats.tx_aborted_errors++;
	}
      else
	{
	  /* transmit OK */
	  sp->stats.tx_packets++;
	}

      skb = sp->tx_skb[idx];
      sp->tx_skb[idx] = NULL;
      idx = DSC_NEXT (idx);
      sp->stats.tx_bytes += skb->len;
      dev_kfree_skb_irq (skb);
    }

  sp->tx_csm = idx;

  return;
}



static irqreturn_t
ar2313_interrupt (int irq, void *dev_id)
{
  struct net_device *dev = (struct net_device *) dev_id;
  struct ar2313_private *sp = dev->priv;
  unsigned int status, enabled;

  /* clear interrupt */
  /* 
   * Don't clear RI bit if currently disabled.
   */
  status = sp->dma_regs->status;
  enabled = sp->dma_regs->intr_ena;
  sp->dma_regs->status = status & enabled;

  if (status & DMA_STATUS_NIS)
    {
      /* normal status */
      /* 
       * Don't schedule rx processing if interrupt
       * is already disabled.
       */
      if (netif_rx_schedule_prep(dev)) {
    	    __netif_rx_schedule(dev);
	}
    }

  if (status & DMA_STATUS_AIS)
    {
#if DEBUG_INT
      printk ("%s: AIS set %08x & %x\n", __FUNCTION__,
	      status, (DMA_STATUS_FBE | DMA_STATUS_TPS));
#endif
      /* abnormal status */
      if (status & (DMA_STATUS_FBE | DMA_STATUS_TPS))
	{
	  ar2313_restart (dev);
	}
    }
  return IRQ_HANDLED;
}


static int
ar2313_open (struct net_device *dev)
{
  struct ar2313_private *sp;

  sp = dev->priv;

  dev->mtu = 1500;
  netif_start_queue (dev);

  sp->eth_regs->mac_control |= MAC_CONTROL_RE;
  
  enable_irq(dev->irq);
  sp->unloading = 0;

  return 0;
}

static void
ar2313_halt (struct net_device *dev)
{
  struct ar2313_private *sp = dev->priv;
  int j;


  /* kill the MAC */
  sp->eth_regs->mac_control &= ~(MAC_CONTROL_RE |	/* disable Receives */
				 MAC_CONTROL_TE);	/* disable Transmits */
  /* stop dma */
  sp->dma_regs->control = 0;
  sp->dma_regs->bus_mode = DMA_BUS_MODE_SWR;

  /* place phy and MAC in reset */
  *sp->int_regs |= (sp->cfg->reset_mac | sp->cfg->reset_phy);

  /* free buffers on tx ring */
  for (j = 0; j < AR2313_DESCR_ENTRIES; j++)
    {
      struct sk_buff *skb;
      ar2313_descr_t *txdesc;

      txdesc = &sp->tx_ring[j];
      txdesc->descr = 0;

      skb = sp->tx_skb[j];
      if (skb)
	{
	  dev_kfree_skb (skb);
	  sp->tx_skb[j] = NULL;
	}
    }
}

/*
 * close should do nothing. Here's why. It's called when
 * 'ifconfig bond0 down' is run. If it calls free_irq then
 * the irq is gone forever ! When bond0 is made 'up' again,
 * the ar2313_open () does not call request_irq (). Worse,
 * the call to ar2313_halt() generates a WDOG reset due to
 * the write to 'sp->int_regs' and the box reboots.
 * Commenting this out is good since it allows the
 * system to resume when bond0 is made up again.
 */
static int
ar2313_close (struct net_device *dev)
{
  struct ar2313_private *sp = dev->priv;
  /* 
   * Disable interrupts
   */
  disable_irq (dev->irq);

  /* 
   * Without (or before) releasing irq and stopping hardware, this
   * is an absolute non-sense, by the way. It will be reset instantly
   * by the first irq.
   */
  netif_stop_queue (dev);
  sp->unloading = 1;
#if 0

  /* stop the MAC and DMA engines */
  ar2313_halt (dev);

  /* release the interrupt */
  free_irq (dev->irq, dev);

#endif
  return 0;
}

static int
ar2313_start_xmit (struct sk_buff *skb, struct net_device *dev)
{
  struct ar2313_private *sp = dev->priv;
  ar2313_descr_t *td;
  u32 idx;

#ifndef CONFIG_MTD_AR531X
#if DO_VLAN
	if (sp->eth_phy == AR2313_EPHY_MARVELL)
		skb = marvell_add_vlan(sp, skb);
#endif
#endif
  idx = sp->tx_prd;
  td = &sp->tx_ring[idx];

  if (td->status & DMA_TX_OWN)
    {
#if DEBUG_TX
      printk ("%s: No space left to Tx\n", __FUNCTION__);
#endif
      /* free skbuf and lie to the caller that we sent it out */
      sp->stats.tx_dropped++;
      dev_kfree_skb (skb);

      /* restart transmitter in case locked */
      sp->dma_regs->xmt_poll = 0;
      return 0;
    }

  /* Setup the transmit descriptor. */
  td->devcs = ((skb->len << DMA_TX1_BSIZE_SHIFT) |
	       (DMA_TX1_LS | DMA_TX1_IC | DMA_TX1_CHAINED));
  td->addr = dma_map_single (NULL, skb->data, skb->len, DMA_TO_DEVICE);
  td->status = DMA_TX_OWN;

  /* kick transmitter last */
  sp->dma_regs->xmt_poll = 0;

#if DEBUG_TX
  printk ("index %d\n", idx);
  printk ("TX status %08x\n", td->status);
  printk ("TX devcs  %08x\n", td->devcs);
  printk ("TX addr   %08x\n", td->addr);
  printk ("TX descr  %08x\n", td->descr);
#endif

  sp->tx_skb[idx] = skb;
  idx = DSC_NEXT (idx);
  sp->tx_prd = idx;

  return 0;
}

static int
netdev_get_ecmd (struct net_device *dev, struct ethtool_cmd *ecmd)
{
  struct ar2313_private *np = dev->priv;
  u32 tmp;

  ecmd->supported =
    (SUPPORTED_10baseT_Half | SUPPORTED_10baseT_Full |
     SUPPORTED_100baseT_Half | SUPPORTED_100baseT_Full |
     SUPPORTED_Autoneg | SUPPORTED_TP | SUPPORTED_MII);

  ecmd->port = PORT_TP;
  /* only supports internal transceiver */
  ecmd->transceiver = XCVR_INTERNAL;
  /* not sure what this is for */
  ecmd->phy_address = 1;

  ecmd->advertising = ADVERTISED_MII;
  tmp = armiiread (dev, np->phy, MII_ADVERTISE);
  if (tmp & ADVERTISE_10HALF)
    ecmd->advertising |= ADVERTISED_10baseT_Half;
  if (tmp & ADVERTISE_10FULL)
    ecmd->advertising |= ADVERTISED_10baseT_Full;
  if (tmp & ADVERTISE_100HALF)
    ecmd->advertising |= ADVERTISED_100baseT_Half;
  if (tmp & ADVERTISE_100FULL)
    ecmd->advertising |= ADVERTISED_100baseT_Full;

  tmp = armiiread (dev, np->phy, MII_BMCR);
  if (tmp & BMCR_ANENABLE)
    {
      ecmd->advertising |= ADVERTISED_Autoneg;
      ecmd->autoneg = AUTONEG_ENABLE;
    }
  else
    {
      ecmd->autoneg = AUTONEG_DISABLE;
    }

  if (ecmd->autoneg == AUTONEG_ENABLE)
    {
      tmp = armiiread (dev, np->phy, MII_LPA);
      if (tmp & (LPA_100FULL | LPA_10FULL))
	{
	  ecmd->duplex = DUPLEX_FULL;
	}
      else
	{
	  ecmd->duplex = DUPLEX_HALF;
	}
      if (tmp & (LPA_100FULL | LPA_100HALF))
	{
	  ecmd->speed = SPEED_100;
	}
      else
	{
	  ecmd->speed = SPEED_10;
	}
    }
  else
    {
      if (tmp & BMCR_FULLDPLX)
	{
	  ecmd->duplex = DUPLEX_FULL;
	}
      else
	{
	  ecmd->duplex = DUPLEX_HALF;
	}
      if (tmp & BMCR_SPEED100)
	{
	  ecmd->speed = SPEED_100;
	}
      else
	{
	  ecmd->speed = SPEED_10;
	}
    }

  /* ignore maxtxpkt, maxrxpkt for now */

  return 0;
}

static int
netdev_set_ecmd (struct net_device *dev, struct ethtool_cmd *ecmd)
{
  struct ar2313_private *np = dev->priv;
  u32 tmp;

  if (ecmd->speed != SPEED_10 && ecmd->speed != SPEED_100)
    return -EINVAL;
  if (ecmd->duplex != DUPLEX_HALF && ecmd->duplex != DUPLEX_FULL)
    return -EINVAL;
  if (ecmd->port != PORT_TP)
    return -EINVAL;
  if (ecmd->transceiver != XCVR_INTERNAL)
    return -EINVAL;
  if (ecmd->autoneg != AUTONEG_DISABLE && ecmd->autoneg != AUTONEG_ENABLE)
    return -EINVAL;
  /* ignore phy_address, maxtxpkt, maxrxpkt for now */

  /* WHEW! now lets bang some bits */

  tmp = armiiread (dev, np->phy, MII_BMCR);
  if (ecmd->autoneg == AUTONEG_ENABLE)
    {
      /* turn on autonegotiation */
      tmp |= BMCR_ANENABLE;
      printk ("%s: Enabling auto-neg\n", dev->name);
    }
  else
    {
      /* turn off auto negotiation, set speed and duplexity */
      tmp &= ~(BMCR_ANENABLE | BMCR_SPEED100 | BMCR_FULLDPLX);
      if (ecmd->speed == SPEED_100)
	tmp |= BMCR_SPEED100;
      if (ecmd->duplex == DUPLEX_FULL)
	tmp |= BMCR_FULLDPLX;
      printk ("%s: Hard coding %d/%s\n", dev->name,
	      (ecmd->speed == SPEED_100) ? 100 : 10,
	      (ecmd->duplex == DUPLEX_FULL) ? "full" : "half");
    }
  armiiwrite (dev, np->phy, MII_BMCR, tmp);
  np->phyData = 0;
  return 0;
}

static int
netdev_ethtool_ioctl (struct net_device *dev, void *useraddr)
{
  struct ar2313_private *np = dev->priv;
  u32 cmd;

  if (get_user (cmd, (u32 *) useraddr))
    return -EFAULT;

  switch (cmd)
    {
      /* get settings */
    case ETHTOOL_GSET:
      {
	struct ethtool_cmd ecmd = { ETHTOOL_GSET };
	spin_lock_irq (&np->lock);
	netdev_get_ecmd (dev, &ecmd);
	spin_unlock_irq (&np->lock);
	if (copy_to_user (useraddr, &ecmd, sizeof (ecmd)))
	  return -EFAULT;
	return 0;
      }
      /* set settings */
    case ETHTOOL_SSET:
      {
	struct ethtool_cmd ecmd;
	int r;
	if (copy_from_user (&ecmd, useraddr, sizeof (ecmd)))
	  return -EFAULT;
	spin_lock_irq (&np->lock);
	r = netdev_set_ecmd (dev, &ecmd);
	spin_unlock_irq (&np->lock);
	return r;
      }
      /* restart autonegotiation */
    case ETHTOOL_NWAY_RST:
      {
	int tmp;
	int r = -EINVAL;
	/* if autoneg is off, it's an error */
	tmp = armiiread (dev, np->phy, MII_BMCR);
	if (tmp & BMCR_ANENABLE)
	  {
	    tmp |= (BMCR_ANRESTART);
	    armiiwrite (dev, np->phy, MII_BMCR, tmp);
	    r = 0;
	  }
	return r;
      }
      /* get link status */
    case ETHTOOL_GLINK:
      {
	struct ethtool_value edata = { ETHTOOL_GLINK };

	switch (np->eth_phy)
	  {

	  case AR2313_EPHY_MARVELL:
	    edata.data = mv_phyIsFullDuplex(dev)>0 ? 1 : 0;
	    break;
	  case AR2313_EPHY_ICSPLUS:
	    edata.data = ip_phyIsFullDuplex(dev)>0 ? 1 : 0;
	    break;
	  case AR2313_EPHY_ADMTEK:
	    edata.data = adm_phyIsFullDuplex(dev)>0 ? 1 : 0;
	    break;
	  default:
	    edata.data =
	      (armiiread (dev, np->phy, MII_BMSR) & BMSR_LSTATUS) ? 1 : 0;
	    break;

	  }
	if (copy_to_user (useraddr, &edata, sizeof (edata)))
	  return -EFAULT;
	return 0;
      }
    }

  return -EOPNOTSUPP;
}

static int
ar2313_ioctl (struct net_device *dev, struct ifreq *ifr, int cmd)
{
  struct mii_ioctl_data *data = (struct mii_ioctl_data *) &ifr->ifr_data;

  switch (cmd)
    {

    case SIOCETHTOOL:
      return netdev_ethtool_ioctl (dev, (void *) ifr->ifr_data);

    case SIOCGMIIPHY:		/* Get address of MII PHY in use. */
      data->phy_id = 1;
      /* Fall Through */

    case SIOCGMIIREG:		/* Read MII PHY register. */
      data->val_out = armiiread (dev, data->phy_id & 0x1f,
				 data->reg_num & 0x1f);
      return 0;
    case SIOCSMIIREG:		/* Write MII PHY register. */
      if (!capable (CAP_NET_ADMIN))
	return -EPERM;
      armiiwrite (dev, data->phy_id & 0x1f,
		  data->reg_num & 0x1f, data->val_in);
      return 0;

    case SIOCSIFHWADDR:
      if (copy_from_user
	  (dev->dev_addr, ifr->ifr_data, sizeof (dev->dev_addr)))
	return -EFAULT;
      return 0;

    case SIOCGIFHWADDR:
      if (copy_to_user (ifr->ifr_data, dev->dev_addr, sizeof (dev->dev_addr)))
	return -EFAULT;
      return 0;

    default:
      break;
    }

  return -EOPNOTSUPP;
}

static struct net_device_stats *
ar2313_get_stats (struct net_device *dev)
{
  struct ar2313_private *sp = dev->priv;
  return &sp->stats;
}


#define MII_ADDR(phy, reg) \
	((reg << MII_ADDR_REG_SHIFT) | (phy << MII_ADDR_PHY_SHIFT))

static short
armiiread (struct net_device *dev, short phy, short reg)
{

  short outp;

  struct ar2313_private *sp = (struct ar2313_private *) dev->priv;
  volatile ETHERNET_STRUCT *ethernet = sp->phy_regs;
  ethernet->mii_addr = MII_ADDR (phy, reg);
  while (ethernet->mii_addr & MII_ADDR_BUSY);
  outp = ethernet->mii_data >> MII_DATA_SHIFT;
  return (outp);
}

static void
armiiwrite (struct net_device *dev, short phy, short reg, short data)
{
  struct ar2313_private *sp = (struct ar2313_private *) dev->priv;
  volatile ETHERNET_STRUCT *ethernet = sp->phy_regs;
  while (ethernet->mii_addr & MII_ADDR_BUSY);
  ethernet->mii_data = data << MII_DATA_SHIFT;
  ethernet->mii_addr = MII_ADDR (phy, reg) | MII_ADDR_WRITE;
}
