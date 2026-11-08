/* SPDX-License-Identifier: GPL-3.0-or-later
 * Real Phosh 0.57 widgets under Xvfb, synthetic state server (no input APIs). */
#define _GNU_SOURCE
#include <sys/types.h>
static uid_t test_expected_uid;
#define Q25_STATE_UID test_expected_uid
#include "q25-keyboard.c"
#include "status-icons-box.h"
#include "revealer.h"

static int server;

static void pump(void)
{
  for (int i = 0; i < 30; i++) {
    while (g_main_context_iteration (NULL, FALSE));
    g_usleep (1000);
  }
}

static void frame(int client, guint8 shift, guint8 sym)
{
  guint8 data[] = {'Q', 1, shift, sym};
  g_assert_cmpint (send (client, data, 4, MSG_NOSIGNAL), ==, 4);
  pump ();
}

static void listen_state(void)
{
  struct sockaddr_un addr = {.sun_family = AF_UNIX, .sun_path = Q25_STATE_SOCKET};
  unlink (Q25_STATE_SOCKET);
  server = socket (AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
  g_assert_cmpint (bind (server, (struct sockaddr *)&addr, sizeof(addr)), ==, 0);
  g_assert_cmpint (listen (server, 8), ==, 0);
}

static void test_widget(void)
{
  listen_state ();
  test_expected_uid = getuid ();
  GtkWidget *window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  GtkWidget *box = phosh_status_icons_box_new (8);
  gtk_container_add (GTK_CONTAINER (window), box);
  GtkWidget *location = phosh_status_icon_new ();
  GtkWidget *battery = phosh_status_icon_new ();
  phosh_status_icon_set_icon_name (PHOSH_STATUS_ICON (location), "location-services-active-symbolic");
  phosh_status_icon_set_icon_name (PHOSH_STATUS_ICON (battery), "battery-full-symbolic");
  phosh_status_icons_box_append (PHOSH_STATUS_ICONS_BOX (box), location);
  phosh_status_icons_box_append (PHOSH_STATUS_ICONS_BOX (box), battery);
  Q25Keyboard *self = g_object_new (q25_keyboard_get_type (), NULL);
  phosh_status_icons_box_append (PHOSH_STATUS_ICONS_BOX (box), GTK_WIDGET (self));
  gtk_widget_show_all (window);
  pump ();
  /* Wrapping in Phosh's revealer must NOT fabricate an active indicator. */
  g_assert_false (gtk_widget_get_visible (GTK_WIDGET (self)));
  int client = accept (server, NULL, NULL);
  g_assert_cmpint (client, >=, 0);
  frame (client, 0, 0);
  g_assert_false (gtk_widget_get_visible (GTK_WIDGET (self)));
  frame (client, 1, 0);
  g_assert_true (gtk_widget_get_visible (GTK_WIDGET (self)));
  g_assert_cmpstr (phosh_status_icon_get_info (PHOSH_STATUS_ICON (self)), ==, "Shift held");
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (self->badge)), ==, "");
  frame (client, 2, 0);
  g_assert_cmpstr (phosh_status_icon_get_info (PHOSH_STATUS_ICON (self)), ==, "Shift next key");
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (self->badge)), ==, "1");
  frame (client, 4, 4);
  g_assert_cmpstr (phosh_status_icon_get_info (PHOSH_STATUS_ICON (self)), ==, "Shift locked; Sym locked");
  g_assert_true (gtk_widget_get_visible (self->sym_image));
  g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (self->sym_badge)), ==, "•");
  GtkAllocation area = {0, 0, 600, 32}, a, b;
  gtk_widget_size_allocate (box, &area);
  gtk_widget_get_allocation (gtk_widget_get_ancestor (GTK_WIDGET (self), PHOSH_TYPE_REVEALER), &a);
  gtk_widget_get_allocation (gtk_widget_get_ancestor (battery, PHOSH_TYPE_REVEALER), &b);
  g_assert_cmpint (a.width, >, 0);
  g_assert_cmpint (a.x + a.width + 8, ==, b.x);
  frame (client, 8, 0);
  g_autofree char *icon = phosh_status_icon_get_icon_name (PHOSH_STATUS_ICON (self));
  g_assert_cmpstr (icon, ==, "q25-caps-lock-symbolic");
  g_assert_cmpstr (phosh_status_icon_get_info (PHOSH_STATUS_ICON (self)), ==, "Caps Lock");
  frame (client, 0, 2);
  g_assert_cmpstr (phosh_status_icon_get_info (PHOSH_STATUS_ICON (self)), ==, "Sym next key");
  frame (client, 0, 0);
  g_assert_false (gtk_widget_get_visible (GTK_WIDGET (self)));
  /* Partial frame is not an event; only a complete snapshot updates the UI. */
  g_assert_cmpint (send (client, "Q\1", 2, MSG_NOSIGNAL), ==, 2);
  pump ();
  g_assert_false (gtk_widget_get_visible (GTK_WIDGET (self)));
  g_assert_cmpint (send (client, "\4\4", 2, MSG_NOSIGNAL), ==, 2);
  pump ();
  g_assert_true (gtk_widget_get_visible (GTK_WIDGET (self)));
  close (client);
  pump ();
  g_assert_false (gtk_widget_get_visible (GTK_WIDGET (self)));
  g_assert_cmpstr (phosh_status_icon_get_info (PHOSH_STATUS_ICON (self)), ==, "");
  /* Daemon restart: new connection, no guessed state between snapshots. */
  close (server);
  listen_state ();
  for (int i = 0; i < 100 && self->fd < 0; i++) pump ();
  client = accept (server, NULL, NULL);
  g_assert_cmpint (client, >=, 0);
  g_assert_false (gtk_widget_get_visible (GTK_WIDGET (self)));
  frame (client, 0, 4);
  g_assert_cmpstr (phosh_status_icon_get_info (PHOSH_STATUS_ICON (self)), ==, "Sym locked");
  /* Malformed/out-of-version data is fail-closed. */
  frame (client, 0, 255);
  g_assert_false (gtk_widget_get_visible (GTK_WIDGET (self)));
  g_assert_cmpint (self->fd, ==, -1);
  close (client);
  gtk_widget_destroy (window);
  pump ();
  /* New Phosh instance immediately resyncs an existing lock from its server. */
  self = g_object_new (q25_keyboard_get_type (), NULL);
  g_object_ref_sink (self);
  client = accept (server, NULL, NULL);
  frame (client, 4, 0);
  g_assert_true (gtk_widget_get_visible (GTK_WIDGET (self)));
  gtk_widget_destroy (GTK_WIDGET (self));
  g_assert_cmpint (self->fd, ==, -1);
  g_assert_cmpint (self->retry, ==, 0);
  g_assert_cmpint (self->watch, ==, 0);
  g_object_unref (self);
  close (client);
  /* A spoofed/untrusted server cannot publish status. */
  test_expected_uid = getuid () + 1;
  self = g_object_new (q25_keyboard_get_type (), NULL);
  g_object_ref_sink (self);
  g_assert_cmpint (self->fd, ==, -1);
  g_assert_false (gtk_widget_get_visible (GTK_WIDGET (self)));
  gtk_widget_destroy (GTK_WIDGET (self));
  g_object_unref (self);
  close (server);
  unlink (Q25_STATE_SOCKET);
  pump ();
}

int main(int argc, char **argv)
{
  gtk_test_init (&argc, &argv, NULL);
  g_test_add_func ("/q25/keyboard/status-lifecycle-order-and-credentials", test_widget);
  return g_test_run ();
}
