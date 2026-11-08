/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "mm-modem-helpers-mtk-ccci.h"

#include <string.h>

static gchar *
unquote (gchar *field)
{
    gsize len;

    g_strstrip (field);
    len = strlen (field);
    if (len >= 2 && field[0] == '"' && field[len - 1] == '"') {
        field[len - 1] = '\0';
        return field + 1;
    }
    return field;
}

static gboolean
valid_ipv4 (const gchar *text)
{
    g_autoptr(GInetAddress) address = g_inet_address_new_from_string (text);

    return address && g_inet_address_get_family (address) == G_SOCKET_FAMILY_IPV4 &&
           !g_inet_address_get_is_any (address) && !g_inet_address_get_is_multicast (address);
}

gboolean
mm_mtk_ccci_parse_eif (const gchar *line, MMMtkCcciEif *eif)
{
    g_auto(GStrv) fields = NULL;
    const gchar *tag;
    gchar *end, *event;
    guint64 id;

    tag = strstr (line, "+EIF:");
    if (!tag)
        return FALSE;
    fields = g_strsplit (tag + strlen ("+EIF:"), ",", -1);
    if (g_strv_length (fields) < 2)
        return FALSE;
    id = g_ascii_strtoull (g_strstrip (fields[0]), &end, 10);
    if (*end || !*fields[0] || id > G_MAXUINT || id % 100 > 20)
        return FALSE;
    event = unquote (fields[1]);
    if (!g_str_equal (event, "ifup") && !g_str_equal (event, "ifdown"))
        return FALSE;
    memset (eif, 0, sizeof (*eif));
    eif->id = id;
    eif->up = g_str_equal (event, "ifup");
    if (eif->up) {
        const gchar *address;

        if (g_strv_length (fields) < 4 || !g_str_equal (unquote (fields[2]), "3"))
            return FALSE;
        address = unquote (fields[3]);
        if (!valid_ipv4 (address))
            return FALSE;
        g_strlcpy (eif->address, address, sizeof (eif->address));
    }
    return TRUE;
}

gchar **
mm_mtk_ccci_parse_dns (const gchar *response, const gchar *apn, const gchar *address, GError **error)
{
    g_auto(GStrv) lines = g_strsplit_set (response, "\r\n", -1);
    guint i;

    for (i = 0; lines[i]; i++) {
        g_auto(GStrv) fields = NULL;
        g_auto(GStrv) octets = NULL;
        g_autofree gchar *local = NULL;
        const gchar *tag = strstr (lines[i], "+CGCONTRDP:");
        const gchar *reported_apn, *reported_local;
        GPtrArray *dns;
        guint j;

        if (!tag)
            continue;
        fields = g_strsplit (tag + strlen ("+CGCONTRDP:"), ",", -1);
        if (g_strv_length (fields) < 7)
            continue;
        reported_apn = unquote (fields[2]);
        if (g_ascii_strcasecmp (reported_apn, apn) &&
            (g_ascii_strncasecmp (reported_apn, apn, strlen (apn)) ||
             !g_regex_match_simple ("^\\.mnc[0-9]{3}\\.mcc[0-9]{3}\\.gprs$",
                                    reported_apn + strlen (apn), G_REGEX_CASELESS, 0)))
            continue;
        /* MOLY may omit local/gateway and supply the address only in +EIF.
         * Match the APN (including its optional operator suffix) in that case;
         * if an address is supplied it must also match the current EIF. */
        reported_local = unquote (fields[3]);
        if (*reported_local) {
            octets = g_strsplit (reported_local, ".", -1);
            if (g_strv_length (octets) != 4 && g_strv_length (octets) != 8)
                continue;
            local = g_strdup_printf ("%s.%s.%s.%s", octets[0], octets[1], octets[2], octets[3]);
            if (!g_str_equal (local, address))
                continue;
        }
        dns = g_ptr_array_new_with_free_func (g_free);
        for (j = 5; j <= 6; j++) {
            const gchar *value = unquote (fields[j]);

            if (valid_ipv4 (value) &&
                (!dns->len || !g_str_equal (g_ptr_array_index (dns, 0), value)))
                g_ptr_array_add (dns, g_strdup (value));
        }
        if (dns->len) {
            g_ptr_array_add (dns, NULL);
            return (gchar **) g_ptr_array_free (dns, FALSE);
        }
        g_ptr_array_unref (dns);
    }
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                 "No IPv4 DNS for APN %s / EIF address %s in +CGCONTRDP", apn, address);
    return NULL;
}

gboolean
mm_mtk_ccci_valid_apn (const gchar *apn)
{
    const gchar *p;

    if (!apn || !*apn || strlen (apn) > 100)
        return FALSE;
    for (p = apn; *p; p++) {
        if (!g_ascii_isalnum (*p) && *p != '-' && *p != '.')
            return FALSE;
    }
    return TRUE;
}
