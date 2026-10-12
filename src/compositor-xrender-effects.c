/* SPDX-License-Identifier: GPL-2.0-or-later
 * XRender image ownership and rendering. Timing, easing, motion geometry,
 * icon targets and Peek policy are shared with the GL backend. */
#include "config.h"
#ifdef HAVE_COMPOSITOR
#include <math.h>
#include <stdio.h>
#include <X11/Xutil.h>
#include "compositor-xrender-effects.h"
#include "compositor.h"

typedef enum { XR_MINIMIZE, XR_OPEN, XR_CLOSE } XRPhase;
typedef struct _XfwmXRenderAnimation
{
    Picture picture;
    Pixmap pixmap;
    cairo_rectangle_int_t source, target, bounds;
    XfwmAnimationEdge edge;
    gint64 started, requested, duration;
    double from, to, progress;
    XRPhase phase;
    gboolean pending, cached, sticky;
    guint workspace, frames;
    Window window;
    GArray *above;
    gboolean whisker;
} XRAnimation;

typedef struct { GList *closing; guint timer; gint64 capture_last; guint capture_sequence; } XRData;
static XRData *xr_data (ScreenInfo *screen)
{
    if (!screen->xrender_effects) screen->xrender_effects = g_new0 (XRData, 1);
    return screen->xrender_effects;
}
static void xr_damage (ScreenInfo *screen, XRAnimation *a)
{ compositorDamageRegion (screen, &a->bounds); }
static void xr_free (ScreenInfo *screen, XRAnimation *a)
{
    if (!a) return;
    xr_damage (screen, a);
    Display *display = screen->display_info->dpy;
    if (a->picture) XRenderFreePicture (display, a->picture);
    if (a->pixmap) XFreePixmap (display, a->pixmap);
    if (a->above) g_array_unref (a->above);
    g_free (a);
}
void xfwmXRCancelWindow (CWindow *cw)
{ xr_free (cw->screen_info, cw->xr_animation); cw->xr_animation = NULL; }
void xfwmXRFreeWindow (CWindow *cw)
{
    cw->xr_open_pending = FALSE;
    xfwmXRCancelWindow (cw);
    xr_free (cw->screen_info, cw->xr_close_snapshot); cw->xr_close_snapshot = NULL;
}
void xfwmXRFreeClose (CWindow *cw)
{ xr_free (cw->screen_info, cw->xr_close_snapshot); cw->xr_close_snapshot = NULL; }
void xfwmXRCancel (ScreenInfo *screen)
{
    for (GList *li = screen->cwindows; li; li = li->next) xfwmXRFreeWindow (li->data);
    XRData *data = screen->xrender_effects;
    if (!data) return;
    for (GList *li = data->closing; li; li = li->next) xr_free (screen, li->data);
    g_clear_pointer (&data->closing, g_list_free);
    if (data->timer) { g_source_remove (data->timer); data->timer = 0; }
}
void xfwmXRFinish (ScreenInfo *screen)
{ xfwmXRCancel (screen); g_clear_pointer (&screen->xrender_effects, g_free); }

static guint64 xr_bytes (ScreenInfo *screen)
{
    guint64 bytes = 0;
    for (GList *li = screen->cwindows; li; li = li->next)
    {
        CWindow *cw = li->data;
        XRAnimation *images[] = {cw->xr_animation, cw->xr_close_snapshot};
        for (guint i = 0; i < G_N_ELEMENTS (images); ++i)
            if (images[i] && images[i]->picture)
                bytes += (guint64) images[i]->source.width * images[i]->source.height * 4;
    }
    for (GList *li = xr_data (screen)->closing; li; li = li->next)
    {
        XRAnimation *a = li->data;
        bytes += (guint64) a->source.width * a->source.height * 4;
    }
    return bytes;
}
static void xr_bounds (XRAnimation *a)
{
    a->bounds.x = MIN (a->source.x, a->target.x) - 2;
    a->bounds.y = MIN (a->source.y, a->target.y) - 2;
    a->bounds.width = MAX (a->source.x + a->source.width, a->target.x + a->target.width) + 2 - a->bounds.x;
    a->bounds.height = MAX (a->source.y + a->source.height, a->target.y + a->target.height) + 2 - a->bounds.y;
}
static gboolean xr_reserve (ScreenInfo *screen, guint64 additional, XRAnimation *protected, gboolean evict)
{
    const guint64 budget = 64ULL * 1024 * 1024;
    if (additional > budget) return FALSE;
    while (xr_bytes (screen) > budget - additional)
    {
        if (!evict) return FALSE;
        CWindow *owner = NULL;
        XRAnimation *oldest = NULL;
        gboolean close = FALSE;
        for (GList *li = screen->cwindows; li; li = li->next)
        {
            CWindow *cw = li->data;
            XRAnimation *images[] = {cw->xr_animation, cw->xr_close_snapshot};
            for (guint i = 0; i < G_N_ELEMENTS (images); ++i)
                if (images[i] && images[i] != protected && images[i]->picture &&
                    (i == 1 || images[i]->cached) && (!oldest || images[i]->started < oldest->started))
                { oldest = images[i]; owner = cw; close = i == 1; }
        }
        if (!oldest) return FALSE;
        if (close) xfwmXRFreeClose (owner);
        else xfwmXRCancelWindow (owner);
    }
    return TRUE;
}
static gboolean xr_snapshot (CWindow *cw, XRAnimation *a, gboolean evict)
{
    ScreenInfo *screen = cw->screen_info;
    DisplayInfo *display = screen->display_info;
    cairo_rectangle_int_t source;
    compositorXRenderPrepare (cw);
    XserverRegion region = win_extents (cw, &source);
    gboolean reuse = a->picture && source.width == a->source.width && source.height == a->source.height;
    guint64 bytes = (guint64) MAX (0, source.width) * MAX (0, source.height) * 4;
    XRenderPictFormat *format = XRenderFindStandardFormat (display->dpy, PictStandardARGB32);
    if (!region || !cw->picture || !format || source.width <= 0 || source.height <= 0 ||
        source.width > 8192 || source.height > 8192 ||
        (!reuse && !xr_reserve (screen, bytes, a, evict)))
    { if (region) XFixesDestroyRegion (display->dpy, region); return FALSE; }
    myDisplayErrorTrapPush (display);
    Pixmap pixmap = reuse ? a->pixmap : XCreatePixmap (display->dpy, screen->xroot, source.width, source.height, 32);
    Picture picture = reuse ? a->picture : XRenderCreatePicture (display->dpy, pixmap, format, 0, NULL);
    XRenderPictureAttributes no_clip = {.clip_mask = None};
    XRenderChangePicture (display->dpy, picture, CPClipMask, &no_clip);
    XRenderColor clear = {0, 0, 0, 0};
    XRenderFillRectangle (display->dpy, PictOpSrc, picture, &clear, 0, 0, source.width, source.height);
    compositorXRenderWindow (cw, picture, region, 1, source.x, source.y);
    XFixesDestroyRegion (display->dpy, region);
    if (myDisplayErrorTrapPop (display) != Success)
    {
        if (!reuse)
        {
            myDisplayErrorTrapPush (display);
            if (picture) XRenderFreePicture (display->dpy, picture);
            if (pixmap) XFreePixmap (display->dpy, pixmap);
            myDisplayErrorTrapPopIgnored (display);
        }
        return FALSE;
    }
    if (!reuse)
    {
        if (a->picture) XRenderFreePicture (display->dpy, a->picture);
        if (a->pixmap) XFreePixmap (display->dpy, a->pixmap);
    }
    a->picture = picture; a->pixmap = pixmap; a->source = source;
    cw->xr_snapshot_dirty = FALSE;
    xr_bounds (a);
    if (g_getenv ("XFWM4_ANIMATION_DEBUG"))
        g_message ("xrender snapshot 0x%lx %s: bytes=%" G_GUINT64_FORMAT,
                   cw->id, reuse ? "refresh" : "allocate", xr_bytes (screen));
    return TRUE;
}
static gboolean xr_allowed (CWindow *cw)
{
    if (cw->whisker_menu)
        return cw->screen_info->compositor_active && !cw->screen_info->use_gl_render &&
               WIN_IS_REDIRECTED (cw);
    return cw->screen_info->compositor_active && !cw->screen_info->use_gl_render &&
           compositorEffectWindow (cw) && WIN_IS_REDIRECTED (cw) &&
           !cw->screen_info->show_desktop &&
           (FLAG_TEST (cw->c->flags, CLIENT_FLAG_STICKY) ||
            cw->c->win_workspace == cw->screen_info->current_ws);
}
static double xr_progress (XRAnimation *a, gint64 now)
{ return xfwm_animation_progress (a->from, a->to, now - a->started, a->duration); }
static void xr_target (CWindow *cw, XRAnimation *a)
{
    if (a->phase == XR_MINIMIZE) compositorIconTarget (cw, &a->target, &a->edge);
    else
    {
        compositorWindowTarget (cw->screen_info, &a->source, a->whisker, &a->target);
    }
    xr_bounds (a);
}
static void xr_advance (ScreenInfo *screen)
{
    gint64 now = g_get_monotonic_time ();
    for (GList *li = screen->cwindows; li; li = li->next)
    {
        CWindow *cw = li->data;
        XRAnimation *a = cw->xr_animation;
        if (!a || a->cached) continue;
        xr_damage (screen, a);
        if (!xr_allowed (cw) || (!a->whisker &&
            (a->phase == XR_MINIMIZE ? screen->params->minimize_effect != 1 : screen->params->window_effect == 0)))
        { xfwmXRCancelWindow (cw); continue; }
        if (a->pending)
        {
            if (WIN_IS_PAINTABLE (cw) && xr_snapshot (cw, a, TRUE))
            { a->pending = FALSE; a->started = now = g_get_monotonic_time (); xr_target (cw, a); }
            else
            {
                if (now - a->requested > 250000) xfwmXRCancelWindow (cw);
                continue;
            }
        }
        if (a->phase == XR_OPEN && cw->xr_snapshot_dirty && xr_snapshot (cw, a, TRUE)) xr_target (cw, a);
        a->progress = xr_progress (a, now);
        if (now - a->started >= a->duration)
        {
            if (g_getenv ("XFWM4_ANIMATION_DEBUG"))
                g_message ("xrender animation 0x%lx finished: phase=%d frames=%u", cw->id, a->phase, a->frames);
            if (a->phase == XR_OPEN)
            {
                xr_free (screen, cw->xr_close_snapshot);
                cw->xr_close_snapshot = a; cw->xr_animation = NULL; a->cached = TRUE;
            }
            else if (cw->c && a->to >= 1 && FLAG_TEST (cw->c->flags, CLIENT_FLAG_ICONIFIED)) a->cached = TRUE;
            else xfwmXRCancelWindow (cw);
        }
    }
    XRData *data = xr_data (screen);
    for (GList *li = data->closing; li; )
    {
        GList *next = li->next;
        XRAnimation *a = li->data;
        xr_damage (screen, a);
        if ((!a->whisker && (screen->show_desktop || screen->params->window_effect == 0)) ||
            (!a->sticky && a->workspace != screen->current_ws) || now - a->started >= a->duration)
        { xr_free (screen, a); data->closing = g_list_delete_link (data->closing, li); }
        else a->progress = xr_progress (a, now);
        li = next;
    }
}
static gboolean xr_tick (gpointer user)
{
    ScreenInfo *screen = user;
    XRData *data = xr_data (screen);
    xr_advance (screen);
    gboolean active = data->closing != NULL;
    for (GList *li = screen->cwindows; li; li = li->next)
    {
        XRAnimation *a = ((CWindow *) li->data)->xr_animation;
        if (a && !a->cached) active = TRUE;
    }
    if (!active) { data->timer = 0; return G_SOURCE_REMOVE; }
    return G_SOURCE_CONTINUE;
}
static void xr_start (ScreenInfo *screen, XRAnimation *a)
{
    XRData *data = xr_data (screen);
    xr_damage (screen, a);
    if (!data->timer) data->timer = g_timeout_add (8, xr_tick, screen);
}
void xfwmXRAnimateOpen (CWindow *cw)
{
    if (g_getenv ("XFWM4_ANIMATION_DEBUG"))
        g_message ("xrender open request 0x%lx: allowed=%d effect=%d", cw->id, xr_allowed (cw), cw->screen_info->params->window_effect);
    if (!xr_allowed (cw) || (!cw->whisker_menu &&
        (cw->screen_info->params->window_effect == 0 || FLAG_TEST (cw->c->flags, CLIENT_FLAG_ICONIFIED)))) return;
    xfwmXRCancelWindow (cw);
    XRAnimation *a = g_new0 (XRAnimation, 1);
    if (cw->whisker_menu)
    {
        XRData *data = xr_data (cw->screen_info);
        for (GList *li = data->closing; li; )
        {
            GList *next = li->next;
            XRAnimation *old = li->data;
            if (old->whisker && old->window == cw->id)
            { xr_free (cw->screen_info, old); data->closing = g_list_delete_link (data->closing, li); }
            li = next;
        }
    }
    a->phase = XR_OPEN; a->pending = TRUE; a->from = a->progress = 1;
    a->whisker = cw->whisker_menu;
    a->duration = xfwm_animation_duration (cw->screen_info->params->window_duration, 1, 0);
    a->started = a->requested = g_get_monotonic_time ();
    a->source = (cairo_rectangle_int_t) {cw->attr.x, cw->attr.y, cw->attr.width, cw->attr.height};
    xr_target (cw, a); cw->xr_animation = a; xr_start (cw->screen_info, a);
}
void xfwmXRCacheClose (CWindow *cw)
{
    if (!xr_allowed (cw) || !WIN_IS_PAINTABLE (cw) ||
        (!cw->whisker_menu && cw->screen_info->params->window_effect == 0) || cw->xr_animation ||
        (cw->xr_close_snapshot && !cw->xr_snapshot_dirty)) return;
    XRAnimation *a = cw->xr_close_snapshot;
    if (!a) a = g_new0 (XRAnimation, 1);
    if (!xr_snapshot (cw, a, FALSE)) { if (!a->picture) g_free (a); return; }
    a->cached = TRUE; a->started = g_get_monotonic_time (); cw->xr_close_snapshot = a;
}
void xfwmXRAnimateClose (CWindow *cw)
{
    if (g_getenv ("XFWM4_ANIMATION_DEBUG") && cw->whisker_menu)
        g_message ("Whisker close: viewable=%d snapshot=%p active=%p", cw->viewable, cw->xr_close_snapshot, cw->xr_animation);
    if (!xr_allowed (cw) || !WIN_IS_VIEWABLE (cw) || (!cw->whisker_menu &&
        (cw->screen_info->params->window_effect == 0 || compositorPeek (cw->screen_info)->phase > .001))) return;
    XRAnimation *a = cw->xr_animation;
    double from = 0;
    if (a && a->phase == XR_MINIMIZE && !a->cached) return;
    if (a && a->phase == XR_OPEN && a->picture)
    { from = xr_progress (a, g_get_monotonic_time ()); cw->xr_animation = NULL; }
    else { a = cw->xr_close_snapshot; cw->xr_close_snapshot = NULL; }
    if (!a || !a->picture) { xr_free (cw->screen_info, a); return; }
    a->phase = XR_CLOSE; a->cached = a->pending = FALSE;
    a->whisker = cw->whisker_menu;
    a->window = cw->id; a->workspace = cw->c ? cw->c->win_workspace : cw->screen_info->current_ws;
    a->sticky = a->whisker || FLAG_TEST (cw->c->flags, CLIENT_FLAG_STICKY);
    a->from = a->progress = from; a->to = 1; a->started = g_get_monotonic_time ();
    a->duration = xfwm_animation_duration (cw->screen_info->params->window_duration, from, 1);
    xr_target (cw, a); a->above = g_array_new (FALSE, FALSE, sizeof (Window));
    for (GList *li = cw->screen_info->cwindows; li && li->data != cw; li = li->next)
    { Window id = ((CWindow *) li->data)->id; g_array_append_val (a->above, id); }
    xr_data (cw->screen_info)->closing = g_list_append (xr_data (cw->screen_info)->closing, a);
    xr_start (cw->screen_info, a);
}
void xfwmXRAnimateWindow (CWindow *cw, gboolean restore)
{
    if (!xr_allowed (cw)) return;
    XRAnimation *a = cw->xr_animation;
    if (a && a->phase != XR_MINIMIZE) { xfwmXRCancelWindow (cw); a = NULL; }
    xr_free (cw->screen_info, cw->xr_close_snapshot); cw->xr_close_snapshot = NULL;
    gboolean enabled = cw->screen_info->params->minimize_effect == 1;
    if (restore && !enabled) { xfwmXRCancelWindow (cw); return; }
    if (a && a->picture)
    { a->from = a->cached ? 1 : xr_progress (a, g_get_monotonic_time ()); a->cached = a->pending = FALSE; }
    else
    {
        xfwmXRCancelWindow (cw); a = g_new0 (XRAnimation, 1); a->phase = XR_MINIMIZE;
        a->from = restore ? 1 : 0; a->pending = restore; a->requested = g_get_monotonic_time ();
        a->source = (cairo_rectangle_int_t) {cw->attr.x, cw->attr.y, cw->attr.width, cw->attr.height};
        cw->xr_animation = a; xr_target (cw, a);
        if (!restore && (!WIN_IS_VIEWABLE (cw) || !xr_snapshot (cw, a, TRUE)))
        { xfwmXRCancelWindow (cw); return; }
    }
    if (!enabled) { a->cached = TRUE; return; } /* Retained for minimized Peek. */
    xr_target (cw, a); a->to = restore ? 0 : 1; a->started = g_get_monotonic_time (); a->progress = a->from;
    a->duration = xfwm_animation_duration (cw->screen_info->params->minimize_duration, a->from, a->to);
    xr_start (cw->screen_info, a);
    if (g_getenv ("XFWM4_ANIMATION_DEBUG"))
        g_message ("xrender animation 0x%lx %s: effect=1", cw->id, restore ? "restore" : "minimize");
}
void xfwmXRConfigure (CWindow *cw)
{
    if (cw->xr_animation && cw->xr_animation->phase == XR_OPEN &&
        (cw->xr_animation->pending || cw->xr_animation->frames == 0)) cw->xr_animation->requested = g_get_monotonic_time ();
    else xfwmXRCancelWindow (cw);
    xr_free (cw->screen_info, cw->xr_close_snapshot); cw->xr_close_snapshot = NULL;
    cw->xr_snapshot_dirty = TRUE;
}

static void xr_draw (ScreenInfo *screen, Picture buffer, XserverRegion clip, XRAnimation *a, double opacity, gboolean moving)
{
    if (!a || !a->picture || a->pending) return;
    Display *display = screen->display_info->dpy;
    XfwmAnimationRect source = {a->source.x, a->source.y, a->source.width, a->source.height};
    XfwmAnimationRect target = {a->target.x, a->target.y, a->target.width, a->target.height};
    double x1 = source.x, y1 = source.y, x2 = x1 + source.width, y2 = y1 + source.height;
    if (moving)
    {
        XfwmAnimationEffect effect = a->whisker ? XFWM_ANIMATION_SLIDE : XFWM_ANIMATION_SCALE_FADE;
        xfwm_animation_vertex (effect, a->edge, &source, &target, a->progress, 0, 0, &x1, &y1);
        xfwm_animation_vertex (effect, a->edge, &source, &target, a->progress, 1, 1, &x2, &y2);
        opacity *= xfwm_animation_opacity (effect, a->progress);
    }
    if (opacity <= .0001 || x2 <= x1 || y2 <= y1) return;
    gint x = floor (x1), y = floor (y1), width = MAX (1, (gint) ceil (x2) - x), height = MAX (1, (gint) ceil (y2) - y);
    double sx = source.width / (x2 - x1), sy = source.height / (y2 - y1);
    XTransform transform = {{{XDoubleToFixed (sx), 0, XDoubleToFixed ((x - x1) * sx)},
                             {0, XDoubleToFixed (sy), XDoubleToFixed ((y - y1) * sy)}, {0, 0, XDoubleToFixed (1)}}};
    XRenderSetPictureTransform (display, a->picture, &transform);
    XRenderSetPictureFilter (display, a->picture, FilterBilinear, NULL, 0);
    Pixmap mask_pixmap = XCreatePixmap (display, screen->xroot, 1, 1, 8);
    XRenderPictureAttributes attributes = {.repeat = True};
    Picture mask = XRenderCreatePicture (display, mask_pixmap, XRenderFindStandardFormat (display, PictStandardA8), CPRepeat, &attributes);
    XRenderColor alpha = {0, 0, 0, (gushort) (CLAMP (opacity, 0, 1) * 65535)};
    XRenderFillRectangle (display, PictOpSrc, mask, &alpha, 0, 0, 1, 1);
    XFixesSetPictureClipRegion (display, buffer, 0, 0, clip);
    XRenderComposite (display, PictOpOver, a->picture, mask, buffer, 0, 0, 0, 0, x, y, width, height);
    XRenderFreePicture (display, mask); XFreePixmap (display, mask_pixmap);
    XTransform identity = {{{XDoubleToFixed (1), 0, 0}, {0, XDoubleToFixed (1), 0}, {0, 0, XDoubleToFixed (1)}}};
    XRenderSetPictureTransform (display, a->picture, &identity);
    ++a->frames;
}
static CWindow *xr_find (ScreenInfo *screen, Window id)
{
    for (GList *li = screen->cwindows; li; li = li->next)
    { CWindow *cw = li->data; if (cw->id == id && WIN_IS_PAINTABLE (cw)) return cw; }
    return NULL;
}
static void xr_closing_before (ScreenInfo *screen, Picture buffer, XserverRegion clip, Window above)
{
    for (GList *li = xr_data (screen)->closing; li; li = li->next)
    {
        XRAnimation *a = li->data;
        Window anchor = None;
        for (guint i = a->above->len; i > 0; --i)
        { Window id = g_array_index (a->above, Window, i - 1); if (xr_find (screen, id)) { anchor = id; break; } }
        if (anchor == above) xr_draw (screen, buffer, clip, a, 1, TRUE);
    }
}
static void xr_selected (ScreenInfo *screen, Picture buffer, XserverRegion clip, Window id, double opacity)
{
    CWindow *cw = compositorPeekWindow (screen, id);
    if (!cw || opacity <= .001) return;
    if (cw->xr_animation && cw->xr_animation->cached) xr_draw (screen, buffer, clip, cw->xr_animation, opacity, FALSE);
    else if (WIN_IS_PAINTABLE (cw)) compositorXRenderWindow (cw, buffer, clip, opacity, 0, 0);
}
void xfwmXRCaptureFrame (ScreenInfo *screen, Picture buffer)
{
    const gchar *directory = g_getenv ("XFWM4_ANIMATION_CAPTURE_DIR");
    if (!directory || !g_getenv ("XFWM4_ANIMATION_TEST_SESSION")) return;
    XRData *data = xr_data (screen);
    gint64 now = g_get_monotonic_time ();
    if (now - data->capture_last < 50000) return;
    data->capture_last = now;
    for (guint i = 0; i < N_BUFFERS; ++i)
        if (screen->rootBuffer[i] == buffer && screen->rootPixmap[i] != None)
        {
            gint cx = 0, cy = 0, width = screen->width, height = screen->height;
            const gchar *rectangle = g_getenv ("XFWM4_ANIMATION_CAPTURE_RECT");
            if (rectangle)
            {
                if (sscanf (rectangle, "%d,%d,%d,%d", &cx, &cy, &width, &height) != 4) return;
                cx = CLAMP (cx, 0, screen->width - 1); cy = CLAMP (cy, 0, screen->height - 1);
                width = CLAMP (width, 1, screen->width - cx); height = CLAMP (height, 1, screen->height - cy);
            }
            XImage *source = XGetImage (screen->display_info->dpy, screen->rootPixmap[i],
                                        cx, cy, width, height, AllPlanes, ZPixmap);
            GdkPixbuf *image = source ? gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, width, height) : NULL;
            if (image)
            {
                guint8 *pixels = gdk_pixbuf_get_pixels (image);
                gint stride = gdk_pixbuf_get_rowstride (image);
                unsigned long masks[] = {screen->visual->red_mask, screen->visual->green_mask, screen->visual->blue_mask};
                guint shifts[3] = {0, 0, 0};
                for (guint c = 0; c < 3; ++c)
                    while (masks[c] && !(masks[c] & (1UL << shifts[c]))) ++shifts[c];
                for (gint y = 0; y < height; ++y)
                    for (gint x = 0; x < width; ++x)
                    {
                        unsigned long raw = XGetPixel (source, x, y);
                        for (guint c = 0; c < 3; ++c)
                            pixels[y * stride + x * 3 + c] = masks[c] ? ((raw & masks[c]) >> shifts[c]) * 255 / (masks[c] >> shifts[c]) : 0;
                    }
            }
            if (source) XDestroyImage (source);
            if (image)
            {
                gchar *path = g_strdup_printf ("%s/frame-%06u.png", directory, data->capture_sequence++);
                gdk_pixbuf_save (image, path, "png", NULL, "compression", "0", NULL);
                g_free (path); g_object_unref (image);
            }
            break;
        }
}
gboolean xfwmXRPaintEffects (ScreenInfo *screen, Picture buffer, XserverRegion damage)
{
    xr_advance (screen);
    XfwmPeek *peek = compositorPeek (screen);
    gboolean active = xr_data (screen)->closing != NULL || peek->phase > .001;
    for (GList *li = screen->cwindows; li; li = li->next)
    { XRAnimation *a = ((CWindow *) li->data)->xr_animation; if (a && !a->cached) active = TRUE; }
    if (!active) return FALSE;
    /* Effects change visibility/geometry; repaint the scene bottom-up rather
     * than applying the ordinary opaque-window culling to transformed images. */
    XRectangle whole = {0, 0, screen->width, screen->height};
    XFixesSetRegion (screen->display_info->dpy, damage, &whole, 1);
    compositorXRenderRoot (screen, buffer, damage);
    for (GList *li = g_list_last (screen->cwindows); li; li = li->prev)
    {
        CWindow *cw = li->data;
        xr_closing_before (screen, buffer, damage, cw->id);
        if (cw->xr_animation && !cw->xr_animation->cached && !cw->xr_animation->pending)
            xr_draw (screen, buffer, damage, cw->xr_animation, compositorPeekFactor (cw), TRUE);
        else if (WIN_IS_PAINTABLE (cw))
            compositorXRenderWindow (cw, buffer, damage, compositorPeekFactor (cw), 0, 0);
    }
    xr_closing_before (screen, buffer, damage, None);
    if (peek->phase > .001)
    {
        double switching = peek->previous != None ? peek->switching : 1;
        xr_selected (screen, buffer, damage, peek->previous, peek->phase * (1 - switching));
        xr_selected (screen, buffer, damage, peek->window, peek->phase * switching);
        for (GList *li = g_list_last (screen->cwindows); li; li = li->prev)
        {
            CWindow *cw = li->data;
            if ((WIN_IS_DOCK (cw) || WIN_IS_OVERRIDE (cw)) && WIN_IS_PAINTABLE (cw))
                compositorXRenderWindow (cw, buffer, damage, 1, 0, 0);
        }
    }
    return TRUE;
}
#endif
