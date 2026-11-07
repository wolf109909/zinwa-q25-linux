#ifndef _LINUX_Q25_COMPAT_H
#define _LINUX_Q25_COMPAT_H

#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/hrtimer.h>
#include <linux/alarmtimer.h>
#include <linux/sched/clock.h>
#include <linux/preempt.h>
#include <linux/device/class.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/if_arp.h>
#include <linux/skbuff.h>
#include <net/gro.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>

static inline char *_q25_strncpy(char *dst, const char *src, size_t n)
{
	strscpy(dst, src, n);
	return dst;
}
#ifndef strncpy
#define strncpy(dst, src, n) _q25_strncpy(dst, src, n)
#ifndef strlcpy
#define strlcpy(dst, src, n) strscpy(dst, src, n)
#endif
#endif

#ifndef from_timer
#define from_timer(var, callback_timer, timer_fieldname) \
	timer_container_of(var, callback_timer, timer_fieldname)
#endif

#ifndef del_timer
#define del_timer(t) timer_delete(t)
#endif

#ifndef del_timer_sync
#define del_timer_sync(t) timer_delete_sync(t)
#endif

#ifndef hrtimer_init
#define hrtimer_init(t, clk, mode) hrtimer_setup(t, hrtimer_dummy_timeout, clk, mode)
#endif

#ifndef alarm_start_relative
#define alarm_start_relative(a, kt) alarm_start_timer(a, kt, true)
#endif

#ifndef MODULE_SUPPORTED_DEVICE
#define MODULE_SUPPORTED_DEVICE(name)
#endif

#ifndef in_irq
#define in_irq() in_hardirq()
#endif

#ifndef netif_rx_ni
#define netif_rx_ni(skb) netif_rx(skb)
#endif

#ifndef random_ether_addr
#define random_ether_addr(addr) eth_random_addr(addr)
#endif

#ifndef MTK_SIP_KERNEL_GET_RND
#define MTK_SIP_KERNEL_GET_RND 0xC200025A
#endif

static inline void __skb_frag_set_page(skb_frag_t *frag, struct page *page)
{
	frag->netmem = page_to_netmem(page);
}

static inline int gpio_set_debounce(unsigned int gpio, unsigned int debounce)
{
	return gpiod_set_debounce(gpio_to_desc(gpio), debounce);
}

static inline struct class *_q25_class_create_1(const char *name)
{
	return class_create(name);
}

static inline struct class *_q25_class_create_2(struct module *owner, const char *name)
{
	return class_create(name);
}

#define _q25_class_create_X(x, a, b, FUNC, ...) FUNC
#define class_create(...) _q25_class_create_X(, ##__VA_ARGS__, _q25_class_create_2(__VA_ARGS__), _q25_class_create_1(__VA_ARGS__))

static inline void _q25_netif_napi_add_3(struct net_device *dev, struct napi_struct *napi,
					 int (*poll)(struct napi_struct *, int))
{
	netif_napi_add(dev, napi, poll);
}

static inline void _q25_netif_napi_add_4(struct net_device *dev, struct napi_struct *napi,
					 int (*poll)(struct napi_struct *, int), int weight)
{
	netif_napi_add_weight(dev, napi, poll, weight);
}

#define _q25_netif_napi_add_X(x, a, b, c, d, FUNC, ...) FUNC
#define netif_napi_add(...) _q25_netif_napi_add_X(, ##__VA_ARGS__, _q25_netif_napi_add_4(__VA_ARGS__), _q25_netif_napi_add_3(__VA_ARGS__))

#include <linux/pid.h>
#include <net/iw_handler.h>
#define find_task_by_vpid(nr) pid_task(find_vpid(nr), PIDTYPE_PID)
#define sched_setattr(p, attr) (0)
#define wireless_send_event(dev, cmd, wrqu, extra) do { (void)(dev); (void)(cmd); (void)(wrqu); (void)(extra); } while (0)

#ifndef CONFIG_MTK_ECCCI_DRIVER_MODULE
#define CONFIG_MTK_ECCCI_DRIVER_MODULE 1
#endif
#ifndef CONFIG_MTK_ECCCI_C2K
#define CONFIG_MTK_ECCCI_C2K 1
#endif

#endif
