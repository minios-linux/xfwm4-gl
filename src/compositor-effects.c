/* SPDX-License-Identifier: GPL-2.0-or-later
 * Shared effect policy, motion targets and visual-only Peek state. Backend
 * resources and drawing remain with their respective compositors. */
#include "config.h"
#ifdef HAVE_COMPOSITOR
#include <math.h>
#include <string.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include "compositor-effects.h"
#include "compositor.h"
#include "hints.h"

XfwmPeek *
compositorPeek (ScreenInfo *screen)
{
    if (!screen->peek_data) screen->peek_data = g_new0 (XfwmPeek, 1);
    return screen->peek_data;
}

gboolean
compositorEffectWindow (CWindow *cw)
{
    return cw->c && !WIN_IS_OVERRIDE (cw) &&
           !(cw->c->type & (WINDOW_DESKTOP | WINDOW_DOCK));
}

CWindow *
compositorPeekWindow (ScreenInfo *screen, Window id)
{
    if (id == None) return NULL;
    for (GList *li = screen->cwindows; li; li = li->next)
    {
        CWindow *cw = li->data;
        if (compositorEffectWindow (cw) && cw->c->window == id) return cw;
    }
    return NULL;
}

static void
peek_damage (ScreenInfo *screen)
{
    cairo_rectangle_int_t rect = {0, 0, screen->width, screen->height};
    compositorDamageRegion (screen, &rect);
}

static gboolean
peek_tick (gpointer data)
{
    ScreenInfo *screen = data;
    XfwmPeek *peek = compositorPeek (screen);
    gint64 now = g_get_monotonic_time ();
    if (!screen->compositor_active) { peek->timer = 0; return G_SOURCE_REMOVE; }
    if (peek->to > 0 && (now - peek->heartbeat > 1100000 ||
                         !compositorPeekWindow (screen, peek->window)))
    {
        peek->from = peek->phase; peek->to = 0; peek->started = now;
        peek->previous = None; peek->switching = 1;
    }
    double fraction = CLAMP ((double) (now - peek->started) / MAX (peek->duration, 1), 0, 1);
    peek->phase = peek->from + (peek->to - peek->from) * xfwm_animation_ease (fraction);
    if (peek->previous != None)
    {
        double switching = CLAMP ((double) (now - peek->switched) / MAX (peek->duration, 1), 0, 1);
        peek->switching = xfwm_animation_ease (switching);
        if (switching >= 1) peek->previous = None;
    }
    peek_damage (screen);
    if (fraction >= 1 && peek->to <= 0)
    {
        peek->window = peek->previous = None; peek->mode = 0; peek->phase = 0;
    }
    if (fraction >= 1 && peek->previous == None)
    { peek->timer = 0; return G_SOURCE_REMOVE; }
    return G_SOURCE_CONTINUE;
}

static gboolean
peek_watchdog (gpointer data)
{
    ScreenInfo *screen = data;
    XfwmPeek *peek = compositorPeek (screen);
    if (!screen->compositor_active || peek->to <= 0)
    { peek->watchdog = 0; return G_SOURCE_REMOVE; }
    if (g_get_monotonic_time () - peek->heartbeat > 1100000 ||
        !compositorPeekWindow (screen, peek->window))
    {
        peek->watchdog = 0;
        compositorSetPeek (screen, None, 0, 0, 180);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

void
compositorSetPeek (ScreenInfo *screen, Window window, gint mode, gint opacity, gint duration)
{
    if (!screen->compositor_active) return;
    XfwmPeek *peek = compositorPeek (screen);
    gint64 now = g_get_monotonic_time ();
    mode = CLAMP (mode, 0, 2);
    gboolean active = window != None && mode > 0 && compositorPeekWindow (screen, window);
    double other = mode == 2 ? 0 : CLAMP (opacity, 0, 100) / 100.0;
    if (active && peek->to > 0 && peek->window == window && peek->mode == mode &&
        fabs (peek->opacity - other) < .0001)
    { peek->heartbeat = now; return; }
    if (!active)
    {
        if (peek->to <= 0) return;
        peek->from = peek->phase; peek->to = 0; peek->started = now;
        peek->previous = None; peek->switching = 1;
    }
    else
    {
        gboolean different = peek->window != window;
        peek->heartbeat = now; peek->opacity = other;
        peek->duration = (gint64) CLAMP (duration, 50, 600) * 1000;
        if (different && peek->window != None && peek->to > 0 && peek->phase > .001)
        { peek->previous = peek->window; peek->switching = 0; peek->switched = now; }
        else if (different || peek->to <= 0)
        { peek->previous = None; peek->switching = 1; }
        peek->window = window; peek->mode = mode;
        peek->from = peek->phase; peek->to = 1; peek->started = now;
        if (!peek->watchdog) peek->watchdog = g_timeout_add (160, peek_watchdog, screen);
    }
    if (!peek->timer) peek->timer = g_timeout_add (16, peek_tick, screen);
    peek_damage (screen);
}

void
compositorClearPeek (ScreenInfo *screen)
{
    XfwmPeek *peek = screen->peek_data;
    if (!peek) return;
    if (peek->timer) g_source_remove (peek->timer);
    if (peek->watchdog) g_source_remove (peek->watchdog);
    g_free (peek); screen->peek_data = NULL;
}

double
compositorPeekFactor (CWindow *cw)
{
    XfwmPeek *peek = compositorPeek (cw->screen_info);
    if (peek->phase <= 0 || !compositorEffectWindow (cw)) return 1;
    if (cw->c->window == peek->window)
        return 1 - peek->phase + peek->phase * peek->opacity *
               (peek->previous != None ? 1 - peek->switching : 0);
    return 1 - peek->phase * (1 - peek->opacity);
}

void
compositorIconTarget (CWindow *cw, cairo_rectangle_int_t *target, XfwmAnimationEdge *edge)
{
    ScreenInfo *screen = cw->screen_info;
    DisplayInfo *display = screen->display_info;
    GdkRectangle monitor;
    myScreenFindMonitorAtPoint (screen, cw->attr.x + cw->attr.width / 2,
                               cw->attr.y + cw->attr.height / 2, &monitor);
    *target = (cairo_rectangle_int_t) {monitor.x + monitor.width / 2 - 12,
                                      monitor.y + monitor.height - 1, 24, 1};
    Atom type = None; int format = 0;
    unsigned long count = 0, remaining = 0;
    unsigned char *property = NULL;
    myDisplayErrorTrapPush (display);
    int status = XGetWindowProperty (display->dpy, cw->c->window, display->atoms[NET_WM_ICON_GEOMETRY],
                                    0, 4, False, XA_CARDINAL, &type, &format, &count, &remaining, &property);
    gboolean found = FALSE;
    if (myDisplayErrorTrapPop (display) == Success && status == Success && type == XA_CARDINAL &&
        format == 32 && count == 4 && property)
    {
        unsigned long *v = (unsigned long *) property;
        gint64 x = (gint32) v[0], y = (gint32) v[1];
        guint64 width = (guint32) v[2], height = (guint32) v[3];
        if (width && height && width <= (guint) screen->width && height <= (guint) screen->height &&
            x >= -256 && y >= -256 && x < screen->width + 256 && y < screen->height + 256)
        { *target = (cairo_rectangle_int_t) {x, y, width, height}; found = TRUE; }
    }
    if (property) XFree (property);
    gint cx = target->x + target->width / 2, cy = target->y + target->height / 2;
    if (found) myScreenFindMonitorAtPoint (screen, cx, cy, &monitor);
    for (GList *li = screen->cwindows; found && li; li = li->next)
    {
        CWindow *dock = li->data;
        if (WIN_IS_DOCK (dock) && cx >= dock->attr.x && cy >= dock->attr.y &&
            cx < dock->attr.x + dock->attr.width && cy < dock->attr.y + dock->attr.height)
        {
            *edge = dock->attr.width >= dock->attr.height
              ? (cy < monitor.y + monitor.height / 2 ? XFWM_ANIMATION_TOP : XFWM_ANIMATION_BOTTOM)
              : (cx < monitor.x + monitor.width / 2 ? XFWM_ANIMATION_LEFT : XFWM_ANIMATION_RIGHT);
            return;
        }
    }
    double distances[] = {fabs ((double) cy - monitor.y), fabs ((double) cy - monitor.y - monitor.height),
                           fabs ((double) cx - monitor.x), fabs ((double) cx - monitor.x - monitor.width)};
    *edge = 0;
    for (guint i = 1; i < G_N_ELEMENTS (distances); ++i)
        if (distances[i] < distances[*edge]) *edge = i;
}

gboolean
compositorIsWhiskerMenu (CWindow *cw)
{
    DisplayInfo *display = cw->screen_info->display_info;
    Window window = cw->c ? cw->c->window : cw->id;
    gchar *name = NULL;
    XClassHint hint = {0};
    myDisplayErrorTrapPush (display);
    gboolean identified = XFetchName (display->dpy, window, &name) && name &&
                           !strcmp (name, "Whisker Menu");
    if (identified)
        identified = XGetClassHint (display->dpy, window, &hint) && hint.res_class &&
          (!strcmp (hint.res_class, "Wrapper-2.0") || !strcmp (hint.res_class, "Xfce4-panel"));
    if (myDisplayErrorTrapPop (display) != Success) identified = FALSE;
    if (name) XFree (name);
    if (hint.res_name) XFree (hint.res_name);
    if (hint.res_class) XFree (hint.res_class);
    return identified;
}

void
compositorWindowTarget (ScreenInfo *screen, const cairo_rectangle_int_t *source,
                        gboolean whisker, cairo_rectangle_int_t *target)
{
    if (whisker)
    {
        GdkRectangle monitor;
        myScreenFindMonitorAtPoint (screen, source->x + source->width / 2,
                                   source->y + source->height / 2, &monitor);
        *target = *source;
        /* Match tasklist preview arrival/dismissal: up to 18 logical pixels,
         * constant size, with room kept for the native shadow at screen edges. */
        gint scale = gdk_window_get_scale_factor (gdk_screen_get_root_window (screen->gscr));
        gint room = MAX (0, monitor.y + monitor.height - source->y - source->height);
        target->y += MIN (18 * scale, room);
    }
    else
    {
        XfwmAnimationRect rect = {source->x, source->y, source->width, source->height};
        XfwmAnimationRect result = xfwm_animation_window_target (&rect, screen->params->window_effect);
        *target = (cairo_rectangle_int_t) {result.x, result.y, result.width, result.height};
    }
}
#endif
