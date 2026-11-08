// SPDX-License-Identifier: GPL-2.0
/* Exercise the real address initialization without devices or calibration files. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int mock_open(const char *path, int flags, ...);
static ssize_t mock_read(int fd, void *buf, size_t count);
static ssize_t mock_write(int fd, const void *buf, size_t count);
static int mock_close(int fd);
static int mock_poll(struct pollfd *fds, nfds_t nfds, int timeout);

#define open mock_open
#define read mock_read
#define write mock_write
#define close mock_close
#define poll mock_poll
#define main q25_bt_vhci_main
#include "../src/q25-bt-vhci.c"
#undef open
#undef read
#undef write
#undef close
#undef poll
#undef main

enum { ADDRESS_FD = 11, STP_FD = 12 };
static int scenario, opens, writes;

static int mock_open(const char *path, int flags, ...)
{
	assert(flags == O_RDONLY);
	if (opens++ == 0) {
		assert(strcmp(path, "/var/lib/q25/android/mnt/vendor/nvdata/APCFG/APRDEB/BT_Addr") == 0);
		if (scenario == 1 || scenario == 2) {
			errno = ENOENT;
			return -1;
		}
	} else {
		assert(opens == 2);
		assert(strcmp(path, "/lib/firmware/BT_Addr") == 0);
		if (scenario == 2) {
			errno = ENOENT;
			return -1;
		}
	}
	return ADDRESS_FD;
}

static ssize_t mock_read(int fd, void *buf, size_t count)
{
	if (fd == ADDRESS_FD) {
		/* Synthetic bytes, never a real device identifier. */
		const uint8_t address[] = { 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc };
		assert(count == sizeof(address));
		memcpy(buf, address, sizeof(address));
		return scenario == 3 ? 5 : 6;
	}
	assert(fd == STP_FD && count >= 7);
	const uint8_t event[] = { 0x04, 0x0e, 0x04, 0x01, 0x1a, 0xfc, 0x00 };
	memcpy(buf, event, sizeof(event));
	return sizeof(event);
}

static ssize_t mock_write(int fd, const void *buf, size_t count)
{
	const uint8_t command[] = { 0x01, 0x1a, 0xfc, 0x06, 0xbc, 0x9a, 0x78, 0x56, 0x34, 0x12 };
	assert(fd == STP_FD && count == sizeof(command));
	assert(memcmp(buf, command, sizeof(command)) == 0);
	writes++;
	return scenario == 4 ? (ssize_t)count - 1 : (ssize_t)count;
}

static int mock_close(int fd)
{
	assert(fd == ADDRESS_FD);
	return 0;
}

static int mock_poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
	assert(nfds == 1 && fds[0].fd == STP_FD);
	assert(fds[0].events == POLLIN && timeout == 1000);
	return 1;
}

int main(int argc, char **argv)
{
	assert(argc == 2);
	scenario = atoi(argv[1]);
	assert(scenario >= 0 && scenario <= 4);
	program_bdaddr(STP_FD);
	assert(opens == ((scenario == 1 || scenario == 2) ? 2 : 1));
	assert(writes == ((scenario == 2 || scenario == 3) ? 0 : 1));
	return 0;
}
