/*
 * Copyright (C), 2020 CCX Technologies
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/module.h>
#include <linux/netdevice.h>
#include <net/sock.h>
#include <net/rtnetlink.h>
#include <linux/skbuff.h>
#include <linux/init.h>
#include <linux/spi/spi.h>
#include <linux/of_gpio.h>
#include <linux/of_irq.h>
#include <linux/interrupt.h>

#include "avionics.h"
#include "avionics-device.h"

MODULE_DESCRIPTION("HOLT Hi-6138 MIL-1553 Driver");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Charles Eidsness <charles@ccxtechnologies.com>");
MODULE_VERSION("1.0.0");

#define HI6138_FAST_ACCESS_MCFG1	0
#define HI6138_REG_MCFG1_TXINHA		(1<<15)
#define HI6138_REG_MCFG1_TXINHB		(1<<14)
#define HI6138_REG_MCFG1_MTENA		(1<<8)
#define HI6138_REG_MCFG1_INTSEL		(1<<2)
#define HI6138_REG_MCFG1_IMTA		(1<<1)

#define HI6138_REG_MEMPTRA		0x000b
#define HI6138_REG_MEMPTRB		0x000c
#define HI6138_REG_MEMPTRC		0x000d
#define HI6138_REG_MEMPTRD		0x000e

#define HI6138_REG_MCFG2		0x004e

#define HI6138_OPCODE_ENABLE_MEMPTRA	0xd8
#define HI6138_OPCODE_ENABLE_MEMPTRB	0xd9
#define HI6138_OPCODE_ENABLE_MEMPTRC	0xda
#define HI6138_OPCODE_ENABLE_MEMPTRD	0xdb

#define HI6138_OPCODE_ADD1_MEMPTR	0xd0
#define HI6138_OPCODE_ADD2_MEMPTR	0xd2
#define HI6138_OPCODE_ADD4_MEMPTR	0xd4

#define HI6138_OPCODE_READ_MEMPTR	0x40
#define HI6138_OPCODE_WRITE_MEMPTR	0xc0

#define HI6138_REG_ACCESS_PTR		HI6138_REG_MEMPTRA
#define HI6138_OPCODE_REG_ACCESS_PTR	HI6138_OPCODE_ENABLE_MEMPTRA

struct hi6138 {
	struct net_device *bm;
	struct workqueue_struct *wq;
	int reset_gpio;
	int irq;
	__u32 aclk;
	struct mutex lock;
	atomic_t bm_enabled;
};

struct hi6138_priv {
	struct net_device *dev;
	struct spi_device *spi;
	struct sk_buff_head skbq;
	struct mutex *lock;
	struct workqueue_struct *wq;
	struct delayed_work worker;
	int irq;
	atomic_t *bm_enabled;
};

static int hi6138_get_fastaccess(struct spi_device *spi, __u8 address, __u16 *value)
{
	int err;
	__u16 buffer;
	__u8 cmd = ((address&0x0f) << 2);

	err = spi_write_then_read(spi, &cmd, sizeof(cmd),
				  &buffer, sizeof(buffer));
	if (err < 0) {
		pr_err("avionics-hi6138: Failed to fast-access read 0x%x\n",
		       address);
		return err;
	}

	*value = be16_to_cpu(buffer);

	return 0;
}

static int hi6138_set_fastaccess(struct spi_device *spi, __u8 address, __u16 value)
{
	int err;
	__u16 vbuffer = cpu_to_be16(value);
	__u8 buffer[3];

	buffer[0] = 0x80 | (address&0x3f);
	memcpy(&buffer[1], &vbuffer, 2);

	err = spi_write(spi, buffer, sizeof(buffer));
	if (err < 0) {
		pr_err("avionics-hi6138: Failed to fast-access write 0x%x\n",
		       address);
		return err;
	}

	return 0;
}

static int hi6138_get_reg(struct spi_device *spi, __u16 address, __u16 *value)
{
	int err;
	__u16 buffer;
	__u8 cmd = HI6138_OPCODE_REG_ACCESS_PTR;

	err = hi6138_set_fastaccess(spi, HI6138_REG_ACCESS_PTR, address);
	if (err < 0) {
		pr_err("avionics-hi6138: Failed to set reg address 0x%x\n",
		       address);
		return err;
	}

	err = spi_write_then_read(spi, &cmd, sizeof(cmd),
				  &buffer, sizeof(buffer));
	if (err < 0) {
		pr_err("avionics-hi6138: Failed to read reg 0x%x\n",
		       address);
		return err;
	}

	*value = be16_to_cpu(buffer);

	return 0;
}

static void hi6138_get_mil1553bm(struct avionics_mil1553bm *config,
		       const struct net_device *dev)
{
	struct hi6138_priv *priv;

	priv = avionics_device_priv(dev);
	if (!priv) {
		pr_err("avionics-hi6138-bm: Failed to get private data\n");
		return;
	}

	/* TODO: add get settings here */

}

static int hi6138_set_mil1553bm(struct avionics_mil1553bm *config,
				 const struct net_device *dev)
{
	struct hi6138_priv *priv;

	priv = avionics_device_priv(dev);
	if (!priv) {
		pr_err("avionics-hi6138-bm: Failed to get private data\n");
		return -ENODEV;
	}

	/* TODO: add set settings here */

	return 0;
}

static struct avionics_ops hi6138_mil553bm_ops = {
	.name = "mil1553bm%d",
	.get_mil1553bm = hi6138_get_mil1553bm,
	.set_mil1553bm = hi6138_set_mil1553bm,
};

static int hi6138_bm_open(struct net_device *dev)
{
	struct hi6138_priv *priv;

	priv = avionics_device_priv(dev);
	if (!priv) {
		pr_err("avionics-hi6138-bm: Failed to get private data\n");
		return -EINVAL;
	}

	if (atomic_read(priv->bm_enabled)) {
		pr_err("avionics-hi6138-bm: Bus Monitor already running\n");
		return 0;
	}

	atomic_set(priv->bm_enabled, 1);
	enable_irq(priv->irq);

	return 0;
}

static int hi6138_bm_stop(struct net_device *dev)
{
	struct hi6138_priv *priv;

	pr_warn("avionics-hi6138-bm: Disabling Receiver\n");

	netif_stop_queue(dev);

	priv = avionics_device_priv(dev);
	if (!priv) {
		pr_err("avionics-hi6138-bm: Failed to get private data\n");
		return -EINVAL;
	}

	atomic_set(priv->bm_enabled, 0);
	disable_irq(priv->irq);

	return 0;
}

static void hi6138_bm_worker(struct work_struct *work)
{
	struct net_device *dev;
	struct net_device_stats *stats;
	struct hi6138_priv *priv;

	priv = container_of((struct delayed_work*)work,
			    struct hi6138_priv, worker);
	dev = priv->dev;
	stats = &dev->stats;

	priv = avionics_device_priv(dev);
	if (!priv) {
		pr_err("avionics-hi6138: Failed to get private data\n");
		return;
	}

	mutex_lock(priv->lock);

	/* TODO: Add IRQ service

done: */
	mutex_unlock(priv->lock);
	enable_irq(priv->irq);

}

static irqreturn_t hi6138_irq(int irq, void *data)
{
	struct hi6138 *hi6138 = data;
	struct hi6138_priv *priv;

	if (unlikely(irq != hi6138->irq)) {
		pr_err("avionics-hi6138: Unexpected irq %d\n", irq);
		return IRQ_HANDLED;
	}

	disable_irq_nosync(hi6138->irq);

	if (atomic_read(&hi6138->bm_enabled)) {
		priv = avionics_device_priv(hi6138->bm);
		queue_delayed_work(priv->wq, &priv->worker, 10); /* TODO: Calculate a propper delay */
	}

	return IRQ_HANDLED;
}

static const struct net_device_ops hi6138_bm_netdev_ops = {
	.ndo_open = hi6138_bm_open,
	.ndo_stop = hi6138_bm_stop,
};

static const struct of_device_id hi6138_of_device_id[] = {
	{ .compatible	= "holt,hi6138" },
	{}
};
MODULE_DEVICE_TABLE(of, hi6138_of_device_id);

static const struct spi_device_id hi6138_spi_device_id[] = {
	{
		.name		= "hi6138",
		.driver_data	= (kernel_ulong_t)0,
	},
	{}
};
MODULE_DEVICE_TABLE(spi, hi6138_spi_device_id);

static int hi6138_get_config(struct spi_device *spi)
{
	struct hi6138 *hi6138 = spi_get_drvdata(spi);
	struct device *dev = &spi->dev;
	int err;

	hi6138->reset_gpio = of_get_named_gpio(dev->of_node, "reset-gpio", 0);
	if (hi6138->reset_gpio > 0 ) {
		if (!gpio_is_valid(hi6138->reset_gpio)) {
			pr_err("avionics-hi6138: Reset GPIO is not valid\n");
			return -EINVAL;
		}

		err = devm_gpio_request_one(&spi->dev, hi6138->reset_gpio,
					    GPIOF_OUT_INIT_LOW, "reset");
		if (err) {
			pr_err("avionics-hi6138: Failed to"
			       " register Reset GPIO\n");
			return err;
		}
	}

	hi6138->irq = irq_of_parse_and_map(dev->of_node, 0);
	if (hi6138->irq < 0) {
		pr_err("avionics-hi6138: Failed to get irq: %d\n",
		       hi6138->irq);
		return hi6138->irq;
	}

	return 0;
}

static int hi6138_reset(struct spi_device *spi)
{
	struct hi6138 *hi6138 = spi_get_drvdata(spi);
	__u16 mcfg2;
	__u8 dev_id, rev_id;
	int err;

	gpio_set_value(hi6138->reset_gpio, 0);
	usleep_range(100, 150);
	gpio_set_value(hi6138->reset_gpio, 1);

	/* TODO: Add id check */

	err = hi6138_get_reg(spi, HI6138_REG_MCFG2, &mcfg2);
	if (err < 0) {
		pr_err("avionics-hi6138: Failed read master config register 2\n");
		return err;
	}

	dev_id = (mcfg2>>12)&0x03;
	rev_id = (mcfg2>>8)&0x0f;

	if (dev_id != 0x03) {
		pr_err("avionics-hi6138: Wrong Device ID: 0x%x,"
		       " this is the first verified access from this device"
		       " so this error could be due to an issue with the device or"
		       " the SPI bus.\n", dev_id);
		return -1;
	}

	if (rev_id != 0x01) {
		pr_err("avionics-hi6138: Wrong Revision ID: 0x%x\n", rev_id);
		return -1;
	}

	pr_info("avionics-hi6138: Device ID %d, Revision ID %d\n",
		dev_id, rev_id);

	err = hi6138_set_fastaccess(spi, HI6138_FAST_ACCESS_MCFG1,
				    HI6138_REG_MCFG1_TXINHA |
				    HI6138_REG_MCFG1_TXINHB |
				    HI6138_REG_MCFG1_INTSEL);
	if (err < 0) {
		pr_err("avionics-hi6138: Failed set master config register 1\n");
		return err;
	}

	pr_info("avionics-hi6138: Device up\n");
	return 0;
}

static struct avionics_mil1553bm avionics_mil1553bm_default = {
	.flags = 0,
};

static int hi6138_create_netdevs(struct spi_device *spi)
{
	struct hi6138 *hi6138 = spi_get_drvdata(spi);
	struct hi6138_priv *priv;
	int err;

	hi6138->wq = alloc_workqueue("hi6138", WQ_HIGHPRI, 0);
	if (!hi6138->wq) {
		pr_err("avionics-hi6138: Failed to allocate work-queue\n");
		return -ENOMEM;
	}

	hi6138->bm = avionics_device_alloc(sizeof(*priv),
					   &hi6138_mil553bm_ops);

	if (!hi6138->bm ) {
		pr_err("avionics-hi6138: Failed to allocate"
		       " Bus Monitor netdev\n");
		return -ENOMEM;
	}

	hi6138->bm->netdev_ops = &hi6138_bm_netdev_ops;
	priv = avionics_device_priv(hi6138->bm);

	if (!priv) {
		pr_err("avionics-hi6138: Failed to get private data"
		       " for Bus Monitor\n");
		return -EINVAL;
	}
	priv->dev = hi6138->bm;
	priv->spi = spi;
	priv->lock = &hi6138->lock;
	priv->bm_enabled = &hi6138->bm_enabled;
	skb_queue_head_init(&priv->skbq);
	priv->wq = hi6138->wq;

	INIT_DELAYED_WORK(&priv->worker, hi6138_bm_worker);

	err = hi6138_set_mil1553bm(&avionics_mil1553bm_default,
				    hi6138->bm);
	if (err) {
		pr_err("avionics-hi6138: Failed to set Bus Monitor"
		       " default settings\n");
		return -EINVAL;
	}

	err = avionics_device_register(hi6138->bm);
	if (err) {
		pr_err("avionics-hi6138: Failed to register"
		       " Bus Monitor netdev\n");
		return -EINVAL;
	}

	err = request_irq(hi6138->irq, hi6138_irq,
			  IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
			  "hi6138", hi6138);
	if (err) {
		pr_err("avionics-hi6138: Failed to register"
		       " irq %d\n", hi6138->irq);
		return -EINVAL;
	}
	priv->irq = hi6138->irq;
	disable_irq_nosync(priv->irq);

	return 0;
}

static int hi6138_remove(struct spi_device *spi)
{
	struct hi6138 *hi6138 = spi_get_drvdata(spi);
	struct hi6138_priv *priv;

	pr_info("avionics-hi6138: Removing Device\n");

	if (hi6138->bm) {
		priv = avionics_device_priv(hi6138->bm);
		if (priv) {
			skb_queue_purge(&priv->skbq);
			cancel_delayed_work_sync(&priv->worker);
		}
		avionics_device_unregister(hi6138->bm);
		avionics_device_free(hi6138->bm);
		hi6138->bm = NULL;
	}

	if (hi6138->irq) {
		free_irq(hi6138->irq, hi6138);
	}

	if (hi6138->reset_gpio > 0) {
		gpio_set_value(hi6138->reset_gpio, 1);
		gpio_free(hi6138->reset_gpio);
		hi6138->reset_gpio = 0;
	}

	if (hi6138->wq) {
		flush_scheduled_work();
		flush_workqueue(hi6138->wq);
		destroy_workqueue(hi6138->wq);
	}

	return 0;
}

static int hi6138_probe(struct spi_device *spi)
{
	struct hi6138 *hi6138;
	struct device *dev = &spi->dev;
	int err;

	pr_info("avionics-hi6138: Adding Device\n");

	hi6138 = devm_kzalloc(dev, sizeof(*hi6138), GFP_KERNEL);
	if (!hi6138) {
		pr_err("avionics-hi6138: Failed to allocate hi6138 memory\n");
		return -ENOMEM;
	}
	spi_set_drvdata(spi, hi6138);
	mutex_init(&hi6138->lock);
	atomic_set(&hi6138->bm_enabled, 0);

	err = hi6138_get_config(spi);
	if (err) {
		pr_err("avionics-hi6138: Failed to get system configuration"
		       " from dts file: %d\n",err);
		hi6138_remove(spi);
		return err;
	}

	err = hi6138_reset(spi);
	if (err) {
		pr_err("avionics-hi6138: Failed to bring device"
		       " out of reset: %d\n",err);
		hi6138_remove(spi);
		return err;
	}

	err = hi6138_create_netdevs(spi);
	if (err) {
		pr_err("avionics-hi6138: Failed to"
		       " register netdevs: %d\n", err);
		hi6138_remove(spi);
		return err;
	}

	return 0;
}

static struct spi_driver hi6138_spi_driver = {
	.driver = {
		.name = "hi6138",
		.of_match_table = hi6138_of_device_id,
	},
	.id_table = hi6138_spi_device_id,
	.probe = hi6138_probe,
	.remove = hi6138_remove,
};
module_spi_driver(hi6138_spi_driver);