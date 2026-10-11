/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <X11/Xlib.h>
#include <X11/cursorfont.h>
#include <X11/extensions/Xfixes.h>
#include <stdio.h>
#include <string.h>

#include "../src/spinning_cursor.h"

/* Diagnostic executable using the actual production busy-cursor constructor.
 * Run only on a disposable X11 display or an explicitly selected test session. */
int
main (int argc, char **argv)
{
    Display *display;
    Window window;
    Cursor cursor;
    XFixesCursorImage *image;
    gint size;
    guint shape;
    const gchar *mode;

    gtk_init (&argc, &argv);
    mode = argc > 1 ? argv[1] : "busy";
    g_object_get (gtk_settings_get_default (), "gtk-cursor-theme-size", &size, NULL);
    display = GDK_DISPLAY_XDISPLAY (gdk_display_get_default ());
    window = XCreateSimpleWindow (display, DefaultRootWindow (display), 100, 100, 120, 120, 0, 0, 0);
    XSetWindowAttributes attributes = { .override_redirect = True };
    XChangeWindowAttributes (display, window, CWOverrideRedirect, &attributes);
    XMapWindow (display, window);
    shape = strcmp (mode, "busy") == 0 ? XC_watch
        : strcmp (mode, "resize") == 0 ? XC_bottom_right_corner
        : strcmp (mode, "move") == 0 ? XC_fleur : XC_left_ptr;
    if (strcmp (mode, "busy") == 0)
        cursor = cursorCreateSpinning (display);
    else
        cursor = XCreateFontCursor (display, shape);
    XDefineCursor (display, window, cursor);
    XWarpPointer (display, None, window, 0, 0, 0, 0, 60, 60);
    XSync (display, False);
    image = XFixesGetCursorImage (display);
    if (image == NULL)
        return 1;
    /* XFixes reports a 1x1 placeholder for an animated cursor on Xvfb. Inspect
     * the themed source image as well, through GTK's Xcursor-backed API. */
    GdkCursor *source = gdk_cursor_new_for_display (gdk_display_get_default (), (GdkCursorType) shape);
    gdouble source_xhot, source_yhot;
    cairo_surface_t *pixels = gdk_cursor_get_surface (source, &source_xhot, &source_yhot);
    printf ("{\"mode\":\"%s\",\"setting_size\":%d,\"width\":%u,\"height\":%u,\"xhot\":%u,\"yhot\":%u,\"theme_width\":%d,\"theme_height\":%d}\n",
            mode, size, image->width, image->height, image->xhot, image->yhot,
            pixels != NULL ? cairo_image_surface_get_width (pixels) : 0,
            pixels != NULL ? cairo_image_surface_get_height (pixels) : 0);
    if (pixels != NULL)
        cairo_surface_destroy (pixels);
    g_object_unref (source);
    XFree (image);
    XDestroyWindow (display, window);
    XFreeCursor (display, cursor);
    XSync (display, False);
    return 0;
}
