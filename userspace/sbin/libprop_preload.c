// SPDX-License-Identifier: GPL-2.0
/*
 * libprop_preload.c — Standalone Bionic-compatible LD_PRELOAD shim for
 * Android property_get / property_set and acquire_wake_lock / release_wake_lock
 * (used by MediaTek nvram_daemon, ccci_mdinit, ccci_fsd, and ccci_rpcd on
 * native Linux without Android init or Binder servicemanager).
 *
 * Compiled with -nostdlib so it has zero libc dependencies and loads cleanly
 * under both Bionic linker64 and glibc.
 */

typedef unsigned long size_t;
typedef long ssize_t;

#define AT_FDCWD (-100)
#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT  0100
#define O_TRUNC  01000
#define O_APPEND 02000

static inline long sys3(long n, long a0, long a1, long a2)
{
	register long x8 __asm__("x8") = n;
	register long x0 __asm__("x0") = a0;
	register long x1 __asm__("x1") = a1;
	register long x2 __asm__("x2") = a2;
	__asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory", "cc");
	return x0;
}

static inline long sys4(long n, long a0, long a1, long a2, long a3)
{
	register long x8 __asm__("x8") = n;
	register long x0 __asm__("x0") = a0;
	register long x1 __asm__("x1") = a1;
	register long x2 __asm__("x2") = a2;
	register long x3 __asm__("x3") = a3;
	__asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x8) : "memory", "cc");
	return x0;
}

static int s_len(const char *s)
{
	int n = 0;
	if (!s)
		return 0;
	while (s[n])
		n++;
	return n;
}

static int s_eq(const char *a, const char *b)
{
	if (!a || !b)
		return 0;
	while (*a && *b && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

static int s_copy(char *dst, const char *src, int max)
{
	int i = 0;
	if (!dst)
		return 0;
	if (!src) {
		dst[0] = '\0';
		return 0;
	}
	while (src[i] && i < max - 1) {
		dst[i] = src[i];
		i++;
	}
	dst[i] = '\0';
	return i;
}

static void make_prop_path(char *out, int max, const char *key)
{
	const char *prefix = "/tmp/mtk_props/";
	int i = 0;
	while (*prefix && i < max - 1)
		out[i++] = *prefix++;
	while (*key && i < max - 1) {
		char c = *key++;
		if (c == '/')
			c = '_';
		out[i++] = c;
	}
	out[i] = '\0';
}

static const char *lookup_builtin(const char *key)
{
	if (s_eq(key, "vendor.service.nvram_init"))
		return "Ready";
	if (s_eq(key, "vendor.service.nvram_restore"))
		return "Ready";
	if (s_eq(key, "vendor.mtk.nvram.ready"))
		return "1";
	if (s_eq(key, "vendor.connsys.driver.ready"))
		return "yes";
	if (s_eq(key, "vendor.connsys.adie.chipid"))
		return "0x6631";
	if (s_eq(key, "persist.vendor.connsys.chipid"))
		return "0x6789";
	if (s_eq(key, "ro.vendor.mtk_protocol1_rat_config"))
		return "Lf/Lt/W/G";
	if (s_eq(key, "ro.vendor.mtk_ps1_rat"))
		return "Lf/Lt/W/G";
	if (s_eq(key, "ro.vendor.mtk_ril_mode"))
		return "c6m_1rild";
	if (s_eq(key, "ro.vendor.md_prop_ver"))
		return "1";
	if (s_eq(key, "ro.vendor.mtk_md_world_mode_support"))
		return "1";
	if (s_eq(key, "ro.vendor.md_auto_setup_ims"))
		return "1";
	if (s_eq(key, "ro.vendor.mtk_data_config"))
		return "1";
	if (s_eq(key, "ro.vendor.mtk_eccci_c2k"))
		return "1";
	if (s_eq(key, "persist.vendor.md_c2k_cap_dep_check"))
		return "0";
	if (s_eq(key, "ro.build.type"))
		return "user";
	if (s_eq(key, "ro.boot.slot_suffix"))
		return "_a";
	if (s_eq(key, "ro.telephony.sim.count"))
		return "1";
	return 0;
}

__attribute__((visibility("default")))
int property_get(const char *key, char *value, const char *default_value)
{
	char path[256];
	if (!value)
		return 0;
	value[0] = '\0';
	if (!key)
		return s_copy(value, default_value, 92);

	make_prop_path(path, sizeof(path), key);
	long fd = sys4(56 /* __NR_openat */, AT_FDCWD, (long)path, O_RDONLY, 0);
	if (fd >= 0) {
		long n = sys3(63 /* __NR_read */, fd, (long)value, 91);
		sys3(57 /* __NR_close */, fd, 0, 0);
		if (n > 0) {
			while (n > 0 && (value[n - 1] == '\n' || value[n - 1] == '\r'))
				n--;
			value[n] = '\0';
			return (int)n;
		}
	}

	const char *b = lookup_builtin(key);
	if (b)
		return s_copy(value, b, 92);

	return s_copy(value, default_value, 92);
}

__attribute__((visibility("default")))
int property_set(const char *key, const char *value)
{
	char path[256];
	if (!key || !value)
		return -1;
	sys3(34 /* __NR_mkdirat */, AT_FDCWD, (long)"/tmp/mtk_props", 0777);
	make_prop_path(path, sizeof(path), key);
	long fd = sys4(56 /* __NR_openat */, AT_FDCWD, (long)path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd < 0)
		return 0;
	sys3(64 /* __NR_write */, fd, (long)value, s_len(value));
	sys3(57 /* __NR_close */, fd, 0, 0);
	return 0;
}

__attribute__((visibility("default")))
int __system_property_get(const char *name, char *value)
{
	return property_get(name, value, "");
}

__attribute__((visibility("default")))
int __system_property_set(const char *key, const char *value)
{
	return property_set(key, value);
}

/*
 * Stub out Android libhardware_legacy wakelock functions so MediaTek daemons
 * (ccci_mdinit / ccci_fsd / ccci_rpcd) do not block forever waiting for
 * Android's ISystemSuspend Binder service on native Linux.
 */
__attribute__((visibility("default")))
int acquire_wake_lock(int lock, const char *id)
{
	(void)lock;
	(void)id;
	return 0;
}

__attribute__((visibility("default")))
int release_wake_lock(const char *id)
{
	(void)id;
	return 0;
}

static void write_log_line(const char *tag, const char *msg)
{
	if (!msg)
		return;
	if (tag) {
		sys3(64 /* __NR_write */, 2, (long)"[", 1);
		sys3(64 /* __NR_write */, 2, (long)tag, s_len(tag));
		sys3(64 /* __NR_write */, 2, (long)"] ", 2);
	}
	int len = s_len(msg);
	sys3(64 /* __NR_write */, 2, (long)msg, len);
	if (len == 0 || msg[len - 1] != '\n')
		sys3(64 /* __NR_write */, 2, (long)"\n", 1);
}

__attribute__((visibility("default")))
int __android_log_print(int prio, const char *tag, const char *fmt, const char *arg1)
{
	(void)prio;
	if (fmt && s_eq(fmt, "%s"))
		write_log_line(tag, arg1);
	else
		write_log_line(tag, fmt);
	return 0;
}

__attribute__((visibility("default")))
int __android_log_buf_print(int bufID, int prio, const char *tag, const char *fmt, const char *arg1)
{
	(void)bufID;
	return __android_log_print(prio, tag, fmt, arg1);
}

