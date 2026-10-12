/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef XFWM_COMPOSITOR_EFFECTS_H
#define XFWM_COMPOSITOR_EFFECTS_H
#include "compositor-priv.h"
#include "animation-math.h"
#ifdef HAVE_COMPOSITOR
typedef struct
{
    Window window, previous;
    gint mode;
    double opacity, phase, from, to, switching;
    gint64 started, heartbeat, duration, switched;
    guint timer, watchdog;
} XfwmPeek;

XfwmPeek *compositorPeek (ScreenInfo *);
void compositorSetPeek (ScreenInfo *, Window, gint, gint, gint);
void compositorClearPeek (ScreenInfo *);
CWindow *compositorPeekWindow (ScreenInfo *, Window);
gboolean compositorEffectWindow (CWindow *);
double compositorPeekFactor (CWindow *);
void compositorIconTarget (CWindow *, cairo_rectangle_int_t *, XfwmAnimationEdge *);
gboolean compositorIsWhiskerMenu (CWindow *);
void compositorWindowTarget (ScreenInfo *, const cairo_rectangle_int_t *,
                              gboolean whisker, cairo_rectangle_int_t *);
#endif
#endif
