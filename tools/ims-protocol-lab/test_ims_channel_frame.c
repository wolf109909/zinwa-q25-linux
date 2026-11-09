#include "ims_channel_frame.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#ifdef NDEBUG
#error "Tests require assertions"
#endif

static size_t
read_fixture(const char *path, uint8_t *bytes, size_t capacity)
{
    FILE *file = fopen(path, "r");
    size_t length = 0;
    unsigned int value;
    int result;

    assert(file);
    while ((result = fscanf(file, "%x", &value)) == 1) {
        assert(value <= 255 && length < capacity);
        bytes[length++] = (uint8_t)value;
    }
    assert(result == EOF && !ferror(file));
    assert(fclose(file) == 0);
    return length;
}

static void
fixture_round_trip(const char *path, uint32_t id, uint32_t word8,
                   const uint8_t *payload, size_t length)
{
    uint8_t expected[64], encoded[64], unaligned[65];
    size_t wire_len = read_fixture(path, expected, sizeof(expected));
    size_t written = SIZE_MAX;
    ims_channel_frame input = {id, word8, payload, length}, decoded;

    assert(wire_len == IMS_CHANNEL_HEADER_SIZE + length);
    memset(encoded, 0xa5, sizeof(encoded));
    assert(ims_channel_encode(&input, encoded, sizeof(encoded), &written) == IMS_CHANNEL_OK);
    assert(written == wire_len && memcmp(encoded, expected, wire_len) == 0);
    assert(encoded[wire_len] == 0xa5); /* No host pointer, terminator or padding. */
    assert(ims_channel_decode(expected, wire_len, &decoded) == IMS_CHANNEL_OK);
    assert(decoded.message_id == id && decoded.opaque_word8 == word8);
    assert(decoded.payload_len == length);
    if (length) {
        assert(decoded.payload == expected + IMS_CHANNEL_HEADER_SIZE);
        assert(memcmp(decoded.payload, payload, length) == 0);
    } else {
        assert(decoded.payload == NULL);
    }
    /* Explicit byte loads/stores, not unaligned uint32_t or native struct casts. */
    assert(ims_channel_encode(&input, unaligned + 1, sizeof(unaligned) - 1, &written) == IMS_CHANNEL_OK);
    assert(memcmp(unaligned + 1, expected, wire_len) == 0);
    assert(ims_channel_decode(unaligned + 1, wire_len, &decoded) == IMS_CHANNEL_OK);
    assert(decoded.message_id == id && decoded.opaque_word8 == word8);
}

static void
expect_decode_error(const uint8_t *wire, size_t length, ims_channel_result error)
{
    const uint8_t sentinel[] = {0xee};
    ims_channel_frame output = {UINT32_C(0x12345678), 9, sentinel, 1};
    unsigned char before[sizeof(output)];
    memcpy(before, &output, sizeof(output));
    assert(ims_channel_decode(wire, length, &output) == error);
    assert(memcmp(before, &output, sizeof(output)) == 0);
}

static void
test_truncation_and_trailing(void)
{
    uint8_t wire[64], combined[128];
    size_t length = read_fixture("fixtures/service-binary.hex", wire, sizeof(wire));
    size_t i;

    for (i = 0; i < length; i++)
        expect_decode_error(wire, i, IMS_CHANNEL_TRUNCATED);
    wire[length] = 0;
    expect_decode_error(wire, length + 1, IMS_CHANNEL_TRAILING_DATA);
    memcpy(combined, wire, length);
    memcpy(combined + length, wire, length);
    expect_decode_error(combined, length * 2, IMS_CHANNEL_TRAILING_DATA);

    /* An unexplained fourth header word is NOT an on-wire pointer. */
    length = read_fixture("fixtures/empty.hex", wire, sizeof(wire));
    memset(wire + length, 0xcc, 4);
    expect_decode_error(wire, length + 4, IMS_CHANNEL_TRAILING_DATA);
}

static void
test_invalid_lengths(void)
{
    static const uint8_t bad_lengths[][4] = {
        {0xff, 0xff, 0xff, 0x7f}, /* 0x7fffffff exceeds source limit. */
        {0x00, 0x00, 0x00, 0x80}, /* Negative in the vendor signed check. */
        {0xff, 0xff, 0xff, 0xff}
    };
    uint8_t header[12] = {0};
    size_t i;

    for (i = 0; i < sizeof(bad_lengths) / sizeof(bad_lengths[0]); i++) {
        memcpy(header + 4, bad_lengths[i], 4);
        expect_decode_error(header, sizeof(header), IMS_CHANNEL_INVALID_LENGTH);
    }
    memcpy(header + 4, (const uint8_t[]){0xfe, 0xff, 0xff, 0x7f}, 4);
    /* Legal declared ceiling, missing body: no 2 GiB allocation/read occurs. */
    expect_decode_error(header, sizeof(header), IMS_CHANNEL_TRUNCATED);
    {
        uint8_t swapped_length[15] = {0};
        swapped_length[7] = 3;
        /* Three body bytes exist: a wrongly big-endian decoder would accept. */
        expect_decode_error(swapped_length, sizeof(swapped_length), IMS_CHANNEL_TRUNCATED);
    }
}

static void
test_encode_bounds(void)
{
    uint8_t payload[] = {0, 0xff, 0x7f}, output[32], before[32];
    ims_channel_frame frame = {UINT32_MAX, UINT32_MAX, payload, sizeof(payload)};
    size_t capacity, written;

    memset(output, 0x5a, sizeof(output));
    memcpy(before, output, sizeof(output));
    for (capacity = 0; capacity < IMS_CHANNEL_HEADER_SIZE + sizeof(payload); capacity++) {
        written = 99;
        assert(ims_channel_encode(&frame, output, capacity, &written) == IMS_CHANNEL_BUFFER_TOO_SMALL);
        assert(written == 99 && memcmp(output, before, sizeof(output)) == 0);
    }
    frame.payload_len = IMS_CHANNEL_MAX_PAYLOAD;
    written = 99;
    assert(ims_channel_encode(&frame, output, sizeof(output), &written) == IMS_CHANNEL_BUFFER_TOO_SMALL);
    frame.payload_len = (size_t)IMS_CHANNEL_MAX_PAYLOAD + 1;
    assert(ims_channel_encode(&frame, output, sizeof(output), &written) == IMS_CHANNEL_INVALID_LENGTH);
    frame.payload_len = SIZE_MAX;
    assert(ims_channel_encode(&frame, output, sizeof(output), &written) == IMS_CHANNEL_INVALID_LENGTH);
    assert(written == 99 && memcmp(output, before, sizeof(output)) == 0);
}

static void
test_arguments(void)
{
    uint8_t output[16] = {0};
    ims_channel_frame frame = {1, 0, NULL, 1}, decoded;
    size_t written = 77;

    assert(ims_channel_encode(&frame, output, sizeof(output), &written) == IMS_CHANNEL_INVALID_ARGUMENT);
    assert(ims_channel_encode(NULL, output, sizeof(output), &written) == IMS_CHANNEL_INVALID_ARGUMENT);
    assert(ims_channel_encode(&frame, NULL, sizeof(output), &written) == IMS_CHANNEL_INVALID_ARGUMENT);
    assert(ims_channel_encode(&frame, output, sizeof(output), NULL) == IMS_CHANNEL_INVALID_ARGUMENT);
    assert(written == 77);
    assert(ims_channel_decode(NULL, 0, &decoded) == IMS_CHANNEL_INVALID_ARGUMENT);
    assert(ims_channel_decode(output, sizeof(output), NULL) == IMS_CHANNEL_INVALID_ARGUMENT);
}

static void
test_length_sweep(void)
{
    uint8_t payload[512], wire[524], encoded_again[524];
    ims_channel_frame input = {UINT32_MAX, UINT32_C(0xa5f00f5a), payload, 0}, decoded;
    size_t length, i, written, again;

    for (length = 0; length <= sizeof(payload); length++) {
        for (i = 0; i < length; i++)
            payload[i] = (uint8_t)(i ^ length);
        input.payload_len = length;
        assert(ims_channel_encode(&input, wire, sizeof(wire), &written) == IMS_CHANNEL_OK);
        assert(written == IMS_CHANNEL_HEADER_SIZE + length);
        assert(ims_channel_decode(wire, written, &decoded) == IMS_CHANNEL_OK);
        assert(decoded.payload_len == length && decoded.message_id == input.message_id);
        assert(decoded.opaque_word8 == input.opaque_word8);
        if (length)
            assert(memcmp(payload, decoded.payload, length) == 0);
        assert(ims_channel_encode(&decoded, encoded_again, sizeof(encoded_again), &again) == IMS_CHANNEL_OK);
        assert(again == written && memcmp(wire, encoded_again, written) == 0);
    }
}

int
main(void)
{
    fixture_round_trip("fixtures/empty.hex", UINT32_C(0x01020304), 0, NULL, 0);
    fixture_round_trip("fixtures/service-binary.hex", UINT32_C(0x89abcdef), 0,
                      (const uint8_t[]){0, 0xff, 0x10, 0x7f}, 4);
    fixture_round_trip("fixtures/opaque-word8.hex", UINT32_C(0x11223344), UINT32_C(0xa1b2c3d4),
                      (const uint8_t[]){0xa5, 0, 0x5a}, 3);
    test_truncation_and_trailing();
    test_invalid_lengths();
    test_encode_bounds();
    test_arguments();
    test_length_sweep();
    puts("PASS ims_channel_frame: 3 synthetic fixtures, truncation/trailing/length/argument bounds, 513 round trips");
    return 0;
}
