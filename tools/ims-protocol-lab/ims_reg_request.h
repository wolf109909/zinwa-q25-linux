/* Memory-only inspection, not a registration constructor or live backend. */
#ifndef IMS_REG_REQUEST_H
#define IMS_REG_REQUEST_H

#include <stddef.h>
#include <stdint.h>

#define IMS_REG_REQUEST_IMCB_EVENT UINT32_C(11001)
#define IMS_REG_REQUEST_STACK_EVENT UINT32_C(91001)
#define IMS_REG_REQUEST_SIZE ((size_t)256)
#define IMS_REG_REQUEST_CELL_ID_SIZE ((size_t)64)

typedef struct {
    uint32_t event_id;        /* Supplied metadata, preserved, never remapped. */
    uint32_t account_id;      /* Decoded wire LE32; private runtime account index. */
    uint8_t opaque_4_7[4];
    uint8_t cell_id[64];      /* Raw fixed bytes, not a validated/NUL-terminated string. */
    uint8_t opaque_72_199[128];
    uint8_t opaque_200_251[52]; /* Entire block opaque, including its first 8 bytes. */
    uint8_t opaque_252_255[4];
} ims_reg_request_inspection;

typedef enum {
    IMS_REG_REQUEST_OK = 0,
    IMS_REG_REQUEST_INVALID_ARGUMENT,
    IMS_REG_REQUEST_UNSUPPORTED_EVENT,
    IMS_REG_REQUEST_TRUNCATED,
    IMS_REG_REQUEST_TRAILING_DATA
} ims_reg_request_result;

/* Inspect the payload of event11001 (IMCB -> UA) or event91001 (UA -> stack).
 * Payload length must be exactly256; event metadata is not part of the payload.
 * Copies a snapshot into out, which must not overlap payload. Failure leaves
 * out unchanged. No allocation, logging, wire-address dereference or account lookup.
 * The snapshot is NOT a wire struct and no payload encoder is provided.
 * A successful inspection does not establish a valid/private runtime account. */
ims_reg_request_result ims_reg_request_inspect(uint32_t event_id,
                                             const uint8_t *payload,
                                             size_t payload_len,
                                             ims_reg_request_inspection *out);

#endif
