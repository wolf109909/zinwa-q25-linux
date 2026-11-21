/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Q25 IMS bridge (see header). Line protocol over a SOCK_STREAM Unix socket,
 * one client at a time (the newest connection replaces the old one).
 *
 * client -> MM:
 *   AT <id> <command>          run <command> on the primary AT port
 *   REG <0|1>                  IMS registration state of the client
 *   MT <smsc|-> <tpdu-hex>     SMS received over IMS (SMS-DELIVER TPDU)
 *   MORES <id> OK|ERR [text]   result of an MO request
 * MM -> client:
 *   ATRES <id> OK <response>   (CR/LF escaped as \n)
 *   ATRES <id> ERR <message>
 *   URC <line>                 +EIF / IMS indications
 *   MO <id> <tpdu-hex>         send this SMS-SUBMIT TPDU over IMS
 *   MTRES OK|ERR [text]
 */
#include <config.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mm-ims-bridge-mtk-ccci.h"
#include "mm-base-modem-at.h"
#include "mm-iface-modem-messaging.h"
#include "mm-sms-part-3gpp.h"
#include "mm-log-object.h"

#define MO_TIMEOUT_S 60

struct _MMMtkCcciImsBridge {
    MMBaseModem       *modem;      /* owner; not referenced */
    GSocketService    *service;
    GSocketConnection *conn;
    GDataInputStream  *in;
    GOutputStream     *out;
    GCancellable      *cancel;
    gboolean           registered;
    guint              next_mo;
    GHashTable        *mo;         /* id -> GTask */
};

static MMMtkCcciImsBridge *
bridge_of (gpointer modem)
{
    return modem ? g_object_get_data (G_OBJECT (modem), MM_MTK_CCCI_IMS_BRIDGE_KEY) : NULL;
}

static void
send_line (MMMtkCcciImsBridge *b, const gchar *fmt, ...) G_GNUC_PRINTF (2, 3);

static void
send_line (MMMtkCcciImsBridge *b, const gchar *fmt, ...)
{
    g_autofree gchar *line = NULL;
    g_autoptr(GError) error = NULL;
    va_list ap;

    if (!b->out)
        return;
    va_start (ap, fmt);
    line = g_strdup_vprintf (fmt, ap);
    va_end (ap);
    if (!g_output_stream_write_all (b->out, line, strlen (line), NULL, NULL, &error) ||
        !g_output_stream_write_all (b->out, "\n", 1, NULL, NULL, &error))
        mm_obj_dbg (b->modem, "ims bridge: write failed: %s", error->message);
}

static void
fail_all_mo (MMMtkCcciImsBridge *b, const gchar *why)
{
    GHashTableIter it;
    gpointer key, task;

    g_hash_table_iter_init (&it, b->mo);
    while (g_hash_table_iter_next (&it, &key, &task)) {
        g_task_return_new_error (G_TASK (task), MM_CORE_ERROR, MM_CORE_ERROR_FAILED, "IMS: %s", why);
        g_hash_table_iter_remove (&it);
    }
}

static void
drop_client (MMMtkCcciImsBridge *b)
{
    if (b->cancel) {
        g_cancellable_cancel (b->cancel);
        g_clear_object (&b->cancel);
    }
    g_clear_object (&b->in);
    b->out = NULL;
    if (b->conn) {
        g_io_stream_close (G_IO_STREAM (b->conn), NULL, NULL);
        g_clear_object (&b->conn);
    }
    if (b->registered)
        mm_obj_msg (b->modem, "ims bridge: client gone, SMS falls back to AT");
    b->registered = FALSE;
    fail_all_mo (b, "client disconnected");
}

/* ------------------------------------------------------------- AT passthrough */
typedef struct { gchar *id; } AtCtx;

static void
at_ready (MMBaseModem *modem, GAsyncResult *res, AtCtx *ctx)
{
    g_autoptr(GError) error = NULL;
    const gchar *response = mm_base_modem_at_command_finish (modem, res, &error);
    MMMtkCcciImsBridge *b = bridge_of (modem);

    if (b && b->out) {
        if (response) {
            g_autofree gchar *flat = g_strdup (response);
            g_autoptr(GString) esc = g_string_new (NULL);
            const gchar *p;

            for (p = flat; *p; p++) {
                if (*p == '\r')
                    continue;
                if (*p == '\n')
                    g_string_append (esc, "\\n");
                else
                    g_string_append_c (esc, *p);
            }
            send_line (b, "ATRES %s OK %s", ctx->id, esc->str);
        } else
            send_line (b, "ATRES %s ERR %s", ctx->id, error->message);
    }
    g_free (ctx->id);
    g_free (ctx);
}

/* -------------------------------------------------------------- MT injection */
static guint8 *
smsc_field (const gchar *smsc, gsize *len)
{
    GByteArray *a = g_byte_array_new ();
    const gchar *d = smsc;
    guint8 b = 0;
    gsize n, i;

    if (!smsc || g_str_equal (smsc, "-") || !*smsc) {
        guint8 zero = 0;
        g_byte_array_append (a, &zero, 1);
        *len = a->len;
        return g_byte_array_free (a, FALSE);
    }
    if (*d == '+')
        d++;
    n = strlen (d);
    b = (guint8) (1 + (n + 1) / 2);
    g_byte_array_append (a, &b, 1);
    b = smsc[0] == '+' ? 0x91 : 0x81;
    g_byte_array_append (a, &b, 1);
    for (i = 0; i < n; i += 2) {
        guint8 lo = (guint8) (d[i] - '0');
        guint8 hi = (i + 1 < n) ? (guint8) (d[i + 1] - '0') : 0x0F;
        b = (guint8) (hi << 4 | lo);
        g_byte_array_append (a, &b, 1);
    }
    *len = a->len;
    return g_byte_array_free (a, FALSE);
}

static void
handle_mt (MMMtkCcciImsBridge *b, const gchar *smsc, const gchar *hex)
{
    g_autoptr(GError) error = NULL;
    g_autofree guint8 *sc = NULL;
    g_autofree guint8 *tpdu = NULL;
    g_autofree guint8 *pdu = NULL;
    gsize sclen = 0, tlen = 0;
    MMSmsPart *part;

    tpdu = mm_utils_hexstr2bin (hex, -1, &tlen, &error);
    if (!tpdu) {
        send_line (b, "MTRES ERR %s", error->message);
        return;
    }
    sc = smsc_field (smsc, &sclen);
    pdu = g_malloc (sclen + tlen);
    memcpy (pdu, sc, sclen);
    memcpy (pdu + sclen, tpdu, tlen);
    part = mm_sms_part_3gpp_new_from_binary_pdu (SMS_PART_INVALID_INDEX, pdu, sclen + tlen,
                                                 b->modem, FALSE, &error);
    if (!part) {
        send_line (b, "MTRES ERR %s", error->message);
        return;
    }
    if (!mm_iface_modem_messaging_take_part (MM_IFACE_MODEM_MESSAGING (b->modem), part,
                                             MM_SMS_STATE_RECEIVED, MM_SMS_STORAGE_UNKNOWN)) {
        send_line (b, "MTRES ERR not taken");
        return;
    }
    mm_obj_msg (b->modem, "ims bridge: SMS received over IMS");
    send_line (b, "MTRES OK");
}

/* ------------------------------------------------------------------- MO */
static gboolean
mo_timeout (gpointer data)
{
    MMMtkCcciImsBridge *b = g_object_get_data (G_OBJECT (g_task_get_task_data (data)), MM_MTK_CCCI_IMS_BRIDGE_KEY);
    guint id = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (data), "mo-id"));

    if (b) {
        GTask *task = g_hash_table_lookup (b->mo, GUINT_TO_POINTER (id));
        if (task) {
            g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_TIMEOUT, "IMS: no SMS result");
            g_hash_table_remove (b->mo, GUINT_TO_POINTER (id));
        }
    }
    g_object_unref (data);
    return G_SOURCE_REMOVE;
}

static void
handle_mores (MMMtkCcciImsBridge *b, guint id, gboolean ok, const gchar *text)
{
    GTask *task = g_hash_table_lookup (b->mo, GUINT_TO_POINTER (id));

    if (!task)
        return;
    if (ok)
        g_task_return_boolean (task, TRUE);
    else
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                 "SMS over IMS failed: %s", text && *text ? text : "unknown");
    g_hash_table_remove (b->mo, GUINT_TO_POINTER (id));
}

static gboolean
bridge_can_send (MMMtkCcciImsBridge *b)
{
    return b && b->out && b->registered;
}

static void
bridge_send_tpdu (MMMtkCcciImsBridge *b, const guint8 *tpdu, gsize len,
                  GAsyncReadyCallback callback, gpointer user_data)
{
    GTask *task = g_task_new (NULL, NULL, callback, user_data);
    g_autofree gchar *hex = mm_utils_bin2hexstr (tpdu, len);
    guint id = ++b->next_mo;

    /* the timeout finds the bridge through the modem; keep it referenced */
    g_task_set_task_data (task, g_object_ref (b->modem), g_object_unref);
    g_object_set_data (G_OBJECT (task), "mo-id", GUINT_TO_POINTER (id));
    g_hash_table_insert (b->mo, GUINT_TO_POINTER (id), task);
    g_timeout_add_seconds (MO_TIMEOUT_S, mo_timeout, g_object_ref (task));
    send_line (b, "MO %u %s", id, hex);
}

/* ------------------------------------------------------------- line reader */
static void read_next (MMMtkCcciImsBridge *b);

static void
handle_line (MMMtkCcciImsBridge *b, const gchar *line)
{
    g_auto(GStrv) f = g_strsplit (line, " ", 3);
    guint n = g_strv_length (f);

    if (n >= 3 && g_str_equal (f[0], "AT")) {
        AtCtx *ctx = g_new0 (AtCtx, 1);
        const gchar *cmd = f[2];

        if (g_ascii_strncasecmp (cmd, "AT", 2) == 0)
            cmd += 2;
        ctx->id = g_strdup (f[1]);
        mm_base_modem_at_command (b->modem, cmd, 90, FALSE, (GAsyncReadyCallback) at_ready, ctx);
    } else if (n >= 2 && g_str_equal (f[0], "REG")) {
        gboolean reg = g_str_equal (f[1], "1");
        if (reg != b->registered)
            mm_obj_msg (b->modem, "ims bridge: IMS %s", reg ? "registered" : "not registered");
        b->registered = reg;
    } else if (n >= 3 && g_str_equal (f[0], "MT")) {
        handle_mt (b, f[1], f[2]);
    } else if (n >= 3 && g_str_equal (f[0], "MORES")) {
        g_auto(GStrv) r = g_strsplit (f[2], " ", 2);
        handle_mores (b, (guint) g_ascii_strtoull (f[1], NULL, 10),
                      g_str_equal (r[0], "OK"), r[1]);
    } else
        mm_obj_dbg (b->modem, "ims bridge: unknown request '%s'", f[0] ? f[0] : "");
}

static void
read_ready (GDataInputStream *in, GAsyncResult *res, MMMtkCcciImsBridge *b)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *line = g_data_input_stream_read_line_finish_utf8 (in, res, NULL, &error);

    if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;           /* bridge may already be gone */
    if (!line) {
        drop_client (b);
        return;
    }
    handle_line (b, line);
    if (b->in == in)
        read_next (b);
}

static void
read_next (MMMtkCcciImsBridge *b)
{
    g_data_input_stream_read_line_async (b->in, G_PRIORITY_DEFAULT, b->cancel,
                                         (GAsyncReadyCallback) read_ready, b);
}

static gboolean
on_incoming (GSocketService *service, GSocketConnection *conn, GObject *source, MMMtkCcciImsBridge *b)
{
    drop_client (b);
    b->conn = g_object_ref (conn);
    b->cancel = g_cancellable_new ();
    b->in = g_data_input_stream_new (g_io_stream_get_input_stream (G_IO_STREAM (conn)));
    g_data_input_stream_set_newline_type (b->in, G_DATA_STREAM_NEWLINE_TYPE_LF);
    b->out = g_io_stream_get_output_stream (G_IO_STREAM (conn));
    mm_obj_msg (b->modem, "ims bridge: client connected");
    read_next (b);
    return TRUE;
}

void
mm_mtk_ccci_ims_bridge_urc (MMMtkCcciImsBridge *b, const gchar *line)
{
    g_autofree gchar *clean = NULL;

    if (!b || !b->out)
        return;
    clean = g_strstrip (g_strdelimit (g_strdup (line), "\r\n", ' '));
    send_line (b, "URC %s", clean);
}

MMMtkCcciImsBridge *
mm_mtk_ccci_ims_bridge_new (MMBaseModem *modem)
{
    MMMtkCcciImsBridge *b = g_new0 (MMMtkCcciImsBridge, 1);
    g_autoptr(GSocketAddress) addr = NULL;
    g_autoptr(GError) error = NULL;

    b->modem = modem;
    b->mo = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_object_unref);
    b->service = g_socket_service_new ();
    unlink (MM_MTK_CCCI_IMS_BRIDGE_PATH);
    addr = g_unix_socket_address_new (MM_MTK_CCCI_IMS_BRIDGE_PATH);
    if (!g_socket_listener_add_address (G_SOCKET_LISTENER (b->service), addr, G_SOCKET_TYPE_STREAM,
                                        G_SOCKET_PROTOCOL_DEFAULT, NULL, NULL, &error)) {
        mm_obj_warn (modem, "ims bridge: cannot listen on %s: %s",
                     MM_MTK_CCCI_IMS_BRIDGE_PATH, error->message);
        g_clear_object (&b->service);
        return b;
    }
    if (chmod (MM_MTK_CCCI_IMS_BRIDGE_PATH, 0600) < 0)
        mm_obj_warn (modem, "ims bridge: chmod failed: %s", g_strerror (errno));
    g_signal_connect (b->service, "incoming", G_CALLBACK (on_incoming), b);
    g_socket_service_start (b->service);
    return b;
}

void
mm_mtk_ccci_ims_bridge_free (MMMtkCcciImsBridge *b)
{
    if (!b)
        return;
    drop_client (b);
    if (b->service) {
        g_socket_service_stop (b->service);
        g_socket_listener_close (G_SOCKET_LISTENER (b->service));
        g_clear_object (&b->service);
        unlink (MM_MTK_CCCI_IMS_BRIDGE_PATH);
    }
    g_hash_table_unref (b->mo);
    g_free (b);
}

/* ================================================================ SMS object */
struct _MMSmsMtkIms {
    MMBaseSms parent;
};

G_DEFINE_TYPE (MMSmsMtkIms, mm_sms_mtk_ims, MM_TYPE_BASE_SMS)

typedef struct {
    MMBaseModem *modem;
    GList       *current;
} SendCtx;

static void
send_ctx_free (SendCtx *ctx)
{
    g_clear_object (&ctx->modem);
    g_free (ctx);
}

static gboolean
sms_send_finish (MMBaseSms *self, GAsyncResult *res, GError **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void send_next_part (GTask *task);

static void
part_sent (GObject *source, GAsyncResult *res, GTask *task)
{
    GError *error = NULL;
    SendCtx *ctx = g_task_get_task_data (task);

    if (!g_task_propagate_boolean (G_TASK (res), &error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    ctx->current = g_list_next (ctx->current);
    send_next_part (task);
}

static void
send_next_part (GTask *task)
{
    MMSmsMtkIms *self = g_task_get_source_object (task);
    SendCtx *ctx = g_task_get_task_data (task);
    MMMtkCcciImsBridge *b = bridge_of (ctx->modem);
    g_autofree guint8 *pdu = NULL;
    guint len = 0, start = 0;
    GError *error = NULL;

    if (!ctx->current) {
        g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;
    }
    if (!bridge_can_send (b)) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
                                 "IMS registration lost while sending");
        g_object_unref (task);
        return;
    }
    pdu = mm_sms_part_3gpp_get_submit_pdu ((MMSmsPart *) ctx->current->data, &len, &start, self, &error);
    if (!pdu) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }
    /* drop the SMSC address field: the RP layer carries the SC address */
    bridge_send_tpdu (b, pdu + start, len - start, (GAsyncReadyCallback) part_sent, task);
}

static void
parent_send_ready (MMBaseSms *self, GAsyncResult *res, GTask *task)
{
    GError *error = NULL;

    if (!MM_BASE_SMS_CLASS (mm_sms_mtk_ims_parent_class)->send_finish (self, res, &error))
        g_task_return_error (task, error);
    else
        g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

static void
sms_send (MMBaseSms *self, GAsyncReadyCallback callback, gpointer user_data)
{
    GTask *task = g_task_new (self, NULL, callback, user_data);
    SendCtx *ctx = g_new0 (SendCtx, 1);

    g_object_get (self, MM_BASE_SMS_MODEM, &ctx->modem, NULL);
    g_task_set_task_data (task, ctx, (GDestroyNotify) send_ctx_free);
    if (!bridge_can_send (bridge_of (ctx->modem))) {
        /* no IMS registration: plain AT (+CMGS) path */
        MM_BASE_SMS_CLASS (mm_sms_mtk_ims_parent_class)->send (self, (GAsyncReadyCallback) parent_send_ready, task);
        return;
    }
    ctx->current = mm_base_sms_get_parts (self);
    send_next_part (task);
}

MMBaseSms *
mm_sms_mtk_ims_new (MMBaseModem *modem)
{
    return MM_BASE_SMS (g_object_new (MM_TYPE_SMS_MTK_IMS, MM_BASE_SMS_MODEM, modem, NULL));
}

static void
mm_sms_mtk_ims_init (MMSmsMtkIms *self)
{
}

static void
mm_sms_mtk_ims_class_init (MMSmsMtkImsClass *klass)
{
    MMBaseSmsClass *base = MM_BASE_SMS_CLASS (klass);

    base->send = sms_send;
    base->send_finish = sms_send_finish;
}
