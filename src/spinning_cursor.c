/* SPDX-License-Identifier: GPL-2.0-or-later
 * mozilla - (c) 1998 mozilla.org
 * xfwm4   - (c) 2002-2011 Olivier Fourdan
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <X11/Xlib.h>
#include <X11/cursorfont.h>

#include "spinning_cursor.h"

Cursor
cursorCreateSpinning (Display * dpy)
{
    /* Use the same Xcursor theme/size path as root, move and resize cursors.
     * Xcursor.theme_core lets Xlib resolve XC_watch through the active theme;
     * without a theme, Xlib retains its normal core-cursor fallback. The old
     * fixed 32x32 monochrome bitmap ignored HiDPI and theme changes. */
    return XCreateFontCursor (dpy, XC_watch);
}
