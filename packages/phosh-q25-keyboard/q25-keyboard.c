/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include <phosh-plugin.h>
#include <status-icon.h>
#include <glib-unix.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <errno.h>

#ifndef Q25_STATE_SOCKET
#define Q25_STATE_SOCKET "/run/keyd-q25-state.sock"
#endif
#ifndef Q25_STATE_UID
#define Q25_STATE_UID 0
#endif

typedef struct {
  PhoshStatusIcon parent;
  GtkWidget *extra, *badge, *sym_image, *sym_badge;
  int fd;
  guint watch, retry;
  guint8 frame[4];
  gsize used;
  gboolean active;
} Q25Keyboard;
typedef PhoshStatusIconClass Q25KeyboardClass;
G_DEFINE_TYPE (Q25Keyboard, q25_keyboard, PHOSH_TYPE_STATUS_ICON)

static gboolean connect_state (gpointer data);

static const char *
badge (guint8 flags)
{
  return flags & (4 | 8) ? "•" : flags & 2 ? "1" : "";
}

static void
append_state (GString *text, const char *name, guint8 flags)
{
  if (!flags)
    return;
  if (text->len)
    g_string_append (text, "; ");
  g_string_append (text, name);
  if (flags & 1) g_string_append (text, " held");
  if (flags & 2) g_string_append (text, " next key");
  if (flags & 4) g_string_append (text, " locked");
}

static void
show_state (Q25Keyboard *self, guint8 shift, guint8 sym)
{
  g_autoptr (GString) text = g_string_new (NULL);
  const char *icon = shift & 8 ? "q25-caps-lock-symbolic" :
    shift ? "q25-shift-symbolic" : "q25-sym-symbolic";
  phosh_status_icon_set_icon_name (PHOSH_STATUS_ICON (self), icon);
  gtk_label_set_text (GTK_LABEL (self->badge), badge (shift ? shift : sym));
  gtk_widget_set_visible (self->badge, *badge (shift ? shift : sym) != '\0');
  gtk_widget_set_visible (self->sym_image, shift && sym);
  gtk_label_set_text (GTK_LABEL (self->sym_badge), badge (sym));
  gtk_widget_set_visible (self->sym_badge, shift && sym && *badge (sym));
  gtk_widget_set_visible (self->extra, (shift && sym) || *badge (shift ? shift : sym));
  append_state (text, "Shift", shift & 7);
  append_state (text, "Sym", sym);
  if (shift & 8) {
    if (text->len) g_string_append (text, "; ");
    g_string_append (text, "Caps Lock");
  }
  phosh_status_icon_set_info (PHOSH_STATUS_ICON (self), text->str);
  gtk_widget_set_tooltip_text (GTK_WIDGET (self), text->str);
  atk_object_set_name (gtk_widget_get_accessible (GTK_WIDGET (self)), text->str);
  self->active = shift || sym;
  gtk_widget_set_visible (GTK_WIDGET (self), self->active);
}

static void
lost_state (Q25Keyboard *self)
{
  if (self->fd >= 0) close (self->fd);
  self->fd = -1;
  self->used = 0;
  self->watch = 0;
  show_state (self, 0, 0);
  if (!self->retry)
    self->retry = g_timeout_add_seconds (1, connect_state, self);
}

static gboolean
read_state (int fd, GIOCondition condition, gpointer data)
{
  Q25Keyboard *self = data;
  /* Discard queued snapshots on disconnect, never redisplay a dead daemon. */
  if (condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL))
    goto lost;
  for (int i = 0; i < 32; i++) {
    ssize_t n = recv (fd, self->frame + self->used, sizeof(self->frame) - self->used, MSG_DONTWAIT);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      return G_SOURCE_CONTINUE;
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      goto lost;
    self->used += n;
    if (self->used != sizeof(self->frame))
      continue;
    if (self->frame[0] != 'Q' || self->frame[1] != 1 ||
        self->frame[2] > 15 || self->frame[3] > 7)
      goto lost;
    show_state (self, self->frame[2], self->frame[3]);
    self->used = 0;
  }
  return G_SOURCE_CONTINUE;
lost:
  lost_state (self);
  return G_SOURCE_REMOVE;
}

static gboolean
connect_state (gpointer data)
{
  Q25Keyboard *self = data;
  struct sockaddr_un addr = {.sun_family = AF_UNIX, .sun_path = Q25_STATE_SOCKET};
  struct ucred peer;
  socklen_t len = sizeof(peer);
  self->fd = socket (AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (self->fd < 0)
    return G_SOURCE_CONTINUE;
  if (connect (self->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
      getsockopt (self->fd, SOL_SOCKET, SO_PEERCRED, &peer, &len) < 0 || peer.uid != Q25_STATE_UID) {
    close (self->fd);
    self->fd = -1;
    return G_SOURCE_CONTINUE;
  }
  self->watch = g_unix_fd_add (self->fd, G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
                              read_state, self);
  self->retry = 0;
  return G_SOURCE_REMOVE;
}

static void
q25_keyboard_destroy (GtkWidget *widget)
{
  Q25Keyboard *self = (Q25Keyboard *)widget;
  g_clear_handle_id (&self->watch, g_source_remove);
  g_clear_handle_id (&self->retry, g_source_remove);
  if (self->fd >= 0) close (self->fd);
  self->fd = -1;
  GTK_WIDGET_CLASS (q25_keyboard_parent_class)->destroy (widget);
}

static void
q25_keyboard_show (GtkWidget *widget)
{
  /* PhoshStatusIconsBox initially shows every plugin via its revealer. Do not
   * allow that (or show_all) to fabricate an active state before a snapshot. */
  if (((Q25Keyboard *)widget)->active)
    GTK_WIDGET_CLASS (q25_keyboard_parent_class)->show (widget);
}

static void
q25_keyboard_class_init (Q25KeyboardClass *klass)
{
  GTK_WIDGET_CLASS (klass)->destroy = q25_keyboard_destroy;
  GTK_WIDGET_CLASS (klass)->show = q25_keyboard_show;
}

static void
q25_keyboard_init (Q25Keyboard *self)
{
  self->fd = -1;
  gtk_icon_theme_add_resource_path (gtk_icon_theme_get_default (),
                                    "/mobi/phosh/plugins/q25-keyboard/icons");
  phosh_status_icon_set_pixel_size (PHOSH_STATUS_ICON (self), 16);
  /* 0.57 sorts equal priorities newest first. Battery is the first built-in
   * priority-10 icon, so our priority-10 plugin sits immediately before it. */
  phosh_status_icon_set_priority (PHOSH_STATUS_ICON (self), 10);
  self->extra = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 2);
  self->badge = gtk_label_new (NULL);
  self->sym_image = gtk_image_new_from_icon_name ("q25-sym-symbolic", GTK_ICON_SIZE_MENU);
  gtk_image_set_pixel_size (GTK_IMAGE (self->sym_image), 16);
  self->sym_badge = gtk_label_new (NULL);
  gtk_container_add (GTK_CONTAINER (self->extra), self->badge);
  gtk_container_add (GTK_CONTAINER (self->extra), self->sym_image);
  gtk_container_add (GTK_CONTAINER (self->extra), self->sym_badge);
  phosh_status_icon_set_extra_widget (PHOSH_STATUS_ICON (self), self->extra);
  show_state (self, 0, 0);
  if (connect_state (self) == G_SOURCE_CONTINUE)
    self->retry = g_timeout_add_seconds (1, connect_state, self);
}

void
g_io_module_load (GIOModule *module)
{
  g_type_module_use (G_TYPE_MODULE (module));
  g_io_extension_point_implement (PHOSH_PLUGIN_EXTENSION_POINT_STATUS_ICON_WIDGET,
                                 q25_keyboard_get_type (), "q25-keyboard", 10);
}

void
g_io_module_unload (GIOModule *module)
{
}

char **
g_io_phosh_plugin_q25_keyboard_query (void)
{
  char *points[] = {PHOSH_PLUGIN_EXTENSION_POINT_STATUS_ICON_WIDGET, NULL};
  return g_strdupv (points);
}
