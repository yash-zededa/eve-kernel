/*
 * drivers/leds/leds-siemends-ipc127.c
 * Copyright (C) 2021 Roman Shaposhnik, rvs at apache dot org
 * Based on the leds-apu.c
 * Copyright (C) 2017 Alan Mizrahi, alan at mizrahi dot com dot ve
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the names of the copyright holders nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * Alternatively, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") version 2 as published by the Free
 * Software Foundation.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/dmi.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#define IPC127_FCH_ACPI_MMIO_BASE 0xD0C50500
#define IPC127_FCH_GPIO_BASE      IPC127_FCH_ACPI_MMIO_BASE /* we should have a longer window instead, but see below */
#define IPC127_LEDON              0x00 /* FIXME: we can't seem to be able to turn it off */
#define IPC127_LEDOFF             0x01 /* fully -- so we go between RED and GREEN        */
#define IPC127_IOSIZE             sizeof(u8)

/* LED private data */
struct ipc127_led_priv {
	struct led_classdev cdev;
	void __iomem *addr; /* for ioread/iowrite */
};
#define cdev_to_priv(c) container_of(c, struct ipc127_led_priv, cdev)

static struct ipc127_led_pdata {
	struct platform_device *pdev;
	struct ipc127_led_priv *pled;
	spinlock_t lock;
} *leds;

/* LED profile */
/* based on https://cache.industry.siemens.com/dl/dl-media/673/109762673/att_975118/v3/ipc127e_operating_instructions/en-US/index.html#29951493a19e32f30da71d71fa3159d4 */
static const struct ipc127_led_profile {
	const char *name;
	enum led_brightness brightness;
	unsigned long offset; /* for devm_ioremap */
} ipc127_led_profile[] = {
	{ "ipc127:red:1",   LED_OFF,  IPC127_FCH_GPIO_BASE + 0x1A0 },
	{ "ipc127:green:1", LED_OFF,  IPC127_FCH_GPIO_BASE + 0x1A8 },
	{ "ipc127:red:2",   LED_OFF,  IPC127_FCH_GPIO_BASE + 0x1C8 },
	{ "ipc127:green:2", LED_OFF,  IPC127_FCH_GPIO_BASE + 0x1D0 },
	{ "ipc127:red:3",   LED_OFF,  IPC127_FCH_GPIO_BASE + 0x1E0 },
	{ "ipc127:green:3", LED_OFF,  IPC127_FCH_GPIO_BASE + 0x198 },
};

static const struct dmi_system_id ipc127_led_dmi_table[] __initconst = {
	{
		.ident = "ipc127",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "SIEMENS AG"),
			DMI_MATCH(DMI_PRODUCT_NAME, "SIMATIC IPC127E")
		}
	},
	{}
};
MODULE_DEVICE_TABLE(dmi, ipc127_led_dmi_table);


static void ipc127_led_brightness_set(struct led_classdev *led, enum led_brightness value)
{
	struct ipc127_led_priv *pled = cdev_to_priv(led);

	spin_lock(&leds->lock);
	iowrite8(value ? IPC127_LEDON : IPC127_LEDOFF, pled->addr);
	spin_unlock(&leds->lock);
}

static int __init ipc127_led_probe(struct platform_device *pdev)
{
	int i;
	int err;

	leds = devm_kzalloc(&pdev->dev, sizeof(*leds), GFP_KERNEL);
	if (!leds)
		return -ENOMEM;

	leds->pled = devm_kcalloc(&pdev->dev,
		ARRAY_SIZE(ipc127_led_profile), sizeof(struct ipc127_led_priv),
		GFP_KERNEL);
	if (!leds->pled)
		return -ENOMEM;

	leds->pdev = pdev;
	spin_lock_init(&leds->lock);

	for (i = 0; i < ARRAY_SIZE(ipc127_led_profile); i++) {
		struct ipc127_led_priv *pled = &leds->pled[i];
		struct led_classdev *led_cdev = &pled->cdev;

		led_cdev->name = ipc127_led_profile[i].name;
		led_cdev->brightness = ipc127_led_profile[i].brightness;
		led_cdev->max_brightness = 1;
		led_cdev->flags = LED_CORE_SUSPENDRESUME;
		led_cdev->brightness_set = ipc127_led_brightness_set;

		pled->addr = devm_ioremap(&pdev->dev,
				ipc127_led_profile[i].offset, IPC127_IOSIZE);
		if (!pled->addr) {
			err = -ENOMEM;
			goto error;
		}

		err = led_classdev_register(&pdev->dev, led_cdev);
		if (err)
			goto error;

		ipc127_led_brightness_set(led_cdev, ipc127_led_profile[i].brightness);
	}

	return 0;

error:
	while (i-- > 0)
		led_classdev_unregister(&leds->pled[i].cdev);

	return err;
}

static struct platform_driver ipc127_led_driver = {
	.driver = {
		.name = KBUILD_MODNAME,
	},
};

static int __init ipc127_led_init(void)
{
	struct platform_device *pdev;
	int err;

	if (!(dmi_match(DMI_SYS_VENDOR, "SIEMENS AG") &&
	      dmi_match(DMI_PRODUCT_NAME, "SIMATIC IPC127E"))) {
		pr_err("No SIMATIC IPC127E detected.\n");
		return -ENODEV;
	}

	pdev = platform_device_register_simple(KBUILD_MODNAME, -1, NULL, 0);
	if (IS_ERR(pdev)) {
		pr_err("Device allocation failed\n");
		return PTR_ERR(pdev);
	}

	err = platform_driver_probe(&ipc127_led_driver, ipc127_led_probe);
	if (err) {
		pr_err("Probe platform driver failed\n");
		platform_device_unregister(pdev);
	}

	return err;
}

static void __exit ipc127_led_exit(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(ipc127_led_profile); i++)
		led_classdev_unregister(&leds->pled[i].cdev);

	platform_device_unregister(leds->pdev);
	platform_driver_unregister(&ipc127_led_driver);
}

module_init(ipc127_led_init);
module_exit(ipc127_led_exit);

MODULE_AUTHOR("Roman Shaposhnik");
MODULE_DESCRIPTION("Siemens IPC127 MMIO GPIO-driven LEDS");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:leds_siemens_ipc127");
