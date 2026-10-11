/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "compositor-damage.h"
#include <assert.h>
#include <stdio.h>

int
main (void)
{
    cairo_rectangle_int_t screen = {0, 0, 1000, 800};
    cairo_rectangle_int_t raw = {120, 120, 2, 2}, old = {900, 600, 10, 10};
    /* Reverse dependency order requires more than one expansion iteration. */
    cairo_rectangle_int_t samples[] = {{180, 100, 100, 100}, {100, 100, 100, 100}};
    cairo_region_t *history[4] = {NULL}, *current, *repair, *swap_region;
    guint index = 0;
    gint rects[16], count;
    gboolean whole;

    xfwm_damage_record (history, &index, 4, cairo_region_create_rectangle (&old));
    current = cairo_region_create_rectangle (&raw);
    xfwm_damage_expand (current, samples, 2);
    assert (cairo_region_contains_point (current, 279, 199));
    assert (!cairo_region_contains_point (current, 900, 600));
    repair = xfwm_damage_repair (current, history, index, 4, 2, &screen, &whole);
    assert (!whole && cairo_region_contains_point (repair, 900, 600));
    assert (!cairo_region_contains_point (current, 900, 600));
    cairo_region_destroy (repair);

    count = xfwm_damage_swap_rects (current, screen.height, 4, rects);
    swap_region = cairo_region_create ();
    for (gint i = 0; i < count; i++)
    {
        cairo_rectangle_int_t r = {rects[4*i], screen.height - rects[4*i+1] - rects[4*i+3],
                                   rects[4*i+2], rects[4*i+3]};
        cairo_region_union_rectangle (swap_region, &r);
    }
    assert (cairo_region_equal (current, swap_region));
    cairo_region_destroy (swap_region);
    xfwm_damage_record (history, &index, 4, current);
    /* A later frame must repair the earlier blur-expanded output, but must
     * not record its old backbuffer-only repairs as new visible damage. */
    current = cairo_region_create_rectangle (&old);
    repair = xfwm_damage_repair (current, history, index, 4, 2, &screen, &whole);
    assert (!whole && cairo_region_contains_point (repair, 279, 199));
    assert (!cairo_region_contains_point (current, 279, 199));
    cairo_region_destroy (repair);
    xfwm_damage_record (history, &index, 4, current);
    assert (!cairo_region_contains_point (history[2], 279, 199));
    repair = xfwm_damage_repair (history[2], history, index, 4, 3, &screen, &whole);
    assert (!whole && cairo_region_contains_point (repair, 279, 199));
    cairo_region_destroy (repair);

    for (guint i = 0; i < 6; i++)
        xfwm_damage_record (history, &index, 4, cairo_region_create_rectangle (&old));
    repair = xfwm_damage_repair (history[0], history, index, 4, 1, &screen, &whole);
    assert (!whole && !cairo_region_contains_point (repair, 279, 199));
    cairo_region_destroy (repair);
    repair = xfwm_damage_repair (history[0], history, index, 4, 0, &screen, &whole);
    assert (whole && cairo_region_contains_rectangle (repair, &screen) == CAIRO_REGION_OVERLAP_IN);
    cairo_region_destroy (repair);
    cairo_region_destroy (history[(index + 3) % 4]);
    history[(index + 3) % 4] = NULL;
    repair = xfwm_damage_repair (history[index], history, index, 4, 2, &screen, &whole);
    assert (whole);
    cairo_region_destroy (repair);

    current = cairo_region_create_rectangle (&raw);
    cairo_region_union_rectangle (current, &old);
    assert (xfwm_damage_swap_rects (current, 800, 1, rects) == 1);
    assert (rects[0] == 120 && rects[1] == 190 && rects[2] == 790 && rects[3] == 490);
    cairo_region_destroy (current);
    for (guint i = 0; i < 4; i++)
        if (history[i]) cairo_region_destroy (history[i]);
    puts ("PASS compositor damage: expanded-current swap rectangles, age 1/2/3 repair, no history pollution, ring wrap, missing history and bounds fallback");
    return 0;
}
