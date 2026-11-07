// SPDX-License-Identifier: GPL-2.0
/*
 * q25-bt-vhci.c — Native Linux BlueZ HCI bridge (/dev/stpbt <-> /dev/vhci)
 *
 * Bridges MediaTek CONNSYS Bluetooth (/dev/stpbt from bt_drv_connac1x.ko) to a
 * standard Linux BlueZ controller (hci0 via /dev/vhci) with proper H4 packet
 * framing and factory BD_ADDR programming from /lib/firmware/BT_Addr.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t keep_running = 1;
static int vhci_global_fd = -1;

static void sig_handler(int sig)
{
	(void)sig;
	keep_running = 0;
}

static ssize_t h4_frame_len(const uint8_t *buf, size_t len)
{
	if (len < 1)
		return 0;
	switch (buf[0]) {
	case 0x01: /* HCI Command: [0x01, op_lo, op_hi, plen, ...] */
		if (len < 4)
			return 0;
		return 4 + buf[3];
	case 0x02: /* HCI ACL: [0x02, h_lo, h_hi, dlen_lo, dlen_hi, ...] */
		if (len < 5)
			return 0;
		return 5 + (buf[3] | ((size_t)buf[4] << 8));
	case 0x03: /* HCI SCO: [0x03, h_lo, h_hi, dlen, ...] */
		if (len < 4)
			return 0;
		return 4 + buf[3];
	case 0x04: /* HCI Event: [0x04, evt, plen, ...] */
		if (len < 3)
			return 0;
		return 3 + buf[2];
	case 0xff: /* VHCI vendor packet: [0xff, ...] */
		return (ssize_t)len;
	default:
		return -1;
	}
}

static void program_bdaddr(int stpbt_fd)
{
	uint8_t bdaddr[6];
	int fd = open("/lib/firmware/BT_Addr", O_RDONLY);
	if (fd < 0)
		fd = open("/opt/arch/usr/lib/firmware/BT_Addr", O_RDONLY);
	if (fd < 0)
		return;
	ssize_t n = read(fd, bdaddr, 6);
	close(fd);
	if (n != 6)
		return;

	/* MediaTek vendor HCI Set BD_ADDR (0xFC1A): LSB first */
	uint8_t cmd[10] = {
		0x01, 0x1a, 0xfc, 0x06,
		bdaddr[5], bdaddr[4], bdaddr[3], bdaddr[2], bdaddr[1], bdaddr[0]
	};
	if (write(stpbt_fd, cmd, sizeof(cmd)) == sizeof(cmd)) {
		struct pollfd pfd = { .fd = stpbt_fd, .events = POLLIN };
		if (poll(&pfd, 1, 1000) > 0) {
			uint8_t evt[64];
			(void)read(stpbt_fd, evt, sizeof(evt));
		}
		fprintf(stderr, "[q25-bt-vhci] Set factory BD_ADDR %02X:%02X:%02X:%02X:%02X:%02X\n",
			bdaddr[0], bdaddr[1], bdaddr[2], bdaddr[3], bdaddr[4], bdaddr[5]);
	}
}

static void forward_h4_packets(int dst_fd, uint8_t *ring, size_t *ring_len, int is_from_vhci)
{
	while (*ring_len > 0) {
		ssize_t flen = h4_frame_len(ring, *ring_len);
		if (flen == 0)
			break; /* Need more bytes */
		if (flen < 0 || (size_t)flen > *ring_len) {
			if (flen < 0) {
				/* Resync: drop 1 unknown byte */
				memmove(ring, ring + 1, *ring_len - 1);
				(*ring_len)--;
				continue;
			}
			break;
		}
		if (!(is_from_vhci && ring[0] == 0xff)) {
			if (is_from_vhci && ring[0] == 0x01 && flen >= 3) {
				uint16_t opcode = ring[1] | ((uint16_t)ring[2] << 8);
				if (opcode == 0x0c77) {
					/* HCI_OP_READ_SYNC_TRAIN_PARAMS: synthesize success */
					uint8_t rp[14] = {
						0x04, 0x0e, 0x0b, 0x01, 0x77, 0x0c,
						0x00, 0x80, 0x00, 0xa0, 0x06, 0x00, 0x00, 0x00
					};
					ssize_t w = write(vhci_global_fd, rp, sizeof(rp));
					(void)w;
					goto next_pkt;
				} else if (opcode == 0x100b || opcode == 0x100d) {
					/* HCI_OP_READ_LOCAL_CODECS[_V2]: synthesize 0 codecs */
					uint8_t rp[9] = {
						0x04, 0x0e, 0x06, 0x01, ring[1], ring[2],
						0x00, 0x00, 0x00
					};
					ssize_t w = write(vhci_global_fd, rp, sizeof(rp));
					(void)w;
					goto next_pkt;
				}
			}
			ssize_t w = write(dst_fd, ring, (size_t)flen);
			(void)w;
		}
next_pkt:
		if ((size_t)flen < *ring_len)
			memmove(ring, ring + flen, *ring_len - (size_t)flen);
		*ring_len -= (size_t)flen;
	}
}

int main(void)
{
	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);

	int stpbt_fd = open("/dev/stpbt", O_RDWR | O_NOCTTY);
	if (stpbt_fd < 0) {
		perror("open(/dev/stpbt)");
		return 1;
	}

	program_bdaddr(stpbt_fd);

	int vhci_fd = open("/dev/vhci", O_RDWR | O_NOCTTY);
	if (vhci_fd < 0) {
		perror("open(/dev/vhci)");
		close(stpbt_fd);
		return 1;
	}
	vhci_global_fd = vhci_fd;

	/* Create HCI_PRIMARY (0x00) controller */
	uint8_t create_req[2] = { 0xff, 0x00 };
	if (write(vhci_fd, create_req, sizeof(create_req)) != sizeof(create_req)) {
		perror("write(/dev/vhci HCI_PRIMARY)");
		close(vhci_fd);
		close(stpbt_fd);
		return 1;
	}

	fprintf(stderr, "[q25-bt-vhci] Bridging /dev/stpbt <-> /dev/vhci (hci0)\n");

	uint8_t rx_stp[8192], rx_vhci[8192];
	size_t len_stp = 0, len_vhci = 0;

	struct pollfd pfds[2] = {
		{ .fd = stpbt_fd, .events = POLLIN },
		{ .fd = vhci_fd,  .events = POLLIN },
	};

	while (keep_running) {
		int rc = poll(pfds, 2, 1000);
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (rc == 0)
			continue;

		if (pfds[0].revents & POLLIN) {
			ssize_t n = read(stpbt_fd, rx_stp + len_stp, sizeof(rx_stp) - len_stp);
			if (n > 0) {
				len_stp += (size_t)n;
				forward_h4_packets(vhci_fd, rx_stp, &len_stp, 0);
			} else if (n < 0 && errno != EAGAIN && errno != EINTR) {
				break;
			}
		}

		if (pfds[1].revents & POLLIN) {
			ssize_t n = read(vhci_fd, rx_vhci + len_vhci, sizeof(rx_vhci) - len_vhci);
			if (n > 0) {
				len_vhci += (size_t)n;
				forward_h4_packets(stpbt_fd, rx_vhci, &len_vhci, 1);
			} else if (n < 0 && errno != EAGAIN && errno != EINTR) {
				break;
			}
		}
	}

	close(vhci_fd);
	close(stpbt_fd);
	return 0;
}
