/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef XFWM_XRENDER_EFFECTS_H
#define XFWM_XRENDER_EFFECTS_H
#include "compositor-effects.h"
#ifdef HAVE_COMPOSITOR
void xfwmXRAnimateOpen (CWindow *);
void xfwmXRAnimateClose (CWindow *);
void xfwmXRAnimateWindow (CWindow *, gboolean);
void xfwmXRConfigure (CWindow *);
void xfwmXRFreeWindow (CWindow *);
void xfwmXRFreeClose (CWindow *);
void xfwmXRCancelWindow (CWindow *);
void xfwmXRCancel (ScreenInfo *);
void xfwmXRFinish (ScreenInfo *);
void xfwmXRCacheClose (CWindow *);
gboolean xfwmXRPaintEffects (ScreenInfo *, Picture, XserverRegion);
void xfwmXRCaptureFrame (ScreenInfo *, Picture);
void compositorXRenderPrepare (CWindow *);
void compositorXRenderWindow (CWindow *, Picture, XserverRegion, double, gint, gint);
void compositorXRenderRoot (ScreenInfo *, Picture, XserverRegion);
#endif
#endif
