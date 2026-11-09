/* Offline framing only; not a live IMS backend. See PROVENANCE.md. */
#ifndef IMS_CHANNEL_FRAME_H
#define IMS_CHANNEL_FRAME_H

#include <stddef.h>
#include <stdint.h>

#define IMS_CHANNEL_HEADER_SIZE ((size_t)12)
#define IMS_CHANNEL_MAX_PAYLOAD UINT32_C(0x7ffffffe)

typedef struct {
    uint32_t message_id;       /* Opaque ID: no message/payload schema validation. */
    uint32_t opaque_word8;     /* Unknown semantics; service sender uses zero. */
    const uint8_t *payload;
    size_t payload_len;
} ims_channel_frame;

typedef enum {
    IMS_CHANNEL_OK = 0,
    IMS_CHANNEL_INVALID_ARGUMENT,
    IMS_CHANNEL_INVALID_LENGTH,
    IMS_CHANNEL_BUFFER_TOO_SMALL,
    IMS_CHANNEL_TRUNCATED,
    IMS_CHANNEL_TRAILING_DATA
} ims_channel_result;

/* Serialize into caller-owned memory. No allocation or I/O. Outputs (including
 * written) are unchanged on failure. All arguments/storage must be disjoint.
 * A NULL payload is allowed only for length zero. The host pointer is not sent. */
ims_channel_result ims_channel_encode(const ims_channel_frame *frame,
                                      uint8_t *out, size_t capacity,
                                      size_t *written);

/* Decode exactly one complete frame; reject truncation AND trailing bytes.
 * The successful payload view borrows wire memory (NULL for zero length).
 * No allocation, dereference of wire addresses, or payload interpretation.
 * out is unchanged on failure and must not overlap wire. */
ims_channel_result ims_channel_decode(const uint8_t *wire, size_t wire_len,
                                      ims_channel_frame *out);

#endif
