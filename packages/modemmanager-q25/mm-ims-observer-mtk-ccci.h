/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MM_IMS_OBSERVER_MTK_CCCI_H
#define MM_IMS_OBSERVER_MTK_CCCI_H

#include "mm-modem-helpers-mtk-ccci.h"

typedef struct _MMBaseModem MMBaseModem;

typedef struct {
    const gchar *status;       /* Fixed literals only; never response/error text. */
    const gchar *error_class;
    gint error_code;           /* -1 means absent/unknown; known classes cap at 65535. */
    MMMtkCcciImsShape shape;
} MMMtkCcciImsDiagnostic;

typedef struct {
    MMBaseModem *modem;          /* Borrowed; dispatches/AT tasks hold the modem. */
    GCancellable *cancellable;
    gulong state_id;
    gulong registration_id;
    guint dispatch_id;
    guint query;
    gboolean consumed;
    gboolean stopped;
    gboolean disposed;
    gboolean in_flight;
    MMMtkCcciCireg cireg;
    MMMtkCcciCirep cirep;
    MMMtkCcciImsDiagnostic diagnostics[2];
} MMMtkCcciImsObserver;

void mm_mtk_ccci_ims_observer_init (MMMtkCcciImsObserver *observer, MMBaseModem *modem);
void mm_mtk_ccci_ims_observer_dispose (MMMtkCcciImsObserver *observer);
void mm_mtk_ccci_ims_observer_clear (MMMtkCcciImsObserver *observer);

#endif
