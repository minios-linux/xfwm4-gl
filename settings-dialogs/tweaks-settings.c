/*
 *  Copyright (c) 2008 Stephan Arts <stephan@xfce.org>
 *  Copyright (c) 2008 Jannis Pohlmann <jannis@xfce.org>
 *  Copyright (c) 2008 Olivier Fourdan <olivier@xfce.org>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either opt_version 2 of the License, or
 *  (at your option) any later opt_version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#include <glib.h>

#if defined (GETTEXT_PACKAGE)
#include <glib/gi18n-lib.h>
#else
#include <glib/gi18n.h>
#endif

#include <gdk/gdk.h>
#include <gdk/gdkx.h>
#include <gtk/gtk.h>
#include <gtk/gtkx.h>

#include <libxfce4ui/libxfce4ui.h>
#include <xfconf/xfconf.h>

#include <common/xfwm-common.h>

#include "xfwm4-tweaks-dialog_ui.h"
#include "range-debouncer.h"
#include "../src/xfwm-tabwin-style.h"
#include "../src/xfwm-tabwin-preview.h"

static Window opt_socket_id = 0;
static gboolean opt_version = FALSE;

static const gchar *const modifier_list[] = {
    N_("None"),
    "Alt",
    "Control",
    "Hyper",
    "Meta",
    "Shift",
    "Super",
    "Mod1",
    "Mod2",
    "Mod3",
    "Mod4",
    "Mod5",
    NULL
};

static void
cb_easy_click_combo_box_changed (GtkComboBox *combo, XfconfChannel *channel)
{
    gint n;

    n = 0;
    while (modifier_list[n])
    {
        if (gtk_combo_box_get_active (combo) == n)
        {
            xfconf_channel_set_string (channel, "/general/easy_click", _(modifier_list[n]));
        }
        n++;
    }
}

static void
cb_use_compositing_check_button_toggled (GtkToggleButton *toggle, GtkWidget *box)
{
    gtk_widget_set_sensitive (box, gtk_toggle_button_get_active (toggle));
}
#if 0
static void
cb_prevent_focus_stealing_check_button_toggled (GtkToggleButton *toggle, GtkWidget *box)
{
    gtk_widget_set_sensitive (box, gtk_toggle_button_get_active (toggle));
}
#endif
static void
cb_activate_action_bring_radio_toggled (GtkToggleButton *toggle, XfconfChannel *channel)
{
    if (gtk_toggle_button_get_active (toggle))
    {
        xfconf_channel_set_string (channel, "/general/activate_action", "bring");
    }
}

static void
cb_activate_action_switch_radio_toggled (GtkToggleButton *toggle, XfconfChannel *channel)
{
    if (gtk_toggle_button_get_active (toggle))
    {
        xfconf_channel_set_string (channel, "/general/activate_action", "switch");
    }
}

static void
cb_activate_action_none_radio_toggled (GtkToggleButton *toggle, XfconfChannel *channel)
{
    if (gtk_toggle_button_get_active (toggle))
    {
        xfconf_channel_set_string (channel, "/general/activate_action", "none");
    }
}

static void
cb_activate_placement_center_radio_toggled (GtkToggleButton *toggle, XfconfChannel *channel)
{
    if (gtk_toggle_button_get_active (toggle))
    {
        xfconf_channel_set_string (channel, "/general/placement_mode", "center");
    }
}

static void
cb_activate_placement_mouse_radio_toggled (GtkToggleButton *toggle, XfconfChannel *channel)
{
    if (gtk_toggle_button_get_active (toggle))
    {
        xfconf_channel_set_string (channel, "/general/placement_mode", "mouse");
    }
}

static void
cb_urgent_blink_button_toggled (GtkToggleButton *toggle, GtkWidget *repeat_urgent_blink)
{
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (repeat_urgent_blink), FALSE);
    gtk_widget_set_sensitive (repeat_urgent_blink, gtk_toggle_button_get_active (toggle));
}

static void
cb_tile_on_move_check_toggled (GtkToggleButton *toggle, XfconfChannel *channel)
{
    if (gtk_toggle_button_get_active (toggle))
    {
        xfconf_channel_set_bool (channel, "/general/wrap_windows", FALSE);
    }
}

static void
cb_borderless_maximize_button_toggled (GtkToggleButton *toggle, GtkWidget *titleless_maximize_check)
{
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (titleless_maximize_check), FALSE);
    gtk_widget_set_sensitive (titleless_maximize_check, gtk_toggle_button_get_active (toggle));
}

#define RENDER_BACKEND_PROP     "_XFWM4_RENDER_BACKEND"
#define VSYNC_PROP              "_XFWM4_VSYNC"

static Atom render_backend_atom = None;
static Atom vsync_atom = None;

static void
intern_backend_atoms (Display *dpy)
{
    if (render_backend_atom == None)
    {
        render_backend_atom = XInternAtom (dpy, RENDER_BACKEND_PROP, False);
        vsync_atom = XInternAtom (dpy, VSYNC_PROP, False);
    }
}

static gchar *
read_root_string (Display *dpy, Window root, Atom property)
{
    Atom actual_type;
    gint actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop = NULL;
    gchar *value = NULL;

    if ((XGetWindowProperty (dpy, root, property,
                             0, 256, False, AnyPropertyType, &actual_type,
                             &actual_format, &nitems, &bytes_after, &prop) == Success)
        && (prop != NULL) && (actual_format == 8) && (nitems > 0))
    {
        value = g_strndup ((const gchar *) prop, nitems);
    }

    if (prop != NULL)
    {
        XFree (prop);
    }

    return value;
}

static void cb_minimize_effect_changed (GtkComboBox *, GtkBuilder *);

static void
minimize_effect_cell (GtkCellLayout *layout, GtkCellRenderer *cell,
                      GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
    GtkTreePath *path = gtk_tree_model_get_path (model, iter);
    gint row = gtk_tree_path_get_indices (path)[0];
    gboolean gl = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (layout), "backend-is-gl"));
    g_object_set (cell, "sensitive", row != 2 || gl, NULL);
    gtk_tree_path_free (path);
}

static void
update_render_backend_label (GtkWidget *label)
{
    Display *dpy;
    Window root;
    gchar *backend;
    gchar *vsync;
    gchar *text = NULL;
    const gchar *renderer = NULL;
    const gchar *sync = NULL;

    dpy = gdk_x11_display_get_xdisplay (gdk_display_get_default ());
    root = GDK_WINDOW_XID (gdk_get_default_root_window ());
    intern_backend_atoms (dpy);

    backend = read_root_string (dpy, root, render_backend_atom);
    vsync = read_root_string (dpy, root, vsync_atom);

    if (backend != NULL)
    {
        if (!strcmp (backend, "opengl egl"))
        {
            renderer = _("OpenGL EGL");
        }
        else if (!strcmp (backend, "opengl glx"))
        {
            renderer = _("OpenGL GLX");
        }
        else
        {
            renderer = _("XRender");
        }
    }

    if (vsync != NULL)
    {
        if (!strcmp (vsync, "on"))
        {
            sync = _("vsync on");
        }
        else if (!strcmp (vsync, "off"))
        {
            sync = _("vsync off");
        }
        else if (!strcmp (vsync, "adaptive"))
        {
            sync = _("adaptive vsync");
        }
    }

    if (renderer != NULL && sync != NULL)
    {
        text = g_strdup_printf (_("(currently: %s, %s)"), renderer, sync);
    }
    else if (renderer != NULL)
    {
        text = g_strdup_printf (_("(currently: %s)"), renderer);
    }
    else
    {
        text = g_strdup (_("(currently off)"));
    }

    {
        GtkWidget *controls = g_object_get_data (G_OBJECT (label), "minimize-animation-controls");
        /* A requested GL backend can fall back to XRender. Use the actual
         * compositor backend, not just the saved use_gl_compositing value. */
        if (controls)
        {
            gtk_widget_set_sensitive (controls, backend &&
                (g_str_has_prefix (backend, "opengl ") || !strcmp (backend, "xrender")));
        }
        controls = g_object_get_data (G_OBJECT (label), "window-animation-controls");
        if (controls)
            gtk_widget_set_sensitive (controls, backend &&
                (g_str_has_prefix (backend, "opengl ") || !strcmp (backend, "xrender")));
        GtkWidget *combo = g_object_get_data (G_OBJECT (label), "minimize-effect-combo");
        if (combo)
        {
            g_object_set_data (G_OBJECT (combo), "backend-is-gl",
                               GINT_TO_POINTER (backend && g_str_has_prefix (backend, "opengl ")));
            cb_minimize_effect_changed (GTK_COMBO_BOX (combo), g_object_get_data (G_OBJECT (combo), "settings-builder"));
            gtk_widget_queue_draw (combo);
        }
    }

    g_free (backend);
    g_free (vsync);

    gtk_label_set_text (GTK_LABEL (label), text);
    g_free (text);
}

static GdkFilterReturn
render_backend_filter (GdkXEvent *xevent, GdkEvent *event, gpointer data)
{
    XEvent *xev = (XEvent *) xevent;

    if (xev->type != PropertyNotify)
    {
        return GDK_FILTER_CONTINUE;
    }

    intern_backend_atoms (xev->xproperty.display);
    if (xev->xproperty.atom == render_backend_atom ||
        xev->xproperty.atom == vsync_atom)
    {
        update_render_backend_label (GTK_WIDGET (data));
    }

    return GDK_FILTER_CONTINUE;
}

static void
cb_cycle_frame_toggled (GtkToggleButton *frame, GtkWidget *fill)
{
    gboolean active = gtk_toggle_button_get_active (frame);
    if (!active)
        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (fill), FALSE);
    gtk_widget_set_sensitive (fill, active);
}

static void
cb_cycle_peek_changed (GtkComboBox *combo, GtkWidget *frame)
{
    /* With GL Peek Off, an Alt+Tab selection still needs its native frame. */
    gboolean may_disable = gtk_combo_box_get_active (combo) > 0;
    if (!may_disable)
        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (frame), TRUE);
    gtk_widget_set_sensitive (frame, may_disable);
}

static void
cb_minimize_effect_changed (GtkComboBox *combo, GtkBuilder *builder)
{
    gint effect = gtk_combo_box_get_active (combo);
    gboolean enabled = effect == 1 ||
      (effect == 2 && GPOINTER_TO_INT (g_object_get_data (G_OBJECT (combo), "backend-is-gl")));
    gtk_widget_set_sensitive (GTK_WIDGET (gtk_builder_get_object (builder, "minimize_duration_scale")), enabled);
    gtk_widget_set_sensitive (GTK_WIDGET (gtk_builder_get_object (builder, "minimize_duration_label")), enabled);
}

static void
cb_window_effect_changed (GtkComboBox *combo, GtkBuilder *builder)
{
    gboolean enabled = gtk_combo_box_get_active (combo) > 0;
    gtk_widget_set_sensitive (GTK_WIDGET (gtk_builder_get_object (builder, "window_duration_scale")), enabled);
    gtk_widget_set_sensitive (GTK_WIDGET (gtk_builder_get_object (builder, "window_duration_label")), enabled);
}

static void
cycle_appearance_spin (GtkGrid *grid, XfconfChannel *channel, const gchar *property,
                       const gchar *text, gint maximum, gint initial, gint row)
{
    GtkWidget *label = gtk_label_new_with_mnemonic (text);
    GtkWidget *spin = gtk_spin_button_new_with_range (0, maximum, 1);
    if (!strcmp (property, "/general/cycle_preview_width"))
        gtk_spin_button_set_range (GTK_SPIN_BUTTON (spin), 120, maximum);
    else if (!strcmp (property, "/general/cycle_preview_height"))
        gtk_spin_button_set_range (GTK_SPIN_BUTTON (spin), 80, maximum);
    gtk_label_set_xalign (GTK_LABEL (label), 0.0);
    gtk_label_set_mnemonic_widget (GTK_LABEL (label), spin);
    gtk_widget_set_hexpand (spin, TRUE);
    gtk_spin_button_set_numeric (GTK_SPIN_BUTTON (spin), TRUE);
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (spin), xfconf_channel_get_int (channel, property, initial));
    gtk_grid_attach (grid, label, 0, row, 1, 1);
    gtk_grid_attach (grid, spin, 1, row, 1, 1);
    xfconf_g_property_bind (channel, property, G_TYPE_INT, G_OBJECT (spin), "value");
}

static void
cycle_appearance_color_set (GtkColorButton *button, XfconfChannel *channel)
{
    GdkRGBA color;
    gchar *text;
    gtk_color_chooser_get_rgba (GTK_COLOR_CHOOSER (button), &color);
    text = g_strdup_printf ("#%02x%02x%02x", (guint) (255 * color.red + 0.5),
                            (guint) (255 * color.green + 0.5), (guint) (255 * color.blue + 0.5));
    const gchar *property = g_object_get_data (G_OBJECT (button), "appearance-color-property");
    xfconf_channel_set_string (channel, property ? property : "/general/cycle_background_color", text);
    g_free (text);
}

static void
cycle_appearance_color_changed (XfconfChannel *channel, const gchar *property,
                                const GValue *value, GtkColorButton *button)
{
    GdkRGBA color;
    const gchar *key = g_object_get_data (G_OBJECT (button), "appearance-color-property");
    if (!strcmp (property, key ? key : "/general/cycle_background_color"))
    {
        const gchar *text = G_VALUE_HOLDS_STRING (value) ? g_value_get_string (value) : XFWM_TABWIN_STYLE_COLOR;
        if (text != NULL && gdk_rgba_parse (&color, text))
            gtk_color_chooser_set_rgba (GTK_COLOR_CHOOSER (button), &color);
    }
}

static void
reset_settings_clicked (GtkButton *button, XfconfChannel *channel)
{
    const gchar *const *properties = g_object_get_data (G_OBJECT (button), "reset-properties");
    XfceRc *defaults = xfce_rc_simple_open (DATADIR "/xfwm4/defaults", TRUE);
    if (defaults == NULL)
    {
        xfce_dialog_show_error (GTK_WINDOW (gtk_widget_get_toplevel (GTK_WIDGET (button))),
                                NULL, "%s", _("Unable to load default settings"));
        return;
    }
    for (guint i = 0; properties[i] != NULL; i++)
    {
        const gchar *value = xfce_rc_read_entry (defaults, properties[i], NULL);
        gchar *property = g_strconcat ("/general/", properties[i], NULL);
        if (value != NULL)
        {
            if (!strcmp (value, "true") || !strcmp (value, "false"))
                xfconf_channel_set_bool (channel, property, !strcmp (value, "true"));
            else
            {
                gchar *end;
                gint64 number = g_ascii_strtoll (value, &end, 10);
                if (end != value && *end == '\0')
                    xfconf_channel_set_int (channel, property, number);
                else
                    xfconf_channel_set_string (channel, property, value);
            }
        }
        g_free (property);
    }
    xfce_rc_close (defaults);
}

static const gchar *const appearance_properties[] = {
    "cycle_background_custom", "cycle_background_color", "cycle_background_opacity",
    "cycle_corner_radius", "cycle_border_width", "cycle_spacing", "cycle_content_padding",
    "cycle_preview_width", "cycle_preview_height", "cycle_selection_padding",
    "cycle_selection_custom", "cycle_selection_color", "cycle_selection_radius",
    "cycle_selection_opacity", NULL
};

static const gchar *const compositor_properties[] = {
    "use_compositing", "use_gl_compositing", "suspend_compositing_fullscreen",
    "unredirect_overlays", "blur_enabled", "blur_docks", "blur_transparent_windows",
    "blur_previews", "blur_alt_tab", "blur_menus", "blur_tooltips", "blur_notifications",
    "blur_radius", "blur_strength", "minimize_effect", "minimize_duration",
    "window_effect", "window_duration", "show_frame_shadow", "show_popup_shadow",
    "show_dock_shadow", "zoom_desktop", "zoom_pointer", "frame_opacity",
    "resize_opacity", "move_opacity", "inactive_opacity", "popup_opacity", NULL
};

static GtkWidget *
settings_reset_button (XfconfChannel *channel, const gchar *const *properties)
{
    GtkWidget *button = gtk_button_new_with_mnemonic (_("_Reset"));
    g_object_set_data (G_OBJECT (button), "reset-properties", (gpointer) properties);
    g_signal_connect (button, "clicked", G_CALLBACK (reset_settings_clicked), channel);
    return button;
}

static void
cycle_appearance_clicked (GtkButton *button, XfconfChannel *channel)
{
    GtkWidget *parent = gtk_widget_get_toplevel (GTK_WIDGET (button));
    GtkWidget *dialog = gtk_dialog_new_with_buttons (_("Window switcher appearance"),
        GTK_IS_WINDOW (parent) ? GTK_WINDOW (parent) : NULL,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, _("_Close"), GTK_RESPONSE_CLOSE, NULL);
    GtkWidget *grid = gtk_grid_new ();
    GtkWidget *reset = settings_reset_button (channel, appearance_properties);
    gtk_box_pack_start (GTK_BOX (gtk_dialog_get_action_area (GTK_DIALOG (dialog))), reset, FALSE, FALSE, 0);
    GtkWidget *custom = gtk_check_button_new_with_mnemonic (_("Use _custom background color"));
    GtkWidget *color_label = gtk_label_new_with_mnemonic (_("Background _color:"));
    GtkWidget *color_button = gtk_color_button_new ();
    GdkRGBA color;
    gchar *text = xfconf_channel_get_string (channel, "/general/cycle_background_color", XFWM_TABWIN_STYLE_COLOR);

    gtk_window_set_resizable (GTK_WINDOW (dialog), FALSE);
    gtk_container_set_border_width (GTK_CONTAINER (grid), 16);
    gtk_grid_set_row_spacing (GTK_GRID (grid), 10);
    gtk_grid_set_column_spacing (GTK_GRID (grid), 14);
    gtk_box_pack_start (GTK_BOX (gtk_dialog_get_content_area (GTK_DIALOG (dialog))), grid, TRUE, TRUE, 0);
    gtk_grid_attach (GTK_GRID (grid), custom, 0, 0, 2, 1);
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (custom), xfconf_channel_get_bool (channel, "/general/cycle_background_custom", FALSE));
    xfconf_g_property_bind (channel, "/general/cycle_background_custom", G_TYPE_BOOLEAN, G_OBJECT (custom), "active");
    gtk_label_set_xalign (GTK_LABEL (color_label), 0.0);
    gtk_label_set_mnemonic_widget (GTK_LABEL (color_label), color_button);
    gtk_color_chooser_set_use_alpha (GTK_COLOR_CHOOSER (color_button), FALSE);
    if (!gdk_rgba_parse (&color, text))
        gdk_rgba_parse (&color, XFWM_TABWIN_STYLE_COLOR);
    gtk_color_chooser_set_rgba (GTK_COLOR_CHOOSER (color_button), &color);
    g_free (text);
    gtk_grid_attach (GTK_GRID (grid), color_label, 0, 1, 1, 1);
    gtk_grid_attach (GTK_GRID (grid), color_button, 1, 1, 1, 1);
    g_object_bind_property (custom, "active", color_button, "sensitive", G_BINDING_SYNC_CREATE);
    g_object_bind_property (custom, "active", color_label, "sensitive", G_BINDING_SYNC_CREATE);
    g_signal_connect (color_button, "color-set", G_CALLBACK (cycle_appearance_color_set), channel);
    g_signal_connect_object (channel, "property-changed", G_CALLBACK (cycle_appearance_color_changed), color_button, 0);
    cycle_appearance_spin (GTK_GRID (grid), channel, "/general/cycle_background_opacity", _("Background _opacity (%):"), 100, XFWM_TABWIN_STYLE_OPACITY, 2);
    cycle_appearance_spin (GTK_GRID (grid), channel, "/general/cycle_corner_radius", _("Corner _radius (px):"), 36, XFWM_TABWIN_STYLE_RADIUS, 3);
    cycle_appearance_spin (GTK_GRID (grid), channel, "/general/cycle_border_width", _("_Border width (px):"), 5, XFWM_TABWIN_STYLE_BORDER, 4);
    cycle_appearance_spin (GTK_GRID (grid), channel, "/general/cycle_spacing", _("Space between _windows (px):"), 20, XFWM_TABWIN_STYLE_SPACING, 5);
    cycle_appearance_spin (GTK_GRID (grid), channel, "/general/cycle_content_padding", _("_Padding around previews (px):"), 64, XFWM_TABWIN_STYLE_PADDING, 6);
    cycle_appearance_spin (GTK_GRID (grid), channel, "/general/cycle_preview_width", _("Maximum _width (px):"), 800, 300, 7);
    cycle_appearance_spin (GTK_GRID (grid), channel, "/general/cycle_preview_height", _("Maximum _height (px):"), 340, 192, 8);
    GtkWidget *selection_label = gtk_label_new (NULL);
    gtk_label_set_markup (GTK_LABEL (selection_label), _("<b>Selection</b>"));
    gtk_widget_set_halign (selection_label, GTK_ALIGN_START);
    gtk_grid_attach (GTK_GRID (grid), selection_label, 0, 9, 2, 1);
    cycle_appearance_spin (GTK_GRID (grid), channel, "/general/cycle_selection_padding", _("Selection _padding (px):"), 32, XFWM_TABWIN_PREVIEW_PADDING, 10);
    GtkWidget *selection_custom = gtk_check_button_new_with_mnemonic (_("Use custom _selection color"));
    GtkWidget *selection_button = gtk_color_button_new ();
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (selection_custom), xfconf_channel_get_bool (channel, "/general/cycle_selection_custom", FALSE));
    xfconf_g_property_bind (channel, "/general/cycle_selection_custom", G_TYPE_BOOLEAN, G_OBJECT (selection_custom), "active");
    gtk_grid_attach (GTK_GRID (grid), selection_custom, 0, 11, 2, 1);
    gtk_grid_attach (GTK_GRID (grid), gtk_label_new (_("Selection color:")), 0, 12, 1, 1);
    gtk_grid_attach (GTK_GRID (grid), selection_button, 1, 12, 1, 1);
    text = xfconf_channel_get_string (channel, "/general/cycle_selection_color", "#398ee7");
    if (!gdk_rgba_parse (&color, text)) gdk_rgba_parse (&color, "#398ee7");
    gtk_color_chooser_set_rgba (GTK_COLOR_CHOOSER (selection_button), &color);
    gtk_color_chooser_set_use_alpha (GTK_COLOR_CHOOSER (selection_button), FALSE);
    g_free (text);
    g_object_set_data (G_OBJECT (selection_button), "appearance-color-property", "/general/cycle_selection_color");
    g_signal_connect (selection_button, "color-set", G_CALLBACK (cycle_appearance_color_set), channel);
    g_signal_connect_object (channel, "property-changed", G_CALLBACK (cycle_appearance_color_changed), selection_button, 0);
    g_object_bind_property (selection_custom, "active", selection_button, "sensitive", G_BINDING_SYNC_CREATE);
    cycle_appearance_spin (GTK_GRID (grid), channel, "/general/cycle_selection_radius", _("Selection corner radius (px):"), 36, XFWM_TABWIN_SELECTION_RADIUS, 13);
    cycle_appearance_spin (GTK_GRID (grid), channel, "/general/cycle_selection_opacity", _("Selection opacity (%):"), 100, XFWM_TABWIN_SELECTION_OPACITY, 14);
    gtk_widget_show_all (dialog);
    gtk_dialog_run (GTK_DIALOG (dialog));
    gtk_widget_destroy (dialog);
}

static void
wm_tweaks_dialog_configure_widgets (GtkBuilder *builder)
{
    GtkWidget *vbox;
    GtkTreeIter iter;
    GtkListStore *list_store;
    GtkCellRenderer *renderer;
    XfconfChannel *xfwm4_channel = xfconf_channel_new ("xfwm4");
    gchar *easy_click = NULL;
    gchar *activate_action = NULL;
    gchar *default_placement = NULL;
    gboolean modifier_set = FALSE;
    guint n;

    /* Cycling tab */
    GtkWidget *cycle_workspaces_check = GTK_WIDGET (gtk_builder_get_object (builder, "cycle_workspaces_check"));
    GtkWidget *cycle_hidden_check = GTK_WIDGET (gtk_builder_get_object (builder, "cycle_hidden_check"));
    GtkWidget *cycle_minimum_check = GTK_WIDGET (gtk_builder_get_object (builder, "cycle_minimum_check"));
    GtkWidget *cycle_minimized_check = GTK_WIDGET (gtk_builder_get_object (builder, "cycle_minimized_check"));
    GtkWidget *cycle_draw_frame = GTK_WIDGET (gtk_builder_get_object (builder, "cycle_draw_frame"));
    GtkWidget *cycle_frame_fill = GTK_WIDGET (gtk_builder_get_object (builder, "cycle_frame_fill"));
    GtkWidget *cycle_raise = GTK_WIDGET (gtk_builder_get_object (builder, "cycle_raise"));
    GtkWidget *cycle_tabwin_mode = GTK_WIDGET (gtk_builder_get_object (builder, "cycle_tabwin_mode"));
    GtkWidget *cycle_peek_combo = GTK_WIDGET (gtk_builder_get_object (builder, "cycle_peek_combo"));
    GtkWidget *cycle_peek_opacity_scale = GTK_WIDGET (gtk_builder_get_object (builder, "cycle_peek_opacity_scale"));

    /* Focus tab */
    GtkWidget *prevent_focus_stealing_check = GTK_WIDGET (gtk_builder_get_object (builder, "prevent_focus_stealing_check"));
    GtkWidget *focus_hint_check = GTK_WIDGET (gtk_builder_get_object (builder, "focus_hint_check"));

    GtkWidget *activate_action_bring_option = GTK_WIDGET (gtk_builder_get_object (builder, "activate_action_bring_option"));
    GtkWidget *activate_action_switch_option = GTK_WIDGET (gtk_builder_get_object (builder, "activate_action_switch_option"));
    GtkWidget *activate_action_none_option = GTK_WIDGET (gtk_builder_get_object (builder, "activate_action_none_option"));

    /* Accessibility tab */
    GtkWidget *easy_click_combo_box = GTK_WIDGET (gtk_builder_get_object (builder, "easy_click_combo_box"));
    GtkWidget *raise_with_any_button_check = GTK_WIDGET (gtk_builder_get_object (builder, "raise_with_any_button_check"));
    GtkWidget *borderless_maximize_check = GTK_WIDGET (gtk_builder_get_object (builder, "borderless_maximize_check"));
    GtkWidget *titleless_maximize_check = GTK_WIDGET (gtk_builder_get_object (builder, "titleless_maximize_check"));
    GtkWidget *tile_on_move_check = GTK_WIDGET (gtk_builder_get_object (builder, "tile_on_move_check"));
    GtkWidget *snap_resist_check = GTK_WIDGET (gtk_builder_get_object (builder, "snap_resist_check"));
    GtkWidget *urgent_blink = GTK_WIDGET (gtk_builder_get_object (builder, "urgent_blink"));
    GtkWidget *repeat_urgent_blink = GTK_WIDGET (gtk_builder_get_object (builder, "repeat_urgent_blink"));
    GtkWidget *mousewheel_rollup = GTK_WIDGET (gtk_builder_get_object (builder, "mousewheel_rollup"));

    /* Workspaces tab */
    GtkWidget *scroll_workspaces_check = GTK_WIDGET (gtk_builder_get_object (builder, "scroll_workspaces_check"));
    GtkWidget *toggle_workspaces_check = GTK_WIDGET (gtk_builder_get_object (builder, "toggle_workspaces_check"));
    GtkWidget *wrap_layout_check = GTK_WIDGET (gtk_builder_get_object (builder, "wrap_layout_check"));
    GtkWidget *wrap_cycle_check = GTK_WIDGET (gtk_builder_get_object (builder, "wrap_cycle_check"));

    /* Placement tab */
    GtkWidget *placement_ratio_scale = GTK_WIDGET (gtk_builder_get_object (builder, "placement_ratio_scale"));
    GtkWidget *placement_center_option = GTK_WIDGET (gtk_builder_get_object (builder, "placement_center_option"));
    GtkWidget *placement_mouse_option = GTK_WIDGET (gtk_builder_get_object (builder, "placement_mouse_option"));

    /* Compositing tab */
    GtkWidget *use_compositing_check = GTK_WIDGET (gtk_builder_get_object (builder, "use_compositing_check"));
    GtkWidget *use_compositing_box = GTK_WIDGET (gtk_builder_get_object (builder, "use_compositing_box"));

    GtkWidget *render_backend_label = GTK_WIDGET (gtk_builder_get_object (builder, "render_backend_label"));
    GtkWidget *suspend_compositing_fullscreen_check = GTK_WIDGET (gtk_builder_get_object (builder, "suspend_compositing_fullscreen_check"));
    GtkWidget *use_gl_compositing_check = GTK_WIDGET (gtk_builder_get_object (builder, "use_gl_compositing_check"));
    GtkWidget *minimize_animation_controls = GTK_WIDGET (gtk_builder_get_object (builder, "minimize_animation_controls"));
    GtkWidget *window_animation_controls = GTK_WIDGET (gtk_builder_get_object (builder, "window_animation_controls"));
    GtkWidget *window_effect_combo = GTK_WIDGET (gtk_builder_get_object (builder, "window_effect_combo"));
    GtkWidget *window_duration_scale = GTK_WIDGET (gtk_builder_get_object (builder, "window_duration_scale"));
    GtkWidget *minimize_effect_combo = GTK_WIDGET (gtk_builder_get_object (builder, "minimize_effect_combo"));
    GtkWidget *minimize_duration_scale = GTK_WIDGET (gtk_builder_get_object (builder, "minimize_duration_scale"));
    GtkWidget *blur_enabled_check = GTK_WIDGET (gtk_builder_get_object (builder, "blur_enabled_check"));
    GtkWidget *blur_docks_check = GTK_WIDGET (gtk_builder_get_object (builder, "blur_docks_check"));
    GtkWidget *blur_transparent_windows_check = GTK_WIDGET (gtk_builder_get_object (builder, "blur_transparent_windows_check"));
    GtkWidget *blur_previews_check = GTK_WIDGET (gtk_builder_get_object (builder, "blur_previews_check"));
    GtkWidget *blur_alt_tab_check = GTK_WIDGET (gtk_builder_get_object (builder, "blur_alt_tab_check"));
    GtkWidget *blur_menus_check = GTK_WIDGET (gtk_builder_get_object (builder, "blur_menus_check"));
    GtkWidget *blur_tooltips_check = GTK_WIDGET (gtk_builder_get_object (builder, "blur_tooltips_check"));
    GtkWidget *blur_notifications_check = GTK_WIDGET (gtk_builder_get_object (builder, "blur_notifications_check"));
    GtkWidget *blur_radius_scale = GTK_WIDGET (gtk_builder_get_object (builder, "blur_radius_scale"));
    GtkWidget *blur_radius_label = GTK_WIDGET (gtk_builder_get_object (builder, "blur_radius_label"));
    GtkWidget *blur_strength_scale = GTK_WIDGET (gtk_builder_get_object (builder, "blur_strength_scale"));
    GtkWidget *blur_strength_label = GTK_WIDGET (gtk_builder_get_object (builder, "blur_strength_label"));
    GtkWidget *unredirect_overlays_check = GTK_WIDGET (gtk_builder_get_object (builder, "unredirect_overlays_check"));
    GtkWidget *cycle_preview_check = GTK_WIDGET (gtk_builder_get_object (builder, "cycle_preview_check"));
    GtkWidget *show_frame_shadow_check = GTK_WIDGET (gtk_builder_get_object (builder, "show_frame_shadow_check"));
    GtkWidget *show_popup_shadow_check = GTK_WIDGET (gtk_builder_get_object (builder, "show_popup_shadow_check"));
    GtkWidget *show_dock_shadow_check = GTK_WIDGET (gtk_builder_get_object (builder, "show_dock_shadow_check"));
    GtkWidget *zoom_desktop_check = GTK_WIDGET (gtk_builder_get_object (builder, "zoom_desktop_check"));
    GtkWidget *zoom_pointer_check = GTK_WIDGET (gtk_builder_get_object (builder, "zoom_pointer_check"));

    GtkWidget *frame_opacity_scale = GTK_WIDGET (gtk_builder_get_object (builder, "frame_opacity_scale"));
    GtkWidget *inactive_opacity_scale = GTK_WIDGET (gtk_builder_get_object (builder, "inactive_opacity_scale"));
    GtkWidget *move_opacity_scale = GTK_WIDGET (gtk_builder_get_object (builder, "move_opacity_scale"));
    GtkWidget *popup_opacity_scale = GTK_WIDGET (gtk_builder_get_object (builder, "popup_opacity_scale"));
    GtkWidget *resize_opacity_scale = GTK_WIDGET (gtk_builder_get_object (builder, "resize_opacity_scale"));

    /* Fill combo-box */
    list_store = gtk_list_store_new (1, G_TYPE_STRING);

    easy_click = xfconf_channel_get_string (xfwm4_channel, "/general/easy_click", "Alt");
    gtk_cell_layout_clear (GTK_CELL_LAYOUT (easy_click_combo_box));
    renderer = gtk_cell_renderer_text_new ();
    gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (easy_click_combo_box), renderer, TRUE);
    gtk_cell_layout_add_attribute (GTK_CELL_LAYOUT (easy_click_combo_box), renderer, "text", 0);
    gtk_combo_box_set_model (GTK_COMBO_BOX (easy_click_combo_box), GTK_TREE_MODEL (list_store));
    for (n = 0; modifier_list[n]; n++)
    {
        gtk_list_store_append (list_store, &iter);
        gtk_list_store_set (list_store, &iter, 0, N_(modifier_list[n]), -1);
        if (!modifier_set && !strcmp (easy_click, modifier_list[n]))
        {
            gtk_combo_box_set_active (GTK_COMBO_BOX (easy_click_combo_box), n);
            modifier_set = TRUE;
        }
    }
    /* If not specified, set to "None" */
    if (!modifier_set)
        gtk_combo_box_set_active (GTK_COMBO_BOX (easy_click_combo_box), 0);

    activate_action = xfconf_channel_get_string (xfwm4_channel, "/general/activate_action", "bring");
    if (!strcmp (activate_action, "switch"))
    {
        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (activate_action_bring_option), FALSE);
        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (activate_action_switch_option), TRUE);
    }
    if (!strcmp (activate_action, "none"))
        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (activate_action_none_option), TRUE);

    default_placement = xfconf_channel_get_string (xfwm4_channel, "/general/placement_mode", "center");
    if (!strcmp (default_placement, "mouse"))
    {
        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (placement_center_option), FALSE);
        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (placement_mouse_option), TRUE);
    }
    g_free (default_placement);

    /* not so easy properties */
    g_signal_connect (G_OBJECT (activate_action_bring_option),
                      "toggled",
                      G_CALLBACK (cb_activate_action_bring_radio_toggled),
                      xfwm4_channel);
    g_signal_connect (G_OBJECT (activate_action_switch_option),
                      "toggled",
                      G_CALLBACK (cb_activate_action_switch_radio_toggled),
                      xfwm4_channel);
    g_signal_connect (G_OBJECT (activate_action_none_option),
                      "toggled",
                      G_CALLBACK (cb_activate_action_none_radio_toggled),
                      xfwm4_channel);
    g_signal_connect (G_OBJECT (borderless_maximize_check),
                      "toggled",
                      G_CALLBACK (cb_borderless_maximize_button_toggled),
                      titleless_maximize_check);
    g_signal_connect (G_OBJECT (placement_center_option),
                      "toggled",
                      G_CALLBACK (cb_activate_placement_center_radio_toggled),
                      xfwm4_channel);
    g_signal_connect (G_OBJECT (placement_mouse_option),
                      "toggled",
                      G_CALLBACK (cb_activate_placement_mouse_radio_toggled),
                      xfwm4_channel);
    g_signal_connect (G_OBJECT (use_compositing_check),
                      "toggled",
                      G_CALLBACK (cb_use_compositing_check_button_toggled),
                      use_compositing_box);
    g_signal_connect (G_OBJECT (easy_click_combo_box),
                      "changed",
                      G_CALLBACK (cb_easy_click_combo_box_changed),
                      xfwm4_channel);
    g_signal_connect (G_OBJECT (urgent_blink),
                      "toggled",
                      G_CALLBACK (cb_urgent_blink_button_toggled),
                      repeat_urgent_blink);
    g_signal_connect (G_OBJECT (tile_on_move_check),
                      "toggled",
                      G_CALLBACK (cb_tile_on_move_check_toggled),
                      xfwm4_channel);

    /* Bind easy properties */
    /* Cycling tab */
    xfconf_g_property_bind (xfwm4_channel,
                            "/general/cycle_minimum",
                            G_TYPE_BOOLEAN,
                            (GObject *)cycle_minimum_check, "active");
    xfconf_g_property_bind (xfwm4_channel,
                            "/general/cycle_minimized",
                            G_TYPE_BOOLEAN,
                            (GObject *)cycle_minimized_check, "active");
    xfconf_g_property_bind (xfwm4_channel,
                            "/general/cycle_hidden",
                            G_TYPE_BOOLEAN,
                            (GObject *)cycle_hidden_check, "active");
    xfconf_g_property_bind (xfwm4_channel,
                            "/general/cycle_workspaces",
                            G_TYPE_BOOLEAN,
                            (GObject *)cycle_workspaces_check, "active");
    xfconf_g_property_bind (xfwm4_channel,
                            "/general/cycle_draw_frame",
                            G_TYPE_BOOLEAN,
                            (GObject *)cycle_draw_frame, "active");
    if (!xfconf_channel_has_property (xfwm4_channel, "/general/cycle_frame_fill"))
        xfconf_channel_set_bool (xfwm4_channel, "/general/cycle_frame_fill", FALSE);
    xfconf_g_property_bind (xfwm4_channel,
                            "/general/cycle_frame_fill",
                            G_TYPE_BOOLEAN,
                            (GObject *)cycle_frame_fill, "active");
    xfconf_g_property_bind (xfwm4_channel,
                            "/general/cycle_raise",
                            G_TYPE_BOOLEAN,
                            (GObject *)cycle_raise, "active");
    xfconf_g_property_bind (xfwm4_channel,
                            "/general/cycle_tabwin_mode",
                            G_TYPE_INT,
                            (GObject *)cycle_tabwin_mode, "active");
    if (!xfconf_channel_has_property (xfwm4_channel, "/general/cycle_peek_mode"))
        xfconf_channel_set_int (xfwm4_channel, "/general/cycle_peek_mode", 1);
    if (!xfconf_channel_has_property (xfwm4_channel, "/general/cycle_peek_opacity"))
        xfconf_channel_set_int (xfwm4_channel, "/general/cycle_peek_opacity", 15);
    xfconf_g_property_bind (xfwm4_channel,
                            "/general/cycle_peek_mode",
                            G_TYPE_INT,
                            (GObject *)cycle_peek_combo, "active");
    xfconf_g_property_bind (xfwm4_channel,
                            "/general/cycle_peek_opacity",
                            G_TYPE_INT,
                            (GObject *)range_debouncer_bind (GTK_RANGE (cycle_peek_opacity_scale)), "value");
    g_object_bind_property (use_compositing_check, "active",
                             cycle_peek_combo, "sensitive", G_BINDING_SYNC_CREATE);
    g_object_bind_property (use_compositing_check, "active",
                             cycle_peek_opacity_scale, "sensitive", G_BINDING_SYNC_CREATE);
    g_signal_connect (cycle_draw_frame, "toggled",
                      G_CALLBACK (cb_cycle_frame_toggled), cycle_frame_fill);
    g_signal_connect (cycle_peek_combo, "changed",
                      G_CALLBACK (cb_cycle_peek_changed), cycle_draw_frame);
    cb_cycle_frame_toggled (GTK_TOGGLE_BUTTON (cycle_draw_frame), cycle_frame_fill);
    cb_cycle_peek_changed (GTK_COMBO_BOX (cycle_peek_combo), cycle_draw_frame);
    g_signal_connect (gtk_builder_get_object (builder, "cycle_appearance_button"), "clicked",
                       G_CALLBACK (cycle_appearance_clicked), xfwm4_channel);

    /* Focus tab */
    xfconf_g_property_bind (xfwm4_channel,
                            "/general/prevent_focus_stealing",
                            G_TYPE_BOOLEAN,
                            (GObject *)prevent_focus_stealing_check, "active");
    xfconf_g_property_bind (xfwm4_channel,
                            "/general/focus_hint",
                            G_TYPE_BOOLEAN,
                            (GObject *)focus_hint_check, "active");

    /* Accessibility tab */
    xfconf_g_property_bind (xfwm4_channel,
                            "/general/raise_with_any_button",
                            G_TYPE_BOOLEAN,
                            (GObject *)raise_with_any_button_check, "active");
    xfconf_g_property_bind (xfwm4_channel,
                            "/general/borderless_maximize",
                            G_TYPE_BOOLEAN,
                            (GObject *)borderless_maximize_check, "active");
    xfconf_g_property_bind (xfwm4_channel,
                            "/general/titleless_maximize",
                            G_TYPE_BOOLEAN,
                            (GObject *)titleless_maximize_check, "active");
    xfconf_g_property_bind (xfwm4_channel,
                            "/general/tile_on_move",
                            G_TYPE_BOOLEAN,
                            (GObject *)tile_on_move_check, "active");
    xfconf_g_property_bind (xfwm4_channel,
                            "/general/snap_resist",
                            G_TYPE_BOOLEAN,
                            (GObject *)snap_resist_check, "active");
    xfconf_g_property_bind (xfwm4_channel,
                            "/general/urgent_blink",
                            G_TYPE_BOOLEAN,
                            (GObject *)urgent_blink, "active");
    xfconf_g_property_bind (xfwm4_channel,
                            "/general/repeat_urgent_blink",
                            G_TYPE_BOOLEAN,
                            (GObject *)repeat_urgent_blink, "active");
    xfconf_g_property_bind (xfwm4_channel,
                            "/general/mousewheel_rollup",
                            G_TYPE_BOOLEAN,
                            (GObject *)mousewheel_rollup, "active");
    gtk_widget_set_sensitive (repeat_urgent_blink,
                              gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (urgent_blink)));
    gtk_widget_set_sensitive (titleless_maximize_check,
                              gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (borderless_maximize_check)));

    /* Workspaces tab */
    xfconf_g_property_bind (xfwm4_channel,
                            "/general/toggle_workspaces",
                            G_TYPE_BOOLEAN,
                            (GObject *)toggle_workspaces_check, "active");
    xfconf_g_property_bind (xfwm4_channel,
                            "/general/scroll_workspaces",
                            G_TYPE_BOOLEAN,
                            (GObject *)scroll_workspaces_check, "active");
    xfconf_g_property_bind (xfwm4_channel,
                            "/general/wrap_layout",
                            G_TYPE_BOOLEAN,
                            (GObject *)wrap_layout_check, "active");
    xfconf_g_property_bind (xfwm4_channel,
                            "/general/wrap_cycle",
                            G_TYPE_BOOLEAN,
                            (GObject *)wrap_cycle_check, "active");

    /* Placement tab */
    xfconf_g_property_bind (xfwm4_channel,
                            "/general/placement_ratio",
                            G_TYPE_INT,
                            (GObject *) gtk_range_get_adjustment (GTK_RANGE (placement_ratio_scale)), "value");

    /* Compositing tab */
    xfconf_g_property_bind (xfwm4_channel,
                            "/general/use_compositing",
                            G_TYPE_BOOLEAN,
                            (GObject *)use_compositing_check, "active");
    xfconf_g_property_bind (xfwm4_channel,
                            "/general/suspend_compositing_fullscreen",
                            G_TYPE_BOOLEAN,
                            (GObject *)suspend_compositing_fullscreen_check, "active");
    xfconf_g_property_bind (xfwm4_channel,
                            "/general/use_gl_compositing",
                            G_TYPE_BOOLEAN,
                            (GObject *)use_gl_compositing_check, "active");
    xfconf_g_property_bind (xfwm4_channel,
                            "/general/blur_enabled",
                            G_TYPE_BOOLEAN,
                            (GObject *)blur_enabled_check, "active");
    xfconf_g_property_bind (xfwm4_channel,
                            "/general/blur_docks",
                            G_TYPE_BOOLEAN,
                            (GObject *)blur_docks_check, "active");
    xfconf_g_property_bind (xfwm4_channel,
                            "/general/blur_transparent_windows",
                            G_TYPE_BOOLEAN,
                            (GObject *)blur_transparent_windows_check, "active");
    /* GTK check buttons default to FALSE if an xfconf key is absent.
     * Preserve the compositor's TRUE default for existing Xfce profiles. */
    if (!xfconf_channel_has_property (xfwm4_channel, "/general/blur_previews"))
        xfconf_channel_set_bool (xfwm4_channel, "/general/blur_previews", TRUE);
    xfconf_g_property_bind (xfwm4_channel,
                            "/general/blur_previews",
                            G_TYPE_BOOLEAN,
                            (GObject *)blur_previews_check, "active");
    if (!xfconf_channel_has_property (xfwm4_channel, "/general/blur_alt_tab"))
        xfconf_channel_set_bool (xfwm4_channel, "/general/blur_alt_tab", TRUE);
    xfconf_g_property_bind (xfwm4_channel,
                            "/general/blur_alt_tab",
                            G_TYPE_BOOLEAN,
                            (GObject *)blur_alt_tab_check, "active");
    xfconf_g_property_bind (xfwm4_channel,
                            "/general/blur_menus",
                            G_TYPE_BOOLEAN,
                            (GObject *)blur_menus_check, "active");
    xfconf_g_property_bind (xfwm4_channel,
                            "/general/blur_tooltips",
                            G_TYPE_BOOLEAN,
                            (GObject *)blur_tooltips_check, "active");
    xfconf_g_property_bind (xfwm4_channel,
                            "/general/blur_notifications",
                            G_TYPE_BOOLEAN,
                            (GObject *)blur_notifications_check, "active");
    xfconf_g_property_bind (xfwm4_channel,
                            "/general/blur_radius",
                            G_TYPE_INT,
                            (GObject *)range_debouncer_bind (GTK_RANGE (blur_radius_scale)), "value");
    xfconf_g_property_bind (xfwm4_channel,
                            "/general/blur_strength",
                            G_TYPE_INT,
                            (GObject *)range_debouncer_bind (GTK_RANGE (blur_strength_scale)), "value");

    g_object_bind_property (blur_enabled_check, "active",
                            blur_docks_check, "sensitive",
                            G_BINDING_SYNC_CREATE);
    g_object_bind_property (blur_enabled_check, "active",
                            blur_transparent_windows_check, "sensitive",
                            G_BINDING_SYNC_CREATE);
    g_object_bind_property (blur_enabled_check, "active",
                            blur_previews_check, "sensitive",
                            G_BINDING_SYNC_CREATE);
    g_object_bind_property (blur_enabled_check, "active",
                            blur_alt_tab_check, "sensitive",
                            G_BINDING_SYNC_CREATE);
    g_object_bind_property (blur_enabled_check, "active",
                            blur_menus_check, "sensitive",
                            G_BINDING_SYNC_CREATE);
    g_object_bind_property (blur_enabled_check, "active",
                            blur_tooltips_check, "sensitive",
                            G_BINDING_SYNC_CREATE);
    g_object_bind_property (blur_enabled_check, "active",
                            blur_notifications_check, "sensitive",
                            G_BINDING_SYNC_CREATE);
    g_object_bind_property (blur_enabled_check, "active",
                            blur_radius_scale, "sensitive",
                            G_BINDING_SYNC_CREATE);
    g_object_bind_property (blur_enabled_check, "active",
                            blur_radius_label, "sensitive",
                            G_BINDING_SYNC_CREATE);
    g_object_bind_property (blur_enabled_check, "active",
                            blur_strength_scale, "sensitive",
                            G_BINDING_SYNC_CREATE);
    g_object_bind_property (blur_enabled_check, "active",
                            blur_strength_label, "sensitive",
                            G_BINDING_SYNC_CREATE);
    xfconf_g_property_bind (xfwm4_channel,
                            "/general/unredirect_overlays",
                            G_TYPE_BOOLEAN,
                            (GObject *)unredirect_overlays_check, "active");

    xfconf_g_property_bind (xfwm4_channel, "/general/minimize_effect", G_TYPE_INT,
                            G_OBJECT (minimize_effect_combo), "active");
    xfconf_g_property_bind (xfwm4_channel, "/general/minimize_duration", G_TYPE_INT,
                            G_OBJECT (range_debouncer_bind (GTK_RANGE (minimize_duration_scale))), "value");
    g_signal_connect (minimize_effect_combo, "changed",
                      G_CALLBACK (cb_minimize_effect_changed), builder);
    cb_minimize_effect_changed (GTK_COMBO_BOX (minimize_effect_combo), builder);
    g_object_set_data (G_OBJECT (minimize_effect_combo), "settings-builder", builder);
    g_object_set_data (G_OBJECT (render_backend_label), "minimize-effect-combo", minimize_effect_combo);
    GList *cells = gtk_cell_layout_get_cells (GTK_CELL_LAYOUT (minimize_effect_combo));
    for (GList *li = cells; li; li = li->next)
        gtk_cell_layout_set_cell_data_func (GTK_CELL_LAYOUT (minimize_effect_combo), li->data,
                                            minimize_effect_cell, NULL, NULL);
    g_list_free (cells);
    g_object_set_data (G_OBJECT (render_backend_label), "minimize-animation-controls",
                       minimize_animation_controls);
    if (!xfconf_channel_has_property (xfwm4_channel, "/general/window_effect"))
        xfconf_channel_set_int (xfwm4_channel, "/general/window_effect", 1);
    if (!xfconf_channel_has_property (xfwm4_channel, "/general/window_duration"))
        xfconf_channel_set_int (xfwm4_channel, "/general/window_duration", 160);
    xfconf_g_property_bind (xfwm4_channel, "/general/window_effect", G_TYPE_INT,
                            G_OBJECT (window_effect_combo), "active");
    xfconf_g_property_bind (xfwm4_channel, "/general/window_duration", G_TYPE_INT,
                            G_OBJECT (range_debouncer_bind (GTK_RANGE (window_duration_scale))), "value");
    g_signal_connect (window_effect_combo, "changed", G_CALLBACK (cb_window_effect_changed), builder);
    cb_window_effect_changed (GTK_COMBO_BOX (window_effect_combo), builder);
    g_object_set_data (G_OBJECT (render_backend_label), "window-animation-controls", window_animation_controls);

    {
        GdkWindow *root_window = gdk_get_default_root_window ();

        gdk_window_set_events (root_window,
                               gdk_window_get_events (root_window) | GDK_PROPERTY_CHANGE_MASK);
        gdk_window_add_filter (root_window, render_backend_filter, render_backend_label);
        update_render_backend_label (render_backend_label);
    }
    xfconf_g_property_bind (xfwm4_channel,
                            "/general/cycle_preview",
                            G_TYPE_BOOLEAN,
                            (GObject *)cycle_preview_check, "active");
    xfconf_g_property_bind (xfwm4_channel,
                            "/general/show_frame_shadow",
                            G_TYPE_BOOLEAN,
                            (GObject *)show_frame_shadow_check, "active");
    xfconf_g_property_bind (xfwm4_channel,
                            "/general/show_popup_shadow",
                            G_TYPE_BOOLEAN,
                            (GObject *)show_popup_shadow_check, "active");
    xfconf_g_property_bind (xfwm4_channel,
                            "/general/show_dock_shadow",
                            G_TYPE_BOOLEAN,
                            (GObject *)show_dock_shadow_check, "active");
    xfconf_g_property_bind (xfwm4_channel,
                            "/general/zoom_desktop",
                            G_TYPE_BOOLEAN,
                            (GObject *)zoom_desktop_check, "active");
    xfconf_g_property_bind (xfwm4_channel,
                            "/general/zoom_pointer",
                            G_TYPE_BOOLEAN,
                            (GObject *)zoom_pointer_check, "active");
    xfconf_g_property_bind (xfwm4_channel,
                            "/general/frame_opacity",
                            G_TYPE_INT,
                            (GObject *) range_debouncer_bind (GTK_RANGE (frame_opacity_scale)), "value");
    xfconf_g_property_bind (xfwm4_channel,
                            "/general/resize_opacity",
                            G_TYPE_INT,
                            (GObject *) range_debouncer_bind (GTK_RANGE (resize_opacity_scale)), "value");
    xfconf_g_property_bind (xfwm4_channel,
                            "/general/move_opacity",
                            G_TYPE_INT,
                            (GObject *) range_debouncer_bind (GTK_RANGE (move_opacity_scale)), "value");
    xfconf_g_property_bind (xfwm4_channel,
                            "/general/inactive_opacity",
                            G_TYPE_INT,
                            (GObject *) range_debouncer_bind (GTK_RANGE (inactive_opacity_scale)), "value");
    xfconf_g_property_bind (xfwm4_channel,
                            "/general/popup_opacity",
                            G_TYPE_INT,
                            (GObject *) range_debouncer_bind (GTK_RANGE (popup_opacity_scale)), "value");

    {
        GtkWidget *reset = settings_reset_button (xfwm4_channel, compositor_properties);
        gtk_widget_set_halign (reset, GTK_ALIGN_END);
        gtk_box_pack_end (GTK_BOX (gtk_builder_get_object (builder, "vbox7")), reset, FALSE, FALSE, 0);
    }
    vbox = GTK_WIDGET (gtk_builder_get_object (builder, "main-vbox"));
    gtk_widget_show_all (vbox);

    g_free (easy_click);
    g_free (activate_action);
}

static void
wm_tweaks_dialog_response (GtkWidget *dialog,
                           gint response_id)
{
    if (response_id == GTK_RESPONSE_HELP)
    {
        xfce_dialog_show_help (GTK_WINDOW (dialog), "xfwm4",
                               "wmtweaks", NULL);
    }
    else
    {
        gtk_main_quit ();
    }
}

static GOptionEntry entries[] =
{
    { "socket-id", 's', G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_INT, &opt_socket_id, N_ ("Session manager socket"), N_ ("SOCKET ID") },
    { "version", 'v', G_OPTION_FLAG_IN_MAIN, G_OPTION_ARG_NONE, &opt_version, N_ ("Version information"), NULL },
    { NULL }
};

int
main (int argc, gchar **argv)
{
    GtkWidget *dialog;
    GtkWidget *plug;
    GtkWidget *plug_child;
    GtkBuilder *builder;
    GError *cli_error = NULL;
    const gchar  *wm_name;

    xfce_textdomain (GETTEXT_PACKAGE, LOCALEDIR, "UTF-8");

    if (!gtk_init_with_args (&argc, &argv, _ ("."), entries, PACKAGE, &cli_error))
    {
        if (cli_error != NULL)
        {
            g_print (_ ("%s: %s\nTry %s --help to see a full list of available command line options.\n"), PACKAGE, cli_error->message, PACKAGE_NAME);
            g_error_free (cli_error);
            return 1;
        }
    }

    wm_name = gdk_x11_screen_get_window_manager_name (gdk_screen_get_default ());
    if (G_UNLIKELY (g_ascii_strcasecmp (wm_name, "Xfwm4")))
    {
        g_print ("These settings cannot work with your current window manager (%s)\n", wm_name);
        return 1;
    }

    if (opt_version)
    {
        g_print ("%s\n", PACKAGE_STRING);
        return 0;
    }

    xfconf_init (NULL);

    builder = gtk_builder_new ();

    if (xfce_titled_dialog_get_type () == 0)
      return 1;

    gtk_builder_add_from_string (builder, tweaks_dialog_ui, tweaks_dialog_ui_length, NULL);

    if (builder)
    {
        wm_tweaks_dialog_configure_widgets (builder);

        if (opt_socket_id == 0)
        {
            dialog = GTK_WIDGET (gtk_builder_get_object (builder, "main-dialog"));
            gtk_widget_show (dialog);
            g_signal_connect (dialog, "response", G_CALLBACK (wm_tweaks_dialog_response), NULL);

            /* To prevent the settings dialog to be saved in the session */
            gdk_x11_set_sm_client_id ("FAKE ID");

            gtk_main ();

            gtk_widget_destroy (dialog);
        }
        else
        {
            /* Create plug widget */
            plug = gtk_plug_new (opt_socket_id);
            g_signal_connect (plug, "delete-event", G_CALLBACK (gtk_main_quit), NULL);
            gtk_widget_show (plug);

            /* Get plug child widget */
            plug_child = GTK_WIDGET (gtk_builder_get_object (builder, "plug-child"));
            xfwm_widget_reparent (plug_child, plug);
            gtk_widget_show (plug_child);

            /* To prevent the settings dialog to be saved in the session */
            gdk_x11_set_sm_client_id ("FAKE ID");

            /* Stop startup notification */
            gdk_notify_startup_complete ();

            /* Enter main loop */
            gtk_main ();
        }

        g_object_unref (builder);
    }

    xfconf_shutdown ();

    return 0;
}
