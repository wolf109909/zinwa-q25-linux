/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MM_MODEM_HELPERS_MTK_CCCI_H
#define MM_MODEM_HELPERS_MTK_CCCI_H

#include <gio/gio.h>

typedef struct {
    guint id;
    gboolean up;
    gchar address[16];
} MMMtkCcciEif;

gboolean mm_mtk_ccci_parse_eif (const gchar *line, MMMtkCcciEif *eif);
gchar **mm_mtk_ccci_parse_dns (const gchar *response, const gchar *apn, const gchar *address, GError **error);
gboolean mm_mtk_ccci_valid_apn (const gchar *apn);

typedef struct {
    gint reporting;              /* -1 means unknown. */
    gint registered;
    gboolean capabilities_known;
    guint32 capabilities;
} MMMtkCcciCireg;

typedef struct {
    gint reporting;              /* -1 means unknown, including absent optional fields. */
    gint vops;
    gint vops_n3gpp;
} MMMtkCcciCirep;

gboolean mm_mtk_ccci_parse_cireg (const gchar *response, MMMtkCcciCireg *result);
gboolean mm_mtk_ccci_parse_cirep (const gchar *response, MMMtkCcciCirep *result);

#define MM_MTK_CCCI_IMS_RESPONSE_MAX 96

typedef struct {
    gint length;           /* -1 absent; 97 means at least 97 bytes. */
    gint prefix_matches;   /* Remaining fields are -1 when not inspected. */
    gint multiline;
    gint fields;           /* 4 means four or more comma-separated fields. */
    gint decimal_fields;   /* Bits 0..2: complete unsigned uint32 decimal tokens. */
    gint hex_fields;       /* Bits 0..2: complete unquoted 1..8 digit hex tokens. */
} MMMtkCcciImsShape;

void mm_mtk_ccci_inspect_ims_response (const gchar *response, const gchar *tag, MMMtkCcciImsShape *shape);

#endif
