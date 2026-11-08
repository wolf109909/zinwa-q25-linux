/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <gtk/gtk.h>
#include "plugin-loader.h"
#include "status-icon.h"
#include "phosh-plugin.h"

int main(int argc, char **argv)
{
  gtk_test_init (&argc, &argv, NULL);
  g_assert_cmpint (argc, ==, 2);
  char *dirs[] = {argv[1], NULL};
  g_autoptr (PhoshPluginLoader) loader = phosh_plugin_loader_new (
    dirs, PHOSH_PLUGIN_EXTENSION_POINT_STATUS_ICON_WIDGET);
  GtkWidget *widget = phosh_plugin_loader_load_plugin (loader, "q25-keyboard");
  g_assert_nonnull (widget);
  g_assert_true (PHOSH_IS_STATUS_ICON (widget));
  g_assert_cmpint (phosh_status_icon_get_priority (PHOSH_STATUS_ICON (widget)), ==, 10);
  g_assert_false (gtk_widget_get_visible (widget));
  g_object_ref_sink (widget);
  gtk_widget_destroy (widget);
  g_object_unref (widget);
  return 0;
}
