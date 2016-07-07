/*
 * Virt Viewer: A virtual machine console viewer
 *
 * Copyright (C) 2007-2012 Red Hat, Inc.
 * Copyright (C) 2009-2012 Daniel P. Berrange
 * Copyright (C) 2010 Marc-André Lureau
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Author: Daniel P. Berrange <berrange@redhat.com>
 */

#include <config.h>

#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <locale.h>
#include <glib/gprintf.h>
#include <glib/gi18n.h>
#include <math.h>

#include "virt-viewer-window.h"
#include "virt-viewer-display.h"
#include "virt-viewer-session.h"
#include "virt-viewer-app.h"
#include "virt-viewer-util.h"
#include "virt-viewer-timed-revealer.h"

#define ZOOM_STEP 10

/* Signal handlers for main window (move in a VirtViewerMainWindow?) */
gboolean virt_viewer_window_delete(GtkWidget *src, void *dummy, VirtViewerWindow *self);
void virt_viewer_window_guest_details_response(GtkDialog *dialog, gint response_id, gpointer user_data);
void virt_viewer_window_menu_send(GtkWidget *menu, VirtViewerWindow *self);

/* Internal methods */
static void virt_viewer_window_enable_modifiers(VirtViewerWindow *self);
static void virt_viewer_window_disable_modifiers(VirtViewerWindow *self);
static void virt_viewer_window_queue_resize(VirtViewerWindow *self);
static void virt_viewer_window_fullscreen_headerbar_setup(VirtViewerWindow *self);
static gint virt_viewer_window_get_minimal_zoom_level(VirtViewerWindow *self);

G_DEFINE_TYPE (VirtViewerWindow, virt_viewer_window, G_TYPE_OBJECT)

#define GET_PRIVATE(o)                                                  \
    (G_TYPE_INSTANCE_GET_PRIVATE ((o), VIRT_VIEWER_TYPE_WINDOW, VirtViewerWindowPrivate))

enum {
    PROP_0,
    PROP_WINDOW,
    PROP_DISPLAY,
    PROP_SUBTITLE,
    PROP_APP,
};

struct _VirtViewerWindowPrivate {
    VirtViewerApp *app;

    GtkBuilder *builder;
    GtkWidget *window;
    GtkWidget *header;
    GtkWidget *fullscreen_headerbar;
    GtkWidget *toolbar_usb_device_selection;
    GtkAccelGroup *accel_group;
    VirtViewerNotebook *notebook;
    VirtViewerDisplay *display;
    VirtViewerTimedRevealer *revealer;

    gboolean accel_enabled;
    GValue accel_setting;
    GSList *accel_list;
    gboolean enable_mnemonics_save;
    gboolean grabbed;
    gint fullscreen_monitor;
    gboolean desktop_resize_pending;
    gboolean kiosk;

    gint zoomlevel;
    gboolean fullscreen;
    gchar *subtitle;
    gboolean initial_zoom_set;
};

static void
virt_viewer_window_get_property (GObject *object, guint property_id,
                                 GValue *value, GParamSpec *pspec)
{
    VirtViewerWindow *self = VIRT_VIEWER_WINDOW(object);
    VirtViewerWindowPrivate *priv = self->priv;

    switch (property_id) {
    case PROP_SUBTITLE:
        g_value_set_string(value, priv->subtitle);
        break;

    case PROP_WINDOW:
        g_value_set_object(value, priv->window);
        break;

    case PROP_DISPLAY:
        g_value_set_object(value, virt_viewer_window_get_display(self));
        break;

    case PROP_APP:
        g_value_set_object(value, priv->app);
        break;

    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
virt_viewer_window_set_property (GObject *object, guint property_id,
                                 const GValue *value, GParamSpec *pspec)
{
    VirtViewerWindowPrivate *priv = VIRT_VIEWER_WINDOW(object)->priv;

    switch (property_id) {
    case PROP_SUBTITLE:
        g_free(priv->subtitle);
        priv->subtitle = g_value_dup_string(value);
        virt_viewer_window_update_title(VIRT_VIEWER_WINDOW(object));
        break;

    case PROP_APP:
        g_return_if_fail(priv->app == NULL);
        priv->app = g_value_get_object(value);
        break;

    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
virt_viewer_window_dispose (GObject *object)
{
    VirtViewerWindowPrivate *priv = VIRT_VIEWER_WINDOW(object)->priv;
    GSList *it;

    if (priv->display) {
        g_object_unref(priv->display);
        priv->display = NULL;
    }

    g_debug("Disposing window %p\n", object);

    if (priv->window) {
        gtk_widget_destroy(priv->window);
        priv->window = NULL;
    }
    if (priv->builder) {
        g_object_unref(priv->builder);
        priv->builder = NULL;
    }

    g_clear_object(&priv->revealer);

    for (it = priv->accel_list ; it != NULL ; it = it->next) {
        g_object_unref(G_OBJECT(it->data));
    }
    g_slist_free(priv->accel_list);
    priv->accel_list = NULL;

    g_free(priv->subtitle);
    priv->subtitle = NULL;

    g_value_unset(&priv->accel_setting);
    g_clear_object(&priv->fullscreen_headerbar);

    G_OBJECT_CLASS (virt_viewer_window_parent_class)->dispose (object);
}

static void
virt_viewer_window_class_init (VirtViewerWindowClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    g_type_class_add_private (klass, sizeof (VirtViewerWindowPrivate));

    object_class->get_property = virt_viewer_window_get_property;
    object_class->set_property = virt_viewer_window_set_property;
    object_class->dispose = virt_viewer_window_dispose;

    g_object_class_install_property(object_class,
                                    PROP_SUBTITLE,
                                    g_param_spec_string("subtitle",
                                                        "Subtitle",
                                                        "Window subtitle",
                                                        "",
                                                        G_PARAM_READABLE |
                                                        G_PARAM_WRITABLE |
                                                        G_PARAM_STATIC_STRINGS));

    g_object_class_install_property(object_class,
                                    PROP_WINDOW,
                                    g_param_spec_object("window",
                                                        "Window",
                                                        "GtkWindow",
                                                        GTK_TYPE_WIDGET,
                                                        G_PARAM_READABLE |
                                                        G_PARAM_STATIC_STRINGS));

    g_object_class_install_property(object_class,
                                    PROP_DISPLAY,
                                    g_param_spec_object("display",
                                                        "Display",
                                                        "VirtDisplay",
                                                        VIRT_VIEWER_TYPE_DISPLAY,
                                                        G_PARAM_READABLE |
                                                        G_PARAM_STATIC_STRINGS));

    g_object_class_install_property(object_class,
                                    PROP_APP,
                                    g_param_spec_object("app",
                                                        "App",
                                                        "VirtViewerApp",
                                                        VIRT_VIEWER_TYPE_APP,
                                                        G_PARAM_READABLE |
                                                        G_PARAM_WRITABLE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));
}

static void
usb_device_selection_activated(GSimpleAction *action G_GNUC_UNUSED,
                               GVariant      *parameter G_GNUC_UNUSED,
                               gpointer       window)
{
    VirtViewerWindow *self =  VIRT_VIEWER_WINDOW(window);

    virt_viewer_session_usb_device_selection(virt_viewer_app_get_session(self->priv->app),
                                             GTK_WINDOW(self->priv->window));
}
static void
ctrl_alt_del_activated(GSimpleAction *action G_GNUC_UNUSED,
                       GVariant      *parameter G_GNUC_UNUSED,
                       gpointer       window)
{
    VirtViewerWindow *self =  VIRT_VIEWER_WINDOW(window);

    guint keys[] = { GDK_KEY_Control_L, GDK_KEY_Alt_L, GDK_KEY_Delete };

    guint nkeys = (guint)(sizeof(keys)/sizeof(keys[0]));

    virt_viewer_display_send_keys(VIRT_VIEWER_DISPLAY(self->priv->display),
                                  keys, nkeys);
}

static void
ctrl_alt_backspace_activated(GSimpleAction *action G_GNUC_UNUSED,
                             GVariant      *parameter G_GNUC_UNUSED,
                             gpointer       window)
{
    VirtViewerWindow *self =  VIRT_VIEWER_WINDOW(window);

    guint keys[] = { GDK_KEY_Control_L, GDK_KEY_Alt_L, GDK_KEY_BackSpace };

    guint nkeys = (guint)(sizeof(keys)/sizeof(keys[0]));

    virt_viewer_display_send_keys(VIRT_VIEWER_DISPLAY(self->priv->display),
                                  keys, nkeys);
}

static void
ctrl_alt_fn_activated(GSimpleAction *action G_GNUC_UNUSED,
                      GVariant      *parameter G_GNUC_UNUSED,
                      gpointer       window)
{
    VirtViewerWindow *self =  VIRT_VIEWER_WINDOW(window);

    guint keys[] = { GDK_KEY_Control_L, GDK_KEY_Alt_L, 0 };

    guint nkeys = (guint)(sizeof(keys)/sizeof(keys[0]));

    const char *name = g_action_get_name(G_ACTION(action));

    if (g_str_has_suffix(name, "f1"))
        keys[2] = GDK_KEY_F1;
    else if (g_str_has_suffix(name, "f2"))
        keys[2] = GDK_KEY_F2;
    else if (g_str_has_suffix(name, "f3"))
        keys[2] = GDK_KEY_F3;
    else if (g_str_has_suffix(name, "f4"))
        keys[2] = GDK_KEY_F4;
    else if (g_str_has_suffix(name, "f5"))
        keys[2] = GDK_KEY_F5;
    else if (g_str_has_suffix(name, "f6"))
        keys[2] = GDK_KEY_F6;
    else if (g_str_has_suffix(name, "f7"))
        keys[2] = GDK_KEY_F7;
    else if (g_str_has_suffix(name, "f8"))
        keys[2] = GDK_KEY_F8;
    else if (g_str_has_suffix(name, "f9"))
        keys[2] = GDK_KEY_F9;
    else if (g_str_has_suffix(name, "f10"))
        keys[2] = GDK_KEY_F10;
    else if (g_str_has_suffix(name, "f11"))
        keys[2] = GDK_KEY_F11;
    else if (g_str_has_suffix(name, "f12"))
        keys[2] = GDK_KEY_F12;
    else
        return;

    virt_viewer_display_send_keys(VIRT_VIEWER_DISPLAY(self->priv->display),
                                  keys, nkeys);
}

static void
printscreen_activated(GSimpleAction *action G_GNUC_UNUSED,
                      GVariant      *parameter G_GNUC_UNUSED,
                      gpointer       window)
{
    VirtViewerWindow *self =  VIRT_VIEWER_WINDOW(window);

    guint keys[] = { GDK_KEY_Print };

    guint nkeys = (guint)(sizeof(keys)/sizeof(keys[0]));

    virt_viewer_display_send_keys(VIRT_VIEWER_DISPLAY(self->priv->display),
                                  keys, nkeys);
}

static GActionEntry send_key_entries[] = {
    { "usb-device-selection", usb_device_selection_activated, NULL, NULL, NULL, {0,0,0} },
    { "ctrl+alt+del", ctrl_alt_del_activated, NULL, NULL, NULL, {0,0,0} },
    { "ctrl+alt+backspace", ctrl_alt_backspace_activated, NULL, NULL, NULL, {0,0,0} },
    { "ctrl+alt+f1", ctrl_alt_fn_activated, NULL, NULL, NULL, {0,0,0} },
    { "ctrl+alt+f2", ctrl_alt_fn_activated, NULL, NULL, NULL, {0,0,0} },
    { "ctrl+alt+f3", ctrl_alt_fn_activated, NULL, NULL, NULL, {0,0,0} },
    { "ctrl+alt+f4", ctrl_alt_fn_activated, NULL, NULL, NULL, {0,0,0} },
    { "ctrl+alt+f5", ctrl_alt_fn_activated, NULL, NULL, NULL, {0,0,0} },
    { "ctrl+alt+f6", ctrl_alt_fn_activated, NULL, NULL, NULL, {0,0,0} },
    { "ctrl+alt+f7", ctrl_alt_fn_activated, NULL, NULL, NULL, {0,0,0} },
    { "ctrl+alt+f8", ctrl_alt_fn_activated, NULL, NULL, NULL, {0,0,0} },
    { "ctrl+alt+f9", ctrl_alt_fn_activated, NULL, NULL, NULL, {0,0,0} },
    { "ctrl+alt+f10", ctrl_alt_fn_activated, NULL, NULL, NULL, {0,0,0} },
    { "ctrl+alt+f11", ctrl_alt_fn_activated, NULL, NULL, NULL, {0,0,0} },
    { "ctrl+alt+f12", ctrl_alt_fn_activated, NULL, NULL, NULL, {0,0,0} },
    { "printscreen", printscreen_activated, NULL, NULL, NULL, {0,0,0} },
};

static void
virt_viewer_window_fullscreen_cb(GtkButton *button G_GNUC_UNUSED, VirtViewerWindow *self)
{
    virt_viewer_window_menu_view_fullscreen(self);
}

static void
virt_viewer_window_init (VirtViewerWindow *self)
{
    VirtViewerWindowPrivate *priv;
    GtkWidget *vbox;
    GdkRGBA color;
    GSList *accels;
    GtkWidget *gears;
    GtkWidget *fullscreen;
    GtkWidget *keyboard_shortcut;
    GMenuModel *gears_menu, *keyboard_menu;

    self->priv = GET_PRIVATE(self);
    priv = self->priv;

    priv->fullscreen_monitor = -1;
    g_value_init(&priv->accel_setting, G_TYPE_STRING);

    priv->notebook = virt_viewer_notebook_new();
    gtk_widget_show(GTK_WIDGET(priv->notebook));

    priv->builder = virt_viewer_util_load_ui("virt-viewer.ui");

    gtk_builder_connect_signals(priv->builder, self);

    priv->accel_group = GTK_ACCEL_GROUP(gtk_builder_get_object(priv->builder, "accelgroup"));

    vbox = GTK_WIDGET(gtk_builder_get_object(priv->builder, "viewer-box"));
    virt_viewer_window_fullscreen_headerbar_setup(self);

    gtk_box_pack_end(GTK_BOX(vbox), GTK_WIDGET(priv->notebook), TRUE, TRUE, 0);
    gdk_rgba_parse(&color, "black");
    /* FIXME:
     * This method has been deprecated in 3.16.
     * For more details on how to deal with this in the future, please, see:
     * https://developer.gnome.org/gtk3/stable/GtkWidget.html#gtk-widget-override-background-color
     * For the bug report about this deprecated function, please, see:
     * https://bugs.freedesktop.org/show_bug.cgi?id=94276
     */
    gtk_widget_override_background_color(GTK_WIDGET(priv->notebook), GTK_STATE_FLAG_NORMAL, &color);

    priv->header = GTK_WIDGET(gtk_builder_get_object(priv->builder, "header"));

    gears = GTK_WIDGET(gtk_builder_get_object(priv->builder, "gears"));

    gears_menu = G_MENU_MODEL (gtk_builder_get_object (priv->builder, "gears-menu"));
    gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (gears), gears_menu);

    fullscreen = GTK_WIDGET(gtk_builder_get_object(priv->builder, "fullscreen"));
    g_signal_connect(fullscreen, "clicked", G_CALLBACK(virt_viewer_window_fullscreen_cb), self);

    keyboard_shortcut = GTK_WIDGET(gtk_builder_get_object(priv->builder, "keyboard"));

    keyboard_menu = G_MENU_MODEL (gtk_builder_get_object (priv->builder, "keyboard-menu"));
    gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (keyboard_shortcut), keyboard_menu);

    priv->window = GTK_WIDGET(gtk_builder_get_object(priv->builder, "viewer"));
    gtk_window_add_accel_group(GTK_WINDOW(priv->window), priv->accel_group);

    virt_viewer_window_update_title(self);
    gtk_window_set_resizable(GTK_WINDOW(priv->window), TRUE);
    gtk_window_set_has_resize_grip(GTK_WINDOW(priv->window), FALSE);
    priv->accel_enabled = TRUE;

    accels = gtk_accel_groups_from_object(G_OBJECT(priv->window));
    for ( ; accels ; accels = accels->next) {
        priv->accel_list = g_slist_append(priv->accel_list, accels->data);
        g_object_ref(G_OBJECT(accels->data));
    }

    priv->zoomlevel = NORMAL_ZOOM_LEVEL;

    g_action_map_add_action_entries (G_ACTION_MAP (priv->window), send_key_entries, G_N_ELEMENTS (send_key_entries), self);

    gtk_window_set_titlebar(GTK_WINDOW(priv->window), priv->header);
}

static void
virt_viewer_window_desktop_resize(VirtViewerDisplay *display G_GNUC_UNUSED,
                                  VirtViewerWindow *self)
{
    if (!gtk_widget_get_visible(self->priv->window)) {
        self->priv->desktop_resize_pending = TRUE;
        return;
    }
    virt_viewer_window_queue_resize(self);
}

static gint
virt_viewer_window_get_real_zoom_level(VirtViewerWindow *self)
{
    GtkAllocation allocation;
    guint width, height;

    g_return_val_if_fail(self->priv->display != NULL, NORMAL_ZOOM_LEVEL);

    gtk_widget_get_allocation(GTK_WIDGET(self->priv->display), &allocation);
    virt_viewer_display_get_desktop_size(self->priv->display, &width, &height);

    return round((double) NORMAL_ZOOM_LEVEL * allocation.width / width);
}

/* Kick GtkWindow to tell it to adjust to our new widget sizes */
static void
virt_viewer_window_queue_resize(VirtViewerWindow *self)
{
    VirtViewerWindowPrivate *priv = self->priv;
    GtkRequisition nat;

    gtk_window_set_default_size(GTK_WINDOW(priv->window), -1, -1);
    gtk_widget_get_preferred_size(priv->window, NULL, &nat);
    gtk_window_resize(GTK_WINDOW(priv->window), nat.width, nat.height);
}

static void
virt_viewer_window_move_to_monitor(VirtViewerWindow *self)
{
    VirtViewerWindowPrivate *priv = self->priv;
    GdkRectangle mon;
    gint n = priv->fullscreen_monitor;

    if (n == -1)
        return;

    gdk_screen_get_monitor_geometry(gdk_screen_get_default(), n, &mon);
    gtk_window_move(GTK_WINDOW(priv->window), mon.x, mon.y);

    gtk_widget_set_size_request(priv->window,
                                mon.width,
                                mon.height);
}

static gboolean
mapped(GtkWidget *widget, GdkEvent *event G_GNUC_UNUSED,
       VirtViewerWindow *self)
{
    g_signal_handlers_disconnect_by_func(widget, mapped, self);
    self->priv->fullscreen = FALSE;
    virt_viewer_window_enter_fullscreen(self, self->priv->fullscreen_monitor);
    return FALSE;
}

void
virt_viewer_window_leave_fullscreen(VirtViewerWindow *self)
{
    VirtViewerWindowPrivate *priv = self->priv;

    /* if we enter and leave fullscreen mode before being shown, make sure to
     * disconnect the mapped signal handler */
    g_signal_handlers_disconnect_by_func(priv->window, mapped, self);

    if (!priv->fullscreen)
        return;

    priv->fullscreen = FALSE;
    priv->fullscreen_monitor = -1;
    if (priv->display) {
        virt_viewer_display_set_monitor(priv->display, -1);
        virt_viewer_display_set_fullscreen(priv->display, FALSE);
    }
    virt_viewer_timed_revealer_force_reveal(priv->revealer, FALSE);
    gtk_widget_hide(priv->fullscreen_headerbar);
    gtk_widget_set_size_request(priv->window, -1, -1);
    gtk_window_unfullscreen(GTK_WINDOW(priv->window));

}

void
virt_viewer_window_enter_fullscreen(VirtViewerWindow *self, gint monitor)
{
    VirtViewerWindowPrivate *priv = self->priv;

    if (priv->fullscreen && priv->fullscreen_monitor != monitor)
        virt_viewer_window_leave_fullscreen(self);

    if (priv->fullscreen)
        return;

    priv->fullscreen_monitor = monitor;
    priv->fullscreen = TRUE;

    if (!gtk_widget_get_mapped(priv->window)) {
        /*
         * To avoid some races with metacity, the window should be placed
         * as early as possible, before it is (re)allocated & mapped
         * Position & size should not be queried yet. (rhbz#809546).
         */
        virt_viewer_window_move_to_monitor(self);
        g_signal_connect(priv->window, "map-event", G_CALLBACK(mapped), self);
        return;
    }

    gtk_widget_show(priv->fullscreen_headerbar);
    virt_viewer_timed_revealer_force_reveal(priv->revealer, TRUE);

    if (priv->display) {
        virt_viewer_display_set_monitor(priv->display, monitor);
        virt_viewer_display_set_fullscreen(priv->display, TRUE);
    }
    virt_viewer_window_move_to_monitor(self);

    gtk_window_fullscreen(GTK_WINDOW(priv->window));
}

void
virt_viewer_window_disable_modifiers(VirtViewerWindow *self)
{
    GtkSettings *settings = gtk_settings_get_default();
    VirtViewerWindowPrivate *priv = self->priv;
    GValue empty;
    GSList *accels;

    if (!priv->accel_enabled)
        return;

    /* This stops F10 activating menu bar */
    memset(&empty, 0, sizeof empty);
    g_value_init(&empty, G_TYPE_STRING);
    g_object_get_property(G_OBJECT(settings), "gtk-menu-bar-accel", &priv->accel_setting);
    g_object_set_property(G_OBJECT(settings), "gtk-menu-bar-accel", &empty);

    /* This stops global accelerators like Ctrl+Q == Quit */
    for (accels = priv->accel_list ; accels ; accels = accels->next) {
        if (virt_viewer_app_get_enable_accel(priv->app) &&
            priv->accel_group == accels->data)
            continue;
        gtk_window_remove_accel_group(GTK_WINDOW(priv->window), accels->data);
    }

    /* This stops menu bar shortcuts like Alt+F == File */
    g_object_get(settings,
                 "gtk-enable-mnemonics", &priv->enable_mnemonics_save,
                 NULL);
    g_object_set(settings,
                 "gtk-enable-mnemonics", FALSE,
                 NULL);

    priv->accel_enabled = FALSE;
}

void
virt_viewer_window_enable_modifiers(VirtViewerWindow *self)
{
    GtkSettings *settings = gtk_settings_get_default();
    VirtViewerWindowPrivate *priv = self->priv;
    GSList *accels;

    if (priv->accel_enabled)
        return;

    /* This allows F10 activating menu bar */
    g_object_set_property(G_OBJECT(settings), "gtk-menu-bar-accel", &priv->accel_setting);

    /* This allows global accelerators like Ctrl+Q == Quit */
    for (accels = priv->accel_list ; accels ; accels = accels->next) {
        if (virt_viewer_app_get_enable_accel(priv->app) &&
            priv->accel_group == accels->data)
            continue;
        gtk_window_add_accel_group(GTK_WINDOW(priv->window), accels->data);
    }

    /* This allows menu bar shortcuts like Alt+F == File */
    g_object_set(settings,
                 "gtk-enable-mnemonics", priv->enable_mnemonics_save,
                 NULL);

    priv->accel_enabled = TRUE;
}


G_MODULE_EXPORT gboolean
virt_viewer_window_delete(GtkWidget *src G_GNUC_UNUSED,
                          void *dummy G_GNUC_UNUSED,
                          VirtViewerWindow *self)
{
    g_debug("Window closed");
    virt_viewer_app_maybe_quit(self->priv->app, self);
    return TRUE;
}

static void
virt_viewer_window_set_fullscreen(VirtViewerWindow *self,
                                  gboolean fullscreen)
{
    if (fullscreen) {
        virt_viewer_window_enter_fullscreen(self, -1);
    } else {
        /* leave all windows fullscreen state */
        if (virt_viewer_app_get_fullscreen(self->priv->app))
            g_object_set(self->priv->app, "fullscreen", FALSE, NULL);
        /* or just this window */
        else
            virt_viewer_window_leave_fullscreen(self);
    }
}

static void
virt_viewer_window_headerbar_leave_fullscreen(GtkWidget *button G_GNUC_UNUSED,
                                            VirtViewerWindow *self)
{
    virt_viewer_window_set_fullscreen(self, FALSE);
}

static void add_if_writable (GdkPixbufFormat *data, GHashTable *formats)
{
    if (gdk_pixbuf_format_is_writable(data)) {
        gchar **extensions;
        gchar **it;
        extensions = gdk_pixbuf_format_get_extensions(data);
        for (it = extensions; *it != NULL; it++) {
            g_hash_table_insert(formats, g_strdup(*it), data);
        }
        g_strfreev(extensions);
    }
}

static GHashTable *init_image_formats(void)
{
    GHashTable *format_map;
    GSList *formats = gdk_pixbuf_get_formats();

    format_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_slist_foreach(formats, (GFunc)add_if_writable, format_map);
    g_slist_free (formats);

    return format_map;
}

static GdkPixbufFormat *get_image_format(const char *filename)
{
    static GOnce image_formats_once = G_ONCE_INIT;
    const char *ext;

    g_once(&image_formats_once, (GThreadFunc)init_image_formats, NULL);

    ext = strrchr(filename, '.');
    if (ext == NULL)
        return NULL;

    ext++; /* skip '.' */

    return g_hash_table_lookup(image_formats_once.retval, ext);
}

static void
virt_viewer_window_save_screenshot(VirtViewerWindow *self,
                                   const char *file)
{
    VirtViewerWindowPrivate *priv = self->priv;
    GdkPixbuf *pix = virt_viewer_display_get_pixbuf(VIRT_VIEWER_DISPLAY(priv->display));
    GdkPixbufFormat *format = get_image_format(file);

    if (format == NULL) {
        g_debug("unknown file extension, falling back to png");
        if (!g_str_has_suffix(file, ".png")) {
            char *png_filename;
            png_filename = g_strconcat(file, ".png", NULL);
            gdk_pixbuf_save(pix, png_filename, "png", NULL,
                            "tEXt::Generator App", PACKAGE, NULL);
            g_free(png_filename);
        } else {
            gdk_pixbuf_save(pix, file, "png", NULL,
                            "tEXt::Generator App", PACKAGE, NULL);
        }
    } else {
        char *type = gdk_pixbuf_format_get_name(format);
        g_debug("saving to %s", type);
        gdk_pixbuf_save(pix, file, type, NULL, NULL);
        g_free(type);
    }

    g_object_unref(pix);
}

void
virt_viewer_window_menu_view_fullscreen(VirtViewerWindow *self)
{
    virt_viewer_window_set_fullscreen(self, !self->priv->fullscreen);
}

void
virt_viewer_window_menu_file_screenshot(VirtViewerWindow *self)
{
    GtkWidget *dialog;
    VirtViewerWindowPrivate *priv = self->priv;
    const char *image_dir;

    g_return_if_fail(priv->display != NULL);

    dialog = gtk_file_chooser_dialog_new("Save screenshot",
                                         NULL,
                                         GTK_FILE_CHOOSER_ACTION_SAVE,
                                         _("_Cancel"), GTK_RESPONSE_CANCEL,
                                         _("_Save"), GTK_RESPONSE_ACCEPT,
                                         NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER (dialog), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(dialog),
                                 GTK_WINDOW(self->priv->window));
    image_dir = g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
    if (image_dir != NULL)
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER (dialog), image_dir);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER (dialog), _("Screenshot"));

    if (gtk_dialog_run(GTK_DIALOG (dialog)) == GTK_RESPONSE_ACCEPT) {
        char *filename;

        filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER (dialog));
        virt_viewer_window_save_screenshot(self, filename);
        g_free(filename);
    }

    gtk_widget_destroy(dialog);
}

G_MODULE_EXPORT void
virt_viewer_window_guest_details_response(GtkDialog *dialog,
                                          gint response_id,
                                          gpointer user_data G_GNUC_UNUSED)
{
    if (response_id == GTK_RESPONSE_CLOSE)
        gtk_widget_hide(GTK_WIDGET(dialog));
}

void
virt_viewer_window_menu_help_guest_details(VirtViewerWindow *self)
{
    GtkBuilder *ui = virt_viewer_util_load_ui("virt-viewer-guest-details.ui");
    char *name = NULL;
    char *uuid = NULL;

    g_return_if_fail(ui != NULL);

    GtkWidget *dialog = GTK_WIDGET(gtk_builder_get_object(ui, "guestdetailsdialog"));
    GtkWidget *namelabel = GTK_WIDGET(gtk_builder_get_object(ui, "namevaluelabel"));
    GtkWidget *guidlabel = GTK_WIDGET(gtk_builder_get_object(ui, "guidvaluelabel"));

    g_return_if_fail(dialog && namelabel && guidlabel);

    g_object_get(self->priv->app, "guest-name", &name, "uuid", &uuid, NULL);

    if (!name || *name == '\0')
        name = g_strdup(_("Unknown"));
    if (!uuid || *uuid == '\0')
        uuid = g_strdup(_("Unknown"));
    gtk_label_set_text(GTK_LABEL(namelabel), name);
    gtk_label_set_text(GTK_LABEL(guidlabel), uuid);
    g_free(name);
    g_free(uuid);

    gtk_window_set_transient_for(GTK_WINDOW(dialog),
                                 GTK_WINDOW(self->priv->window));

    gtk_builder_connect_signals(ui, self);

    gtk_widget_show_all(dialog);

    g_object_unref(G_OBJECT(ui));
}

static void
virt_viewer_window_headerbar_usb_device_selection(GtkWidget *menu G_GNUC_UNUSED,
                                                  VirtViewerWindow *self)
{
    virt_viewer_session_usb_device_selection(virt_viewer_app_get_session(self->priv->app),
                                             GTK_WINDOW(self->priv->window));
}

static void
virt_viewer_window_fullscreen_headerbar_setup(VirtViewerWindow *self)
{
    GtkWidget  *overlay;
    GtkWidget  *leave_fullscreen;
    GtkWidget  *fullscreen_keyboard_button;
    GMenuModel *keyboard_menu;
    GtkWidget  *usb_button;

    VirtViewerWindowPrivate *priv = self->priv;

    priv->fullscreen_headerbar = GTK_WIDGET(gtk_builder_get_object(priv->builder,
                                                                   "fullscreen_headerbar"));

    leave_fullscreen = GTK_WIDGET(gtk_builder_get_object(priv->builder,
                                                         "leave_fullscreen_button"));

    fullscreen_keyboard_button = GTK_WIDGET(gtk_builder_get_object(priv->builder,
                                                                   "fullscreen_keyboard_button"));

    keyboard_menu = G_MENU_MODEL (gtk_builder_get_object (priv->builder, "keyboard-menu"));
    gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (fullscreen_keyboard_button), keyboard_menu);

    usb_button = GTK_WIDGET(gtk_builder_get_object(priv->builder, "fullscreen_usb_device"));
    priv->toolbar_usb_device_selection = usb_button;

    g_signal_connect(usb_button, "clicked",
                     G_CALLBACK(virt_viewer_window_headerbar_usb_device_selection), self);
    g_signal_connect(leave_fullscreen, "clicked",
                     G_CALLBACK(virt_viewer_window_headerbar_leave_fullscreen), self);

    priv->revealer = virt_viewer_timed_revealer_new(priv->fullscreen_headerbar);
    overlay = GTK_WIDGET(gtk_builder_get_object(priv->builder, "viewer-overlay"));
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay),
                            virt_viewer_timed_revealer_get_overlay_widget(priv->revealer));
}

VirtViewerNotebook*
virt_viewer_window_get_notebook (VirtViewerWindow *self)
{
    return VIRT_VIEWER_NOTEBOOK(self->priv->notebook);
}

GtkWindow*
virt_viewer_window_get_window (VirtViewerWindow *self)
{
    return GTK_WINDOW(self->priv->window);
}

static void
virt_viewer_window_pointer_grab(VirtViewerDisplay *display G_GNUC_UNUSED,
                                VirtViewerWindow *self)
{
    VirtViewerWindowPrivate *priv = self->priv;

    priv->grabbed = TRUE;
    virt_viewer_window_update_title(self);
}

static void
virt_viewer_window_pointer_ungrab(VirtViewerDisplay *display G_GNUC_UNUSED,
                                  VirtViewerWindow *self)
{
    VirtViewerWindowPrivate *priv = self->priv;

    priv->grabbed = FALSE;
    virt_viewer_window_update_title(self);
}

static void
virt_viewer_window_keyboard_grab(VirtViewerDisplay *display G_GNUC_UNUSED,
                                 VirtViewerWindow *self)
{
    virt_viewer_window_disable_modifiers(self);
}

static void
virt_viewer_window_keyboard_ungrab(VirtViewerDisplay *display G_GNUC_UNUSED,
                                   VirtViewerWindow *self)
{
    virt_viewer_window_enable_modifiers(self);
}

void
virt_viewer_window_update_title(VirtViewerWindow *self)
{
    VirtViewerWindowPrivate *priv = self->priv;
    char *title;
    gchar *ungrab = NULL;

    if (priv->grabbed) {
        gchar *label;
        GtkAccelKey key = {0, 0, 0};

        if (virt_viewer_app_get_enable_accel(priv->app))
            gtk_accel_map_lookup_entry("<virt-viewer>/view/release-cursor", &key);

        if (key.accel_key || key.accel_mods) {
            g_debug("release-cursor accel key: key=%u, mods=%x, flags=%u", key.accel_key, key.accel_mods, key.accel_flags);
            label = gtk_accelerator_get_label(key.accel_key, key.accel_mods);
        } else {
            label = g_strdup(_("Ctrl+Alt"));
        }

        ungrab = g_strdup_printf(_("(Press %s to release pointer)"), label);
        g_free(label);
    }

    if (!ungrab && !priv->subtitle)
        title = g_strdup(g_get_application_name());
    else
        /* translators:
         * This is "<ungrab (or empty)><space (or empty)><subtitle (or empty)> - <appname>"
         * Such as: "(Press Ctrl+Alt to release pointer) BigCorpTycoon MOTD - Virt Viewer"
         */
        title = g_strdup_printf(_("%s%s%s - %s"),
                                /* translators: <ungrab empty> */
                                ungrab ? ungrab : "",
                                /* translators: <space> */
                                ungrab && priv->subtitle ? _(" ") : "",
                                priv->subtitle,
                                g_get_application_name());

    gtk_header_bar_set_title(GTK_HEADER_BAR(priv->header), title);

    g_free(title);
    g_free(ungrab);
}

void
virt_viewer_window_set_headerbar_displays_sensitive(VirtViewerWindow *self, gboolean sensitive)
{
    VirtViewerWindowPrivate *priv;
    GtkWidget *displays;

    g_return_if_fail(VIRT_VIEWER_IS_WINDOW(self));

    priv = self->priv;
    displays = GTK_WIDGET(gtk_builder_get_object(priv->builder, "displays"));
    gtk_widget_set_sensitive(displays, sensitive);
}

void
virt_viewer_window_set_usb_options_sensitive(VirtViewerWindow *self, gboolean sensitive)
{
    VirtViewerWindowPrivate *priv;
    GAction *action;

    g_return_if_fail(VIRT_VIEWER_IS_WINDOW(self));

    priv = self->priv;
    action = g_action_map_lookup_action(G_ACTION_MAP(priv->window), "usb-device-selection");
    g_simple_action_set_enabled(G_SIMPLE_ACTION(action), sensitive);

    gtk_widget_set_visible(priv->toolbar_usb_device_selection, sensitive);
}

void
virt_viewer_window_set_menus_sensitive(VirtViewerWindow *self, gboolean sensitive)
{
    VirtViewerWindowPrivate *priv;
    GAction *action;

    g_return_if_fail(VIRT_VIEWER_IS_WINDOW(self));

    priv = self->priv;

    action = g_action_map_lookup_action(G_ACTION_MAP(priv->window), "screenshot");
    g_simple_action_set_enabled(G_SIMPLE_ACTION(action), sensitive);

    action = g_action_map_lookup_action(G_ACTION_MAP(priv->window), "zoom-in");
    g_simple_action_set_enabled(G_SIMPLE_ACTION(action), sensitive);

    action = g_action_map_lookup_action(G_ACTION_MAP(priv->window), "zoom-out");
    g_simple_action_set_enabled(G_SIMPLE_ACTION(action), sensitive);

    action = g_action_map_lookup_action(G_ACTION_MAP(priv->window), "zoom-reset");
    g_simple_action_set_enabled(G_SIMPLE_ACTION(action), sensitive);

    action = g_action_map_lookup_action(G_ACTION_MAP(priv->window), "guest-details");
    g_simple_action_set_enabled(G_SIMPLE_ACTION(action), sensitive);
}

static void
display_show_hint(VirtViewerDisplay *display,
                  GParamSpec *pspec G_GNUC_UNUSED,
                  VirtViewerWindow *self)
{
    guint hint;

    g_object_get(display, "show-hint", &hint, NULL);

    hint = (hint & VIRT_VIEWER_DISPLAY_SHOW_HINT_READY);

    if (!self->priv->initial_zoom_set && hint && virt_viewer_display_get_enabled(display)) {
        self->priv->initial_zoom_set = TRUE;
        virt_viewer_window_set_zoom_level(self, self->priv->zoomlevel);
    }
}
static gboolean
window_key_pressed (GtkWidget *widget G_GNUC_UNUSED,
                    GdkEvent *event,
                    GtkWidget *display)
{
    gtk_widget_grab_focus(display);
    return gtk_widget_event(display, event);
}

static void
screenshot_activated(GSimpleAction *action G_GNUC_UNUSED,
                     GVariant *parameter G_GNUC_UNUSED,
                     gpointer data)
{
    VirtViewerWindow *self = VIRT_VIEWER_WINDOW(data);;

    virt_viewer_window_menu_file_screenshot(self);
}

static void
fullscreen_activated(GSimpleAction *action G_GNUC_UNUSED,
                     GVariant *parameter G_GNUC_UNUSED,
                     gpointer data)
{
    VirtViewerWindow *self = VIRT_VIEWER_WINDOW(data);;

    virt_viewer_window_menu_view_fullscreen(self);
}

static void
zoom_in_activated(GSimpleAction *action G_GNUC_UNUSED,
                  GVariant *parameter G_GNUC_UNUSED,
                  gpointer data)
{
    VirtViewerWindow *self = VIRT_VIEWER_WINDOW(data);;

    virt_viewer_window_set_zoom_level(self,
                                      virt_viewer_window_get_real_zoom_level(self) + ZOOM_STEP);
}

static void
zoom_out_activated(GSimpleAction *action G_GNUC_UNUSED,
                   GVariant *parameter G_GNUC_UNUSED,
                   gpointer data)
{
    VirtViewerWindow *self = VIRT_VIEWER_WINDOW(data);;

    virt_viewer_window_set_zoom_level(self,
                                      virt_viewer_window_get_real_zoom_level(self) - ZOOM_STEP);
}

static void
zoom_reset_activated(GSimpleAction *action G_GNUC_UNUSED,
                     GVariant *parameter G_GNUC_UNUSED,
                     gpointer data)
{
    VirtViewerWindow *self = VIRT_VIEWER_WINDOW(data);;

    virt_viewer_window_set_zoom_level(self, NORMAL_ZOOM_LEVEL);
}

static void
guest_details_activated(GSimpleAction *action G_GNUC_UNUSED,
                        GVariant *parameter G_GNUC_UNUSED,
                        gpointer data)
{
    VirtViewerWindow *self = VIRT_VIEWER_WINDOW(data);;

    virt_viewer_window_menu_help_guest_details(self);
}

static GActionEntry gear_entries[] = {
    { "screenshot", screenshot_activated, NULL, NULL, NULL, {0,0,0} },
    { "fullscreen", fullscreen_activated, NULL, NULL, NULL, {0,0,0} },
    { "zoom-in", zoom_in_activated, NULL, NULL, NULL, {0,0,0} },
    { "zoom-out", zoom_out_activated, NULL, NULL, NULL, {0,0,0} },
    { "zoom-reset", zoom_reset_activated, NULL, NULL, NULL, {0,0,0} },
    { "guest-details", guest_details_activated, NULL, NULL, NULL, {0,0,0} },
};

void
virt_viewer_window_set_display(VirtViewerWindow *self, VirtViewerDisplay *display)
{
    VirtViewerWindowPrivate *priv;

    g_return_if_fail(VIRT_VIEWER_IS_WINDOW(self));
    g_return_if_fail(display == NULL || VIRT_VIEWER_IS_DISPLAY(display));

    priv = self->priv;
    if (priv->display) {
        gtk_notebook_remove_page(GTK_NOTEBOOK(priv->notebook), 1);
        g_object_unref(priv->display);
        priv->display = NULL;
    }

    if (display != NULL) {
        priv->display = g_object_ref(display);

        virt_viewer_display_set_monitor(VIRT_VIEWER_DISPLAY(priv->display), priv->fullscreen_monitor);
        virt_viewer_display_set_fullscreen(VIRT_VIEWER_DISPLAY(priv->display), priv->fullscreen);

        gtk_widget_show_all(GTK_WIDGET(display));
        gtk_notebook_append_page(GTK_NOTEBOOK(priv->notebook), GTK_WIDGET(display), NULL);
        gtk_widget_realize(GTK_WIDGET(display));

        virt_viewer_signal_connect_object(priv->window, "key-press-event",
                                          G_CALLBACK(window_key_pressed), display, 0);

        /* switch back to non-display if not ready */
        if (!(virt_viewer_display_get_show_hint(display) &
              VIRT_VIEWER_DISPLAY_SHOW_HINT_READY))
            gtk_notebook_set_current_page(GTK_NOTEBOOK(priv->notebook), 0);

        virt_viewer_signal_connect_object(display, "display-pointer-grab",
                                          G_CALLBACK(virt_viewer_window_pointer_grab), self, 0);
        virt_viewer_signal_connect_object(display, "display-pointer-ungrab",
                                          G_CALLBACK(virt_viewer_window_pointer_ungrab), self, 0);
        virt_viewer_signal_connect_object(display, "display-keyboard-grab",
                                          G_CALLBACK(virt_viewer_window_keyboard_grab), self, 0);
        virt_viewer_signal_connect_object(display, "display-keyboard-ungrab",
                                          G_CALLBACK(virt_viewer_window_keyboard_ungrab), self, 0);
        virt_viewer_signal_connect_object(display, "display-desktop-resize",
                                          G_CALLBACK(virt_viewer_window_desktop_resize), self, 0);
        virt_viewer_signal_connect_object(display, "notify::show-hint",
                                          G_CALLBACK(display_show_hint), self, 0);

        display_show_hint(display, NULL, self);

        if (virt_viewer_display_get_enabled(display))
            virt_viewer_window_desktop_resize(display, self);

        g_action_map_add_action_entries (G_ACTION_MAP (priv->window), gear_entries, G_N_ELEMENTS (gear_entries), self);
    }
}

static void
virt_viewer_window_enable_kiosk(VirtViewerWindow *self)
{
    VirtViewerWindowPrivate *priv;

    g_return_if_fail(VIRT_VIEWER_IS_WINDOW(self));
    priv = self->priv;

    virt_viewer_timed_revealer_force_reveal(priv->revealer, FALSE);

    /* You probably also want X11 Option "DontVTSwitch" "true" */
    /* and perhaps more distro/desktop-specific options */
    virt_viewer_window_disable_modifiers(self);
}

void
virt_viewer_window_show(VirtViewerWindow *self)
{
    if (self->priv->display && !virt_viewer_display_get_enabled(self->priv->display))
        virt_viewer_display_enable(self->priv->display);

    if (self->priv->desktop_resize_pending) {
        virt_viewer_window_queue_resize(self);
        self->priv->desktop_resize_pending = FALSE;
    }

    gtk_widget_show(self->priv->window);

    if (self->priv->kiosk)
        virt_viewer_window_enable_kiosk(self);

    if (self->priv->fullscreen)
        virt_viewer_window_move_to_monitor(self);
}

void
virt_viewer_window_hide(VirtViewerWindow *self)
{
    if (self->priv->kiosk) {
        g_warning("Can't hide windows in kiosk mode");
        return;
    }

    gtk_widget_hide(self->priv->window);

    if (self->priv->display) {
        VirtViewerDisplay *display = self->priv->display;
        virt_viewer_display_disable(display);
    }
}

void
virt_viewer_window_set_zoom_level(VirtViewerWindow *self, gint zoom_level)
{
    VirtViewerWindowPrivate *priv;
    gint min_zoom;

    g_return_if_fail(VIRT_VIEWER_IS_WINDOW(self));
    priv = self->priv;

    if (zoom_level < MIN_ZOOM_LEVEL)
        zoom_level = MIN_ZOOM_LEVEL;
    if (zoom_level > MAX_ZOOM_LEVEL)
        zoom_level = MAX_ZOOM_LEVEL;
    priv->zoomlevel = zoom_level;

    if (!priv->display)
        return;

    min_zoom = virt_viewer_window_get_minimal_zoom_level(self);
    if (min_zoom > priv->zoomlevel) {
        g_debug("Cannot set zoom level %d, using %d", priv->zoomlevel, min_zoom);
        priv->zoomlevel = min_zoom;
    }

    if (priv->zoomlevel == virt_viewer_display_get_zoom_level(priv->display) &&
        priv->zoomlevel == virt_viewer_window_get_real_zoom_level(self)) {
        g_debug("Zoom level not changed, using: %d", priv->zoomlevel);
        return;
    }

    virt_viewer_display_set_zoom_level(VIRT_VIEWER_DISPLAY(priv->display), priv->zoomlevel);

    virt_viewer_window_queue_resize(self);
}

gint virt_viewer_window_get_zoom_level(VirtViewerWindow *self)
{
    g_return_val_if_fail(VIRT_VIEWER_IS_WINDOW(self), NORMAL_ZOOM_LEVEL);
    return self->priv->zoomlevel;
}

GtkMenuButton*
virt_viewer_window_get_menu_button_displays(VirtViewerWindow *self)
{
    g_return_val_if_fail(VIRT_VIEWER_IS_WINDOW(self), NULL);

    return GTK_MENU_BUTTON(gtk_builder_get_object(self->priv->builder, "displays"));
}

GtkBuilder*
virt_viewer_window_get_builder(VirtViewerWindow *self)
{
    g_return_val_if_fail(VIRT_VIEWER_IS_WINDOW(self), NULL);

    return self->priv->builder;
}

VirtViewerDisplay*
virt_viewer_window_get_display(VirtViewerWindow *self)
{
    g_return_val_if_fail(VIRT_VIEWER_WINDOW(self), NULL);

    return self->priv->display;
}

void
virt_viewer_window_set_kiosk(VirtViewerWindow *self, gboolean enabled)
{
    g_return_if_fail(VIRT_VIEWER_IS_WINDOW(self));
    g_return_if_fail(enabled == !!enabled);

    if (self->priv->kiosk == enabled)
        return;

    self->priv->kiosk = enabled;

    if (enabled)
        virt_viewer_window_enable_kiosk(self);
    else
        g_debug("disabling kiosk not implemented yet");
}

static void
virt_viewer_window_get_minimal_dimensions(VirtViewerWindow *self G_GNUC_UNUSED,
                                          guint *width,
                                          guint *height)
{
    GtkRequisition req;
    GtkWidget *top_menu;

    top_menu = GTK_WIDGET(gtk_builder_get_object(virt_viewer_window_get_builder(self), "top-menu"));
    gtk_widget_get_preferred_size(top_menu, &req, NULL);
    /* minimal dimensions of the window are the maximum of dimensions of the top-menu
     * and minimal dimension of the display
     */
    *height = MIN_DISPLAY_HEIGHT;
    *width = MAX(MIN_DISPLAY_WIDTH, req.width);
}

/**
 * virt_viewer_window_get_minimal_zoom_level:
 * @self: a #VirtViewerWindow
 *
 * Calculates the zoom level with respect to the desktop dimensions
 *
 * Returns: minimal possible zoom level (multiple of ZOOM_STEP)
 */
static gint
virt_viewer_window_get_minimal_zoom_level(VirtViewerWindow *self)
{
    guint min_width, min_height;
    guint width, height; /* desktop dimensions */
    gint zoom;
    double width_ratio, height_ratio;

    g_return_val_if_fail(VIRT_VIEWER_IS_WINDOW(self) &&
                         self->priv->display != NULL, MIN_ZOOM_LEVEL);

    virt_viewer_window_get_minimal_dimensions(self, &min_width, &min_height);
    virt_viewer_display_get_desktop_size(virt_viewer_window_get_display(self), &width, &height);

    /* e.g. minimal width = 200, desktop width = 550 => width ratio = 0.36
     * which means that the minimal zoom level is 40 (4 * ZOOM_STEP)
     */
    width_ratio = (double) min_width / width;
    height_ratio = (double) min_height / height;
    zoom = ceil(10 * MAX(width_ratio, height_ratio));

    /* make sure that the returned zoom level is in the range from MIN_ZOOM_LEVEL to NORMAL_ZOOM_LEVEL */
    return CLAMP(zoom * ZOOM_STEP, MIN_ZOOM_LEVEL, NORMAL_ZOOM_LEVEL);
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 *  indent-tabs-mode: nil
 * End:
 */
