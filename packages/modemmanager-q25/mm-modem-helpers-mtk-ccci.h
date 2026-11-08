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

#endif
