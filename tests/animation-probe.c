/* SPDX-License-Identifier: GPL-2.0-or-later
 * Standalone X11 client for compositor tests; never touches user windows.
 */
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <stdlib.h>
#include <string.h>

int
main (int argc, char **argv)
{
    Display *d = XOpenDisplay (NULL);
    XVisualInfo visual;
    XSetWindowAttributes attrs;
    Window w;
    GC gc;
    XClassHint hint = { "xfwm4-animation-probe", "Xfwm4AnimationProbe" };
    unsigned long target[4];
    Atom close_atom, icon_atom;
    int argb = argc > 1 && !strcmp (argv[1], "--argb");
    int green = argc > 1 && !strcmp (argv[1], "--green");
    if (!d) return 1;
    attrs.background_pixel = argb ? 0xbbaa3030 : (green ? 0x38ee38 : 0xee3838);
    attrs.border_pixel = 0;
    attrs.event_mask = ExposureMask | StructureNotifyMask;
    if (argb && XMatchVisualInfo (d, DefaultScreen (d), 32, TrueColor, &visual))
    {
        attrs.colormap = XCreateColormap (d, DefaultRootWindow (d), visual.visual, AllocNone);
        w = XCreateWindow (d, DefaultRootWindow (d), 140, 100, 520, 330, 0, 32,
                           InputOutput, visual.visual, CWColormap | CWBackPixel | CWBorderPixel | CWEventMask, &attrs);
    }
    else
    {
        w = XCreateWindow (d, DefaultRootWindow (d), 140, 100, 520, 330, 0,
                           CopyFromParent, InputOutput, CopyFromParent, CWBackPixel | CWBorderPixel | CWEventMask, &attrs);
        argb = 0;
    }
    XStoreName (d, w, "xfwm4-animation-probe");
    XSetClassHint (d, w, &hint);
    close_atom = XInternAtom (d, "WM_DELETE_WINDOW", False);
    XSetWMProtocols (d, w, &close_atom, 1);
    icon_atom = XInternAtom (d, "_NET_WM_ICON_GEOMETRY", False);
    target[0] = 80; target[1] = DisplayHeight (d, DefaultScreen (d)) - 40;
    target[2] = 40; target[3] = 40;
    XChangeProperty (d, w, icon_atom, XA_CARDINAL, 32, PropModeReplace, (unsigned char *) target, 4);
    gc = XCreateGC (d, w, 0, NULL);
    XMapWindow (d, w);
    for (;;)
    {
        XEvent e;
        XNextEvent (d, &e);
        if (e.type == Expose)
        {
            XSetForeground (d, gc, argb ? 0xbbaa3030 : (green ? 0x38ee38 : 0xee3838));
            XFillRectangle (d, w, gc, 0, 0, 520, 330);
            XSetForeground (d, gc, argb ? 0xbb3030aa : 0x3030ee);
            XFillRectangle (d, w, gc, 25, 25, 120, 100);
            XSetForeground (d, gc, argb ? 0xbbbbbbbb : 0xffffff);
            XDrawString (d, w, gc, 175, 60, "MiniOS animation probe", 22);
            XFlush (d);
        }
        if (e.type == ClientMessage && (Atom) e.xclient.data.l[0] == close_atom) break;
    }
    XFreeGC (d, gc);
    XDestroyWindow (d, w);
    XCloseDisplay (d);
    return 0;
}
