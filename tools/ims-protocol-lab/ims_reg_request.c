#include "ims_reg_request.h"

#include <limits.h>
#include <string.h>

_Static_assert(CHAR_BIT == 8, "Payload inspection requires 8-bit bytes");

ims_reg_request_result
ims_reg_request_inspect(uint32_t event_id, const uint8_t *payload,
                        size_t payload_len, ims_reg_request_inspection *out)
{
    ims_reg_request_inspection result;

    if (!payload || !out)
        return IMS_REG_REQUEST_INVALID_ARGUMENT;
    if (event_id != IMS_REG_REQUEST_IMCB_EVENT && event_id != IMS_REG_REQUEST_STACK_EVENT)
        return IMS_REG_REQUEST_UNSUPPORTED_EVENT;
    if (payload_len < IMS_REG_REQUEST_SIZE)
        return IMS_REG_REQUEST_TRUNCATED;
    if (payload_len > IMS_REG_REQUEST_SIZE)
        return IMS_REG_REQUEST_TRAILING_DATA;

    result.event_id = event_id;
    result.account_id = (uint32_t)payload[0] | ((uint32_t)payload[1] << 8) |
                        ((uint32_t)payload[2] << 16) | ((uint32_t)payload[3] << 24);
    memcpy(result.opaque_4_7, payload + 4, sizeof(result.opaque_4_7));
    memcpy(result.cell_id, payload + 8, sizeof(result.cell_id));
    memcpy(result.opaque_72_199, payload + 72, sizeof(result.opaque_72_199));
    memcpy(result.opaque_200_251, payload + 200, sizeof(result.opaque_200_251));
    memcpy(result.opaque_252_255, payload + 252, sizeof(result.opaque_252_255));
    *out = result;
    return IMS_REG_REQUEST_OK;
}
