/* SPDX-License-Identifier: GPL-2.0-or-later
 * Shared window-switcher thumbnail composition for xfwm4-gl and the tasklist.
 * Both clients compile this identical implementation locally; X11 captures
 * and GPU textures remain owned by their respective processes.
 */
#ifndef XFWM_TABWIN_PREVIEW_H
#define XFWM_TABWIN_PREVIEW_H

#define XFWM_TABWIN_PREVIEW_PADDING 5

#include <gdk-pixbuf/gdk-pixbuf.h>

static inline GdkPixbuf *
xfwm_tabwin_compose_pixbuf (GdkPixbuf *content, GdkPixbuf *application_icon,
                             gint width, gint height)
{
    GdkPixbuf *canvas, *scaled = NULL;
    gint content_w, content_h, icon_size;

    if (width <= 0 || height <= 0)
        return NULL;
    canvas = gdk_pixbuf_new (GDK_COLORSPACE_RGB, TRUE, 8, width, height);
    if (!canvas)
        return NULL;
    gdk_pixbuf_fill (canvas, 0x00000000);

    if (content)
    {
        gdouble scale;
        content_w = gdk_pixbuf_get_width (content);
        content_h = gdk_pixbuf_get_height (content);
        if (content_w > 0 && content_h > 0)
        {
            scale = MIN (1.0, MIN ((gdouble) width / content_w,
                                  (gdouble) height / content_h));
            if (scale < 1.0)
                scaled = gdk_pixbuf_scale_simple (content,
                         MAX (1, (gint) (content_w * scale)),
                         MAX (1, (gint) (content_h * scale)),
                         GDK_INTERP_BILINEAR);
            else
                scaled = g_object_ref (content);
            if (scaled)
            {
                gint w = gdk_pixbuf_get_width (scaled);
                gint h = gdk_pixbuf_get_height (scaled);
                gint x = (width - w) / 2, y = (height - h) / 2;
                gdk_pixbuf_composite (scaled, canvas, x, y, w, h,
                                      x, y, 1.0, 1.0, GDK_INTERP_BILINEAR, 255);
                g_object_unref (scaled);
            }
        }
    }

    /* This is getClientIcon()'s original badge geometry: quarter of the
     * complete square canvas, bottom centred, never of the cropped window. */
    icon_size = MIN (48, MIN (width / 4, height / 4));
    if (application_icon && icon_size > 0)
    {
        GdkPixbuf *badge = gdk_pixbuf_scale_simple (application_icon,
                                      icon_size, icon_size, GDK_INTERP_BILINEAR);
        if (badge)
        {
            gint x = (width - icon_size) / 2;
            gint y = height - icon_size;
            gdk_pixbuf_composite (badge, canvas, x, y, icon_size, icon_size,
                                  x, y, 1.0, 1.0, GDK_INTERP_BILINEAR, 255);
            g_object_unref (badge);
        }
    }

    return canvas;
}
#endif /* XFWM_TABWIN_PREVIEW_H */
