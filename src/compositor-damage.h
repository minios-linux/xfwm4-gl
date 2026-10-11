/* SPDX-License-Identifier: GPL-2.0-or-later
 * Current output damage, buffer-age repair and swap coordinates are distinct.
 * Pure region operations shared by the renderer and its regression gate.
 */
#ifndef XFWM_COMPOSITOR_DAMAGE_H
#define XFWM_COMPOSITOR_DAMAGE_H

#include <glib.h>
#include <cairo.h>

static inline void
xfwm_damage_expand (cairo_region_t *region, const cairo_rectangle_int_t *samples, guint count)
{
    gboolean expanded;
    do
    {
        expanded = FALSE;
        for (guint i = 0; i < count; i++)
        {
            if (cairo_region_contains_rectangle (region, &samples[i]) == CAIRO_REGION_OVERLAP_PART)
            {
                cairo_region_union_rectangle (region, &samples[i]);
                expanded = TRUE;
            }
        }
    } while (expanded);
}

static inline cairo_region_t *
xfwm_damage_repair (const cairo_region_t *current, cairo_region_t *const *history,
                    guint index, guint count, guint age,
                    const cairo_rectangle_int_t *screen, gboolean *whole)
{
    cairo_region_t *repair;
    *whole = age == 0 || age > count;
    if (*whole)
        return cairo_region_create_rectangle (screen);
    repair = cairo_region_copy (current);
    for (guint i = 0; i < age - 1; i++)
    {
        guint slot = (index + count - i - 1) % count;
        if (!history[slot])
        {
            cairo_region_destroy (repair);
            *whole = TRUE;
            return cairo_region_create_rectangle (screen);
        }
        cairo_region_union (repair, history[slot]);
    }
    return repair;
}

/* Takes ownership of CURRENT damage, never the age-accumulated repair region. */
static inline void
xfwm_damage_record (cairo_region_t **history, guint *index, guint count, cairo_region_t *current)
{
    if (history[*index])
        cairo_region_destroy (history[*index]);
    history[*index] = current;
    *index = (*index + 1) % count;
}

static inline gint
xfwm_damage_swap_rects (const cairo_region_t *current, gint height, gint maximum, gint *rects)
{
    gint count = cairo_region_num_rectangles (current);
    gboolean bounds = count > maximum;
    if (bounds)
        count = 1;
    for (gint i = 0; i < count; i++)
    {
        cairo_rectangle_int_t r;
        if (bounds)
            cairo_region_get_extents (current, &r);
        else
            cairo_region_get_rectangle (current, i, &r);
        rects[4 * i] = r.x;
        rects[4 * i + 1] = height - r.y - r.height;
        rects[4 * i + 2] = r.width;
        rects[4 * i + 3] = r.height;
    }
    return count;
}
#endif
