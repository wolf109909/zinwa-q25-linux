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

/* Only one short, complete solicited response is accepted. Never salvage fields
 * from a mixed response (which could contain an unrelated unsolicited record). */
static gchar **
ims_response_fields (const gchar *response, const gchar *tag)
{
    g_autofree gchar *copy = NULL;
    g_auto(GStrv) fields = NULL;
    guint count, i;

    if (!response || strnlen (response, MM_MTK_CCCI_IMS_RESPONSE_MAX + 1) > MM_MTK_CCCI_IMS_RESPONSE_MAX)
        return NULL;
    copy = g_strdup (response);
    g_strstrip (copy);
    if (!g_str_has_prefix (copy, tag) || strpbrk (copy, "\r\n\v\f"))
        return NULL;
    fields = g_strsplit (copy + strlen (tag), ",", 4);
    count = g_strv_length (fields);
    if (count != 2 && count != 3)
        return NULL;
    for (i = 0; i < count; i++)
        g_strstrip (fields[i]);
    return g_steal_pointer (&fields);
}

static gboolean
ims_enum (const gchar *field, gint maximum, gint *value)
{
    if (strlen (field) != 1 || field[0] < '0' || field[0] > '0' + maximum)
        return FALSE;
    *value = field[0] - '0';
    return TRUE;
}

gboolean
mm_mtk_ccci_parse_cireg (const gchar *response, MMMtkCcciCireg *result)
{
    g_auto(GStrv) fields = ims_response_fields (response, "+CIREG:");
    MMMtkCcciCireg parsed = { .reporting = -1, .registered = -1 };
    const gchar *p;

    *result = parsed;
    if (!fields || !ims_enum (fields[0], 2, &parsed.reporting) ||
        !ims_enum (fields[1], 1, &parsed.registered))
        return FALSE;
    if (fields[2]) {
        guint32 capabilities;

        /* Stock RIL reads the first two enums independently. Still validate
         * the entire hex tail, but do not interpret unregistered/zero padding
         * as TS 27.007 ext_info or let it erase valid registration. */
        if (!*fields[2] || strlen (fields[2]) > 8)
            return FALSE;
        for (p = fields[2]; *p; p++) {
            if (!g_ascii_isxdigit (*p))
                return FALSE;
        }
        capabilities = (guint32) g_ascii_strtoull (fields[2], NULL, 16);
        if (parsed.registered && capabilities) {
            parsed.capabilities = capabilities;
            parsed.capabilities_known = TRUE;
        }
    }
    *result = parsed;
    return TRUE;
}

gboolean
mm_mtk_ccci_parse_cirep (const gchar *response, MMMtkCcciCirep *result)
{
    g_auto(GStrv) fields = ims_response_fields (response, "+CIREP:");
    MMMtkCcciCirep parsed = { .reporting = -1, .vops = -1, .vops_n3gpp = -1 };

    *result = parsed;
    if (!fields || !ims_enum (fields[0], 1, &parsed.reporting) ||
        !ims_enum (fields[1], 1, &parsed.vops) ||
        (fields[2] && !ims_enum (fields[2], 1, &parsed.vops_n3gpp)))
        return FALSE;
    *result = parsed;
    return TRUE;
}

/* Diagnostic predicates only: never retain tokens or turn a rejected response
 * into registration. Ignore unrelated/multiline tails and cap all counts. */
void
mm_mtk_ccci_inspect_ims_response (const gchar *response, const gchar *tag, MMMtkCcciImsShape *shape)
{
    g_autofree gchar *copy = NULL;
    g_auto(GStrv) fields = NULL;
    gchar *body;
    guint i;

    *shape = (MMMtkCcciImsShape) { -1, -1, -1, -1, -1, -1 };
    if (!response)
        return;
    shape->length = strnlen (response, MM_MTK_CCCI_IMS_RESPONSE_MAX + 1);
    if (shape->length > MM_MTK_CCCI_IMS_RESPONSE_MAX)
        return;
    copy = g_strdup (response);
    g_strstrip (copy);
    shape->prefix_matches = g_str_has_prefix (copy, tag);
    shape->multiline = strpbrk (copy, "\r\n\v\f") != NULL;
    if (!shape->prefix_matches || shape->multiline)
        return;
    body = g_strstrip (copy + strlen (tag));
    shape->fields = shape->decimal_fields = shape->hex_fields = 0;
    if (!*body)
        return;
    fields = g_strsplit (body, ",", 4);
    shape->fields = g_strv_length (fields);
    for (i = 0; i < MIN ((guint) shape->fields, 3); i++) {
        const gchar *p = g_strstrip (fields[i]);
        guint length = strlen (p);
        gboolean decimal = length > 0 && length <= 10;
        gboolean hex = length > 0 && length <= 8;

        for (; *p; p++) {
            decimal = decimal && g_ascii_isdigit (*p);
            hex = hex && g_ascii_isxdigit (*p);
        }
        if (decimal && g_ascii_strtoull (fields[i], NULL, 10) <= G_MAXUINT32)
            shape->decimal_fields |= 1 << i;
        if (hex)
            shape->hex_fields |= 1 << i;
    }
}
