/*      $Id$

        This program is free software; you can redistribute it and/or modify
        it under the terms of the GNU General Public License as published by
        the Free Software Foundation; either version 2, or (at your option)
        any later version.

        This program is distributed in the hope that it will be useful,
        but WITHOUT ANY WARRANTY; without even the implied warranty of
        MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
        GNU General Public License for more details.

        You should have received a copy of the GNU General Public License
        along with this program; if not, write to the Free Software
        Foundation, Inc., Inc., 51 Franklin Street, Fifth Floor, Boston,
        MA 02110-1301, USA.


        xfwm4    - (c) 2002-2015 Olivier Fourdan

 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifndef WIN_ICON_SIZE
#define WIN_ICON_SIZE 48
#endif

#ifndef WIN_PREVIEW_SIZE
#define WIN_PREVIEW_SIZE (6 * WIN_ICON_SIZE)
#endif

#ifndef LISTVIEW_WIN_ICON_SIZE
#define LISTVIEW_WIN_ICON_SIZE (WIN_ICON_SIZE / 2)
#endif

#ifndef WIN_ICON_BORDER
#define WIN_ICON_BORDER XFWM_TABWIN_PREVIEW_PADDING
#endif

#ifndef WIN_MAX_RATIO
#define WIN_MAX_RATIO 0.80
#endif

#include <math.h>
#include <glib.h>
#include <gdk/gdk.h>
#include <gdk/gdkx.h>
#include <X11/Xatom.h>
#include <gtk/gtk.h>
#include <libxfce4util/libxfce4util.h>

#include <common/xfwm-common.h>

#include "icons.h"
#include "focus.h"
#include "tabwin.h"
#include "settings.h"
#include "compositor.h"
#include "xfwm-tabwin-preview.h"
#include "xfwm-tabwin-style.h"

#define XFWM_TABWIN_NAME XFWM_TABWIN_STYLE_NAME

static void tabwin_widget_class_init (TabwinWidgetClass *klass, gpointer data);

static GType
tabwin_widget_get_type (void)
{
    static GType type = G_TYPE_INVALID;

    if (G_UNLIKELY (type == G_TYPE_INVALID))
    {
        static const GTypeInfo info =
        {
            sizeof (TabwinWidgetClass),
            NULL,
            NULL,
            (GClassInitFunc) tabwin_widget_class_init,
            NULL,
            NULL,
            sizeof (TabwinWidget),
            0,
            NULL,
            NULL,
        };

        type = g_type_register_static (GTK_TYPE_WINDOW, "XfwmTabwinWidget", &info, 0);
    }

    return type;
}

static gboolean
tabwin_draw (GtkWidget *tabwin_widget, cairo_t *cr, gpointer data)
{
    GtkAllocation    allocation;
    GtkStyleContext *ctx;
    GdkScreen       *screen;
    GdkRGBA         *bg_color = NULL;
    GdkRGBA         *border_color = NULL;
    GtkBorder        border = {0, };

    gtk_widget_get_allocation (tabwin_widget, &allocation);
    ctx = gtk_widget_get_style_context (tabwin_widget);
    screen = gtk_widget_get_screen (tabwin_widget);

    if (gdk_screen_is_composited (screen))
    {
        gtk_render_background (ctx, cr, 0, 0, allocation.width, allocation.height);
        gtk_render_frame (ctx, cr, 0, 0, allocation.width, allocation.height);
    }
    else
    {
        gtk_style_context_get (ctx, GTK_STATE_FLAG_NORMAL,
                               GTK_STYLE_PROPERTY_BACKGROUND_COLOR, &bg_color,
                               GTK_STYLE_PROPERTY_BORDER_COLOR, &border_color,
                               NULL);
        gtk_style_context_get_border (ctx, GTK_STATE_FLAG_NORMAL, &border);

        if (border_color != NULL)
        {
            border_color->alpha = 1;
            cairo_rectangle (cr, 0, 0, allocation.width, allocation.height);
            gdk_cairo_set_source_rgba (cr, border_color);
            cairo_fill (cr);
            gdk_rgba_free (border_color);
        }

        if (bg_color != NULL)
        {
            bg_color->alpha = 1;
            cairo_rectangle (cr, border.left, border.top,
                             allocation.width - border.left - border.right,
                             allocation.height - border.top - border.bottom);
            gdk_cairo_set_source_rgba (cr, bg_color);
            cairo_fill (cr);
            gdk_rgba_free (bg_color);
        }
    }

    return FALSE;
}

static void
apply_default_theme (TabwinWidget *tabwin_widget, ScreenInfo *screen_info)
{
    if (!screen_info->tabwin_provider_ready)
    {
        screen_info->tabwin_provider = xfwm_tabwin_default_provider (GTK_WIDGET (tabwin_widget));
        screen_info->tabwin_provider_ready = TRUE;
    }

    if (screen_info->tabwin_provider != NULL)
    {
        gtk_style_context_add_provider (gtk_widget_get_style_context (GTK_WIDGET (tabwin_widget)),
                                        GTK_STYLE_PROVIDER (screen_info->tabwin_provider),
                                        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
}

/* Efficiency is definitely *not* the goal here! */
static gchar *
pretty_string (const gchar *s)
{
    gchar *canonical;

    if (s)
    {
        canonical = g_strdup_printf ("%s",s);
        g_strcanon (canonical, "[]()0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz", ' ');
        g_strstrip (canonical);
    }
    else
    {
        canonical = g_strdup ("...");
    }

    return canonical;
}

static void
tabwinSetLabel (TabwinWidget *tabwin_widget, GtkWidget *buttonlabel, gchar *class, gchar *label, int workspace)
{
    gchar *message;
    PangoLayout *layout;

    g_return_if_fail (tabwin_widget);
    TRACE ("class \"%s\", label \"%s\", workspace %i", class, label, workspace);

    message = pretty_string (class);
    gtk_label_set_text (GTK_LABEL (buttonlabel), message);
    g_free (message);

    if (tabwin_widget->tabwin->display_workspace)
    {
        message = g_strdup_printf ("[%i] - %s", workspace + 1, label);
    }
    else
    {
        message = g_strdup_printf ("%s", label);
    }

    gtk_label_set_text (GTK_LABEL (tabwin_widget->label), message);
    /* Need to update the layout after setting the text */
    layout = gtk_label_get_layout (GTK_LABEL(tabwin_widget->label));
    pango_layout_set_auto_dir (layout, FALSE);
    /* the layout belong to the gtk_label and must not be freed */

    g_free (message);
}

static void
tabwinSetSelected (TabwinWidget *tabwin_widget, GtkWidget *w, GtkWidget *l)
{
    Client *c;
    gchar *classname;

    g_return_if_fail (tabwin_widget);
    g_return_if_fail (GTK_IS_WIDGET(w));

    if (tabwin_widget->selected)
    {
        gtk_widget_unset_state_flags (tabwin_widget->selected, GTK_STATE_FLAG_CHECKED);
    }
    tabwin_widget->selected = w;
    gtk_widget_set_state_flags (w, GTK_STATE_FLAG_CHECKED, FALSE);
    c = g_object_get_data (G_OBJECT (tabwin_widget->selected), "client-ptr-val");

    if (c != NULL)
    {
        TRACE ("client \"%s\" (0x%lx)", c->name, c->window);

        /* We don't update labels here */
        if (c->screen_info->params->cycle_tabwin_mode == OVERFLOW_COLUMN_GRID)
        {
            return;
        }

        classname = g_strdup(c->class.res_class);
        tabwinSetLabel (tabwin_widget, l, classname, c->name, c->win_workspace);
        g_free (classname);
    }
}

static Client *
tabwinSelectWidget (Tabwin *tabwin)
{
    GList *tabwin_list, *widgets, *selected, *children;
    GtkWidget *window_button, *buttonbox, *buttonlabel;
    TabwinWidget *tabwin_widget;
    Client *c;

    g_return_val_if_fail (tabwin != NULL, NULL);
    TRACE ("entering");

    for (tabwin_list = tabwin->tabwin_list; tabwin_list; tabwin_list = g_list_next (tabwin_list))
    {
        tabwin_widget = (TabwinWidget *) tabwin_list->data;
        for (widgets = tabwin_widget->widgets; widgets; widgets = g_list_next (widgets))
        {
            window_button = GTK_WIDGET (widgets->data);
            children = gtk_container_get_children (GTK_CONTAINER (window_button));
            buttonbox = GTK_WIDGET (g_list_nth_data (children, 0));
            g_list_free (children);

            children = gtk_container_get_children (GTK_CONTAINER (buttonbox));
            buttonlabel = GTK_WIDGET (g_list_nth_data (children, 1) );
            g_list_free (children);
            gtk_label_set_text (GTK_LABEL (buttonlabel), "");

            if (gtk_widget_is_focus (window_button))
            {
                c = g_object_get_data (G_OBJECT (window_button), "client-ptr-val");
                selected = g_list_find (*tabwin->client_list, c);

                if (selected)
                {
                    tabwin->selected = selected;
                }

                tabwinSetSelected (tabwin_widget, window_button, buttonlabel);
                gtk_widget_queue_draw (GTK_WIDGET (tabwin_widget));
            }
        }
    }

    return tabwinGetSelected (tabwin);
}

static GtkWidget *
createWindowIcon (GdkScreen *screen, GdkPixbuf *icon_pixbuf, gint size, gint scale)
{
    GtkIconTheme *icon_theme;
    GtkWidget * icon;
    GdkPixbuf *fallback_pixbuf;
    cairo_surface_t *surface;

    TRACE ("entering");

    fallback_pixbuf = NULL;
    if (icon_pixbuf == NULL)
    {
        icon_theme = gtk_icon_theme_get_for_screen (screen);
        fallback_pixbuf = gtk_icon_theme_load_icon (icon_theme, "xfwm4-default",
                                                    size * scale, 0, NULL);
        icon_pixbuf = fallback_pixbuf;
    }

    icon = gtk_image_new ();
    if (icon_pixbuf != NULL)
    {
        surface = gdk_cairo_surface_create_from_pixbuf (icon_pixbuf, scale, NULL);
        if (surface != NULL) {
            gtk_image_set_from_surface (GTK_IMAGE (icon), surface);
            cairo_surface_destroy (surface);
        }
    }
    if (fallback_pixbuf != NULL)
    {
        g_object_unref (fallback_pixbuf);
    }
    return icon;
}

static int
getMinMonitorWidth (ScreenInfo *screen_info)
{
    int i, min_width, num_monitors = myScreenGetNumMonitors (screen_info);
    for (min_width = i = 0; i < num_monitors; i++)
    {
        GdkRectangle monitor;
        xfwm_get_monitor_geometry (screen_info->gscr, i, &monitor, FALSE);
        if (min_width == 0 || monitor.width < min_width)
            min_width = monitor.width;
    }
    return min_width;
}

static int
getMinMonitorHeight (ScreenInfo *screen_info)
{
    int i, min_height, num_monitors = myScreenGetNumMonitors (screen_info);
    for (min_height = i = 0; i < num_monitors; i++)
    {
        GdkRectangle monitor;
        xfwm_get_monitor_geometry (screen_info->gscr, i, &monitor, FALSE);
        if (min_height == 0 || monitor.height < min_height)
        {
            min_height = monitor.height;
        }
    }
    return min_height;
}

static gboolean
cb_window_button_enter (GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
    TabwinWidget *tabwin_widget = user_data;
    Client *c;
    GtkWidget *buttonbox, *buttonlabel;
    GList *children;
    gchar *classname;

    TRACE ("entering");

    g_return_val_if_fail (tabwin_widget != NULL, FALSE);

    /* keep track of which widget we're hovering over */
    tabwin_widget->hovered = widget;

    /* make sure the hovered style is applied */
    if (tabwin_widget->hovered == tabwin_widget->selected)
        gtk_widget_unset_state_flags (tabwin_widget->selected, GTK_STATE_FLAG_CHECKED);

    c = g_object_get_data (G_OBJECT (widget), "client-ptr-val");

    /* when hovering over a window icon, display it's label but don't
     * select it */
    if (c != NULL)
    {
        /* we don't update the labels on mouse over for this mode */
        if (c->screen_info->params->cycle_tabwin_mode == OVERFLOW_COLUMN_GRID)
        {
            return FALSE;
        }

        children = gtk_container_get_children (GTK_CONTAINER (widget));
        buttonbox = GTK_WIDGET (g_list_nth_data (children, 0));
        g_list_free (children);

        children = gtk_container_get_children (GTK_CONTAINER (buttonbox));
        buttonlabel = GTK_WIDGET (g_list_nth_data (children, 1));
        g_list_free (children);

        classname = g_strdup (c->class.res_class);
        tabwinSetLabel (tabwin_widget, buttonlabel, classname, c->name, c->win_workspace);
        g_free (classname);
    }

    return FALSE;
}

static gboolean
cb_window_button_leave (GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
    TabwinWidget *tabwin_widget = user_data;
    Client *c;

    TRACE ("entering");

    g_return_val_if_fail (tabwin_widget != NULL, FALSE);

    tabwin_widget->hovered = NULL;

    /* don't do anything if we have the focus */
    if (gtk_widget_is_focus (widget))
    {
        gtk_widget_set_state_flags (tabwin_widget->selected, GTK_STATE_FLAG_CHECKED, FALSE);
        return FALSE;
    }

    c = g_object_get_data (G_OBJECT (widget), "client-ptr-val");

    /* when hovering over a window icon, display it's label but don't
     * select it */
    if (c != NULL)
    {
        /* we don't update the labels for this mode */
        if (c->screen_info->params->cycle_tabwin_mode == OVERFLOW_COLUMN_GRID)
        {
            return FALSE;
        }

        /* reselect the selected widget, it will clear everything else out */
        tabwinSelectWidget (tabwin_widget->tabwin);
    }

    return FALSE;
}

static GtkWidget *
createWindowlist (ScreenInfo *screen_info, TabwinWidget *tabwin_widget)
{
    Client *c;
    GList *client_list;
    GList *icon_list;
    GtkWidget *windowlist;
    GtkWidget *icon;
    GtkWidget *selected;
    GtkWidget *window_button;
    GtkWidget *buttonbox;
    GtkWidget *buttonlabel;
    GtkWidget *selected_label;
    GdkPixbuf *icon_pixbuf;
    gint packpos;
    gint label_width;
    gint size_request;
    gint spacing;
    Tabwin *tabwin;

    TRACE ("entering");
    g_return_val_if_fail (tabwin_widget != NULL, NULL);
    tabwin = tabwin_widget->tabwin;
    g_return_val_if_fail (tabwin->client_count > 0, NULL);

    packpos = 0;
    c = NULL;
    selected = NULL;
    tabwin_widget->widgets = NULL;
    size_request = tabwin->preview_height + tabwin->label_height + 2 * tabwin->selection_padding;

    windowlist = gtk_grid_new ();
    gtk_grid_set_row_homogeneous (GTK_GRID (windowlist), TRUE);
    spacing = CLAMP (xfconf_channel_get_int (screen_info->xfwm4_channel,
                         "/general/cycle_spacing", XFWM_TABWIN_STYLE_SPACING), 0, 20);
    gtk_grid_set_row_spacing (GTK_GRID (windowlist), spacing);
    gtk_grid_set_column_homogeneous (GTK_GRID (windowlist), TRUE);
    gtk_grid_set_column_spacing (GTK_GRID (windowlist), spacing);

    /* pack the client icons */
    icon_list = tabwin->icon_list;
    for (client_list = *tabwin->client_list; client_list; client_list = g_list_next (client_list))
    {
        c = (Client *) client_list->data;
        TRACE ("adding \"%s\" (0x%lx)", c->name, c->window);
        icon_pixbuf = (GdkPixbuf *) icon_list->data;
        icon_list = g_list_next (icon_list);

        window_button = gtk_button_new ();
        gtk_button_set_relief (GTK_BUTTON (window_button), GTK_RELIEF_NONE);
        gchar *selection_color = xfconf_channel_get_string (screen_info->xfwm4_channel, "/general/cycle_selection_color", "#398ee7");
        XfwmTabwinSelectionStyle selection = {
            xfconf_channel_get_bool (screen_info->xfwm4_channel, "/general/cycle_selection_custom", FALSE), selection_color,
            xfconf_channel_get_int (screen_info->xfwm4_channel, "/general/cycle_selection_radius", XFWM_TABWIN_SELECTION_RADIUS),
            xfconf_channel_get_int (screen_info->xfwm4_channel, "/general/cycle_selection_opacity", XFWM_TABWIN_SELECTION_OPACITY)
        };
        GtkCssProvider *selection_provider = gtk_css_provider_new ();
        GtkWidgetPath *selection_path = gtk_widget_path_copy (gtk_widget_get_path (GTK_WIDGET (tabwin_widget)));
        gtk_widget_path_append_type (selection_path, GTK_TYPE_BUTTON);
        gtk_widget_path_iter_set_object_name (selection_path, -1, "button");
        gtk_widget_path_iter_add_class (selection_path, -1, GTK_STYLE_CLASS_FLAT);
        GtkStyleContext *selection_context = gtk_style_context_new ();
        gtk_style_context_set_path (selection_context, selection_path);
        gtk_style_context_set_parent (selection_context, gtk_widget_get_style_context (GTK_WIDGET (tabwin_widget)));
        gtk_style_context_set_screen (selection_context, screen_info->gscr);
        gtk_style_context_set_state (selection_context, GTK_STATE_FLAG_CHECKED);
        gchar *selection_css = xfwm_tabwin_selection_css (selection_context, &selection);
        gtk_widget_path_unref (selection_path);
        g_object_unref (selection_context);
        gtk_css_provider_load_from_data (selection_provider, selection_css, -1, NULL);
        gtk_style_context_add_provider (gtk_widget_get_style_context (window_button), GTK_STYLE_PROVIDER (selection_provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 2);
        g_object_unref (selection_provider);
        g_free (selection_css);
        g_free (selection_color);
        g_object_set_data (G_OBJECT (window_button), "client-ptr-val", c);
        g_signal_connect (window_button, "enter-notify-event",
                          G_CALLBACK (cb_window_button_enter), tabwin_widget);
        g_signal_connect (window_button, "leave-notify-event",
                          G_CALLBACK (cb_window_button_leave), tabwin_widget);
        gtk_widget_add_events (window_button, GDK_ENTER_NOTIFY_MASK);

        icon = createWindowIcon (screen_info->gscr, icon_pixbuf, tabwin->icon_size, tabwin->icon_scale);
        if (screen_info->params->cycle_tabwin_mode == STANDARD_ICON_GRID)
        {
            gtk_widget_set_size_request (GTK_WIDGET (window_button),
                                         tabwin->preview_width + 2 * tabwin->selection_padding, size_request);
            buttonbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
            buttonlabel = gtk_label_new ("");
            gtk_label_set_xalign (GTK_LABEL (buttonlabel), 0.5);
            gtk_label_set_yalign (GTK_LABEL (buttonlabel), 1.0);

            gtk_widget_set_halign (icon, GTK_ALIGN_CENTER);
            gtk_widget_set_valign (icon, GTK_ALIGN_END);
            gtk_box_pack_start (GTK_BOX (buttonbox), icon, TRUE, TRUE, 0);
        }
        else
        {
            label_width = tabwin->monitor_width / (tabwin->grid_cols + 1);

            if (tabwin->icon_size < tabwin->label_height)
            {
                gtk_widget_set_size_request (GTK_WIDGET (window_button),
                                             label_width, tabwin->label_height + 8);
            }
            else
            {
                gtk_widget_set_size_request (GTK_WIDGET (window_button),
                                             label_width, tabwin->icon_size + 8);
            }
            buttonbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
            buttonlabel = gtk_label_new (c->name);
            gtk_label_set_xalign (GTK_LABEL (buttonlabel), 0);
            gtk_label_set_yalign (GTK_LABEL (buttonlabel), 0.5);

            gtk_widget_set_halign (icon, GTK_ALIGN_CENTER);
            gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
            gtk_box_pack_start (GTK_BOX (buttonbox), icon, FALSE, FALSE, 0);
        }
        /* Keep the inset inside the painted button. GtkButton's container
         * border is outside its CSS node and does not pad the selection. */
        gtk_container_set_border_width (GTK_CONTAINER (buttonbox), tabwin->selection_padding);
        gtk_container_add (GTK_CONTAINER (window_button), buttonbox);

        gtk_label_set_justify (GTK_LABEL (buttonlabel), GTK_JUSTIFY_CENTER);
        gtk_label_set_ellipsize (GTK_LABEL (buttonlabel), PANGO_ELLIPSIZE_END);
        gtk_box_pack_start (GTK_BOX (buttonbox), buttonlabel, TRUE, TRUE, 0);

        if (screen_info->params->cycle_tabwin_mode == STANDARD_ICON_GRID)
        {
            gtk_grid_attach (GTK_GRID (windowlist), GTK_WIDGET (window_button),
                             packpos % tabwin->grid_cols, packpos / tabwin->grid_cols,
                             1, 1);
        }
        else
        {
            gtk_grid_attach (GTK_GRID (windowlist), GTK_WIDGET (window_button),
                             packpos / tabwin->grid_rows, packpos % tabwin->grid_rows,
                             1, 1);
        }

        tabwin_widget->widgets = g_list_append (tabwin_widget->widgets, window_button);
        packpos++;
        if (c == tabwin->selected->data)
        {
            selected = window_button;
            selected_label = buttonlabel;
            gtk_widget_grab_focus (selected);
        }
    }
    if (selected)
    {
        tabwinSetSelected (tabwin_widget, selected, selected_label);
    }
    return windowlist;
}

/* Identified as a separate blur category from GTK menus and tooltips.
 * Only the Alt+Tab card surface blurs; CSS shadow margins remain untouched. */
static void
tabwin_publish_blur_area (TabwinWidget *widget, gint width, gint height)
{
    ScreenInfo *screen_info;
    GdkWindow *window;
    Display *dpy;
    Window xid;
    Atom marker, rect_atom;
    unsigned long enabled = 1, rect[4];

    if (widget->tabwin == NULL || widget->tabwin->selected == NULL)
        return;
    screen_info = ((Client *) widget->tabwin->selected->data)->screen_info;
    if (!screen_info->params->blur_alt_tab)
        return;
    window = gtk_widget_get_window (GTK_WIDGET (widget));
    if (!window || width <= 0 || height <= 0)
        return;
    dpy = myScreenGetXDisplay (screen_info);
    xid = GDK_WINDOW_XID (window);
    marker = XInternAtom (dpy, "_XFWM4_TABWIN_BLUR", False);
    rect_atom = XInternAtom (dpy, "_XFWM4_BLUR_CONTENT_RECT", False);
    /* The container border is layout padding INSIDE the painted background.
     * Native compositor shadows are already outside this drawable. */
    rect[0] = rect[1] = 0;
    rect[2] = width * gtk_widget_get_scale_factor (GTK_WIDGET (widget));
    rect[3] = height * gtk_widget_get_scale_factor (GTK_WIDGET (widget));
    XChangeProperty (dpy, xid, marker, XA_CARDINAL, 32, PropModeReplace,
                     (unsigned char *) &enabled, 1);
    XChangeProperty (dpy, xid, rect_atom, XA_CARDINAL, 32, PropModeReplace,
                     (unsigned char *) rect, 4);
    XFlush (dpy);
}

static gboolean
tabwinConfigure (TabwinWidget *tabwin_widget, GdkEventConfigure *event)
{
    GdkRectangle monitor;
    gint x, y;

    g_return_val_if_fail (tabwin_widget != NULL, FALSE);
    TRACE ("entering");

    if ((tabwin_widget->width == event->width) && (tabwin_widget->height == event->height))
    {
        return FALSE;
    }

    xfwm_get_monitor_geometry (gtk_widget_get_screen (GTK_WIDGET (tabwin_widget)),
                               tabwin_widget->monitor_num, &monitor, FALSE);
    x = monitor.x + (monitor.width - event->width) / 2;
    y = monitor.y + (monitor.height - event->height) / 2;
    gtk_window_move (GTK_WINDOW (tabwin_widget), x, y);

    tabwin_widget->width = event->width;
    tabwin_widget->height = event->height;
    tabwin_publish_blur_area (tabwin_widget, event->width, event->height);

    return FALSE;
}

static void
tabwin_widget_class_init (TabwinWidgetClass *klass, gpointer data)
{
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

    gtk_widget_class_install_style_property (widget_class,
                                             g_param_spec_int ("preview-size",
                                                               "preview size",
                                                                "the size of the application preview",
                                                                48, 512,
                                                                WIN_PREVIEW_SIZE,
                                                                G_PARAM_READABLE));
    gtk_widget_class_install_style_property (widget_class,
                                             g_param_spec_int ("icon-size",
                                                               "icon size",
                                                                "the size of the application icon",
                                                                24, 128,
                                                                WIN_ICON_SIZE,
                                                                G_PARAM_READABLE));
    gtk_widget_class_install_style_property (widget_class,
                                             g_param_spec_int ("listview-icon-size",
                                                               "listview icon size",
                                                               "the size of the application icon in listview",
                                                               16, 48,
                                                               LISTVIEW_WIN_ICON_SIZE,
                                                               G_PARAM_READABLE));
}

static void
computeTabwinData (ScreenInfo *screen_info, TabwinWidget *tabwin_widget)
{
    Tabwin *tabwin;
    GdkPixbuf *icon_pixbuf;
    PangoLayout *layout;
    GList *client_list;
    gint size_request;
    gint standard_icon_size;
    gint spacing, outer_padding, available_width, available_height;
    gboolean preview;

    TRACE ("entering");
    g_return_if_fail (GTK_IS_WIDGET(tabwin_widget));
    tabwin = tabwin_widget->tabwin;
    g_return_if_fail (tabwin->client_count > 0);

    tabwin->monitor_width = getMinMonitorWidth (screen_info);
    tabwin->monitor_height = getMinMonitorHeight (screen_info);
    tabwin->label_height = 30;
    preview = screen_info->params->cycle_preview && compositorIsActive (screen_info);
    tabwin->icon_scale = gtk_widget_get_scale_factor (GTK_WIDGET (tabwin_widget));
    tabwin->selection_padding = CLAMP (xfconf_channel_get_int (screen_info->xfwm4_channel,
                                        "/general/cycle_selection_padding", XFWM_TABWIN_PREVIEW_PADDING), 0, 32);
    spacing = CLAMP (xfconf_channel_get_int (screen_info->xfwm4_channel,
                         "/general/cycle_spacing", XFWM_TABWIN_STYLE_SPACING), 0, 20);
    outer_padding = 2 * gtk_container_get_border_width (GTK_CONTAINER (tabwin_widget));
    available_width = MAX (1, tabwin->monitor_width * WIN_MAX_RATIO - outer_padding);
    available_height = MAX (1, tabwin->monitor_height * WIN_MAX_RATIO - outer_padding);

    /* We need to account for changes to the font size in the user's
     * appearance theme and gtkrc settings */
    layout = gtk_widget_create_pango_layout (GTK_WIDGET (tabwin_widget), "");
    pango_layout_get_pixel_size (layout, NULL, &tabwin->label_height);
    g_object_unref (layout);

    if (screen_info->params->cycle_tabwin_mode == STANDARD_ICON_GRID)
    {
        tabwin->icon_size = WIN_ICON_SIZE;
        gtk_widget_style_get (GTK_WIDGET (tabwin_widget),
                              "icon-size", &tabwin->icon_size, NULL);
        standard_icon_size = tabwin->icon_size;

        if (preview)
        {
            tabwin->preview_width = CLAMP (xfconf_channel_get_int (screen_info->xfwm4_channel, "/general/cycle_preview_width", 300), 120, 800);
            gint maximum_height = CLAMP (xfconf_channel_get_int (screen_info->xfwm4_channel, "/general/cycle_preview_height", 192), 80, 340);
            tabwin->preview_height = MAX (24, maximum_height - tabwin->label_height - 2 * tabwin->selection_padding);
            tabwin->preview_width = MAX (24, tabwin->preview_width - 2 * tabwin->selection_padding);
            tabwin->icon_size = MAX (tabwin->preview_width, tabwin->preview_height);
        }
        else
        {
            tabwin->icon_size = standard_icon_size;
            tabwin->preview_width = tabwin->preview_height = tabwin->icon_size;
        }
        size_request = tabwin->preview_height + tabwin->label_height + 2 * tabwin->selection_padding;
        tabwin->grid_cols = MAX (1, (available_width + spacing) /
                                  (tabwin->preview_width + 2 * tabwin->selection_padding + spacing));
        tabwin->grid_rows = (int) (ceil ((double) tabwin->client_count /
                                         (double) tabwin->grid_cols));

        /* If we run out of space, halve the icon size to make more room. */
        while (size_request * tabwin->grid_rows + spacing * (tabwin->grid_rows - 1) +
               tabwin->label_height > available_height)
        {
            tabwin->icon_size = tabwin->icon_size / 2;
            tabwin->preview_width = MAX (8, tabwin->preview_width / 2);
            tabwin->preview_height = MAX (8, tabwin->preview_height / 2);
            if (preview && tabwin->icon_size <= standard_icon_size)
            {
                /* Disable preview otherwise it'd be too slow */
                preview = FALSE;
                /* switch back to regular icon size */
                tabwin->icon_size = standard_icon_size;
                tabwin->preview_width = tabwin->preview_height = tabwin->icon_size;
            }
            size_request = tabwin->preview_height + tabwin->label_height + 2 * tabwin->selection_padding;

            /* Recalculate with new icon size */
            tabwin->grid_cols = MAX (1, (available_width + spacing) /
                                      (tabwin->preview_width + 2 * tabwin->selection_padding + spacing));
            tabwin->grid_rows = (int) (ceil ((double) tabwin->client_count /
                                             (double) tabwin->grid_cols));

            /* Shrinking the icon too much makes it hard to see */
            if (tabwin->icon_size < 8)
            {
                tabwin->icon_size = 8;
                break;
            }
        }
    }
    else
    {
        tabwin->icon_size = LISTVIEW_WIN_ICON_SIZE;
        gtk_widget_style_get (GTK_WIDGET (tabwin_widget),
                              "listview-icon-size", &tabwin->icon_size, NULL);
        tabwin->preview_width = tabwin->preview_height = tabwin->icon_size;
        tabwin->grid_rows = MAX (1, (int) (floor ((double) tabwin->monitor_height * WIN_MAX_RATIO /
                                                  (double) (tabwin->icon_size + 2 * WIN_ICON_BORDER))));
        tabwin->grid_cols = (int) (ceil ((double) tabwin->client_count /
                                         (double) tabwin->grid_rows));
    }

    /* pack the client icons */
    for (client_list = *tabwin->client_list; client_list; client_list = g_list_next (client_list))
    {
        Client *c = (Client *) client_list->data;

        if (screen_info->params->cycle_tabwin_mode == STANDARD_ICON_GRID)
        {
            if (preview)
            {
                icon_pixbuf = getClientIcon (c, tabwin->preview_width * tabwin->icon_scale,
                                             tabwin->preview_height * tabwin->icon_scale);
            }
            else
            {
                icon_pixbuf = getAppIcon (c, tabwin->icon_size * tabwin->icon_scale,
                                          tabwin->icon_size * tabwin->icon_scale);
            }
        }
        else
        {
            /* No preview in list mode */
            icon_pixbuf = getAppIcon (c, tabwin->icon_size * tabwin->icon_scale,
                                      tabwin->icon_size * tabwin->icon_scale);
        }
        tabwin->icon_list = g_list_append(tabwin->icon_list, icon_pixbuf);
    }
}

/* Keep the original direct box layout unless its actual content overflows.
 * A scrolled window has a tiny minimum request even when natural propagation
 * is enabled; wrapping every popup shrinks an ordinary Alt+Tab to that minimum. */
static void
tabwinFitToMonitor (TabwinWidget *widget, ScreenInfo *screen_info)
{
    GtkWidget *scroller = g_object_get_data (G_OBJECT (widget), "tabwin-scroller");
    GtkWidget *content = g_object_get_data (G_OBJECT (widget), "tabwin-content");
    GdkRectangle monitor;
    GtkRequisition minimum, natural;
    gint width, height, limit_width, limit_height, border;
    if (!content)
        return;
    xfwm_get_monitor_geometry (screen_info->gscr, widget->monitor_num, &monitor, FALSE);
    limit_width = MAX (1, (gint) (monitor.width * WIN_MAX_RATIO));
    limit_height = MAX (1, (gint) (monitor.height * WIN_MAX_RATIO));
    border = MIN ((gint) gtk_container_get_border_width (GTK_CONTAINER (widget)),
                  MIN (limit_width, limit_height) / 4);
    gtk_container_set_border_width (GTK_CONTAINER (widget), border);
    width = MAX (1, limit_width - 2 * border);
    height = MAX (1, limit_height - 2 * border);
    gtk_widget_get_preferred_size (content, &minimum, &natural);
    if (!scroller)
    {
        if (natural.width <= width && natural.height <= height)
            return;
        scroller = gtk_scrolled_window_new (NULL, NULL);
        gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
                                        GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scroller), GTK_SHADOW_NONE);
        gtk_scrolled_window_set_propagate_natural_width (GTK_SCROLLED_WINDOW (scroller), TRUE);
        gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (scroller), TRUE);
        g_object_ref (content);
        gtk_container_remove (GTK_CONTAINER (widget), content);
        gtk_container_add (GTK_CONTAINER (scroller), content);
        gtk_container_add (GTK_CONTAINER (widget), scroller);
        g_object_unref (content);
        g_object_set_data (G_OBJECT (widget), "tabwin-scroller", scroller);
        gtk_widget_show_all (scroller);
        if (widget->selected)
            gtk_widget_grab_focus (widget->selected);
    }
    width = MIN (width, MAX (1, natural.width));
    height = MIN (height, MAX (1, natural.height));
    for (guint i = 0; i < 3; i++)
    {
        gtk_scrolled_window_set_max_content_width (GTK_SCROLLED_WINDOW (scroller), -1);
        gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (scroller), -1);
        gtk_scrolled_window_set_min_content_width (GTK_SCROLLED_WINDOW (scroller), width);
        gtk_scrolled_window_set_min_content_height (GTK_SCROLLED_WINDOW (scroller), height);
        gtk_scrolled_window_set_max_content_width (GTK_SCROLLED_WINDOW (scroller), width);
        gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (scroller), height);
        gtk_widget_get_preferred_size (scroller, &minimum, &natural);
        natural.width += 2 * border;
        natural.height += 2 * border;
        if (natural.width <= limit_width && natural.height <= limit_height)
            break;
        width = MAX (1, width - MAX (0, natural.width - limit_width));
        height = MAX (1, height - MAX (0, natural.height - limit_height));
    }
    if (g_getenv ("XFWM4_ANIMATION_DEBUG"))
        g_message ("tabwin fit: min=%d,%d natural=%d,%d limit=%d,%d",
                   minimum.width, minimum.height, natural.width, natural.height,
                   limit_width, limit_height);
}

static void
tabwinApplyAppearance (TabwinWidget *tabwin_widget, ScreenInfo *screen_info)
{
    XfconfChannel *channel = screen_info->xfwm4_channel;
    GtkStyleContext *context = gtk_widget_get_style_context (GTK_WIDGET (tabwin_widget));
    GtkCssProvider *old_provider = g_object_get_data (G_OBJECT (tabwin_widget), "tabwin-appearance-provider");
    GtkCssProvider *provider = gtk_css_provider_new ();
    gchar *color = xfconf_channel_get_string (channel, "/general/cycle_background_color", XFWM_TABWIN_STYLE_COLOR);
    XfwmTabwinStyle style = {
        xfconf_channel_get_bool (channel, "/general/cycle_background_custom", FALSE), color,
        xfconf_channel_get_int (channel, "/general/cycle_background_opacity", XFWM_TABWIN_STYLE_OPACITY),
        xfconf_channel_get_int (channel, "/general/cycle_corner_radius", XFWM_TABWIN_STYLE_RADIUS),
        xfconf_channel_get_int (channel, "/general/cycle_border_width", XFWM_TABWIN_STYLE_BORDER),
        xfconf_channel_get_int (channel, "/general/cycle_content_padding", XFWM_TABWIN_STYLE_PADDING)
    };
    gchar *css;

    if (old_provider != NULL)
        gtk_style_context_remove_provider (context, GTK_STYLE_PROVIDER (old_provider));
    css = xfwm_tabwin_style_css (context, &style);
    gtk_css_provider_load_from_data (provider, css, -1, NULL);
    gtk_style_context_add_provider (context, GTK_STYLE_PROVIDER (provider),
                                    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    g_object_set_data_full (G_OBJECT (tabwin_widget), "tabwin-appearance-provider", provider, g_object_unref);
    gtk_container_set_border_width (GTK_CONTAINER (tabwin_widget),
                                    CLAMP (style.content_padding, 0, 64) + CLAMP (style.border_width, 0, 5));
    if (GTK_IS_GRID (tabwin_widget->container))
    {
        gint spacing = CLAMP (xfconf_channel_get_int (channel, "/general/cycle_spacing", XFWM_TABWIN_STYLE_SPACING), 0, 20);
        gtk_grid_set_row_spacing (GTK_GRID (tabwin_widget->container), spacing);
        gtk_grid_set_column_spacing (GTK_GRID (tabwin_widget->container), spacing);
    }
    gtk_widget_queue_resize (GTK_WIDGET (tabwin_widget));
    tabwinFitToMonitor (tabwin_widget, screen_info);
    g_free (css);
    g_free (color);
}

static void
tabwinAppearanceChanged (XfconfChannel *channel, const gchar *property,
                         const GValue *value, TabwinWidget *tabwin_widget)
{
    if (g_str_has_prefix (property, "/general/cycle_background_") ||
        !strcmp (property, "/general/cycle_corner_radius") ||
        !strcmp (property, "/general/cycle_border_width") ||
        !strcmp (property, "/general/cycle_content_padding") ||
        !strcmp (property, "/general/cycle_spacing"))
    {
        ScreenInfo *screen_info = g_object_get_data (G_OBJECT (tabwin_widget), "tabwin-screen-info");
        tabwinApplyAppearance (tabwin_widget, screen_info);
    }
}

static TabwinWidget *
tabwinCreateWidget (Tabwin *tabwin, ScreenInfo *screen_info, gint monitor_num)
{
    TabwinWidget *tabwin_widget;
    GdkScreen *screen;
    GdkVisual *visual;
    GtkStyleContext *ctx;
    GtkWidget *vbox;
    GtkWidget *windowlist;
    GdkRectangle monitor;

    TRACE ("monitor %i", monitor_num);

    tabwin_widget = g_object_new (tabwin_widget_get_type(), "type", GTK_WINDOW_POPUP, NULL);

    tabwin_widget->monitor_num = monitor_num;
    tabwin_widget->tabwin = tabwin;
    tabwin_widget->selected = NULL;
    tabwin_widget->selected_callback = 0;
    tabwin_widget->width = -1;
    tabwin_widget->height = -1;
    tabwin_widget->container = NULL;

    gtk_window_set_screen (GTK_WINDOW (tabwin_widget), screen_info->gscr);
    gtk_window_set_default_size (GTK_WINDOW (tabwin_widget), 0, 0);
    gtk_widget_set_name (GTK_WIDGET (tabwin_widget), XFWM_TABWIN_NAME);
    apply_default_theme (tabwin_widget, screen_info);
    tabwinApplyAppearance (tabwin_widget, screen_info);
    g_object_set_data (G_OBJECT (tabwin_widget), "tabwin-screen-info", screen_info);
    g_signal_connect_object (screen_info->xfwm4_channel, "property-changed",
                             G_CALLBACK (tabwinAppearanceChanged), tabwin_widget, 0);

    /* Check for compositing and set visual for it */
    screen = gtk_widget_get_screen (GTK_WIDGET (tabwin_widget));
    if (gdk_screen_is_composited (screen)) {
        visual = gdk_screen_get_rgba_visual (screen);
        if (visual)
        {
            gtk_widget_set_visual (GTK_WIDGET (tabwin_widget), visual);
        }
    }
    gtk_widget_set_app_paintable (GTK_WIDGET (tabwin_widget), TRUE);
    gtk_widget_realize (GTK_WIDGET (tabwin_widget));

    if (tabwin->icon_list == NULL)
    {
        computeTabwinData (screen_info, tabwin_widget);
    }
    ctx = gtk_widget_get_style_context (GTK_WIDGET (tabwin_widget));
    gtk_window_set_position (GTK_WINDOW (tabwin_widget), GTK_WIN_POS_NONE);
    xfwm_get_monitor_geometry (screen_info->gscr, tabwin_widget->monitor_num, &monitor, FALSE);
    gtk_window_move (GTK_WINDOW (tabwin_widget), monitor.x + monitor.width / 2,
                                      monitor.y + monitor.height / 2);

    vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 3);
    gtk_container_add (GTK_CONTAINER (tabwin_widget), vbox);
    g_object_set_data (G_OBJECT (tabwin_widget), "tabwin-content", vbox);

    if (screen_info->params->cycle_tabwin_mode == STANDARD_ICON_GRID)
    {
        tabwin_widget->label = gtk_label_new ("");
        gtk_label_set_use_markup (GTK_LABEL (tabwin_widget->label), TRUE);
        gtk_label_set_justify (GTK_LABEL (tabwin_widget->label), GTK_JUSTIFY_CENTER);
        gtk_label_set_ellipsize (GTK_LABEL (tabwin_widget->label), PANGO_ELLIPSIZE_END);
        gtk_box_pack_end (GTK_BOX (vbox), tabwin_widget->label, TRUE, TRUE, 0);

        gtk_style_context_add_class (ctx, "tabwin-app-grid");
    }
    else
    {
        gtk_style_context_add_class (ctx, "tabwin-app-list");
    }

    windowlist = createWindowlist (screen_info, tabwin_widget);
    tabwin_widget->container = windowlist;
    gtk_box_pack_start (GTK_BOX (vbox), windowlist, TRUE, TRUE, 0);

    g_signal_connect_swapped (tabwin_widget, "configure-event",
                              G_CALLBACK (tabwinConfigure),
                              (gpointer) tabwin_widget);

    g_signal_connect (tabwin_widget, "draw",
                      G_CALLBACK (tabwin_draw),
                      (gpointer) tabwin_widget);

    gtk_widget_show_all (vbox);
    tabwinFitToMonitor (tabwin_widget, screen_info);
    gtk_widget_show_all (GTK_WIDGET (tabwin_widget));
    tabwinFitToMonitor (tabwin_widget, screen_info);
    if (screen_info->params->blur_enabled && screen_info->params->blur_alt_tab)
    {
        GtkAllocation allocation;
        gtk_widget_get_allocation (GTK_WIDGET (tabwin_widget), &allocation);
        tabwin_publish_blur_area (tabwin_widget, allocation.width, allocation.height);
    }

    return tabwin_widget;
}

static Client *
tabwinChange2Selected (Tabwin *tabwin, GList *selected)
{
    GList *tabwin_list, *widgets, *children;
    GtkWidget *window_button, *buttonbox, *buttonlabel;
    TabwinWidget *tabwin_widget;
    Client *c;

    tabwin->selected = selected;
    for (tabwin_list = tabwin->tabwin_list; tabwin_list; tabwin_list = g_list_next (tabwin_list))
    {
        tabwin_widget = (TabwinWidget *) tabwin_list->data;
        for (widgets = tabwin_widget->widgets; widgets; widgets = g_list_next (widgets))
        {
            window_button = GTK_WIDGET (widgets->data);

            children = gtk_container_get_children (GTK_CONTAINER (window_button));
            buttonbox = GTK_WIDGET (g_list_nth_data (children, 0));
            g_list_free (children);

            children = gtk_container_get_children (GTK_CONTAINER (buttonbox));
            buttonlabel = GTK_WIDGET (g_list_nth_data (children, 1));
            g_list_free (children);

            c = g_object_get_data (G_OBJECT (window_button), "client-ptr-val");

            if (c != NULL)
            {
                /* don't clear label if mouse is inside the previously
                 * selected button */
                if (c->screen_info->params->cycle_tabwin_mode == STANDARD_ICON_GRID
                    && window_button != tabwin_widget->hovered)
                {
                    gtk_label_set_text (GTK_LABEL (buttonlabel), "");
                }

                if (c == tabwin->selected->data)
                {
                    gtk_widget_grab_focus (window_button);
                    tabwinSetSelected (tabwin_widget, window_button, buttonlabel);
                    gtk_widget_queue_draw (GTK_WIDGET (tabwin_widget));
                }
            }
        }
    }
    return tabwinGetSelected (tabwin);
}

Tabwin *
tabwinCreate (GList **client_list, GList *selected, gboolean display_workspace)
{
    ScreenInfo *screen_info;
    Client *c;
    Tabwin *tabwin;
    TabwinWidget *win;
    int num_monitors, i;
    gboolean has_primary;

    g_return_val_if_fail (selected, NULL);
    g_return_val_if_fail (client_list, NULL);
    g_return_val_if_fail (*client_list, NULL);

    c = (Client *) selected->data;
    TRACE ("client \"%s\" (0x%lx)", c->name, c->window);

    tabwin = g_new0 (Tabwin, 1);
    screen_info = c->screen_info;
    tabwin->display_workspace = display_workspace;
    tabwin->client_list = client_list;
    tabwin->client_count = g_list_length (*client_list);
    tabwin->selected = selected;
    tabwin->tabwin_list = NULL;
    tabwin->icon_list = NULL;

    num_monitors = myScreenGetNumMonitors (screen_info);
    has_primary = myScreenHasPrimaryMonitor (screen_info, c->window);

    for (i = 0; i < num_monitors; i++)
    {
        gint monitor_index;

        monitor_index = myScreenGetMonitorIndex (screen_info, i);

        if (has_primary &&
            !xfwm_monitor_is_primary (screen_info->gscr, monitor_index))
        {
            continue;
        }

        win = tabwinCreateWidget (tabwin, screen_info, monitor_index);
        tabwin->tabwin_list  = g_list_append (tabwin->tabwin_list, win);
    }

    return tabwin;
}

Client *
tabwinGetSelected (Tabwin *tabwin)
{
    g_return_val_if_fail (tabwin != NULL, NULL);
    TRACE ("entering");

    if (tabwin->selected)
    {
        return (Client *) tabwin->selected->data;
    }

    return NULL;
}

Client *
tabwinRemoveClient (Tabwin *tabwin, Client *c)
{
    GList *client_list, *tabwin_list, *widgets;
    GtkWidget *icon;
    TabwinWidget *tabwin_widget;

    g_return_val_if_fail (tabwin != NULL, NULL);
    g_return_val_if_fail (c != NULL, NULL);
    TRACE ("client \"%s\" (0x%lx)", c->name, c->window);

    if (!*tabwin->client_list)
    {
        return NULL;
    }

    /* First, remove the client from our own client list */
    for (client_list = *tabwin->client_list; client_list; client_list = g_list_next (client_list))
    {
        if (client_list->data == c)
        {
            if (client_list == tabwin->selected)
            {
                if ((g_list_next (client_list) == NULL) && (client_list == *tabwin->client_list))
                {
                    tabwin->selected = NULL;
                }
                else
                {
                    tabwinSelectNext (tabwin);
                }
            }
            *tabwin->client_list = g_list_delete_link (*tabwin->client_list, client_list);
            break;
        }
    }

    /* Second, remove the icon from all boxes */
    for (tabwin_list = tabwin->tabwin_list; tabwin_list; tabwin_list = g_list_next (tabwin_list))
    {
        tabwin_widget = (TabwinWidget *) tabwin_list->data;
        for (widgets = tabwin_widget->widgets; widgets; widgets = g_list_next (widgets))
        {
            icon = GTK_WIDGET (widgets->data);
            if (((Client *) g_object_get_data (G_OBJECT(icon), "client-ptr-val")) == c)
            {
                gtk_container_remove (GTK_CONTAINER (tabwin_widget->container), icon);
                tabwin_widget->widgets = g_list_delete_link (tabwin_widget->widgets, widgets);
                break;
            }
        }
    }

    return tabwinGetSelected (tabwin);
}

Client *
tabwinSelectHead (Tabwin *tabwin)
{
    GList *head, *tabwin_list, *widgets, *children;
    GtkWidget *window_button, *buttonbox, *buttonlabel;
    TabwinWidget *tabwin_widget;

    g_return_val_if_fail (tabwin != NULL, NULL);
    TRACE ("entering");

    head = *tabwin->client_list;
    if (!head)
    {
        return NULL;
    }
    tabwin->selected = head;
    for (tabwin_list = tabwin->tabwin_list; tabwin_list; tabwin_list = g_list_next (tabwin_list))
    {
        tabwin_widget = (TabwinWidget *) tabwin_list->data;
        for (widgets = tabwin_widget->widgets; widgets; widgets = g_list_next (widgets))
        {
            window_button = GTK_WIDGET (widgets->data);

            children = gtk_container_get_children (GTK_CONTAINER (window_button));
            buttonbox = GTK_WIDGET (g_list_nth_data (children, 0));
            g_list_free (children);

            children = gtk_container_get_children (GTK_CONTAINER (buttonbox));
            buttonlabel = GTK_WIDGET (g_list_nth_data (children, 1));
            g_list_free (children);

            if (((Client *) g_object_get_data (G_OBJECT (window_button), "client-ptr-val")) == head->data)
            {
                tabwinSetSelected (tabwin_widget, window_button, buttonlabel);
                gtk_widget_queue_draw (GTK_WIDGET (tabwin_widget));
            }
        }
    }

    return tabwinGetSelected (tabwin);
}

Client *
tabwinSelectNext (Tabwin *tabwin)
{
    GList *next;

    g_return_val_if_fail (tabwin != NULL, NULL);
    TRACE ("entering");

    next = g_list_next (tabwin->selected);
    if (!next)
    {
        next = *tabwin->client_list;
        g_return_val_if_fail (next != NULL, NULL);
    }
    return tabwinChange2Selected (tabwin, next);
}

Client *
tabwinSelectPrev (Tabwin *tabwin)
{
    GList *prev;

    g_return_val_if_fail (tabwin != NULL, NULL);
    TRACE ("entering");

    prev = g_list_previous (tabwin->selected);
    if (!prev)
    {
        prev = g_list_last (*tabwin->client_list);
        g_return_val_if_fail (prev != NULL, NULL);
    }
    return tabwinChange2Selected (tabwin, prev);
}

Client *
tabwinSelectDelta (Tabwin *tabwin, int row_delta, int col_delta)
{
    GList *selected;
    int pos_current, col_current, row_current, nitems, cols, rows;
    Client *c;
    ScreenInfo *screen_info;

    TRACE ("entering");
    g_return_val_if_fail (tabwin != NULL, NULL);

    pos_current = g_list_position (*tabwin->client_list, tabwin->selected);
    nitems = g_list_length (*tabwin->client_list);

    if (tabwin->selected)
    {
        c = (Client *) tabwin->selected->data;
        screen_info = c->screen_info;
        if (!screen_info)
        {
            return NULL;
        }
    }
    else if (*tabwin->client_list)
    {
        c = (Client *) (*tabwin->client_list)->data;
        screen_info = c->screen_info;
        if (!screen_info)
        {
            return NULL;
        }
    }
    else
    {
        /* There's no items? */
        return NULL;
    }

    if (screen_info->params->cycle_tabwin_mode == STANDARD_ICON_GRID)
    {
        cols = MIN (nitems, tabwin->grid_cols);
        rows = (nitems - 1) / cols + 1;
        col_current = pos_current % cols;
        row_current = pos_current / cols;
    }
    else
    {
        rows = MIN (nitems, tabwin->grid_rows);
        cols = (nitems - 1) / rows + 1;
        row_current = pos_current % rows;
        col_current = pos_current / rows;
    }

    /* Wrap column */
    col_current += col_delta;
    if (col_current < 0)
    {
        col_current = cols - 1;
        row_current--;
        if (row_current < 0)
        {
            row_current = rows - 1;
        }
    }
    else if (col_current >= cols)
    {
        col_current = 0;
        if (rows > 1)
        {
            row_current++;
        }
        else
        {
            /* If there's only 1 row then col needs to wrap back to
             * the head of the grid */
            col_current = 0;
        }
    }

    /* Wrap row */
    row_current += row_delta;
    if (row_current < 0)
    {
        row_current = rows - 1;
        col_current--;
        if (col_current < 0)
        {
            col_current = cols - 1;
        }
    }
    else if (row_current >= rows)
    {
        row_current = 0;
        col_current++;
        if (col_current >= cols)
        {
            if (rows != 1)
            {
                col_current = cols - 1;
            }
            else
            {
                /* If there's only 1 row then col needs to wrap back to
                 * the head of the grid */
                col_current = 0;
            }
        }
    }

    /* So here we are at the new (wrapped) position in the rectangle */
    if (screen_info->params->cycle_tabwin_mode == STANDARD_ICON_GRID)
    {
        pos_current = col_current + row_current * cols;
    }
    else
    {
        pos_current = row_current + col_current * rows;
    }

    if (pos_current >= nitems)   /* If that position does not exist */
    {
        if (col_delta)            /* Let horizontal prevail */
        {
            if (col_delta > 0)
            {
                if (screen_info->params->cycle_tabwin_mode == STANDARD_ICON_GRID)
                {
                    /* In this case we're going past the tail, reset to the head
                     * of the grid */
                    col_current = 0;
                    row_current = 0;
                }
                else
                {
                    col_current = 0;
                    row_current++;
                    if (row_current >= rows)
                    {
                        row_current = 0;
                    }
                }
            }
            else
            {
                if (screen_info->params->cycle_tabwin_mode == STANDARD_ICON_GRID)
                {
                    col_current = (nitems - 1) % cols;
                }
                else
                {
                    col_current = cols - 1;
                    row_current = (nitems - 1) % rows;
                }
            }
        }
        else
        {
            if (row_delta > 0)
            {
                row_current = 0;
                col_current++;
                if (col_current >= cols)
                {
                    col_current = 0;
                }
            }
            else
            {
                if (screen_info->params->cycle_tabwin_mode == STANDARD_ICON_GRID)
                {
                    row_current = row_current - 1;
                }
                else
                {
                    row_current = (nitems - 1) % rows;
                    col_current = cols - 1;
                }
            }

        }

        if (screen_info->params->cycle_tabwin_mode == STANDARD_ICON_GRID)
        {
            pos_current = col_current + row_current * cols;
        }
        else
        {
            pos_current = row_current + col_current * rows;
        }
    }

    selected = g_list_nth (*tabwin->client_list, pos_current);

    return tabwinChange2Selected (tabwin, selected);
}

Client*
tabwinSelectHovered (Tabwin *tabwin)
{
    GList *tabwin_list, *selected;
    TabwinWidget *tabwin_widget;
    Client *c;

    g_return_val_if_fail (tabwin != NULL, NULL);
    TRACE ("entering");

    for (tabwin_list = tabwin->tabwin_list; tabwin_list; tabwin_list = g_list_next (tabwin_list))
    {
        tabwin_widget = (TabwinWidget *) tabwin_list->data;
        if (tabwin_widget->hovered)
        {
            gtk_widget_grab_focus (tabwin_widget->hovered);
            c = g_object_get_data (G_OBJECT (tabwin_widget->hovered), "client-ptr-val");
            selected = g_list_find (*tabwin->client_list, c);
            if (selected)
            {
                tabwin->selected = selected;
            }
            return c;
        }
    }

    return tabwinSelectWidget (tabwin);
}

void
tabwinDestroy (Tabwin *tabwin)
{
    GList *tabwin_list;
    TabwinWidget *tabwin_widget;

    g_return_if_fail (tabwin != NULL);
    TRACE ("entering");

    for (tabwin_list = tabwin->tabwin_list; tabwin_list; tabwin_list = g_list_next (tabwin_list))
    {
        tabwin_widget = (TabwinWidget *) tabwin_list->data;
        g_list_free (tabwin_widget->widgets);
        gtk_widget_destroy (GTK_WIDGET (tabwin_widget));
    }
    g_list_free_full (tabwin->icon_list, g_object_unref);
    g_list_free (tabwin->tabwin_list);
}
