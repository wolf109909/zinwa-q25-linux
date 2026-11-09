/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Execute the production observer with real GObject notifications/GTask lifetimes
 * and a mocked MM AT boundary. No serial port, D-Bus service or modem is used. */
#include <gio/gio.h>
#include <stdarg.h>
#define __MODEM_MANAGER_H_INSIDE__
#include "ModemManager-enums.h"
#include "ModemManager-errors.h"
#include "mm-log.h"
#include "mm-ims-observer-mtk-ccci.h"

#define MM_IFACE_MODEM_STATE "iface-modem-state"
#define MM_IFACE_MODEM_3GPP_REGISTRATION_STATE "iface-modem-3gpp-registration-state"
typedef GObject MMIfacePortAt;
struct _MMBaseModem {
    GObject parent;
    MMMtkCcciImsObserver observer;
    gint state, registration;
    GCancellable *modem_cancel;
    GObject *port;
    GTask *pending;
    guint commands;
    gboolean no_port;
};
typedef GObjectClass MMBaseModemClass;
static MMIfacePortAt *mm_base_modem_peek_best_at_port (MMBaseModem *, GError **);
static GCancellable *mm_base_modem_peek_cancellable (MMBaseModem *);
static void mm_base_modem_at_command_full (MMBaseModem *, MMIfacePortAt *, const gchar *, guint,
                                           gboolean, gboolean, GCancellable *, GAsyncReadyCallback, gpointer);
static const gchar *mm_base_modem_at_command_full_finish (MMBaseModem *, GAsyncResult *, GError **);
/* The MM boundary supplies these domains in production; tests need only their
 * identity, not libmm-glib or a daemon. Error enum values above are upstream. */
#define MM_CORE_ERROR             (g_quark_from_static_string ("test-mm-core"))
#define MM_SERIAL_ERROR           (g_quark_from_static_string ("test-mm-serial"))
#define MM_MOBILE_EQUIPMENT_ERROR (g_quark_from_static_string ("test-mm-mobile-equipment"))
#define MM_MESSAGE_ERROR          (g_quark_from_static_string ("test-mm-message"))
#define MM_CONNECTION_ERROR       (g_quark_from_static_string ("test-mm-connection"))
#define MM_MTK_CCCI_IMS_OBSERVER_TEST
#include "mm-ims-observer-mtk-ccci.c"

G_DEFINE_TYPE (MMBaseModem, mm_base_modem, G_TYPE_OBJECT)
static guint finished, messages, finalized;
static gchar *last_message;

typedef struct {
    GObject *port;
    gchar *response;
} MockCommand;

static void
mock_command_free (MockCommand *command)
{
    g_object_unref (command->port);
    g_free (command->response);
    g_free (command);
}

static MMIfacePortAt *
mm_base_modem_peek_best_at_port (MMBaseModem *self, GError **error G_GNUC_UNUSED)
{
    return self->no_port ? NULL : self->port;
}

static GCancellable *
mm_base_modem_peek_cancellable (MMBaseModem *self)
{
    return self->modem_cancel;
}

static void
mm_base_modem_at_command_full (MMBaseModem *self, MMIfacePortAt *port, const gchar *command,
                               guint timeout, gboolean cached, gboolean raw, GCancellable *cancellable,
                               GAsyncReadyCallback callback, gpointer user_data)
{
    MockCommand *ctx = g_new0 (MockCommand, 1);

    g_assert_null (self->pending);
    g_assert_cmpuint (self->commands, <, 2);
    g_assert_cmpstr (command, ==, self->commands ? "+CIREP?" : "+CIREG?");
    g_assert_cmpuint (timeout, ==, 3);
    g_assert_false (cached);
    g_assert_false (raw);
    g_assert_true (cancellable != self->modem_cancel);
    g_assert_false (g_cancellable_is_cancelled (cancellable));
    g_assert_true (port == self->port);
    ctx->port = g_object_ref (port);
    self->commands++;
    self->pending = g_task_new (self, cancellable, callback, user_data);
    g_task_set_task_data (self->pending, ctx, (GDestroyNotify) mock_command_free);
}

static const gchar *
mm_base_modem_at_command_full_finish (MMBaseModem *self G_GNUC_UNUSED, GAsyncResult *res, GError **error)
{
    g_assert_nonnull (error);
    finished++;
    return g_task_propagate_pointer (G_TASK (res), error);
}

void
_mm_log (gpointer object G_GNUC_UNUSED, const gchar *module G_GNUC_UNUSED,
         const gchar *loc G_GNUC_UNUSED, const gchar *func G_GNUC_UNUSED,
         MMLogLevel level, const gchar *format, ...)
{
    va_list args;

    g_assert_cmpint (level, ==, MM_LOG_LEVEL_MSG);
    messages++;
    g_free (last_message);
    va_start (args, format);
    last_message = g_strdup_vprintf (format, args);
    va_end (args);
    g_assert_null (strstr (last_message, "synthetic-private"));
}

static void
get_property (GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
    MMBaseModem *self = (MMBaseModem *) object;
    if (id == 1)
        g_value_set_int (value, self->state);
    else if (id == 2)
        g_value_set_int (value, self->registration);
    else
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, id, pspec);
}

static void
set_property (GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
    MMBaseModem *self = (MMBaseModem *) object;
    if (id == 1)
        self->state = g_value_get_int (value);
    else if (id == 2)
        self->registration = g_value_get_int (value);
    else
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, id, pspec);
}

static void
mock_dispose (GObject *object)
{
    MMBaseModem *self = (MMBaseModem *) object;
    mm_mtk_ccci_ims_observer_dispose (&self->observer);
    g_clear_object (&self->port);
    g_clear_object (&self->modem_cancel);
    G_OBJECT_CLASS (mm_base_modem_parent_class)->dispose (object);
}

static void
mock_finalize (GObject *object)
{
    MMBaseModem *self = (MMBaseModem *) object;
    g_assert_null (self->pending);
    mm_mtk_ccci_ims_observer_clear (&self->observer);
    finalized++;
    G_OBJECT_CLASS (mm_base_modem_parent_class)->finalize (object);
}

static void
mm_base_modem_class_init (MMBaseModemClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    object_class->get_property = get_property;
    object_class->set_property = set_property;
    object_class->dispose = mock_dispose;
    object_class->finalize = mock_finalize;
    g_object_class_install_property (object_class, 1,
        g_param_spec_int (MM_IFACE_MODEM_STATE, "state", "state", -1, 11, 3, G_PARAM_READWRITE));
    g_object_class_install_property (object_class, 2,
        g_param_spec_int (MM_IFACE_MODEM_3GPP_REGISTRATION_STATE, "registration", "registration",
                          0, 11, 4, G_PARAM_READWRITE));
}

static void
mm_base_modem_init (MMBaseModem *self)
{
    self->state = MM_MODEM_STATE_DISABLED;
    self->registration = MM_MODEM_3GPP_REGISTRATION_STATE_UNKNOWN;
    self->port = g_object_new (G_TYPE_OBJECT, NULL);
    self->modem_cancel = g_cancellable_new ();
    mm_mtk_ccci_ims_observer_init (&self->observer, self);
}

static MMBaseModem *
new_modem (const gchar *opt_in)
{
    if (g_strcmp0 (g_getenv ("Q25_MM_IMS_OBSERVE_ONCE"), opt_in)) {
        if (opt_in)
            g_setenv ("Q25_MM_IMS_OBSERVE_ONCE", opt_in, TRUE);
        else
            g_unsetenv ("Q25_MM_IMS_OBSERVE_ONCE");
    }
    finished = messages = finalized = 0;
    g_clear_pointer (&last_message, g_free);
    return g_object_new (mm_base_modem_get_type (), NULL);
}

static void
pump (void)
{
    while (g_main_context_iteration (NULL, FALSE));
}

static void
register_modem (MMBaseModem *self)
{
    g_object_set (self, MM_IFACE_MODEM_STATE, MM_MODEM_STATE_REGISTERED,
                  MM_IFACE_MODEM_3GPP_REGISTRATION_STATE, MM_MODEM_3GPP_REGISTRATION_STATE_HOME, NULL);
}

static void
complete (MMBaseModem *self, const gchar *response)
{
    GTask *task = g_steal_pointer (&self->pending);
    MockCommand *ctx;

    g_assert_nonnull (task);
    ctx = g_task_get_task_data (task);
    if (response) {
        ctx->response = g_strdup (response);
        g_task_return_pointer (task, ctx->response, NULL);
    } else
        g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "synthetic-private error");
    g_object_unref (task);
}

static void
test_opt_out (void)
{
    const gchar *values[] = { NULL, "", "0", "true", "01" };
    guint i;
    for (i = 0; i < G_N_ELEMENTS (values); i++) {
        MMBaseModem *self = new_modem (values[i]);
        g_assert_cmpuint (self->observer.state_id, ==, 0);
        g_assert_cmpuint (self->observer.registration_id, ==, 0);
        g_setenv ("Q25_MM_IMS_OBSERVE_ONCE", "1", TRUE); /* No runtime opt-in. */
        register_modem (self);
        pump ();
        g_assert_cmpuint (self->commands, ==, 0);
        g_assert_cmpuint (messages, ==, 0);
        g_object_unref (self);
    }
}

static void
test_registration_and_one_shot (void)
{
    MMBaseModem *self = new_modem ("1");
    pump ();
    g_assert_cmpuint (self->commands, ==, 0);
    g_object_set (self, MM_IFACE_MODEM_STATE, MM_MODEM_STATE_SEARCHING,
                  MM_IFACE_MODEM_3GPP_REGISTRATION_STATE, MM_MODEM_3GPP_REGISTRATION_STATE_HOME, NULL);
    pump ();
    g_assert_cmpuint (self->commands, ==, 0);
    g_object_set (self, MM_IFACE_MODEM_STATE, MM_MODEM_STATE_REGISTERED,
                  MM_IFACE_MODEM_3GPP_REGISTRATION_STATE, MM_MODEM_3GPP_REGISTRATION_STATE_DENIED, NULL);
    pump ();
    g_assert_cmpuint (self->commands, ==, 0);
    g_object_set (self, MM_IFACE_MODEM_3GPP_REGISTRATION_STATE, MM_MODEM_3GPP_REGISTRATION_STATE_ROAMING, NULL);
    pump ();
    g_assert_cmpuint (self->commands, ==, 1);
    complete (self, "+CIREG: 2,1,5");
    pump ();
    g_assert_cmpuint (self->commands, ==, 2);
    g_assert_cmpuint (messages, ==, 0);
    complete (self, "+CIREP: 1,1");
    pump ();
    g_assert_cmpuint (finished, ==, 2);
    g_assert_cmpuint (messages, ==, 1);
    g_assert_true (g_str_has_prefix (last_message,
        "q25-ims-observer CIREG reporting=2 registered=1 capabilities=0x00000005; CIREP reporting=1 vops=1 vops-n3gpp=unknown; "));
    g_assert_nonnull (strstr (last_message,
        "CIREG-diag[status=parsed class=none code=-1 length=-1 prefix=-1 fields=-1 dec=-1 hex=-1 multiline=-1]"));
    g_assert_nonnull (strstr (last_message,
        "CIREP-diag[status=parsed class=none code=-1 length=-1 prefix=-1 fields=-1 dec=-1 hex=-1 multiline=-1]"));
    g_object_set (self, MM_IFACE_MODEM_STATE, MM_MODEM_STATE_DISABLED, NULL);
    register_modem (self);
    pump ();
    g_assert_cmpuint (self->commands, ==, 2);
    g_assert_cmpuint (messages, ==, 1);
    g_object_unref (self);
}

static void
test_cancel_before_enqueue (void)
{
    MMBaseModem *self = new_modem ("1");
    register_modem (self);
    g_assert_cmpuint (self->observer.dispatch_id, !=, 0);
    g_object_set (self, MM_IFACE_MODEM_STATE, MM_MODEM_STATE_DISABLING, NULL);
    register_modem (self);
    pump ();
    g_assert_cmpuint (self->commands, ==, 0);
    g_assert_true (self->observer.consumed);
    g_assert_cmpuint (messages, ==, 0);
    g_object_unref (self);
}

static void
test_disable_in_flight (gconstpointer data)
{
    MMBaseModem *self = new_modem ("1");
    register_modem (self);
    pump ();
    if (GPOINTER_TO_INT (data) == 2) {
        complete (self, "+CIREG: 2,1,5");
        pump ();
    }
    if (GPOINTER_TO_INT (data) == 1)
        g_object_set (self, MM_IFACE_MODEM_3GPP_REGISTRATION_STATE, MM_MODEM_3GPP_REGISTRATION_STATE_DENIED, NULL);
    else
        g_object_set (self, MM_IFACE_MODEM_STATE, MM_MODEM_STATE_DISABLING, NULL);
    g_assert_false (g_cancellable_is_cancelled (self->observer.cancellable));
    /* Late success must drain, not log or enqueue. */
    complete (self, GPOINTER_TO_INT (data) == 2 ? "+CIREP: 1,1" : "+CIREG: 2,1,5");
    pump ();
    register_modem (self);
    pump ();
    g_assert_cmpuint (finished, ==, GPOINTER_TO_INT (data) == 2 ? 2 : 1);
    g_assert_cmpuint (self->commands, ==, GPOINTER_TO_INT (data) == 2 ? 2 : 1);
    g_assert_cmpuint (messages, ==, 0);
    g_object_unref (self);
}

static void
test_between_queries (void)
{
    MMBaseModem *self = new_modem ("1");
    register_modem (self);
    pump ();
    complete (self, "+CIREG: 0,0");
    g_main_context_iteration (NULL, FALSE); /* Completion only; next dispatch is idle. */
    g_assert_cmpuint (finished, ==, 1);
    g_assert_cmpuint (self->observer.dispatch_id, !=, 0);
    g_object_set (self, MM_IFACE_MODEM_STATE, MM_MODEM_STATE_DISABLING, NULL);
    pump ();
    g_assert_cmpuint (self->commands, ==, 1);
    g_assert_cmpuint (messages, ==, 0);
    g_object_unref (self);
}

static void
test_dispose (gconstpointer data)
{
    MMBaseModem *self = new_modem ("1");
    GObject *weak_port = self->port;
    guint stage = GPOINTER_TO_UINT (data);

    register_modem (self);
    if (stage)
        pump ();
    if (stage == 2) {
        complete (self, "+CIREG: 2,1,5");
        pump ();
    }
    g_object_add_weak_pointer (weak_port, (gpointer *) &weak_port);
    g_object_run_dispose (G_OBJECT (self));
    g_assert_cmpuint (self->observer.state_id, ==, 0);
    g_assert_cmpuint (self->observer.registration_id, ==, 0);
    g_assert_cmpuint (self->observer.dispatch_id, ==, 0);
    if (stage)
        g_assert_false (g_cancellable_is_cancelled (self->observer.cancellable));
    g_object_unref (self);
    if (stage) {
        /* run_dispose clears weak pointers before finalization; count finalizes
         * instead. The real AT task owns its source modem and selected port. */
        g_assert_cmpuint (finalized, ==, 0);
        g_assert_nonnull (weak_port);
        complete (self, stage == 1 ? "+CIREG: 2,1,5" : "+CIREP: 1,1");
        pump ();
        g_assert_cmpuint (finished, ==, stage);
    }
    g_assert_cmpuint (finalized, ==, 1);
    g_assert_null (weak_port);
    g_assert_cmpuint (messages, ==, 0);
}

static void
test_unknown_results (gconstpointer data)
{
    MMBaseModem *self = new_modem ("1");
    register_modem (self);
    pump ();
    complete (self, GPOINTER_TO_INT (data) ? "+CIREG: 2,1,5\r\nsynthetic-private" : NULL);
    pump ();
    g_assert_cmpuint (self->commands, ==, 2); /* Failure never retries the first query. */
    complete (self, "+CIREP: 9,synthetic-private");
    pump ();
    g_assert_cmpuint (messages, ==, 1);
    g_assert_true (g_str_has_prefix (last_message,
        "q25-ims-observer CIREG reporting=unknown registered=unknown capabilities=unknown; CIREP reporting=unknown vops=unknown vops-n3gpp=unknown; "));
    g_assert_cmpstr (self->observer.diagnostics[0].status, ==, GPOINTER_TO_INT (data) ? "rejected" : "error");
    g_assert_cmpstr (self->observer.diagnostics[0].error_class, ==, GPOINTER_TO_INT (data) ? "none" : "io");
    g_assert_cmpint (self->observer.diagnostics[0].error_code, ==, GPOINTER_TO_INT (data) ? -1 : G_IO_ERROR_TIMED_OUT);
    if (GPOINTER_TO_INT (data)) {
        g_assert_cmpint (self->observer.diagnostics[0].shape.multiline, ==, 1);
        g_assert_cmpint (self->observer.diagnostics[0].shape.fields, ==, -1);
    }
    g_assert_cmpstr (self->observer.diagnostics[1].status, ==, "rejected");
    g_object_unref (self);
}

static void
test_modem_cancel (void)
{
    MMBaseModem *self = new_modem ("1");
    register_modem (self);
    g_cancellable_cancel (self->modem_cancel);
    pump ();
    g_assert_cmpuint (self->commands, ==, 0);
    g_assert_cmpuint (messages, ==, 0);
    g_object_unref (self);
}

static void
test_no_port (void)
{
    MMBaseModem *self = new_modem ("1");
    self->no_port = TRUE;
    register_modem (self);
    pump ();
    g_assert_cmpuint (self->commands, ==, 0);
    g_assert_cmpuint (messages, ==, 1);
    g_assert_nonnull (strstr (last_message, "registered=unknown"));
    g_assert_cmpstr (self->observer.diagnostics[0].status, ==, "not-run");
    g_assert_cmpstr (self->observer.diagnostics[1].status, ==, "not-run");
    register_modem (self);
    pump ();
    g_assert_cmpuint (messages, ==, 1);
    g_object_unref (self);
}

static void
complete_error (MMBaseModem *self, GQuark domain, gint code)
{
    GTask *task = g_steal_pointer (&self->pending);
    g_assert_nonnull (task);
    g_task_return_new_error (task, domain, code, "synthetic-private error 123456789012345");
    g_object_unref (task);
}

static void
test_error_classification (void)
{
    struct {
        GQuark domain;
        gint code;
        const gchar *expected_class;
        gint expected_code;
    } cases[] = {
        { G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "io", G_IO_ERROR_TIMED_OUT },
        { MM_SERIAL_ERROR, MM_SERIAL_ERROR_RESPONSE_TIMEOUT, "serial", MM_SERIAL_ERROR_RESPONSE_TIMEOUT },
        { MM_CORE_ERROR, MM_CORE_ERROR_WRONG_STATE, "core", MM_CORE_ERROR_WRONG_STATE },
        { MM_MOBILE_EQUIPMENT_ERROR, MM_MOBILE_EQUIPMENT_ERROR_UNKNOWN, "mobile-equipment", 100 },
        { MM_MOBILE_EQUIPMENT_ERROR, MM_MOBILE_EQUIPMENT_ERROR_NOT_SUPPORTED, "mobile-equipment", 4 },
        { MM_MESSAGE_ERROR, MM_MESSAGE_ERROR_UNKNOWN, "message", MM_MESSAGE_ERROR_UNKNOWN },
        { MM_CONNECTION_ERROR, MM_CONNECTION_ERROR_NO_CARRIER, "connection", MM_CONNECTION_ERROR_NO_CARRIER },
        { MM_CORE_ERROR, G_MAXINT, "core", -1 },
        { MM_CORE_ERROR, -9, "core", -1 },
        { g_quark_from_static_string ("synthetic-private-domain"), 12345, "other", -1 },
    };
    guint i;

    for (i = 0; i < G_N_ELEMENTS (cases); i++) {
        MMBaseModem *self = new_modem ("1");
        MMMtkCcciImsDiagnostic *diagnostic = &self->observer.diagnostics[0];
        register_modem (self);
        pump ();
        complete_error (self, cases[i].domain, cases[i].code);
        pump ();
        g_assert_cmpuint (self->commands, ==, 2);
        g_assert_cmpuint (messages, ==, 0); /* No intermediate/stale error record. */
        g_assert_cmpstr (diagnostic->status, ==, "error");
        g_assert_cmpstr (diagnostic->error_class, ==, cases[i].expected_class);
        g_assert_cmpint (diagnostic->error_code, ==, cases[i].expected_code);
        g_assert_cmpint (diagnostic->shape.length, ==, -1);
        g_assert_cmpint (self->observer.cireg.registered, ==, -1);
        complete (self, "+CIREP: 1,1");
        pump ();
        g_assert_cmpuint (messages, ==, 1);
        g_assert_cmpuint (finished, ==, 2);
        g_assert_nonnull (strstr (last_message, "CIREG-diag[status=error"));
        g_assert_null (strstr (last_message, "123456789012345"));
        g_object_unref (self);
    }
}

static void
test_empty_and_missing_response (void)
{
    MMBaseModem *self = new_modem ("1");
    register_modem (self);
    pump ();
    complete (self, ""); /* Successful terminal OK without a solicited data line. */
    pump ();
    g_assert_cmpstr (self->observer.diagnostics[0].status, ==, "rejected");
    g_assert_cmpint (self->observer.diagnostics[0].shape.length, ==, 0);
    g_assert_cmpint (self->observer.diagnostics[0].shape.prefix_matches, ==, 0);
    g_assert_cmpstr (self->observer.diagnostics[0].error_class, ==, "none");
    {
        GTask *task = g_steal_pointer (&self->pending);
        /* Defensive distinction: MM's normal AT ready path promises a string
         * or GError, but a NULL success must never look like unregistered. */
        g_task_return_pointer (task, NULL, NULL);
        g_object_unref (task);
    }
    pump ();
    g_assert_cmpuint (messages, ==, 1);
    g_assert_cmpstr (self->observer.diagnostics[1].status, ==, "no-response");
    g_assert_cmpint (self->observer.diagnostics[1].shape.length, ==, -1);
    g_assert_cmpint (self->observer.cireg.registered, ==, -1);
    g_object_unref (self);
}

static void
test_rejected_enum_and_recorded_prefix (void)
{
    MMBaseModem *self = new_modem ("1");
    register_modem (self);
    pump ();
    /* Same shape as r4, but unknown enum: shape does not identify its values. */
    complete (self, "+CIREG: 0,2,0");
    pump ();
    g_assert_cmpstr (self->observer.diagnostics[0].status, ==, "rejected");
    g_assert_cmpint (self->observer.diagnostics[0].shape.fields, ==, 3);
    g_assert_cmpint (self->observer.diagnostics[0].shape.decimal_fields, ==, 7);
    g_assert_cmpint (self->observer.cireg.registered, ==, -1);
    complete (self, "+CIREP:1,1");
    pump ();
    g_assert_cmpuint (messages, ==, 1);
    g_assert_nonnull (strstr (last_message, "CIREG-diag[status=rejected class=none code=-1 length=13 prefix=1 fields=3 dec=7 hex=7 multiline=0]"));
    g_object_unref (self);

    self = new_modem ("1");
    register_modem (self);
    pump ();
    complete (self, "+CIREG:0,0"); /* Actual earlier retained diagnostic shape. */
    pump ();
    complete (self, "+CIREP:1,1");
    pump ();
    g_assert_cmpuint (messages, ==, 1);
    g_assert_cmpstr (self->observer.diagnostics[0].status, ==, "parsed");
    g_assert_cmpint (self->observer.cireg.registered, ==, 0);
    g_assert_false (self->observer.cireg.capabilities_known);
    g_object_unref (self);
}

static void
test_cireg_padding (void)
{
    static const struct {
        const gchar *reply;
        gint registered;
        gboolean capabilities_known;
    } cases[] = {
        { "+CIREG: 0,0,0", 0, FALSE },
        { "+CIREG: 0,1,0", 1, FALSE },
        { "+CIREG: 0,0,5", 0, FALSE },
        { "+CIREG: 0,1,5", 1, TRUE },
    };
    guint i;

    for (i = 0; i < G_N_ELEMENTS (cases); i++) {
        MMBaseModem *self = new_modem ("1");
        g_autofree gchar *expected = NULL;

        register_modem (self);
        pump ();
        complete (self, cases[i].reply);
        pump ();
        g_assert_cmpuint (messages, ==, 0);
        complete (self, "+CIREP:1,1");
        pump ();
        g_assert_cmpuint (messages, ==, 1);
        g_assert_cmpuint (self->commands, ==, 2);
        g_assert_cmpuint (finished, ==, 2);
        g_assert_cmpstr (self->observer.diagnostics[0].status, ==, "parsed");
        g_assert_cmpstr (self->observer.diagnostics[1].status, ==, "parsed");
        g_assert_cmpint (self->observer.diagnostics[0].shape.length, ==, -1);
        g_assert_cmpint (self->observer.cireg.reporting, ==, 0);
        g_assert_cmpint (self->observer.cireg.registered, ==, cases[i].registered);
        g_assert_cmpint (self->observer.cireg.capabilities_known, ==, cases[i].capabilities_known);
        g_assert_cmpuint (self->observer.cireg.capabilities, ==, cases[i].capabilities_known ? 5 : 0);
        expected = g_strdup_printf ("q25-ims-observer CIREG reporting=0 registered=%d capabilities=%s;",
                                    cases[i].registered, cases[i].capabilities_known ? "0x00000005" : "unknown");
        g_assert_true (g_str_has_prefix (last_message, expected));
        g_object_unref (self);
    }
}

static void
test_late_error (gconstpointer dispose)
{
    MMBaseModem *self = new_modem ("1");
    register_modem (self);
    pump ();
    if (GPOINTER_TO_INT (dispose)) {
        g_object_run_dispose (G_OBJECT (self));
        g_object_unref (self); /* AT task still holds it, as in the late-success tests. */
        g_assert_cmpuint (finalized, ==, 0);
    } else
        g_object_set (self, MM_IFACE_MODEM_STATE, MM_MODEM_STATE_DISABLING, NULL);
    complete_error (self, MM_MOBILE_EQUIPMENT_ERROR, MM_MOBILE_EQUIPMENT_ERROR_UNKNOWN);
    pump ();
    g_assert_cmpuint (finished, ==, 1);
    g_assert_cmpuint (messages, ==, 0);
    if (!GPOINTER_TO_INT (dispose)) {
        g_assert_cmpuint (self->commands, ==, 1);
        g_assert_cmpstr (self->observer.diagnostics[0].status, ==, "not-run");
        g_object_unref (self);
    }
    g_assert_cmpuint (finalized, ==, 1);
}

int
main (int argc, char **argv)
{
    gint result;
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/mtk-ccci/ims-observer/opt-out", test_opt_out);
    g_test_add_func ("/mtk-ccci/ims-observer/registration-one-shot", test_registration_and_one_shot);
    g_test_add_func ("/mtk-ccci/ims-observer/cancel-before-enqueue", test_cancel_before_enqueue);
    g_test_add_data_func ("/mtk-ccci/ims-observer/disable-in-flight", GINT_TO_POINTER (0), test_disable_in_flight);
    g_test_add_data_func ("/mtk-ccci/ims-observer/registration-loss", GINT_TO_POINTER (1), test_disable_in_flight);
    g_test_add_data_func ("/mtk-ccci/ims-observer/disable-second-query", GINT_TO_POINTER (2), test_disable_in_flight);
    g_test_add_func ("/mtk-ccci/ims-observer/cancel-between-queries", test_between_queries);
    g_test_add_data_func ("/mtk-ccci/ims-observer/dispose-before-enqueue", GUINT_TO_POINTER (0), test_dispose);
    g_test_add_data_func ("/mtk-ccci/ims-observer/dispose-first-query", GUINT_TO_POINTER (1), test_dispose);
    g_test_add_data_func ("/mtk-ccci/ims-observer/dispose-second-query", GUINT_TO_POINTER (2), test_dispose);
    g_test_add_data_func ("/mtk-ccci/ims-observer/timeout-no-retry", GINT_TO_POINTER (0), test_unknown_results);
    g_test_add_data_func ("/mtk-ccci/ims-observer/malformed-no-raw-log", GINT_TO_POINTER (1), test_unknown_results);
    g_test_add_func ("/mtk-ccci/ims-observer/modem-cancel-before-enqueue", test_modem_cancel);
    g_test_add_func ("/mtk-ccci/ims-observer/no-port-no-retry", test_no_port);
    g_test_add_func ("/mtk-ccci/ims-observer/error-classification", test_error_classification);
    g_test_add_func ("/mtk-ccci/ims-observer/empty-and-missing-response", test_empty_and_missing_response);
    g_test_add_func ("/mtk-ccci/ims-observer/rejected-enum-recorded-prefix", test_rejected_enum_and_recorded_prefix);
    g_test_add_func ("/mtk-ccci/ims-observer/cireg-padding", test_cireg_padding);
    g_test_add_data_func ("/mtk-ccci/ims-observer/disable-late-error", GINT_TO_POINTER (0), test_late_error);
    g_test_add_data_func ("/mtk-ccci/ims-observer/dispose-late-error", GINT_TO_POINTER (1), test_late_error);
    result = g_test_run ();
    g_free (last_message);
    return result;
}
