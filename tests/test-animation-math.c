/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "animation-math.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static void
check_rect (XfwmAnimationRect source, XfwmAnimationRect icon, XfwmAnimationEdge edge)
{
    int effect, t, u, v;
    double min_x = fmin (source.x, icon.x), min_y = fmin (source.y, icon.y);
    double max_x = fmax (source.x + source.width, icon.x + icon.width);
    double max_y = fmax (source.y + source.height, icon.y + icon.height);

    for (effect = XFWM_ANIMATION_SCALE_FADE; effect <= XFWM_ANIMATION_MAGIC_LAMP; ++effect)
    {
        double last_opacity = 1;
        for (t = 0; t <= 100; ++t)
        {
            double progress = t / 100.0;
            double opacity = xfwm_animation_opacity (effect, progress);
            assert (opacity <= last_opacity + 1e-9 && opacity >= 0);
            last_opacity = opacity;
            for (u = 0; u <= 10; ++u)
            {
                for (v = 0; v <= 10; ++v)
                {
                    double x, y;
                    xfwm_animation_vertex (effect, edge, &source, &icon,
                                            progress, u / 10.0, v / 10.0, &x, &y);
                    assert (isfinite (x) && isfinite (y));
                    assert (x >= min_x - 1e-6 && x <= max_x + 1e-6);
                    assert (y >= min_y - 1e-6 && y <= max_y + 1e-6);
                    if (t == 0)
                    {
                        assert (fabs (x - source.x - u / 10.0 * source.width) < 1e-6);
                        assert (fabs (y - source.y - v / 10.0 * source.height) < 1e-6);
                    }
                    if (t == 100 && effect == XFWM_ANIMATION_SCALE_FADE)
                    {
                        assert (fabs (x - icon.x - u / 10.0 * icon.width) < 1e-6);
                        assert (fabs (y - icon.y - v / 10.0 * icon.height) < 1e-6);
                    }
                    if (t == 100 && effect == XFWM_ANIMATION_MAGIC_LAMP)
                    {
                        if (edge == XFWM_ANIMATION_BOTTOM) assert (fabs (y - icon.y) < 1e-6);
                        if (edge == XFWM_ANIMATION_TOP) assert (fabs (y - icon.y - icon.height) < 1e-6);
                        if (edge == XFWM_ANIMATION_LEFT) assert (fabs (x - icon.x - icon.width) < 1e-6);
                        if (edge == XFWM_ANIMATION_RIGHT) assert (fabs (x - icon.x) < 1e-6);
                    }
                }
            }
        }
        assert (fabs (last_opacity) < 1e-6);
    }
}

int
main (void)
{
    assert (xfwm_animation_progress (0, 1, -10, 1000) == 0);
    assert (xfwm_animation_progress (0, 1, 2000, 1000) == 1);
    assert (fabs (xfwm_animation_progress (1, 0, 250, 1000) - .75) < 1e-9);
    assert (fabs (xfwm_animation_duration (800, .75, 0) - 600000) < 1e-9);
    assert (xfwm_animation_duration (800, .5, .5) == 1);
    XfwmAnimationRect window = {80, 100, 500, 350};
    XfwmAnimationRect fade = xfwm_animation_window_target (&window, 2);
    XfwmAnimationRect scale = xfwm_animation_window_target (&window, 1);
    assert (fade.x == window.x && fade.y == window.y && fade.width == window.width && fade.height == window.height);
    assert (scale.width == 460 && scale.height == 322 && scale.x == 100 && scale.y == 114);
    XfwmAnimationRect sources[] = {
        {80, 100, 500, 350}, {0, 0, 1366, 718}, {200, 350, 600, 450},
        {-150, -100, 700, 600}, {1920, 120, 900, 800}, {0, 0, 1, 1}
    };
    XfwmAnimationRect icons[] = {
        {150, 720, 48, 48}, {120, 0, 48, 48}, {0, 300, 48, 48},
        {1320, 200, 48, 48}, {300, 300, 30, 30}, {2140, 1030, 48, 48},
        {683, 767, 24, 1}, {0, 0, 1, 1}
    };
    unsigned s, i, edge;
    for (s = 0; s < sizeof sources / sizeof sources[0]; ++s)
        for (i = 0; i < sizeof icons / sizeof icons[0]; ++i)
            for (edge = 0; edge < 4; ++edge)
                check_rect (sources[s], icons[i], edge);
    puts ("PASS: shared timing/reversal/duration, Fade/Scale targets, both effects, all four edges, overlapping/offscreen/HiDPI-sized targets, finite coordinates, endpoints, bounds and opacity");
    return 0;
}
