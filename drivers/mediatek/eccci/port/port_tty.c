// SPDX-License-Identifier: GPL-2.0
/* CCCI UART2 is an AT byte stream, not a userspace-framed CCCI char port. */
#include <linux/module.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/tty_flip.h>
#include <linux/workqueue.h>

#include "ccci_bm.h"
#include "ccci_fsm.h"
#include "ccci_hif.h"
#include "modem_sys.h"
#include "port_t.h"

#define CCCI_TTY_RX_LIMIT 32

struct ccci_tty {
	struct tty_port tty_port;
	struct tty_driver *driver;
	struct port_t *port;
	struct delayed_work rx_work;
	unsigned int mctrl;
};

static void ccci_tty_rx_work(struct work_struct *work)
{
	struct ccci_tty *ct = container_of(to_delayed_work(work),
					 struct ccci_tty, rx_work);
	struct port_t *port = ct->port;
	struct sk_buff *skb;
	int copied;

	while ((skb = skb_dequeue(&port->rx_skb_list))) {
		copied = tty_insert_flip_string(&ct->tty_port, skb->data, skb->len);
		skb_pull(skb, copied);
		tty_flip_buffer_push(&ct->tty_port);
		if (skb->len) {
			/* Keep partial packets: AT responses must never be truncated. */
			skb_queue_head(&port->rx_skb_list, skb);
			schedule_delayed_work(&ct->rx_work, msecs_to_jiffies(10));
			return;
		}
		ccci_free_skb(skb);
		port_ask_more_req_to_md(port);
	}
}

static int ccci_tty_activate(struct tty_port *tport, struct tty_struct *tty)
{
	struct ccci_tty *ct = container_of(tport, struct ccci_tty, tty_port);
	int ret;

	ret = port_user_register(ct->port);
	if (!ret)
		atomic_set(&ct->port->usage_cnt, 1);
	return ret;
}

static void ccci_tty_shutdown(struct tty_port *tport)
{
	struct ccci_tty *ct = container_of(tport, struct ccci_tty, tty_port);
	struct port_t *port = ct->port;
	struct sk_buff *skb;
	unsigned long flags;

	/* Serialize with receive before cancelling work and discarding queued URCs. */
	spin_lock_irqsave(&port->rx_skb_list.lock, flags);
	atomic_set(&port->usage_cnt, 0);
	spin_unlock_irqrestore(&port->rx_skb_list.lock, flags);
	cancel_delayed_work_sync(&ct->rx_work);
	while ((skb = skb_dequeue(&port->rx_skb_list)))
		ccci_free_skb(skb);
	port_ask_more_req_to_md(port);
	port_user_unregister(port);
}

static bool ccci_tty_carrier_raised(struct tty_port *tport)
{
	return true;
}

static const struct tty_port_operations ccci_tty_port_ops = {
	.activate = ccci_tty_activate,
	.shutdown = ccci_tty_shutdown,
	.carrier_raised = ccci_tty_carrier_raised,
};

static int ccci_tty_install(struct tty_driver *driver, struct tty_struct *tty)
{
	struct ccci_tty *ct = driver->driver_state;

	tty->driver_data = ct;
	return tty_port_install(&ct->tty_port, driver, tty);
}

static int ccci_tty_open(struct tty_struct *tty, struct file *file)
{
	struct ccci_tty *ct = tty->driver_data;

	return tty_port_open(&ct->tty_port, tty, file);
}

static void ccci_tty_close(struct tty_struct *tty, struct file *file)
{
	struct ccci_tty *ct = tty->driver_data;

	tty_port_close(&ct->tty_port, tty, file);
}

static void ccci_tty_hangup(struct tty_struct *tty)
{
	struct ccci_tty *ct = tty->driver_data;

	tty_port_hangup(&ct->tty_port);
}

static ssize_t ccci_tty_write(struct tty_struct *tty, const u8 *buf, size_t count)
{
	struct ccci_tty *ct = tty->driver_data;
	struct port_t *port = ct->port;
	struct ccci_header *header;
	struct sk_buff *skb;
	int ret;

	if (!count)
		return 0;
	count = min_t(size_t, count, CCCI_MTU);
	skb = ccci_alloc_skb(count + sizeof(*header), 1, 0);
	if (!skb)
		return 0;
	header = skb_put_zero(skb, sizeof(*header));
	header->data[1] = count + sizeof(*header);
	header->channel = port->tx_ch;
	skb_put_data(skb, buf, count);
	ret = port_send_skb_to_md(port, skb, 0);
	if (ret) {
		ccci_free_skb(skb);
		return ret == -EBUSY ? 0 : ret;
	}
	return count;
}

static unsigned int ccci_tty_write_room(struct tty_struct *tty)
{
	struct ccci_tty *ct = tty->driver_data;
	struct port_t *port = ct->port;

	if (ccci_fsm_get_md_state(port->md_id) != READY)
		return 0;
	return ccci_hif_write_room(port->hif_id, port->txq_index) > 0 ? CCCI_MTU : 0;
}

static int ccci_tty_tiocmget(struct tty_struct *tty)
{
	struct ccci_tty *ct = tty->driver_data;

	return ct->mctrl | TIOCM_CTS | TIOCM_DSR | TIOCM_CAR;
}

static int ccci_tty_tiocmset(struct tty_struct *tty, unsigned int set,
			   unsigned int clear)
{
	struct ccci_tty *ct = tty->driver_data;

	/* No UART pins: dropping DTR must not shut down the shared MD1 modem. */
	ct->mctrl = ((ct->mctrl | set) & ~clear) & (TIOCM_DTR | TIOCM_RTS);
	return 0;
}

static const struct tty_operations ccci_tty_ops = {
	.install = ccci_tty_install,
	.open = ccci_tty_open,
	.close = ccci_tty_close,
	.hangup = ccci_tty_hangup,
	.write = ccci_tty_write,
	.write_room = ccci_tty_write_room,
	.tiocmget = ccci_tty_tiocmget,
	.tiocmset = ccci_tty_tiocmset,
};

static int port_tty_init(struct port_t *port)
{
	struct ccci_tty *ct;
	struct tty_driver *driver;
	struct device *dev;
	int ret;

	ct = kzalloc(sizeof(*ct), GFP_KERNEL);
	if (!ct)
		return -ENOMEM;
	driver = tty_alloc_driver(1, TTY_DRIVER_REAL_RAW | TTY_DRIVER_DYNAMIC_DEV);
	if (IS_ERR(driver)) {
		ret = PTR_ERR(driver);
		goto free_ct;
	}
	ct->driver = driver;
	ct->port = port;
	tty_port_init(&ct->tty_port);
	ct->tty_port.ops = &ccci_tty_port_ops;
	INIT_DELAYED_WORK(&ct->rx_work, ccci_tty_rx_work);
	driver->driver_name = "ccci-at";
	driver->name = "ttyC";
	driver->type = TTY_DRIVER_TYPE_SERIAL;
	driver->subtype = SERIAL_TYPE_NORMAL;
	driver->init_termios = tty_std_termios;
	driver->init_termios.c_iflag = 0;
	driver->init_termios.c_oflag = 0;
	driver->init_termios.c_lflag = 0;
	driver->init_termios.c_cflag = B115200 | CS8 | CREAD | CLOCAL;
	driver->driver_state = ct;
	tty_set_operations(driver, &ccci_tty_ops);
	ret = tty_register_driver(driver);
	if (ret)
		goto destroy_port;
	port->private_data = ct;
	port->skb_from_pool = 1;
	dev = tty_port_register_device(&ct->tty_port, driver, 0,
				       ccci_md_get_dev_by_id(port->md_id));
	if (IS_ERR(dev)) {
		ret = PTR_ERR(dev);
		port->private_data = NULL;
		tty_unregister_driver(driver);
		goto destroy_port;
	}
	/* The enclosing CCCI driver has no remove path and is non-unloadable. */
	return 0;

destroy_port:
	tty_port_destroy(&ct->tty_port);
	tty_driver_kref_put(driver);
free_ct:
	pr_err("ccci-at: cannot register UART2 TTY: %d\n", ret);
	kfree(ct);
	return ret;
}

static int port_tty_recv_skb(struct port_t *port, struct sk_buff *skb)
{
	struct ccci_tty *ct = port->private_data;
	unsigned long flags;

	if (!ct || skb->len < sizeof(struct ccci_header))
		return -CCCI_ERR_DROP_PACKET;
	spin_lock_irqsave(&port->rx_skb_list.lock, flags);
	if (!atomic_read(&port->usage_cnt)) {
		spin_unlock_irqrestore(&port->rx_skb_list.lock, flags);
		return -CCCI_ERR_DROP_PACKET;
	}
	if (port->rx_skb_list.qlen >= CCCI_TTY_RX_LIMIT) {
		port->flags |= PORT_F_RX_FULLED;
		spin_unlock_irqrestore(&port->rx_skb_list.lock, flags);
		return -CCCI_ERR_PORT_RX_FULL;
	}
	port->flags &= ~PORT_F_RX_FULLED;
	skb_pull(skb, sizeof(struct ccci_header));
	__skb_queue_tail(&port->rx_skb_list, skb);
	atomic_inc(&port->rx_pkg_cnt);
	schedule_delayed_work(&ct->rx_work, 0);
	spin_unlock_irqrestore(&port->rx_skb_list.lock, flags);
	__pm_wakeup_event(port->rx_wakelock, 500);
	return 0;
}

static void port_tty_queue_notify(struct port_t *port, int dir, int qno,
				  unsigned int state)
{
	struct ccci_tty *ct = port->private_data;

	if (ct && dir == OUT)
		tty_port_tty_wakeup(&ct->tty_port);
}

static void port_tty_md_notify(struct port_t *port, unsigned int state)
{
	struct ccci_tty *ct = port->private_data;

	if (!ct)
		return;
	if (state == READY)
		tty_port_tty_wakeup(&ct->tty_port);
	else if (state == EXCEPTION || state == GATED)
		tty_port_tty_hangup(&ct->tty_port, false);
}

struct port_ops tty_port_ops = {
	.init = port_tty_init,
	.recv_skb = port_tty_recv_skb,
	.queue_state_notify = port_tty_queue_notify,
	.md_state_notify = port_tty_md_notify,
};
