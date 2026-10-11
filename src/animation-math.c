/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2008 Martin Gräßlin <mgraesslin@kde.org>
 * Copyright (C) 2026 crims0n <crims0n@minios.dev>
 *
 * Magic Lamp's cubic vertex-dependent displacement is adapted from KWin,
 * src/plugins/magiclamp/magiclamp.cpp (based on Rivo Laks' minimize effect),
 * revision aaba4a66778ea05693d26cb1499f2204f3e1c82d.
 * https://invent.kde.org/plasma/kwin/-/tree/aaba4a66778ea05693d26cb1499f2204f3e1c82d/src/plugins/magiclamp
 *
 * Here all four directions share a canonical coordinate system. We also guard
 * zero distances and handle windows extending underneath the target panel.
 * No Qt/KWin runtime or effect plugin ABI is required.
 */
#include "animation-math.h"
#include <math.h>

static double
unit (double value)
{
    return fmax (0.0, fmin (1.0, value));
}

static double
lerp (double a, double b, double t)
{
    return a + (b - a) * t;
}

double
xfwm_animation_ease (double progress)
{
    /* Smooth endpoints; reversing the timeline preserves the current image. */
    double p = unit (progress);
    return p * p * (3.0 - 2.0 * p);
}

double
xfwm_animation_progress (double from, double to, double elapsed, double duration)
{
    return lerp (from, to, unit (elapsed / fmax (duration, 1.0)));
}

double
xfwm_animation_duration (double milliseconds, double from, double to)
{
    return fmax (1.0, milliseconds * 1000.0 * fabs (to - from));
}

XfwmAnimationRect
xfwm_animation_window_target (const XfwmAnimationRect *source, int effect)
{
    XfwmAnimationRect target = *source;
    if (effect == 1)
    {
        target.width = fmax (1, (int) (source->width * .92));
        target.height = fmax (1, (int) (source->height * .92));
        target.x += (int) (source->width - target.width) / 2;
        target.y += (int) (source->height - target.height) / 2;
    }
    return target;
}

void
xfwm_animation_vertex (XfwmAnimationEffect effect, XfwmAnimationEdge edge,
                       const XfwmAnimationRect *source,
                       const XfwmAnimationRect *icon,
                       double progress, double u, double v, double *x, double *y)
{
    double p = xfwm_animation_ease (progress);
    double length, q, cross, target, cross_target;
    double factor, offset, distance, squeeze, travel, reached;

    *x = source->x + u * source->width;
    *y = source->y + v * source->height;
    if (p <= 0.0 || effect == XFWM_ANIMATION_NONE ||
        source->width <= 0.0 || source->height <= 0.0)
    {
        return;
    }
    if (effect == XFWM_ANIMATION_SCALE_FADE)
    {
        *x = lerp (*x, icon->x + u * icon->width, p);
        *y = lerp (*y, icon->y + v * icon->height, p);
        return;
    }

    if (edge == XFWM_ANIMATION_BOTTOM || edge == XFWM_ANIMATION_TOP)
    {
        length = source->height;
        q = (edge == XFWM_ANIMATION_BOTTOM ? v : 1.0 - v) * length;
        cross = u * source->width;
        cross_target = icon->x - source->x + u * icon->width;
        target = edge == XFWM_ANIMATION_BOTTOM
                 ? icon->y - source->y
                 : source->y + source->height - icon->y - icon->height;
    }
    else
    {
        length = source->width;
        q = (edge == XFWM_ANIMATION_RIGHT ? u : 1.0 - u) * length;
        cross = v * source->height;
        cross_target = icon->y - source->y + v * icon->height;
        target = edge == XFWM_ANIMATION_RIGHT
                 ? icon->x - source->x
                 : source->x + source->width - icon->x - icon->width;
    }

    /* KWin's cubic displacement creates the funnel starting at the edge
     * nearest the taskbar. Keep the denominator nonnegative even when the
     * source overlaps the panel; shift to the real target continuously. */
    travel = fmax (target, length);
    factor = (q + (length - q) * p) / length;
    offset = (travel + q) * p * factor * factor * factor;
    distance = travel - q;
    squeeze = unit (offset / fmax (distance, fmax (1.0, length * 0.05)));
    reached = fmin (travel, q + offset);
    reached = lerp (reached, reached + target - travel, p * p);
    reached = fmax (fmin (0.0, target), fmin (fmax (length, target), reached));
    cross = lerp (cross, cross_target, squeeze);

    if (edge == XFWM_ANIMATION_BOTTOM || edge == XFWM_ANIMATION_TOP)
    {
        *x = source->x + cross;
        *y = edge == XFWM_ANIMATION_BOTTOM
             ? source->y + reached : source->y + source->height - reached;
    }
    else
    {
        *y = source->y + cross;
        *x = edge == XFWM_ANIMATION_RIGHT
             ? source->x + reached : source->x + source->width - reached;
    }
}

double
xfwm_animation_opacity (XfwmAnimationEffect effect, double progress)
{
    double p = xfwm_animation_ease (progress);
    /* Lamp retains its texture until the final part of the collapse. */
    return effect == XFWM_ANIMATION_MAGIC_LAMP
           ? 1.0 - unit ((p - 0.85) / 0.15) : 1.0 - p;
}
