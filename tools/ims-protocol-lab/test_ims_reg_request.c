#include "ims_channel_frame.h"
#include "ims_reg_request.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#ifdef NDEBUG
#error "Tests require assertions"
#endif

static void
read_fixture(uint8_t *payload)
{
    FILE *file = fopen("fixtures/reg-request-inspection.hex", "r");
    unsigned int value;
    size_t i;

    assert(file);
    for (i = 0; i < IMS_REG_REQUEST_SIZE; i++) {
        assert(fscanf(file, "%x", &value) == 1 && value <= 255);
        payload[i] = (uint8_t)value;
    }
    assert(fscanf(file, "%x", &value) == EOF && !ferror(file));
    assert(fclose(file) == 0);
}

static void
check_regions(const uint8_t *payload, const ims_reg_request_inspection *out)
{
    assert(memcmp(out->opaque_4_7, payload + 4, 4) == 0);
    assert(memcmp(out->cell_id, payload + 8, 64) == 0);
    assert(memcmp(out->opaque_72_199, payload + 72, 128) == 0);
    assert(memcmp(out->opaque_200_251, payload + 200, 52) == 0);
    assert(memcmp(out->opaque_252_255, payload + 252, 4) == 0);
}

static void
expect_error(uint32_t event_id, const uint8_t *payload, size_t length,
             ims_reg_request_result error)
{
    ims_reg_request_inspection out;
    unsigned char before[sizeof(out)];
    memset(&out, 0xa5, sizeof(out));
    memcpy(before, &out, sizeof(out));
    assert(ims_reg_request_inspect(event_id, payload, length, &out) == error);
    assert(memcmp(before, &out, sizeof(out)) == 0);
}

static void
test_snapshot_and_events(void)
{
    uint8_t payload[256], before[256], unaligned[257];
    ims_reg_request_inspection out;
    const uint32_t events[] = {IMS_REG_REQUEST_IMCB_EVENT, IMS_REG_REQUEST_STACK_EVENT};
    size_t i;

    read_fixture(payload);
    memcpy(before, payload, sizeof(payload));
    for (i = 0; i < sizeof(events) / sizeof(events[0]); i++) {
        assert(ims_reg_request_inspect(events[i], payload, sizeof(payload), &out) == IMS_REG_REQUEST_OK);
        assert(out.event_id == events[i]); /* Inspection does not remap11001 to91001. */
        assert(out.account_id == UINT32_C(0x11223344)); /* Not restricted to producer uint8. */
        check_regions(payload, &out);
        assert(memcmp(payload, before, sizeof(payload)) == 0);
    }
    memcpy(unaligned + 1, payload, sizeof(payload));
    assert(ims_reg_request_inspect(events[0], unaligned + 1, sizeof(payload), &out) == IMS_REG_REQUEST_OK);
    assert(out.account_id == UINT32_C(0x11223344));
    check_regions(payload, &out);
    memset(unaligned, 0, sizeof(unaligned));
    check_regions(payload, &out); /* Snapshot owns copies, not borrowed pointers. */

    memset(payload, 0xff, 4);
    memset(payload + 8, 0xff, 64); /* No NUL/format rule is inferred for raw cell-ID. */
    memset(payload + 200, 0xff, 52); /* No float/pointer normalization. */
    assert(ims_reg_request_inspect(events[0], payload, sizeof(payload), &out) == IMS_REG_REQUEST_OK);
    assert(out.account_id == UINT32_MAX);
    check_regions(payload, &out);
    memset(payload, 0, sizeof(payload));
    assert(ims_reg_request_inspect(events[0], payload, sizeof(payload), &out) == IMS_REG_REQUEST_OK);
    assert(out.account_id == 0);
    check_regions(payload, &out);
}

static void
test_rejections(void)
{
    uint8_t payload[257];
    size_t length;

    read_fixture(payload);
    payload[256] = 0x5a;
    for (length = 0; length < 256; length++)
        expect_error(IMS_REG_REQUEST_IMCB_EVENT, payload, length, IMS_REG_REQUEST_TRUNCATED);
    expect_error(IMS_REG_REQUEST_IMCB_EVENT, payload, 257, IMS_REG_REQUEST_TRAILING_DATA);
    expect_error(IMS_REG_REQUEST_IMCB_EVENT, payload, SIZE_MAX, IMS_REG_REQUEST_TRAILING_DATA);
    expect_error(11002, payload, 256, IMS_REG_REQUEST_UNSUPPORTED_EVENT);
    expect_error(91002, payload, 256, IMS_REG_REQUEST_UNSUPPORTED_EVENT);
    expect_error(UINT32_MAX, payload, 256, IMS_REG_REQUEST_UNSUPPORTED_EVENT);
    expect_error(IMS_REG_REQUEST_IMCB_EVENT, NULL, 256, IMS_REG_REQUEST_INVALID_ARGUMENT);
    assert(ims_reg_request_inspect(IMS_REG_REQUEST_IMCB_EVENT, payload, 256, NULL) == IMS_REG_REQUEST_INVALID_ARGUMENT);
}

static void
test_stack_frame_lengths(void)
{
    /* Artificial UA->stack91001 Channel frame, NOT a registration sequence.
     * Do not infer that IMCB's incoming11001 transport has this envelope. */
    uint8_t wire[269] = {0x79, 0x63, 0x01, 0, 0, 0x01, 0, 0, 0, 0, 0, 0};
    ims_channel_frame frame;
    ims_reg_request_inspection out;

    read_fixture(wire + IMS_CHANNEL_HEADER_SIZE);
    assert(ims_channel_decode(wire, 268, &frame) == IMS_CHANNEL_OK);
    assert(frame.message_id == IMS_REG_REQUEST_STACK_EVENT);
    assert(ims_reg_request_inspect(frame.message_id, frame.payload, frame.payload_len, &out) == IMS_REG_REQUEST_OK);
    assert(out.event_id == IMS_REG_REQUEST_STACK_EVENT);

    wire[4] = 1; /* Declared257, actual256. */
    assert(ims_channel_decode(wire, 268, &frame) == IMS_CHANNEL_TRUNCATED);
    wire[4] = 0xff;
    wire[5] = 0; /* Declared255, actual256. */
    assert(ims_channel_decode(wire, 268, &frame) == IMS_CHANNEL_TRAILING_DATA);
    assert(ims_channel_decode(wire, 267, &frame) == IMS_CHANNEL_OK);
    expect_error(frame.message_id, frame.payload, frame.payload_len, IMS_REG_REQUEST_TRUNCATED);
    wire[4] = 1;
    wire[5] = 1; /* Framing-consistent257, wrong registration payload size. */
    assert(ims_channel_decode(wire, 269, &frame) == IMS_CHANNEL_OK);
    expect_error(frame.message_id, frame.payload, frame.payload_len, IMS_REG_REQUEST_TRAILING_DATA);
}

int
main(void)
{
    test_snapshot_and_events();
    test_rejections();
    test_stack_frame_lengths();
    puts("PASS ims_reg_request: synthetic snapshots, opaque preservation, 256 truncations, event/length/argument checks");
    return 0;
}
