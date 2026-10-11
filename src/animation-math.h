/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 crims0n <crims0n@minios.dev>
 * Backend-independent timing and geometry shared by GL and XRender.
 */
#ifndef XFWM_ANIMATION_MATH_H
#define XFWM_ANIMATION_MATH_H

typedef enum
{
    XFWM_ANIMATION_NONE,
    XFWM_ANIMATION_SCALE_FADE,
    XFWM_ANIMATION_MAGIC_LAMP
} XfwmAnimationEffect;

typedef enum
{
    XFWM_ANIMATION_TOP,
    XFWM_ANIMATION_BOTTOM,
    XFWM_ANIMATION_LEFT,
    XFWM_ANIMATION_RIGHT
} XfwmAnimationEdge;

typedef struct
{
    double x, y, width, height;
} XfwmAnimationRect;

double xfwm_animation_ease (double progress);
double xfwm_animation_progress (double from, double to, double elapsed, double duration);
double xfwm_animation_duration (double milliseconds, double from, double to);
XfwmAnimationRect xfwm_animation_window_target (const XfwmAnimationRect *source, int effect);
void xfwm_animation_vertex (XfwmAnimationEffect effect, XfwmAnimationEdge edge,
                            const XfwmAnimationRect *source,
                            const XfwmAnimationRect *icon,
                            double progress, double u, double v,
                            double *x, double *y);
double xfwm_animation_opacity (XfwmAnimationEffect effect, double progress);

#endif
