/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MM_MTK_CCCI_IMS_OBSERVER_TEST
#include <config.h>
#include "mm-base-modem-at.h"
#include "mm-iface-modem.h"
#include "mm-iface-modem-3gpp.h"
#include "mm-log-object.h"
#include "mm-errors-types.h"
#endif
#include "mm-ims-observer-mtk-ccci.h"

static void schedule_query (MMMtkCcciImsObserver *observer);

static gboolean
normally_registered (MMMtkCcciImsObserver *observer)
{
    MMModemState state;
    MMModem3gppRegistrationState registration;

    g_object_get (observer->modem,
                  MM_IFACE_MODEM_STATE, &state,
                  MM_IFACE_MODEM_3GPP_REGISTRATION_STATE, &registration, NULL);
    return state >= MM_MODEM_STATE_REGISTERED &&
           (registration == MM_MODEM_3GPP_REGISTRATION_STATE_HOME ||
            registration == MM_MODEM_3GPP_REGISTRATION_STATE_ROAMING);
}

static void
stop_observation (MMMtkCcciImsObserver *observer)
{
    observer->stopped = TRUE;
    g_clear_handle_id (&observer->dispatch_id, g_source_remove);
    /* MM 1.24.2 serial cancellation advances the queue before a late response
     * can drain. Do not force-cancel a submitted transaction: its 3s timeout
     * and core removal handling remain in force; discard its completion. */
    if (observer->cancellable && !observer->in_flight)
        g_cancellable_cancel (observer->cancellable);
}

static gboolean
observation_active (MMMtkCcciImsObserver *observer)
{
    return !observer->disposed && !observer->stopped &&
           !g_cancellable_is_cancelled (observer->cancellable) &&
           !g_cancellable_is_cancelled (mm_base_modem_peek_cancellable (observer->modem)) &&
           normally_registered (observer);
}

static const gchar *
enum_text (gint value)
{
    switch (value) {
    case 0: return "0";
    case 1: return "1";
    case 2: return "2";
    default: return "unknown";
    }
}

static const gchar *
error_class (const GError *error)
{
    if (error->domain == G_IO_ERROR)                 return "io";
    if (error->domain == MM_CORE_ERROR)              return "core";
    if (error->domain == MM_SERIAL_ERROR)            return "serial";
    if (error->domain == MM_MOBILE_EQUIPMENT_ERROR)  return "mobile-equipment";
    if (error->domain == MM_MESSAGE_ERROR)           return "message";
    if (error->domain == MM_CONNECTION_ERROR)        return "connection";
    return "other";
}

static void
classify_response (MMMtkCcciImsObserver *observer, const gchar *response, const GError *error)
{
    MMMtkCcciImsDiagnostic *diagnostic = &observer->diagnostics[observer->query];
    gboolean parsed;

    if (error) {
        diagnostic->status = "error";
        diagnostic->error_class = error_class (error);
        if (g_strcmp0 (diagnostic->error_class, "other") && error->code >= 0 && error->code <= G_MAXUINT16)
            diagnostic->error_code = error->code;
        return;
    }
    if (!response) {
        diagnostic->status = "no-response";
        return;
    }
    parsed = observer->query == 0 ? mm_mtk_ccci_parse_cireg (response, &observer->cireg) :
                                   mm_mtk_ccci_parse_cirep (response, &observer->cirep);
    diagnostic->status = parsed ? "parsed" : "rejected";
    if (!parsed)
        mm_mtk_ccci_inspect_ims_response (response, observer->query == 0 ? "+CIREG:" : "+CIREP:",
                                         &diagnostic->shape);
}

static gchar *
diagnostic_text (const MMMtkCcciImsDiagnostic *diagnostic)
{
    return g_strdup_printf ("status=%s class=%s code=%d length=%d prefix=%d fields=%d dec=%d hex=%d multiline=%d",
                            diagnostic->status, diagnostic->error_class, diagnostic->error_code,
                            diagnostic->shape.length, diagnostic->shape.prefix_matches,
                            diagnostic->shape.fields, diagnostic->shape.decimal_fields,
                            diagnostic->shape.hex_fields, diagnostic->shape.multiline);
}

static void
report_observation (MMMtkCcciImsObserver *observer)
{
    gchar capabilities[11];
    g_autofree gchar *cireg_diagnostic = diagnostic_text (&observer->diagnostics[0]);
    g_autofree gchar *cirep_diagnostic = diagnostic_text (&observer->diagnostics[1]);

    g_snprintf (capabilities, sizeof (capabilities), "0x%08x", observer->cireg.capabilities);
    mm_obj_msg (observer->modem,
                "q25-ims-observer CIREG reporting=%s registered=%s capabilities=%s; "
                "CIREP reporting=%s vops=%s vops-n3gpp=%s; CIREG-diag[%s] CIREP-diag[%s]",
                enum_text (observer->cireg.reporting), enum_text (observer->cireg.registered),
                observer->cireg.capabilities_known ? capabilities : "unknown",
                enum_text (observer->cirep.reporting), enum_text (observer->cirep.vops),
                enum_text (observer->cirep.vops_n3gpp), cireg_diagnostic, cirep_diagnostic);
    stop_observation (observer);
}

static void
query_ready (MMBaseModem *modem, GAsyncResult *res, MMMtkCcciImsObserver *observer)
{
    const gchar *response;
    g_autoptr(GError) error = NULL;

    /* Finish even after disable/dispose; the task owns the modem and response.
     * Copy only parsed/scalar diagnostics before that borrowed response expires. */
    response = mm_base_modem_at_command_full_finish (modem, res, &error);
    observer->in_flight = FALSE;
    if (!observation_active (observer)) {
        stop_observation (observer);
        return;
    }
    classify_response (observer, response, error);
    observer->query++;
    if (observer->query == 2)
        report_observation (observer);
    else
        schedule_query (observer);
}

static gboolean
dispatch_query (MMMtkCcciImsObserver *observer)
{
    MMIfacePortAt *port;

    observer->dispatch_id = 0;
    if (!observation_active (observer)) {
        stop_observation (observer);
        return G_SOURCE_REMOVE;
    }
    port = mm_base_modem_peek_best_at_port (observer->modem, NULL);
    if (!port) {
        report_observation (observer);
        return G_SOURCE_REMOVE;
    }
    observer->in_flight = TRUE;
    mm_base_modem_at_command_full (observer->modem, port,
                                   observer->query == 0 ? "+CIREG?" : "+CIREP?",
                                   3, FALSE, FALSE, observer->cancellable,
                                   (GAsyncReadyCallback) query_ready, observer);
    return G_SOURCE_REMOVE;
}

static void
release_dispatch (MMMtkCcciImsObserver *observer)
{
    g_object_unref (observer->modem);
}

static void
schedule_query (MMMtkCcciImsObserver *observer)
{
    /* A single dispatch (not a poll) allows disable notifications to cancel
     * before either enqueue, including between the two transactions. */
    g_object_ref (observer->modem);
    observer->dispatch_id = g_idle_add_full (G_PRIORITY_DEFAULT_IDLE,
                                             (GSourceFunc) dispatch_query, observer,
                                             (GDestroyNotify) release_dispatch);
}

static void
state_changed (GObject *modem G_GNUC_UNUSED, GParamSpec *pspec G_GNUC_UNUSED,
               MMMtkCcciImsObserver *observer)
{
    if (observer->disposed)
        return;
    if (observer->consumed) {
        if (!normally_registered (observer))
            stop_observation (observer);
        return;
    }
    if (!normally_registered (observer))
        return;
    observer->consumed = TRUE; /* Never reset on disable or registration churn. */
    observer->cancellable = g_cancellable_new ();
    schedule_query (observer);
}

void
mm_mtk_ccci_ims_observer_init (MMMtkCcciImsObserver *observer, MMBaseModem *modem)
{
    guint i;

    observer->modem = modem;
    for (i = 0; i < G_N_ELEMENTS (observer->diagnostics); i++) {
        observer->diagnostics[i].status = "not-run";
        observer->diagnostics[i].error_class = "none";
        observer->diagnostics[i].error_code = -1;
        mm_mtk_ccci_inspect_ims_response (NULL, NULL, &observer->diagnostics[i].shape);
    }
    mm_mtk_ccci_parse_cireg (NULL, &observer->cireg);
    mm_mtk_ccci_parse_cirep (NULL, &observer->cirep);
    if (g_strcmp0 (g_getenv ("Q25_MM_IMS_OBSERVE_ONCE"), "1"))
        return;
    observer->state_id = g_signal_connect (modem, "notify::" MM_IFACE_MODEM_STATE,
                                           G_CALLBACK (state_changed), observer);
    observer->registration_id = g_signal_connect (modem, "notify::" MM_IFACE_MODEM_3GPP_REGISTRATION_STATE,
                                                  G_CALLBACK (state_changed), observer);
    state_changed (G_OBJECT (modem), NULL, observer);
}

void
mm_mtk_ccci_ims_observer_dispose (MMMtkCcciImsObserver *observer)
{
    observer->disposed = TRUE;
    if (observer->state_id) {
        g_signal_handler_disconnect (observer->modem, observer->state_id);
        observer->state_id = 0;
    }
    if (observer->registration_id) {
        g_signal_handler_disconnect (observer->modem, observer->registration_id);
        observer->registration_id = 0;
    }
    stop_observation (observer);
}

void
mm_mtk_ccci_ims_observer_clear (MMMtkCcciImsObserver *observer)
{
    g_clear_object (&observer->cancellable);
}
