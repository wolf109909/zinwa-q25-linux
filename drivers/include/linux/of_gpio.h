/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Compatibility <linux/of_gpio.h> shim for Linux 7.1+ / 7.3+ where the legacy
 * header was removed from mainline.
 *
 * On older kernels that still ship <linux/of_gpio.h>, delegates via #include_next.
 * On Linux 7.3+, implements of_get_named_gpio() on top of exported mainline
 * OF phandle and gpio_device APIs.
 */
#ifndef __Q25_COMPAT_LINUX_OF_GPIO_H
#define __Q25_COMPAT_LINUX_OF_GPIO_H

#if __has_include_next(<linux/of_gpio.h>)
#include_next <linux/of_gpio.h>
#else

#include <linux/compiler.h>
#include <linux/errno.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/of.h>

static inline int of_get_named_gpio(const struct device_node *np,
				    const char *propname, int index)
{
	struct of_phandle_args gpiospec;
	struct gpio_device *gdev;
	struct gpio_desc *desc;
	int ret;

	ret = of_parse_phandle_with_args_map(np, propname, "gpio", index,
					     &gpiospec);
	if (ret)
		return ret;

	gdev = gpio_device_find_by_fwnode(of_fwnode_handle(gpiospec.np));
	of_node_put(gpiospec.np);
	if (!gdev)
		return -EPROBE_DEFER;

	desc = gpio_device_get_desc(gdev, gpiospec.args[0]);
	gpio_device_put(gdev);
	if (IS_ERR(desc))
		return PTR_ERR(desc);

	return desc_to_gpio(desc);
}

#endif /* __has_include_next(<linux/of_gpio.h>) */

#endif /* __Q25_COMPAT_LINUX_OF_GPIO_H */
