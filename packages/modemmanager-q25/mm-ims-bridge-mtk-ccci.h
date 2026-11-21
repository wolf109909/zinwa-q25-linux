/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Q25 IMS bridge: a root-only Unix socket through which the q25-imsd SIP/IMS
 * client uses this modem (AT passthrough on MM's serialized port, selected
 * URCs), reports its IMS registration, hands over received SMS (SMS over IP)
 * and carries outgoing SMS while the network has no CS domain.
 */
#ifndef MM_IMS_BRIDGE_MTK_CCCI_H
#define MM_IMS_BRIDGE_MTK_CCCI_H

#include <glib.h>
#include <gio/gio.h>

#include "mm-base-modem.h"
#include "mm-base-sms.h"

#define MM_MTK_CCCI_IMS_BRIDGE_PATH "/run/q25-ims-bridge.sock"
#define MM_MTK_CCCI_IMS_BRIDGE_KEY  "q25-ims-bridge"

typedef struct _MMMtkCcciImsBridge MMMtkCcciImsBridge;

MMMtkCcciImsBridge *mm_mtk_ccci_ims_bridge_new  (MMBaseModem *modem);
void                mm_mtk_ccci_ims_bridge_free (MMMtkCcciImsBridge *bridge);
void                mm_mtk_ccci_ims_bridge_urc  (MMMtkCcciImsBridge *bridge, const gchar *line);

/* SMS object whose Send goes over IMS when the bridge client is registered */
#define MM_TYPE_SMS_MTK_IMS (mm_sms_mtk_ims_get_type ())
G_DECLARE_FINAL_TYPE (MMSmsMtkIms, mm_sms_mtk_ims, MM, SMS_MTK_IMS, MMBaseSms)
MMBaseSms *mm_sms_mtk_ims_new (MMBaseModem *modem);

#endif
