/* Shared themed window-switcher/panel-preview appearance. GPL-2.0-or-later. */
#ifndef XFWM_TABWIN_STYLE_H
#define XFWM_TABWIN_STYLE_H

#include <gtk/gtk.h>

#define XFWM_TABWIN_STYLE_NAME "xfwm-tabwin"
#define XFWM_TABWIN_STYLE_OPACITY 80
#define XFWM_TABWIN_STYLE_RADIUS 12
#define XFWM_TABWIN_STYLE_BORDER 0
#define XFWM_TABWIN_STYLE_SPACING 4
#define XFWM_TABWIN_STYLE_PADDING 24
#define XFWM_TABWIN_STYLE_COLOR "#30343b"
#define XFWM_TABWIN_SELECTION_RADIUS 6
#define XFWM_TABWIN_SELECTION_OPACITY 30

typedef struct
{
    gboolean custom_color;
    const gchar *color;
    gint radius, opacity;
} XfwmTabwinSelectionStyle;

static inline gchar *
xfwm_tabwin_selection_css (GtkStyleContext *context, const XfwmTabwinSelectionStyle *style)
{
    GdkRGBA color;
    gtk_style_context_get_background_color (context, GTK_STATE_FLAG_CHECKED, &color);
    if (color.alpha == 0.0)
        gtk_style_context_lookup_color (context, "theme_selected_bg_color", &color);
    if (style->custom_color && style->color)
    {
        GdkRGBA custom;
        if (gdk_rgba_parse (&custom, style->color)) color = custom;
    }
    color.alpha = CLAMP (style->opacity, 0, 100) / 100.0;
    gchar *text = gdk_rgba_to_string (&color);
    gchar *css = g_strdup_printf (
        "button { padding: 0; } button:checked, button:checked:hover, button:hover {"
        " background-image: none; background-color: %s; border-radius: %dpx; box-shadow: none; }",
        text, CLAMP (style->radius, 0, 36));
    g_free (text);
    return css;
}

typedef struct
{
    gboolean custom_background;
    const gchar *background_color;
    gint background_opacity;
    gint corner_radius;
    gint border_width;
    gint content_padding;
} XfwmTabwinStyle;

/* Keep the switcher's fallback when a GTK theme has no #xfwm-tabwin rules.
 * The returned provider is owned by the caller, or NULL for a themed surface. */
static inline GtkCssProvider *
xfwm_tabwin_default_provider (GtkWidget *widget)
{
    GtkSettings *settings = gtk_widget_get_settings (widget);
    GtkCssProvider *named, *fallback = NULL;
    gchar *theme = NULL, *css;

    g_object_get (settings, "gtk-theme-name", &theme, NULL);
    if (theme == NULL)
        return NULL;
    named = gtk_css_provider_get_named (theme, NULL);
    g_free (theme);
    if (named == NULL)
        return NULL;
    css = gtk_css_provider_to_string (named);
    if (g_strrstr (css, "#" XFWM_TABWIN_STYLE_NAME) == NULL)
    {
        fallback = gtk_css_provider_new ();
        gtk_css_provider_load_from_data (fallback,
            "#" XFWM_TABWIN_STYLE_NAME " { background-color: @theme_bg_color;"
            " background-image: none; border-color: @theme_selected_bg_color; }",
            -1, NULL);
    }
    g_free (css);
    return fallback;
}

/* Resolve theme RGB before attaching the override provider. Opacity affects
 * only the popup surface, never its images, checked buttons or native shadow. */
static inline gchar *
xfwm_tabwin_style_css (GtkStyleContext *context, const XfwmTabwinStyle *style)
{
    GdkRGBA background, border;
    gchar *background_text, *border_text, *css;

    gtk_style_context_get_background_color (context, GTK_STATE_FLAG_NORMAL, &background);
    if (background.alpha == 0.0 &&
        !gtk_style_context_lookup_color (context, "theme_bg_color", &background))
        background = (GdkRGBA) { 0.0, 0.0, 0.0, 1.0 };
    if (style->custom_background && style->background_color != NULL)
    {
        GdkRGBA custom;
        if (gdk_rgba_parse (&custom, style->background_color))
            background = custom;
    }
    background.alpha = CLAMP (style->background_opacity, 0, 100) / 100.0;
    if (!gtk_style_context_lookup_color (context, "borders", &border))
    {
        gtk_style_context_get_border_color (context, GTK_STATE_FLAG_NORMAL, &border);
        if (border.alpha == 0.0)
        {
            gtk_style_context_lookup_color (context, "theme_fg_color", &border);
            border.alpha = 0.18;
        }
    }
    background_text = gdk_rgba_to_string (&background);
    border_text = gdk_rgba_to_string (&border);
    css = g_strdup_printf (
        "#" XFWM_TABWIN_STYLE_NAME " { background-image: none; background-color: %s;"
        " border-radius: %dpx; border: %dpx solid %s; padding: 0; box-shadow: none; }"
        "frame#" XFWM_TABWIN_STYLE_NAME " > border { border: none; border-radius: %dpx; }",
        background_text, CLAMP (style->corner_radius, 0, 36),
        CLAMP (style->border_width, 0, 5), border_text,
        CLAMP (style->corner_radius, 0, 36));
    g_free (background_text);
    g_free (border_text);
    return css;
}

#endif /* XFWM_TABWIN_STYLE_H */
