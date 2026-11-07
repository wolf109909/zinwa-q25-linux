// SPDX-License-Identifier: GPL-2.0
/*
 * q25-wmt-init.c — Native Linux CONNSYS (MT6789 / MT6631) & Wi-Fi initializer
 *
 * Replaces Android Bionic /vendor/bin/wmt_loader + /vendor/bin/wmt_launcher
 * on Mainline Linux 7.3+ for the Zinwa Q25.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define WMT_DETECT_IOC_MAGIC            'w'
#define COMBO_IOCTL_GET_CHIP_ID         _IOR(WMT_DETECT_IOC_MAGIC, 0, int)
#define COMBO_IOCTL_SET_CHIP_ID         _IOW(WMT_DETECT_IOC_MAGIC, 1, int)
#define COMBO_IOCTL_EXT_CHIP_DETECT     _IOR(WMT_DETECT_IOC_MAGIC, 2, int)
#define COMBO_IOCTL_GET_SOC_CHIP_ID     _IOR(WMT_DETECT_IOC_MAGIC, 3, int)
#define COMBO_IOCTL_DO_MODULE_INIT      _IOR(WMT_DETECT_IOC_MAGIC, 4, int)
#define COMBO_IOCTL_MODULE_CLEANUP      _IOR(WMT_DETECT_IOC_MAGIC, 5, int)
#define COMBO_IOCTL_EXT_CHIP_PWR_ON     _IOR(WMT_DETECT_IOC_MAGIC, 6, int)
#define COMBO_IOCTL_GET_ADIE_CHIP_ID    _IOR(WMT_DETECT_IOC_MAGIC, 9, int)
#define COMBO_IOCTL_CONNSYS_SOC_HW_INIT _IOR(WMT_DETECT_IOC_MAGIC, 10, int)

#define WMT_IOC_MAGIC                   0xa0
#define WMT_IOCTL_SET_PATCH_NAME        _IOW(WMT_IOC_MAGIC, 4, char *)
#define WMT_IOCTL_SET_STP_MODE          _IOW(WMT_IOC_MAGIC, 5, int)
#define WMT_IOCTL_FUNC_ONOFF_CTRL       _IOW(WMT_IOC_MAGIC, 6, int)
#define WMT_IOCTL_LPBK_POWER_CTRL       _IOW(WMT_IOC_MAGIC, 7, int)
#define WMT_IOCTL_WMT_CFG_NAME          _IOW(WMT_IOC_MAGIC, 12, char *)
#define WMT_IOCTL_SET_LAUNCHER_KILL     _IOW(WMT_IOC_MAGIC, 13, int)
#define WMT_IOCTL_WMT_QUERY_CHIPID      _IOR(WMT_IOC_MAGIC, 16, int)

static int init_wmt_detect(void)
{
	int fd = open("/dev/wmtdetect", O_RDWR | O_NOCTTY);
	if (fd < 0) {
		perror("open(/dev/wmtdetect)");
		return -1;
	}

	int ret = ioctl(fd, COMBO_IOCTL_CONNSYS_SOC_HW_INIT, 0);
	printf("[q25-wmt-init] COMBO_IOCTL_CONNSYS_SOC_HW_INIT: %d\n", ret);

	/* Allow platform driver probe to settle */
	usleep(100000);

	int pwr = ioctl(fd, COMBO_IOCTL_EXT_CHIP_PWR_ON, 0);
	(void)pwr;

	int chipid = ioctl(fd, COMBO_IOCTL_GET_SOC_CHIP_ID, 0);
	if (chipid <= 0)
		chipid = 0x6789;
	printf("[q25-wmt-init] SoC chipid: 0x%x\n", chipid);

	ioctl(fd, COMBO_IOCTL_SET_CHIP_ID, chipid);

	int adie = ioctl(fd, COMBO_IOCTL_GET_ADIE_CHIP_ID, 0);
	printf("[q25-wmt-init] A-die chipid: 0x%x\n", adie);

	ioctl(fd, COMBO_IOCTL_MODULE_CLEANUP, chipid);
	ret = ioctl(fd, COMBO_IOCTL_DO_MODULE_INIT, chipid);
	printf("[q25-wmt-init] COMBO_IOCTL_DO_MODULE_INIT: %d\n", ret);

	close(fd);
	return ret;
}

static int init_stpwmt(void)
{
	int fd = -1;
	for (int i = 0; i < 20; i++) {
		fd = open("/dev/stpwmt", O_RDWR | O_NOCTTY);
		if (fd >= 0)
			break;
		usleep(100000);
	}
	if (fd < 0) {
		perror("open(/dev/stpwmt)");
		return -1;
	}

	int chipid = ioctl(fd, WMT_IOCTL_WMT_QUERY_CHIPID, 0);
	printf("[q25-wmt-init] WMT_QUERY_CHIPID: 0x%x\n", chipid);

	char cfg_name[256] = "WMT_SOC.cfg";
	ioctl(fd, WMT_IOCTL_WMT_CFG_NAME, cfg_name);

	/* STP_BTIF_FULL (3) | (WMT_FM_I2C (2) << 4) = 0x23 */
	int stp_mode = 0x23;
	int ret = ioctl(fd, WMT_IOCTL_SET_STP_MODE, stp_mode);
	printf("[q25-wmt-init] WMT_IOCTL_SET_STP_MODE(0x%x): %d\n", stp_mode, ret);

	ioctl(fd, WMT_IOCTL_SET_LAUNCHER_KILL, 0);

	/* Power on CONNSYS MCU & download ROM patches (arg=2 forces power on) */
	ret = ioctl(fd, WMT_IOCTL_LPBK_POWER_CTRL, 2);
	printf("[q25-wmt-init] WMT_IOCTL_LPBK_POWER_CTRL(2): %d\n", ret);

	close(fd);
	return ret;
}

static int init_wifi(void)
{
	int fd = -1;
	for (int i = 0; i < 20; i++) {
		fd = open("/dev/wmtWifi", O_RDWR | O_NOCTTY);
		if (fd >= 0)
			break;
		usleep(100000);
	}
	if (fd < 0) {
		perror("open(/dev/wmtWifi)");
		return -1;
	}

	/* Load factory WIFI NVRAM calibration if available */
	int nv_fd = open("/lib/firmware/WIFI", O_RDONLY);
	if (nv_fd < 0)
		nv_fd = open("/opt/arch/usr/lib/firmware/WIFI", O_RDONLY);
	if (nv_fd >= 0) {
		uint8_t buf[12 + 4096];
		memcpy(buf, "WR-BUF:NVRAM", 12);
		ssize_t n = read(nv_fd, buf + 12, 4096);
		close(nv_fd);
		if (n > 0) {
			ssize_t w = write(fd, buf, 12 + n);
			printf("[q25-wmt-init] Wrote WIFI NVRAM (%zd bytes): %zd\n", n, w);
		}
	}

	ssize_t w = write(fd, "1", 1);
	printf("[q25-wmt-init] Enabled Wi-Fi (/dev/wmtWifi '1'): %zd\n", w);
	close(fd);
	return (w == 1) ? 0 : -1;
}

int main(int argc, char **argv)
{
	int do_wifi = 1;
	if (argc > 1 && strcmp(argv[1], "--no-wifi") == 0)
		do_wifi = 0;

	if (access("/dev/stpwmt", F_OK) != 0) {
		if (init_wmt_detect() != 0)
			return 1;
	}

	if (init_stpwmt() != 0)
		return 1;

	if (do_wifi && access("/dev/wmtWifi", F_OK) == 0) {
		if (init_wifi() != 0)
			return 1;
	}

	return 0;
}
