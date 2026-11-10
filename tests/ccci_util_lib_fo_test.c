// SPDX-License-Identifier: GPL-2.0
/* Host regression tests for verbatim ccci_util_lib_fo.c parser functions. */
#include <assert.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define __iomem
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MD_SYS1 0
#define MD_SYS3 2
typedef uint64_t phys_addr_t;

static unsigned char *mmio;
static size_t mmio_size, io_reads, io_bytes, max_io_reads;

static void memcpy_fromio(void *dst, const void *src, size_t length)
{
	uintptr_t address = (uintptr_t)src;
	uintptr_t base = (uintptr_t)mmio;

	/* Check the address BEFORE dereferencing, including zero-length reads. */
	if (address < base || address - base > mmio_size ||
	    length > mmio_size - (address - base)) {
		fprintf(stderr, "MMIO OOB: offset=%#llx length=%zu span=%zu\n",
			(unsigned long long)(address - base), length, mmio_size);
		abort();
	}
	assert(++io_reads <= max_io_reads);
	io_bytes += length;
	memcpy(dst, src, length);
}

static void test_log(const char *format, ...)
{
	char text[256];
	va_list args;

	va_start(args, format);
	vsnprintf(text, sizeof(text), format, args);
	va_end(args);
}

/* Also permits a negative-control build against the unmodified source. */
static int scnprintf(char *dst, size_t size, const char *format, ...)
{
	int ret;
	va_list args;

	va_start(args, format);
	ret = vsnprintf(dst, size, format, args);
	va_end(args);
	return ret < (int)size ? ret : (int)size - 1;
}

#define CCCI_UTIL_INF_MSG(...) test_log(__VA_ARGS__)
#define CCCI_UTIL_ERR_MSG(...) test_log(__VA_ARGS__)
#include "production-parser.inc"

static unsigned int header_size(void)
{
	return s_g_lk_info_tag_version >= 2 ? sizeof(struct _ccci_tag_v2) :
		sizeof(struct _ccci_tag);
}

static void reset_blob(unsigned int version, size_t span, unsigned int count)
{
	free(mmio);
	mmio = calloc(span ? span : 1, 1);
	assert(mmio);
	mmio_size = span;
	s_g_lk_inf_base = mmio;
	s_g_lk_info_tag_version = version;
	s_g_tag_inf_size = span;
	s_g_tag_cnt = count;
	io_reads = 0;
	io_bytes = 0;
	max_io_reads = 2 * MAX_LK_INFO_SIZE / sizeof(struct _ccci_tag) + 16;
	s_g_md_env_rdy_flag = 0;
	s_g_md_usage_case = 0;
	s_g_lk_load_img_status = 0;
	memset(md_resv_mem_size, 0, sizeof(md_resv_mem_size));
	memset(md_resv_mem_addr, 0, sizeof(md_resv_mem_addr));
	memset(md_type_at_lk, 0, sizeof(md_type_at_lk));
	memset(lk_load_img_err_no, 0, sizeof(lk_load_img_err_no));
}

static void put_bytes(size_t offset, const void *data, size_t length)
{
	assert(offset <= mmio_size && length <= mmio_size - offset);
	memcpy(mmio + offset, data, length);
}

static void put_tag(size_t offset, const char *name, unsigned int data_offset,
		    unsigned int data_size, unsigned int next)
{
	if (s_g_lk_info_tag_version >= 2) {
		struct _ccci_tag_v2 tag = {
			.data_offset = data_offset, .data_size = data_size,
			.next_tag_offset = next,
		};
		memcpy(tag.tag_name, name, strnlen(name, sizeof(tag.tag_name)));
		put_bytes(offset, &tag, sizeof(tag));
	} else {
		struct _ccci_tag tag = {
			.data_offset = data_offset, .data_size = data_size,
			.next_tag_offset = next,
		};
		memcpy(tag.tag_name, name, strnlen(name, sizeof(tag.tag_name)));
		put_bytes(offset, &tag, sizeof(tag));
	}
}

static void expect_failure(char *name, int result)
{
	unsigned char output[32], before[32];

	memset(output, 0xa5, sizeof(output));
	memcpy(before, output, sizeof(output));
	assert(find_ccci_tag_inf(name, (char *)output, sizeof(output)) == result);
	assert(memcmp(output, before, sizeof(output)) == 0);
}

static void test_pstore_offset(void)
{
	unsigned int version, i;
	const unsigned int offsets[] = {0xfffbffffU, 0xfffeffffU};

	for (version = 1; version <= 2; version++) {
		for (i = 0; i < ARRAY_SIZE(offsets); i++) {
			unsigned int size = i ? 0xfffbffffU : 4;

			reset_blob(version, 256, 1);
			put_tag(0, "not_requested", offsets[i], size, 0);
			expect_failure("opt_using_lk_val", -1);
			assert(io_reads == 1 && io_bytes == header_size());
			put_tag(0, "opt_using_lk_val", offsets[i], size, 0);
			expect_failure("opt_using_lk_val", -1);
		}
	}
}

static void test_valid_lookup(unsigned int version)
{
	char output[16], full_name[65], longer_name[66];
	unsigned int h, n;

	reset_blob(version, 256, 2);
	h = header_size();
	put_tag(0, "first", 252, 4, h);
	put_tag(h, "second", 248, 4, UINT_MAX); /* Last next is unused. */
	put_bytes(248, "WXYZabcd", 8);
	assert(find_ccci_tag_inf("second", output, sizeof(output)) == 4);
	assert(memcmp(output, "WXYZ", 4) == 0);
	assert(io_reads == 3 && io_bytes == 2 * h + 4);
	assert(find_ccci_tag_inf("first", output, 2) == 2);
	assert(memcmp(output, "ab", 2) == 0);
	assert(find_ccci_tag_inf("first", output, 0) == 0);
	expect_failure("missing", -1);
	expect_failure("firs", -1);
	expect_failure("firstmore", -1);
	expect_failure(NULL, -1);
	assert(find_ccci_tag_inf("first", NULL, 4) == -1);
	s_g_lk_inf_base = NULL;
	expect_failure("first", -2);

	/* Minimal v1 header and short payload must not read a v2-sized union. */
	reset_blob(version, h + 1, 1);
	put_tag(0, "x", h, 1, UINT_MAX);
	put_bytes(h, "Q", 1);
	assert(find_ccci_tag_inf("x", output, sizeof(output)) == 1);
	assert(output[0] == 'Q' && io_bytes == h + 1);

	/* Empty payload exactly at end, including a zero-length request. */
	reset_blob(version, h, 1);
	put_tag(0, "empty", h, 0, UINT_MAX);
	assert(find_ccci_tag_inf("empty", output, sizeof(output)) == 0);
	assert(io_reads == 1 && io_bytes == h);

	/* Exact full-width names may have no NUL, but cannot prefix-match. */
	n = version >= 2 ? 64 : 16;
	memset(full_name, 'N', n);
	full_name[n] = 0;
	memset(longer_name, 'N', n + 1);
	longer_name[n + 1] = 0;
	put_tag(0, full_name, h, 0, 0);
	assert(find_ccci_tag_inf(full_name, output, sizeof(output)) == 0);
	expect_failure(longer_name, -1);
	full_name[n - 1] = 0;
	expect_failure(full_name, -1);
	memset(mmio, 0xff, n);
	expect_failure("missing", -1);

	/* Legacy strcmp behavior ignores bytes after a terminating NUL. */
	put_tag(0, "x", h, 0, 0);
	mmio[2] = 'z';
	assert(find_ccci_tag_inf("x", output, sizeof(output)) == 0);
}

static void test_bounds_and_chains(unsigned int version)
{
	unsigned int h, n;
	char output[8];
	const unsigned int bad_offsets[] = {257, UINT_MAX, 0xfffbffffU, 0xfffeffffU};

	reset_blob(version, 256, 1);
	h = header_size();
	for (n = 0; n < ARRAY_SIZE(bad_offsets); n++) {
		put_tag(0, "x", bad_offsets[n], 4, 0);
		expect_failure("x", -1);
		put_tag(0, "x", bad_offsets[n], 0, 0);
		expect_failure("x", -1);
	}
	put_tag(0, "x", 255, 2, 0);
	expect_failure("x", -1); /* Reject full extent even if caller wants 1. */
	assert(find_ccci_tag_inf("x", output, 1) == -1);
	put_tag(0, "x", 4, UINT_MAX, 0); /* Addition would wrap. */
	expect_failure("x", -1);
	put_tag(0, "x", UINT_MAX, UINT_MAX, 0);
	expect_failure("x", -1);
	put_tag(0, "x", 256, 1, 0);
	expect_failure("x", -1);

	reset_blob(version, 256, 2);
	for (n = 0; n < ARRAY_SIZE(bad_offsets); n++) {
		put_tag(0, "other", 256, 0, bad_offsets[n]);
		expect_failure("missing", -1);
		expect_failure("other", -1);
	}
	put_tag(0, "other", 256, 0, 256 - h + 1);
	expect_failure("missing", -1); /* Truncated next header. */
	put_tag(0, "other", 256, 0, 256 - h);
	put_tag(256 - h, "last", 256, 0, UINT_MAX);
	assert(find_ccci_tag_inf("last", output, 1) == 0);

	/* Non-monotonic but in-range chains need no new wire-layout assumptions. */
	reset_blob(version, 512, 3);
	put_tag(0, "a", 512, 0, 2 * h);
	put_tag(2 * h, "b", 512, 0, h);
	put_tag(h, "c", 512, 0, 0);
	assert(find_ccci_tag_inf("c", output, 1) == 0);

	/* Cyclic/missing chains have a strict small work bound, not an infinite walk. */
	reset_blob(version, 512, 512 / h);
	put_tag(0, "other", 512, 0, 0);
	expect_failure("missing", -1);
	assert(io_reads == s_g_tag_cnt);
	io_reads = 0;
	put_tag(0, "a", 512, 0, h);
	put_tag(h, "b", 512, 0, 0);
	expect_failure("missing", -1);
	assert(io_reads == s_g_tag_cnt);

	/* Impossible counts and spans must fail without any MMIO. */
	reset_blob(version, 256, 256 / h + 1);
	expect_failure("x", -1);
	assert(io_reads == 0);
	s_g_tag_cnt = UINT_MAX;
	expect_failure("x", -1);
	assert(io_reads == 0);
	s_g_tag_cnt = 0;
	expect_failure("x", -1);
	assert(io_reads == 0);
	s_g_tag_cnt = 1;
	s_g_tag_inf_size = MAX_LK_INFO_SIZE + 1;
	expect_failure("x", -1);
	assert(io_reads == 0);
	s_g_tag_inf_size = UINT_MAX;
	expect_failure("x", -1);
	assert(io_reads == 0);
	reset_blob(version, h - 1, 1);
	expect_failure("x", -1);
	assert(io_reads == 0);
	reset_blob(version, 0, 1);
	expect_failure("x", -1);
	assert(io_reads == 0);

	/* Honor reported data span, not just a larger mapped window. */
	reset_blob(version, MAX_LK_INFO_SIZE, 1);
	s_g_tag_inf_size = h;
	put_tag(0, "x", h, 1, 0);
	expect_failure("x", -1);
	put_tag(0, "x", MAX_LK_INFO_SIZE, 0, 0);
	expect_failure("x", -1);
	/* Largest legal copy still fits the signed return convention. */
	{
		char *large = malloc(MAX_LK_INFO_SIZE);
		assert(large);
		s_g_tag_inf_size = MAX_LK_INFO_SIZE;
		put_tag(0, "x", h, MAX_LK_INFO_SIZE - h, 0);
		assert(find_ccci_tag_inf("x", large, UINT_MAX) ==
		       (int)(MAX_LK_INFO_SIZE - h));
		free(large);
	}
}

static void modem_blob(unsigned int version, int count, unsigned int table_size)
{
	struct _modem_info table[4] = {
		{.base_addr = 0x40000000, .size = 0x1000, .md_id = MD_SYS1},
		{.base_addr = 0x50000000, .size = 0x2000, .md_id = MD_SYS3},
		{.base_addr = 0x60000000, .size = 0x3000, .md_id = 1},
		{.base_addr = 0x70000000, .size = 0x4000, .md_id = 3},
	};

	reset_blob(version, 512, 2);
	put_tag(0, "hdr_count", 300, sizeof(count), header_size());
	put_tag(header_size(), "hdr_tbl_inf", 320, table_size, 0);
	put_bytes(300, &count, sizeof(count));
	put_bytes(320, table, sizeof(table));
	s_g_md_usage_case = (1 << MD_SYS1) | (1 << MD_SYS3);
}

static void expect_modem_disabled(void)
{
	md_mem_info_parsing();
	assert(s_g_md_env_rdy_flag == 0);
	assert(s_g_lk_load_img_status != 0);
	verify_md_enable_setting();
	assert(s_g_md_usage_case == 0);
}

static void test_modem_table(unsigned int version)
{
	int count;
	const int invalid_counts[] = {INT_MIN, -1, 0, 5, INT_MAX};
	unsigned int i;

	modem_blob(version, 2, 2 * sizeof(struct _modem_info));
	md_mem_info_parsing();
	assert(s_g_md_env_rdy_flag == ((1 << MD_SYS1) | (1 << MD_SYS3)));
	assert(md_resv_mem_size[MD_SYS1] == 0x1000);
	assert(md_resv_mem_addr[MD_SYS3] == 0x50000000);
	verify_md_enable_setting();
	assert(s_g_md_usage_case == ((1 << MD_SYS1) | (1 << MD_SYS3)));
	/* Caller needs only count entries; extra complete entries are ignored. */
	modem_blob(version, 1, 4 * sizeof(struct _modem_info));
	md_mem_info_parsing();
	assert(s_g_md_env_rdy_flag == (1 << MD_SYS1));
	modem_blob(version, 4, 4 * sizeof(struct _modem_info));
	md_mem_info_parsing();
	assert(s_g_md_env_rdy_flag == 15);

	for (i = 0; i < ARRAY_SIZE(invalid_counts); i++) {
		modem_blob(version, invalid_counts[i], sizeof(struct _modem_info));
		expect_modem_disabled();
	}
	modem_blob(version, 2, 2 * sizeof(struct _modem_info) - 1);
	expect_modem_disabled();
	modem_blob(version, 1, 0);
	expect_modem_disabled();
	modem_blob(version, 1, sizeof(struct _modem_info));
	put_tag(header_size(), "missing", 320, sizeof(struct _modem_info), 0);
	expect_modem_disabled();
	modem_blob(version, 1, sizeof(struct _modem_info));
	put_tag(0, "missing", 300, sizeof(count), header_size());
	expect_modem_disabled();
	modem_blob(version, 1, sizeof(struct _modem_info));
	put_tag(0, "hdr_count", 300, sizeof(count) - 1, header_size());
	expect_modem_disabled();

	for (i = 0; i < 2; i++) {
		struct _modem_info bad = {.size = 1, .md_id = i ? (char)0xff : 4};
		modem_blob(version, 1, sizeof(bad));
		put_bytes(320, &bad, sizeof(bad));
		expect_modem_disabled();
	}
	modem_blob(version, 1, sizeof(struct _modem_info));
	put_tag(0, "hdr_count", 0xfffbffffU, 4, header_size());
	expect_modem_disabled();
}

static uint32_t random_state = 0x25ccc1U;

static uint32_t random_u32(void)
{
	random_state ^= random_state << 13;
	random_state ^= random_state >> 17;
	random_state ^= random_state << 5;
	return random_state;
}

static unsigned int random_offset(unsigned int span)
{
	switch (random_u32() % 5) {
	case 0: return 0xfffbffffU;
	case 1: return UINT_MAX;
	default: return random_u32() % (span + 1);
	}
}

static void test_bounded_fuzz(unsigned int rounds)
{
	unsigned int round;

	for (round = 0; round < rounds; round++) {
		unsigned int version = 1 + random_u32() % 3;
		unsigned int span = random_u32() % 1025;
		unsigned int h = version >= 2 ? 76 : 28;
		unsigned int i, count = random_u32() % (span / h + 2);
		unsigned int requested = random_u32() % 129;
		unsigned char output[130];
		int result;

		reset_blob(version, span, count);
		for (i = 0; i < span; i++)
			mmio[i] = random_u32();
		/* Mix raw bytes with syntactically valid headers and valid chains. */
		if (round % 3) {
			for (i = 0; i + h <= span; i += h) {
				unsigned int off = random_offset(span);
				unsigned int len = random_offset(span);
				unsigned int next = random_offset(span);
				if (round % 3 == 1) {
					off = span;
					len = 0;
					next = i + h;
				}
				put_tag(i, random_u32() % 2 ? "target" : "other",
					off, len, next);
			}
		}
		if (round % 11 == 0)
			s_g_tag_cnt = random_u32();
		if (round % 13 == 0)
			s_g_tag_inf_size = round % 26 ? UINT_MAX :
				random_u32() % (span + 1);
		memset(output, 0xa5, sizeof(output));
		result = find_ccci_tag_inf("target", (char *)output + 1, requested);
		assert(result >= -2 && result <= (int)requested);
		assert(output[0] == 0xa5 && output[129] == 0xa5);
		if (result < 0) {
			for (i = 0; i < sizeof(output); i++)
				assert(output[i] == 0xa5);
		}
		assert(io_reads <= (span / h + 1));
	}
}

int main(int argc, char **argv)
{
	unsigned int rounds = argc > 1 ? strtoul(argv[1], NULL, 0) : 50000;
	unsigned int version;

	assert(sizeof(struct _ccci_tag) == 28);
	assert(sizeof(struct _ccci_tag_v2) == 76);
	test_pstore_offset(); /* Negative control reaches exactly the saved offset. */
	for (version = 1; version <= 3; version++) {
		test_valid_lookup(version);
		test_bounds_and_chains(version);
		test_modem_table(version);
	}
	test_bounded_fuzz(rounds);
	free(mmio);
	printf("PASS: ccci_util_lib_fo regressions; %u bounded fuzz cases; char=%s; debug=%s\n",
		rounds, CHAR_MIN < 0 ? "signed" : "unsigned",
#ifdef LK_LOAD_MD_INFO_DEBUG_EN
		"on"
#else
		"off"
#endif
	);
	return 0;
}
