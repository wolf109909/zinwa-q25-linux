#include "ims_channel_frame.h"

#include <limits.h>
#include <string.h>

_Static_assert(CHAR_BIT == 8, "Wire format requires 8-bit bytes");
_Static_assert(SIZE_MAX >= UINT32_MAX, "Codec requires 32-bit or wider size_t");

static uint32_t
read_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void
write_le32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

ims_channel_result
ims_channel_encode(const ims_channel_frame *frame, uint8_t *out,
                   size_t capacity, size_t *written)
{
    if (!frame || !out || !written)
        return IMS_CHANNEL_INVALID_ARGUMENT;
    if (frame->payload_len > IMS_CHANNEL_MAX_PAYLOAD)
        return IMS_CHANNEL_INVALID_LENGTH;
    if (frame->payload_len && !frame->payload)
        return IMS_CHANNEL_INVALID_ARGUMENT;
    if (capacity < IMS_CHANNEL_HEADER_SIZE ||
        frame->payload_len > capacity - IMS_CHANNEL_HEADER_SIZE)
        return IMS_CHANNEL_BUFFER_TOO_SMALL;

    write_le32(out, frame->message_id);
    write_le32(out + 4, (uint32_t)frame->payload_len);
    write_le32(out + 8, frame->opaque_word8);
    if (frame->payload_len)
        memcpy(out + IMS_CHANNEL_HEADER_SIZE, frame->payload, frame->payload_len);
    *written = IMS_CHANNEL_HEADER_SIZE + frame->payload_len;
    return IMS_CHANNEL_OK;
}

ims_channel_result
ims_channel_decode(const uint8_t *wire, size_t wire_len, ims_channel_frame *out)
{
    uint32_t payload_len;
    ims_channel_frame frame;

    if (!wire || !out)
        return IMS_CHANNEL_INVALID_ARGUMENT;
    if (wire_len < IMS_CHANNEL_HEADER_SIZE)
        return IMS_CHANNEL_TRUNCATED;
    payload_len = read_le32(wire + 4);
    if (payload_len > IMS_CHANNEL_MAX_PAYLOAD)
        return IMS_CHANNEL_INVALID_LENGTH;
    if (payload_len > wire_len - IMS_CHANNEL_HEADER_SIZE)
        return IMS_CHANNEL_TRUNCATED;
    if (payload_len < wire_len - IMS_CHANNEL_HEADER_SIZE)
        return IMS_CHANNEL_TRAILING_DATA;

    frame.message_id = read_le32(wire);
    frame.opaque_word8 = read_le32(wire + 8);
    frame.payload_len = payload_len;
    frame.payload = payload_len ? wire + IMS_CHANNEL_HEADER_SIZE : NULL;
    *out = frame;
    return IMS_CHANNEL_OK;
}
