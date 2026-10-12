/*      $Id$

        This program is free software; you can redistribute it and/or modify
        it under the terms of the GNU General Public License as published by
        the Free Software Foundation; either version 2, or (at your option)
        any later version.

        This program is distributed in the hope that it will be useful,
        but WITHOUT ANY WARRANTY; without even the implied warranty of
        MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
        GNU General Public License for more details.

        You should have received a copy of the GNU General Public License
        along with this program; if not, write to the Free Software
        Foundation, Inc., Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

        xfwm4 - (c) 2002-2011 Olivier Fourdan

        OpenGL rendering backend for the compositor.

        Windows are bound as textures with GLX_EXT_texture_from_pixmap and
        composited by the GPU straight into the back buffer of the overlay,
        which removes the intermediate root buffer the XRender path needs.

        The visible region maths are the same as the XRender path: a top down
        pass collects the opaque areas and shrinks the region left to paint,
        then a bottom up pass draws shadows and translucent windows.

 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_COMPOSITOR

#include <math.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrender.h>

#include <glib.h>

#include "display.h"
#include "screen.h"
#include "client.h"
#include "frame.h"
#include "hints.h"
#include "compositor-priv.h"
#include "compositor-gl.h"
#include "compositor.h"
#include "animation-math.h"
#include "compositor-damage.h"
#include "compositor-effects.h"

#ifdef HAVE_EPOXY

#include <epoxy/egl.h>

#define GL_DAMAGE_HISTORY       8
#define GL_MAX_DEPTHS           32
#define GL_MAX_ROOT_TILES       256
#define GL_MAX_BIND_RETRIES     3

typedef struct
{
    gint depth;
    GLXFBConfig fbconfig;
    gboolean usable;
    gboolean y_inverted;
} XfwmGLDepth;

typedef enum
{
    GL_PRESENT_SWAP,
    GL_PRESENT_COPY,
    GL_PRESENT_FBO,
    GL_PRESENT_SCENE
} XfwmGLPresentMode;

#define GL_MAX_PRESENT_RECTS    32

static const EGLint preserved_image[] = {
    EGL_IMAGE_PRESERVED_KHR, EGL_TRUE,
    EGL_NONE
};

#define GL_PROF_QUERIES         4

#define GL_PROF_AGE_BUCKETS     12

typedef enum { GL_ANIMATION_MINIMIZE, GL_ANIMATION_OPEN, GL_ANIMATION_CLOSE } XfwmGLAnimationPhase;

typedef struct _XfwmGLAnimation
{
    GLuint texture;
    cairo_rectangle_int_t source, icon, bounds;
    XfwmAnimationEffect effect;
    XfwmAnimationEdge edge;
    gint64 started, requested, duration;
    double from, to, progress;
    gboolean pending;
    /* Preserve the complete GL snapshot after minimization.  Reusing it on
     * restore avoids capturing an empty/repaint-pending XComposite pixmap. */
    gboolean cached;
    guint frames;
    XfwmGLAnimationPhase phase;
    Window window;
    guint workspace;
    gboolean sticky;
    GArray *above_windows; /* XIDs only: closed clients/CWindows are not retained. */
    gboolean whisker;
} XfwmGLAnimation;

typedef struct
{
    GLuint program_win;
    GLuint program_2d;
    GLuint program_shadow_profile;
    GLuint program_blur_down;
    GLuint program_blur_up;
    GLuint program_blur_mix;
    GLint u_opacity_win;
    GLint u_opacity_2d;
    GLint u_prof_size;
    GLint u_prof_ramp;
    GLint u_prof_opacity;
    GLint u_blur_pixel_down;
    GLint u_blur_pixel_up;
    GLint u_blur_mix_strength;

    GLuint shadow_profile;
    gfloat shadow_profile_peak;

    GLenum tex_type;
    GLenum tex_target;

    XfwmGLDepth depths[GL_MAX_DEPTHS];
    guint n_depths;

    gboolean has_buffer_age;
    XfwmGLPresentMode present_mode;

    gboolean use_fence;

    gboolean no_paint;

    gboolean wait_new_pixmap;

    EGLDisplay egl_display;
    EGLContext egl_context;
    EGLSurface egl_surface;
    EGLConfig egl_config;
    EGLBoolean (*egl_swap_with_damage) (EGLDisplay, EGLSurface,
                                        EGLint *, EGLint);
    gpointer root_egl_image;

    gboolean stats;
    guint stat_frames;
    gdouble stat_pixels;
    gdouble stat_damage_pixels;
    gdouble stat_paint_pixels;
    gdouble prof_age_frames;
    gint64 stat_since;

    gboolean profile;
    gboolean prof_gpu;
    gdouble prof_damage;
    gdouble prof_pass1;
    gdouble prof_root;
    gdouble prof_pass2;
    gdouble prof_present;
    gdouble prof_wall;
    gdouble prof_quads;
    gdouble prof_rects;
    gdouble prof_windows;
    gdouble prof_shadows;
    gdouble prof_bind;
    gdouble prof_binds;
    gdouble prof_fence_waits;
    gdouble prof_age_over;
    gdouble prof_full_repaint;
    guint prof_age_max;
    gdouble prof_age_hist[GL_PROF_AGE_BUCKETS];

    GLuint prof_query[GL_PROF_QUERIES];
    gboolean prof_query_busy[GL_PROF_QUERIES];
    guint prof_query_next;
    gint prof_query_active;
    gdouble prof_gpu_ns;
    gdouble prof_gpu_frames;

    cairo_region_t *damage_history[GL_DAMAGE_HISTORY];
    guint damage_index;
    gboolean full_repaint;

    GLXPixmap root_glx_pixmap;
    GLuint root_texture;
    GLenum root_tex_type;
    gboolean root_repeat;
    gboolean root_y_inverted;
    GLuint black_texture;
    gboolean root_missing;
    gint root_width;
    gint root_height;
    Pixmap root_pixmap;
    Damage root_damage;
    gboolean root_dirty;

    GLuint fbo;
    GLuint fbo_texture;
    GLuint blur_source_fbo;
    GLuint blur_source_texture;
    GLuint blur_original_texture;
    GLuint blur_fbo[3];
    GLuint blur_texture[3];
    gint blur_source_width;
    gint blur_source_height;
    gint blur_width[3];
    gint blur_height[3];
    gboolean blur_failed;
    gint fbo_width;
    gint fbo_height;
    GLenum fbo_filter;
    gboolean fbo_failed;

    GLuint cursor_texture;
    gint cursor_width;
    gint cursor_height;
    unsigned long cursor_serial;

    gchar *renderer;

    guint animation_timer;
    GList *closing_animations;
    gint64 capture_until, capture_last;
    guint capture_sequence;
    cairo_rectangle_int_t capture_bounds;
    gboolean capture_final;

    gfloat peek_overlay_alpha;
    gboolean peek_overlay_paint;

    gboolean snapshot_active;
    cairo_rectangle_int_t snapshot_bounds;

    gboolean give_up;
    gboolean retry_paint;
    guint bind_failures;
} XfwmGLData;

static const gchar *vertex_source =
    "varying vec2 uv;\n"
    "void main (void)\n"
    "{\n"
    "    gl_Position = gl_Vertex;\n"
    "    uv = gl_MultiTexCoord0.xy;\n"
    "}\n";

static const gchar *fragment_source_2d =
    "uniform sampler2D tex;\n"
    "uniform float opacity;\n"
    "varying vec2 uv;\n"
    "void main (void)\n"
    "{\n"
    "    gl_FragColor = texture2D (tex, uv) * opacity;\n"
    "}\n";

static const gchar *fragment_source_rect =
    "#extension GL_ARB_texture_rectangle : enable\n"
    "uniform sampler2DRect tex;\n"
    "uniform float opacity;\n"
    "varying vec2 uv;\n"
    "void main (void)\n"
    "{\n"
    "    gl_FragColor = texture2DRect (tex, uv) * opacity;\n"
    "}\n";

static const gchar *fragment_blur_down =
    "uniform sampler2D tex;\n"
    "uniform vec2 pixel;\n"
    "varying vec2 uv;\n"
    "void main (void)\n"
    "{\n"
    "    vec4 c = texture2D (tex, uv) * 4.0;\n"
    "    c += texture2D (tex, uv + pixel * vec2 (-0.5, -0.5));\n"
    "    c += texture2D (tex, uv + pixel * vec2 ( 0.5, -0.5));\n"
    "    c += texture2D (tex, uv + pixel * vec2 (-0.5,  0.5));\n"
    "    c += texture2D (tex, uv + pixel * vec2 ( 0.5,  0.5));\n"
    "    gl_FragColor = c / 8.0;\n"
    "}\n";

static const gchar *fragment_blur_up =
    "uniform sampler2D tex;\n"
    "uniform vec2 pixel;\n"
    "varying vec2 uv;\n"
    "void main (void)\n"
    "{\n"
    "    vec4 c = vec4 (0.0);\n"
    "    c += texture2D (tex, uv + pixel * vec2 (-1.0,  0.0)) * 2.0;\n"
    "    c += texture2D (tex, uv + pixel * vec2 ( 1.0,  0.0)) * 2.0;\n"
    "    c += texture2D (tex, uv + pixel * vec2 ( 0.0, -1.0)) * 2.0;\n"
    "    c += texture2D (tex, uv + pixel * vec2 ( 0.0,  1.0)) * 2.0;\n"
    "    c += texture2D (tex, uv + pixel * vec2 (-0.5, -0.5));\n"
    "    c += texture2D (tex, uv + pixel * vec2 ( 0.5, -0.5));\n"
    "    c += texture2D (tex, uv + pixel * vec2 (-0.5,  0.5));\n"
    "    c += texture2D (tex, uv + pixel * vec2 ( 0.5,  0.5));\n"
    "    gl_FragColor = c / 12.0;\n"
    "}\n";

static const gchar *fragment_blur_mix =
    "uniform sampler2D blurred;\n"
    "uniform sampler2D original;\n"
    "uniform float strength;\n"
    "varying vec2 uv;\n"
    "void main (void)\n"
    "{\n"
    "    gl_FragColor = mix (texture2D (original, uv),\n"
    "                        texture2D (blurred, uv), strength);\n"
    "}\n";

static const gchar *fragment_shadow_profile =
    "uniform sampler2D prof;\n"
    "uniform vec2 size;\n"
    "uniform float ramp;\n"
    "uniform float opacity;\n"
    "varying vec2 uv;\n"
    "void main (void)\n"
    "{\n"
    "    vec2 p = uv * size;\n"
    "    vec2 q = clamp (min (p, size - p) / ramp, 0.0, 1.0);\n"
    "    float a = texture2D (prof, vec2 (q.x, 0.5)).a\n"
    "            * texture2D (prof, vec2 (q.y, 0.5)).a;\n"
    "    gl_FragColor = vec4 (0.0, 0.0, 0.0, a * opacity);\n"
    "}\n";

static void
get_window_pixmap_size (CWindow *cw, gint *width, gint *height)
{
    if (cw->gl_pixmap_width > 0 && cw->gl_pixmap_height > 0)
    {
        *width = cw->gl_pixmap_width;
        *height = cw->gl_pixmap_height;

        return;
    }
    *width = cw->attr.width + 2 * cw->attr.border_width;
    *height = cw->attr.height + 2 * cw->attr.border_width;
}

static void
window_painted_size (CWindow *cw, gint *width, gint *height)
{
    gint pixmap_width, pixmap_height;
    gint border = 2 * cw->attr.border_width;

    get_window_pixmap_size (cw, &pixmap_width, &pixmap_height);

    *width = MIN (cw->attr.width + border, pixmap_width);
    *height = MIN (cw->attr.height + border, pixmap_height);
}

static cairo_region_t *
region_from_rects (XRectangle *rects, gint nrects, gint dx, gint dy)
{
    cairo_region_t *region;
    cairo_rectangle_int_t *boxes;
    gint i;

    if (nrects <= 0)
    {
        return cairo_region_create ();
    }

    boxes = g_new (cairo_rectangle_int_t, nrects);
    for (i = 0; i < nrects; i++)
    {
        boxes[i].x = rects[i].x + dx;
        boxes[i].y = rects[i].y + dy;
        boxes[i].width = rects[i].width;
        boxes[i].height = rects[i].height;
    }
    region = cairo_region_create_rectangles (boxes, nrects);
    g_free (boxes);

    return region;
}

static cairo_region_t *
window_shape (CWindow *cw)
{
    ScreenInfo *screen_info = cw->screen_info;
    DisplayInfo *display_info = screen_info->display_info;

    if (cw->gl_shape != NULL)
    {
        return cw->gl_shape;
    }

    if (cw->shaped)
    {
        XRectangle *rects;
        gint nrects = 0, ordering;
        gboolean answered;

        myDisplayErrorTrapPush (display_info);
        rects = XShapeGetRectangles (myScreenGetXDisplay (screen_info), cw->id,
                                     ShapeBounding, &nrects, &ordering);
        answered = (myDisplayErrorTrapPop (display_info) == Success);

        if (rects != NULL || answered)
        {
            cairo_rectangle_int_t drawn;

            cw->gl_shape = region_from_rects (rects, nrects,
                                              cw->attr.x + cw->attr.border_width,
                                              cw->attr.y + cw->attr.border_width);
            if (rects != NULL)
            {
                XFree (rects);
            }
            drawn.x = cw->attr.x;
            drawn.y = cw->attr.y;
            window_painted_size (cw, &drawn.width, &drawn.height);
            cairo_region_intersect_rectangle (cw->gl_shape, &drawn);

            return cw->gl_shape;
        }
    }

    {
        cairo_rectangle_int_t r;

        r.x = cw->attr.x;
        r.y = cw->attr.y;
        window_painted_size (cw, &r.width, &r.height);
        cw->gl_shape = cairo_region_create_rectangle (&r);
    }

    return cw->gl_shape;
}

static gboolean
window_client_area (CWindow *cw, cairo_rectangle_int_t *r)
{
    return client_area (cw, &r->x, &r->y, &r->width, &r->height);
}

static cairo_region_t *
window_opaque_region (CWindow *cw)
{
    cairo_rectangle_int_t client;
    gint dx, dy;

    if (cw->gl_opaque != NULL)
    {
        return cw->gl_opaque;
    }
    if (cw->gl_n_opaque_rects == 0)
    {
        return NULL;
    }

    if (window_client_area (cw, &client))
    {
        dx = client.x;
        dy = client.y;
    }
    else
    {
        dx = cw->attr.x + cw->attr.border_width;
        dy = cw->attr.y + cw->attr.border_width;
    }

    cw->gl_opaque = region_from_rects (cw->gl_opaque_rects,
                                       cw->gl_n_opaque_rects, dx, dy);

    cairo_region_intersect (cw->gl_opaque, window_shape (cw));
    cairo_region_intersect_rectangle (cw->gl_opaque, &client);

    return cw->gl_opaque;
}

static void
xfwmGLInvalidateOpaqueRegion (CWindow *cw)
{
    g_return_if_fail (cw != NULL);

    if (cw->gl_opaque != NULL)
    {
        cairo_region_destroy (cw->gl_opaque);
        cw->gl_opaque = NULL;
    }
}

void
xfwmGLInvalidateWindowRegions (CWindow *cw)
{
    g_return_if_fail (cw != NULL);

    if (cw->gl_shape != NULL)
    {
        cairo_region_destroy (cw->gl_shape);
        cw->gl_shape = NULL;
    }
    if (cw->gl_paint_clip != NULL)
    {
        cairo_region_destroy (cw->gl_paint_clip);
        cw->gl_paint_clip = NULL;
    }
    xfwmGLInvalidateOpaqueRegion (cw);
}

void
xfwmGLTranslateWindowRegions (CWindow *cw, gint dx, gint dy)
{
    g_return_if_fail (cw != NULL);

    if (cw->gl_shape != NULL)
    {
        cairo_region_translate (cw->gl_shape, dx, dy);
    }
    if (cw->gl_opaque != NULL)
    {
        cairo_region_translate (cw->gl_opaque, dx, dy);
    }
    if (cw->gl_paint_clip != NULL)
    {
        cairo_region_destroy (cw->gl_paint_clip);
        cw->gl_paint_clip = NULL;
    }
}

void
xfwmGLSetOpaqueRects (CWindow *cw, XRectangle *rects, gint nrects)
{
    g_return_if_fail (cw != NULL);

    g_free (cw->gl_opaque_rects);
    cw->gl_opaque_rects = NULL;
    cw->gl_n_opaque_rects = 0;
    xfwmGLInvalidateOpaqueRegion (cw);

    if (cw->screen_info->gl_data == NULL)
    {
        return;
    }

    if (rects != NULL && nrects > 0)
    {
        cw->gl_opaque_rects = g_memdup2 (rects, nrects * sizeof (XRectangle));
        cw->gl_n_opaque_rects = nrects;
    }
}

static void
set_tex_params (GLenum target, GLint filter)
{
    glTexParameteri (target, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri (target, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri (target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri (target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

static gboolean build_shadow_profile (ScreenInfo *screen_info);
static gdouble thread_cpu_ms (void);

static XfwmGLData *
gl_data (ScreenInfo *screen_info)
{
    return (XfwmGLData *) screen_info->gl_data;
}

static gboolean
gl_context_is_current (ScreenInfo *screen_info)
{
    XfwmGLData *data = gl_data (screen_info);

    if (data != NULL && screen_info->use_egl_backend)
    {
        return (data->egl_context != EGL_NO_CONTEXT) &&
               (eglGetCurrentContext () == data->egl_context);
    }

    return (screen_info->glx_context != None) &&
           (glXGetCurrentContext () == screen_info->glx_context);
}

static GLuint
compile_shader (GLenum type, const gchar *source)
{
    GLuint shader;
    GLint status;

    shader = glCreateShader (type);
    if (shader == 0)
    {
        return 0;
    }

    glShaderSource (shader, 1, &source, NULL);
    glCompileShader (shader);
    glGetShaderiv (shader, GL_COMPILE_STATUS, &status);
    if (status == GL_FALSE)
    {
        gchar log[1024];
        GLsizei len = 0;

        glGetShaderInfoLog (shader, sizeof (log) - 1, &len, log);
        log[len] = '\0';
        g_warning ("Cannot compile GL shader: %s", log);
        glDeleteShader (shader);

        return 0;
    }

    return shader;
}

static GLuint
link_program (const gchar *fragment_source)
{
    GLuint program, vertex, fragment;
    GLint status;

    vertex = compile_shader (GL_VERTEX_SHADER, vertex_source);
    if (vertex == 0)
    {
        return 0;
    }

    fragment = compile_shader (GL_FRAGMENT_SHADER, fragment_source);
    if (fragment == 0)
    {
        glDeleteShader (vertex);
        return 0;
    }

    program = glCreateProgram ();
    glAttachShader (program, vertex);
    glAttachShader (program, fragment);
    glLinkProgram (program);
    glDeleteShader (vertex);
    glDeleteShader (fragment);

    glGetProgramiv (program, GL_LINK_STATUS, &status);
    if (status == GL_FALSE)
    {
        gchar log[1024];
        GLsizei len = 0;

        glGetProgramInfoLog (program, sizeof (log) - 1, &len, log);
        log[len] = '\0';
        g_warning ("Cannot link GL program: %s", log);
        glDeleteProgram (program);

        return 0;
    }

    return program;
}

static gboolean
find_fbconfig (ScreenInfo *screen_info, gint depth, GLenum want_target,
               GLXFBConfig *fbconfig, gboolean *y_inverted)
{
    Display *dpy = myScreenGetXDisplay (screen_info);
    GLint attribs[] = {
        GLX_DRAWABLE_TYPE,   GLX_PIXMAP_BIT,
        GLX_X_RENDERABLE,    True,
        GLX_RENDER_TYPE,     GLX_RGBA_BIT,
        GLX_BUFFER_SIZE,     depth,
        GLX_DEPTH_SIZE,      0,
        GLX_STENCIL_SIZE,    0,
        None
    };
    GLXFBConfig *configs;
    gint n_configs, i;
    gboolean found = FALSE;

    configs = glXChooseFBConfig (dpy, screen_info->screen, attribs, &n_configs);
    if (configs == NULL)
    {
        return FALSE;
    }

    for (i = 0; i < n_configs; i++)
    {
        XVisualInfo *visual_info;
        int value, status;
        gboolean depth_match;

        visual_info = glXGetVisualFromFBConfig (dpy, configs[i]);
        depth_match = (visual_info != NULL && visual_info->depth == depth);
        if (visual_info)
        {
            XFree (visual_info);
        }
        if (!depth_match)
        {
            continue;
        }

        status = glXGetFBConfigAttrib (dpy, configs[i],
                                       (depth == 32) ? GLX_BIND_TO_TEXTURE_RGBA_EXT
                                                     : GLX_BIND_TO_TEXTURE_RGB_EXT,
                                       &value);
        if (status != Success || value != True)
        {
            continue;
        }

        status = glXGetFBConfigAttrib (dpy, configs[i],
                                       GLX_BIND_TO_TEXTURE_TARGETS_EXT, &value);
        if (status != Success)
        {
            continue;
        }

        if (!(value & ((want_target == GLX_TEXTURE_2D_EXT)
                       ? GLX_TEXTURE_2D_BIT_EXT : GLX_TEXTURE_RECTANGLE_BIT_EXT)))
        {
            continue;
        }

        status = glXGetFBConfigAttrib (dpy, configs[i], GLX_Y_INVERTED_EXT, &value);
        *y_inverted = (status != Success || value != False);
        *fbconfig = configs[i];
        found = TRUE;
        break;
    }
    XFree (configs);

    return found;
}

static void
set_swap_interval_gl (ScreenInfo *screen_info)
{
    XfwmGLData *data = gl_data (screen_info);
    gint interval;

    interval = wanted_swap_interval (screen_info);

    if (data != NULL && screen_info->use_egl_backend)
    {
        if (interval < 0)
        {
            interval = 1;
        }
        screen_info->glx_swap_control =
            eglSwapInterval (data->egl_display, interval);
        screen_info->glx_swap_interval = interval;
    }
    else
    {
        if ((screen_info->vblank_mode == VBLANK_ADAPTIVE) && (interval >= 0))
        {
            g_info ("GLX_EXT_swap_control_tear is missing, syncing to every vblank");
        }

        screen_info->glx_swap_control = apply_swap_interval (screen_info,
                                                             screen_info->glx_window,
                                                             &interval);
        screen_info->glx_swap_interval = interval;
    }

    if (screen_info->glx_swap_control)
    {
        g_info ("GL swap interval set to %i", interval);
    }
    else
    {
        g_info ("No swap control available, frames are not synced to the screen");
    }
}

static XfwmGLDepth *
depth_config (ScreenInfo *screen_info, gint depth)
{
    XfwmGLData *data = gl_data (screen_info);
    XfwmGLDepth *entry;
    guint i;

    for (i = 0; i < data->n_depths; i++)
    {
        if (data->depths[i].depth == depth)
        {
            return &data->depths[i];
        }
    }

    if (data->n_depths == GL_MAX_DEPTHS)
    {
        return NULL;
    }

    entry = &data->depths[data->n_depths++];
    entry->depth = depth;
    entry->usable = find_fbconfig (screen_info, depth, data->tex_target,
                                   &entry->fbconfig, &entry->y_inverted);
    if (!entry->usable)
    {
        g_info ("No GLX config to bind a window of depth %i as a texture", depth);
    }

    return entry;
}

static gboolean
depth_is_usable (ScreenInfo *screen_info, gint depth)
{
    XfwmGLDepth *dc = depth_config (screen_info, depth);

    return (dc != NULL && dc->usable);
}

static gboolean
renderer_needs_2d_target (const char *renderer)
{
    const char *needs_2d[] = { "virgl", NULL };

    if (renderer == NULL)
    {
        return FALSE;
    }

    return renderer_matches_any (renderer, needs_2d);
}

static gboolean
pick_texture_target (ScreenInfo *screen_info)
{
    XfwmGLData *data = gl_data (screen_info);
    gboolean prefer_2d;
    guint i;

    prefer_2d = renderer_needs_2d_target ((const char *) glGetString (GL_RENDERER));

    for (i = 0; i < 2; i++)
    {
        GLenum target;

        if (prefer_2d)
        {
            target = (i == 0) ? GLX_TEXTURE_2D_EXT : GLX_TEXTURE_RECTANGLE_EXT;
        }
        else
        {
            target = (i == 0) ? GLX_TEXTURE_RECTANGLE_EXT : GLX_TEXTURE_2D_EXT;
        }

        if (target == GLX_TEXTURE_RECTANGLE_EXT &&
            !epoxy_has_gl_extension ("GL_ARB_texture_rectangle") &&
            epoxy_gl_version () < 31)
        {
            continue;
        }

        data->n_depths = 0;
        data->tex_target = target;
        data->tex_type = (target == GLX_TEXTURE_2D_EXT) ? GL_TEXTURE_2D
                                                        : GL_TEXTURE_RECTANGLE_ARB;

        if (!depth_is_usable (screen_info, 24) ||
            !depth_is_usable (screen_info, 32))
        {
            continue;
        }

        data->program_win = link_program ((data->tex_type == GL_TEXTURE_2D)
                                          ? fragment_source_2d
                                          : fragment_source_rect);
        if (data->program_win != 0)
        {
            g_info ("Using the %s texture target",
                    (data->tex_type == GL_TEXTURE_2D) ? "2D" : "rectangle");

            return TRUE;
        }

        g_info ("The shader for this texture target does not compile, trying another");
    }

    return FALSE;
}

static guint egl_display_users = 0;

static void
egl_release_surface (XfwmGLData *data)
{
    if (eglGetCurrentContext () == data->egl_context)
    {
        eglMakeCurrent (data->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                        EGL_NO_CONTEXT);
    }
    if (data->egl_surface != EGL_NO_SURFACE)
    {
        eglDestroySurface (data->egl_display, data->egl_surface);
        data->egl_surface = EGL_NO_SURFACE;
    }
}

static gboolean
egl_attach_output_surface (ScreenInfo *screen_info)
{
    XfwmGLData *data = gl_data (screen_info);

    data->egl_surface =
        eglCreateWindowSurface (data->egl_display, data->egl_config,
                                (EGLNativeWindowType) screen_info->output,
                                NULL);

    return (data->egl_surface != EGL_NO_SURFACE &&
            eglMakeCurrent (data->egl_display, data->egl_surface,
                            data->egl_surface, data->egl_context));
}

static void
egl_screen_finish (ScreenInfo *screen_info)
{
    XfwmGLData *data = gl_data (screen_info);

    if (data->egl_display == EGL_NO_DISPLAY)
    {
        return;
    }

    egl_release_surface (data);
    if (data->egl_context != EGL_NO_CONTEXT)
    {
        eglDestroyContext (data->egl_display, data->egl_context);
        data->egl_context = EGL_NO_CONTEXT;
    }
    if (egl_display_users > 0 && --egl_display_users == 0)
    {
        eglTerminate (data->egl_display);
        eglReleaseThread ();
    }
    data->egl_display = EGL_NO_DISPLAY;
}

static gboolean
egl_screen_init (ScreenInfo *screen_info)
{
    XfwmGLData *data = gl_data (screen_info);
    Display *dpy = myScreenGetXDisplay (screen_info);
    XWindowAttributes attr;
    EGLConfig configs[256];
    EGLConfig config = NULL;
    EGLint n_configs = 0, visual_id, major, minor, i;
    const EGLint wanted[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_NONE
    };

    data->egl_display = eglGetDisplay ((EGLNativeDisplayType) dpy);
    if (data->egl_display == EGL_NO_DISPLAY ||
        !eglInitialize (data->egl_display, &major, &minor))
    {
        g_warning ("Cannot initialise EGL, staying on GLX.");
        data->egl_display = EGL_NO_DISPLAY;

        return FALSE;
    }
    egl_display_users++;

    if (!eglBindAPI (EGL_OPENGL_API))
    {
        g_warning ("EGL offers no desktop OpenGL here, staying on GLX.");
        goto failed;
    }

    if (!epoxy_has_egl_extension (data->egl_display, "EGL_KHR_image_pixmap"))
    {
        g_warning ("EGL_KHR_image_pixmap is missing, staying on GLX.");
        goto failed;
    }

    if (!XGetWindowAttributes (dpy, screen_info->output, &attr) ||
        !eglChooseConfig (data->egl_display, wanted, configs,
                          (EGLint) G_N_ELEMENTS (configs), &n_configs))
    {
        g_warning ("No usable EGL configs, staying on GLX.");
        goto failed;
    }
    for (i = 0; i < n_configs; i++)
    {
        if (eglGetConfigAttrib (data->egl_display, configs[i],
                                EGL_NATIVE_VISUAL_ID, &visual_id) &&
            (VisualID) visual_id == XVisualIDFromVisual (attr.visual))
        {
            config = configs[i];
            break;
        }
    }
    if (config == NULL)
    {
        g_warning ("No EGL config matches the output window "
                   "(%d configs, visual 0x%lx), staying on GLX.",
                   (gint) n_configs,
                   (gulong) XVisualIDFromVisual (attr.visual));
        goto failed;
    }

    data->egl_context = eglCreateContext (data->egl_display, config,
                                          EGL_NO_CONTEXT, NULL);
    if (data->egl_context == EGL_NO_CONTEXT)
    {
        g_warning ("Cannot create an EGL context, staying on GLX.");
        goto failed;
    }

    data->egl_config = config;
    if (!egl_attach_output_surface (screen_info))
    {
        g_warning ("Cannot make the EGL context current, staying on GLX.");
        goto failed;
    }

    if (!epoxy_has_gl_extension ("GL_OES_EGL_image"))
    {
        g_warning ("GL_OES_EGL_image is missing, staying on GLX.");
        goto failed;
    }

    screen_info->has_ext_arb_sync = epoxy_has_gl_extension ("GL_ARB_sync");
    data->has_buffer_age = epoxy_has_egl_extension (data->egl_display,
                                                    "EGL_EXT_buffer_age");
    if (epoxy_has_egl_extension (data->egl_display,
                                 "EGL_KHR_swap_buffers_with_damage"))
    {
        data->egl_swap_with_damage = eglSwapBuffersWithDamageKHR;
    }
    else if (epoxy_has_egl_extension (data->egl_display,
                                      "EGL_EXT_swap_buffers_with_damage"))
    {
        data->egl_swap_with_damage = eglSwapBuffersWithDamageEXT;
    }

    g_info ("GL compositing through EGL %d.%d (buffer age %s, "
            "swap with damage %s)", major, minor,
            data->has_buffer_age ? "yes" : "no",
            data->egl_swap_with_damage != NULL ? "yes" : "no");

    return TRUE;

failed:
    egl_screen_finish (screen_info);
    if (screen_info->glx_context != None)
    {
        glXMakeCurrent (dpy, screen_info->glx_window, screen_info->glx_context);
    }

    return FALSE;
}

gboolean
xfwmGLScreenInit (ScreenInfo *screen_info)
{
    XfwmGLData *data;
    Display *dpy;
    gboolean no_ext;
    gboolean can_blit_fbo;

    g_return_val_if_fail (screen_info != NULL, FALSE);
    TRACE ("entering");

    dpy = myScreenGetXDisplay (screen_info);

    if (!screen_info->display_info->have_name_window_pixmap)
    {
        g_warning ("The X server cannot name window pixmaps, GL compositing disabled.");
        return FALSE;
    }

    data = g_new0 (XfwmGLData, 1);
    screen_info->gl_data = data;

    if (screen_info->use_egl_backend)
    {
        egl_screen_init (screen_info);

        if (data->egl_context == EGL_NO_CONTEXT)
        {
            if (screen_info->glx_context == None)
            {
                xfwmGLScreenFinish (screen_info);
                return FALSE;
            }
            screen_info->use_egl_backend = FALSE;
        }
    }

    if (!screen_info->use_egl_backend &&
        !epoxy_has_glx_extension (dpy, screen_info->screen,
                                  "GLX_EXT_texture_from_pixmap"))
    {
        g_warning ("GLX_EXT_texture_from_pixmap is missing, GL compositing disabled.");
        xfwmGLScreenFinish (screen_info);
        return FALSE;
    }

    if (epoxy_gl_version () < 20)
    {
        g_warning ("OpenGL 2.0 is required for GL compositing, disabled.");
        xfwmGLScreenFinish (screen_info);
        return FALSE;
    }

    if (!epoxy_has_gl_extension ("GL_ARB_framebuffer_object") &&
        !epoxy_has_gl_extension ("GL_EXT_framebuffer_object"))
    {
        g_warning ("Frame buffer objects are missing, GL compositing disabled.");
        xfwmGLScreenFinish (screen_info);
        return FALSE;
    }
    can_blit_fbo = epoxy_gl_version () >= 30 ||
                   epoxy_has_gl_extension ("GL_ARB_framebuffer_object") ||
                   epoxy_has_gl_extension ("GL_EXT_framebuffer_blit");

    if (screen_info->use_egl_backend)
    {
        data->tex_target = GLX_TEXTURE_2D_EXT;
        data->tex_type = GL_TEXTURE_2D;
        data->program_win = link_program (fragment_source_2d);
        if (data->program_win == 0)
        {
            g_warning ("The window shader does not compile, GL compositing disabled.");
            xfwmGLScreenFinish (screen_info);
            return FALSE;
        }
    }
    else if (!pick_texture_target (screen_info))
    {
        g_warning ("No GLX config to bind windows as textures, GL compositing disabled.");
        xfwmGLScreenFinish (screen_info);

        return FALSE;
    }

    data->u_opacity_win = glGetUniformLocation (data->program_win, "opacity");
    glUseProgram (data->program_win);
    glUniform1i (glGetUniformLocation (data->program_win, "tex"), 0);

    if (data->tex_type == GL_TEXTURE_2D)
    {
        data->program_2d = data->program_win;
        data->u_opacity_2d = data->u_opacity_win;
    }
    else
    {
        data->program_2d = link_program (fragment_source_2d);
        if (data->program_2d == 0)
        {
            xfwmGLScreenFinish (screen_info);
            return FALSE;
        }
        data->u_opacity_2d = glGetUniformLocation (data->program_2d, "opacity");
        glUseProgram (data->program_2d);
        glUniform1i (glGetUniformLocation (data->program_2d, "tex"), 0);
    }

    data->program_blur_down = link_program (fragment_blur_down);
    data->program_blur_up = link_program (fragment_blur_up);
    if (data->program_blur_down != 0 && data->program_blur_up != 0)
    {
        data->u_blur_pixel_down = glGetUniformLocation (data->program_blur_down, "pixel");
        glUseProgram (data->program_blur_down);
        glUniform1i (glGetUniformLocation (data->program_blur_down, "tex"), 0);
        data->u_blur_pixel_up = glGetUniformLocation (data->program_blur_up, "pixel");
        glUseProgram (data->program_blur_up);
        glUniform1i (glGetUniformLocation (data->program_blur_up, "tex"), 0);
    }
    else
    {
        g_warning ("Dual Kawase backdrop blur shaders unavailable; blur disabled.");
    }

    data->program_blur_mix = link_program (fragment_blur_mix);
    if (data->program_blur_mix != 0)
    {
        data->u_blur_mix_strength = glGetUniformLocation (data->program_blur_mix, "strength");
        glUseProgram (data->program_blur_mix);
        glUniform1i (glGetUniformLocation (data->program_blur_mix, "blurred"), 0);
        glUniform1i (glGetUniformLocation (data->program_blur_mix, "original"), 1);
    }

    data->program_shadow_profile = link_program (fragment_shadow_profile);
    if (data->program_shadow_profile != 0)
    {
        data->u_prof_size = glGetUniformLocation (data->program_shadow_profile, "size");
        data->u_prof_ramp = glGetUniformLocation (data->program_shadow_profile, "ramp");
        data->u_prof_opacity = glGetUniformLocation (data->program_shadow_profile, "opacity");
        glUseProgram (data->program_shadow_profile);
        glUniform1i (glGetUniformLocation (data->program_shadow_profile, "prof"), 0);
    }

    data->renderer = g_strdup ((const gchar *) glGetString (GL_RENDERER));
    {
        static const guchar black[4] = { 0, 0, 0, 0xff };

        glGenTextures (1, &data->black_texture);
        glBindTexture (GL_TEXTURE_2D, data->black_texture);
        set_tex_params (GL_TEXTURE_2D, GL_NEAREST);
        glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0,
                      GL_RGBA, GL_UNSIGNED_BYTE, black);
        glBindTexture (GL_TEXTURE_2D, 0);
    }

    if (!screen_info->use_egl_backend)
    {
        data->has_buffer_age = epoxy_has_glx_extension (dpy, screen_info->screen,
                                                        "GLX_EXT_buffer_age");
    }

    no_ext = (g_getenv ("XFWM4_GL_NO_EXT") != NULL);
    if (no_ext)
    {
        data->has_buffer_age = FALSE;
    }
    data->full_repaint = TRUE;

    data->present_mode = GL_PRESENT_SWAP;
    if (g_strcmp0 (g_getenv ("XFWM4_GL_PRESENT"), "scene") == 0)
    {
        data->present_mode = GL_PRESENT_SCENE;
        g_info ("GL presentation mode: scene buffer");
    }
    else if (!screen_info->use_egl_backend)
    {
        const gchar *mode = g_getenv ("XFWM4_GL_PRESENT");
        gboolean has_copy_sub_buffer = !no_ext &&
            epoxy_has_glx_extension (dpy, screen_info->screen,
                                     "GLX_MESA_copy_sub_buffer");

        if (mode == NULL || g_strcmp0 (mode, "auto") == 0)
        {
            if (data->has_buffer_age || !has_copy_sub_buffer || !can_blit_fbo)
            {
                data->present_mode = GL_PRESENT_SWAP;
            }
            else
            {
                data->present_mode = GL_PRESENT_FBO;
            }
        }
        else if (g_strcmp0 (mode, "front") == 0)
        {
            g_warning ("The front presentation mode was removed, it tears "
                       "over anything that repaints continuously; swapping.");
        }
        else if (g_strcmp0 (mode, "copy") == 0 ||
                 g_strcmp0 (mode, "fbo") == 0)
        {
            if (!has_copy_sub_buffer)
            {
                g_warning ("XFWM4_GL_PRESENT=%s wanted but "
                           "GLX_MESA_copy_sub_buffer is missing, swapping.", mode);
            }
            else
            {
                data->present_mode = g_strcmp0 (mode, "copy") == 0
                                     ? GL_PRESENT_COPY : GL_PRESENT_FBO;
            }
        }
        else if (g_strcmp0 (mode, "swap") != 0)
        {
            g_warning ("Unknown XFWM4_GL_PRESENT=%s, swapping.", mode);
        }
        g_info ("GL presentation mode: %s",
                data->present_mode == GL_PRESENT_SWAP ? "swap" :
                data->present_mode == GL_PRESENT_COPY ? "copy" :
                "fbo");
    }
    if (screen_info->params->blur_enabled &&
        (screen_info->params->blur_docks ||
         screen_info->params->blur_transparent_windows ||
         screen_info->params->blur_previews ||
         screen_info->params->blur_alt_tab ||
         screen_info->params->blur_menus ||
         screen_info->params->blur_tooltips ||
         screen_info->params->blur_notifications) && can_blit_fbo)
    {
        data->present_mode = GL_PRESENT_SCENE;
        g_info ("GL presentation mode: scene buffer (required for backdrop blur)");
    }

    if ((data->present_mode == GL_PRESENT_SCENE ||
         data->present_mode == GL_PRESENT_FBO) && !can_blit_fbo)
    {
        g_warning ("XFWM4_GL_PRESENT=%s wanted but frame buffer blits are "
                   "missing, swapping.", g_getenv ("XFWM4_GL_PRESENT"));
        data->present_mode = GL_PRESENT_SWAP;
    }
    data->wait_new_pixmap =
        (g_strcmp0 (g_getenv ("XFWM4_GL_PIXMAP_WAIT"), "off") != 0);
    data->no_paint = (g_getenv ("XFWM4_GL_NOPAINT") != NULL);
    if (data->no_paint)
    {
        g_warning ("XFWM4_GL_NOPAINT is set: nothing will be drawn, the screen "
                   "will be garbage. This is a measurement mode.");
    }
    data->use_fence =
        (g_strcmp0 (g_getenv ("XFWM4_GL_FENCE"), "off") != 0);
    if (!data->use_fence)
    {
        g_info ("GL frames are not fenced, the repaint loop will not wait");
    }
    data->stats = (g_getenv ("XFWM4_GL_STATS") != NULL);
    data->profile = (g_getenv ("XFWM4_GL_PROFILE") != NULL);
    data->prof_gpu = data->profile &&
                     (epoxy_gl_version () >= 33 ||
                      epoxy_has_gl_extension ("GL_ARB_timer_query"));
    if (data->profile && !data->prof_gpu)
    {
        g_info ("This driver has no timer queries, the profile shows no GPU time");
    }
    data->stats |= data->profile;
    data->stat_since = g_get_monotonic_time ();

    set_swap_interval_gl (screen_info);

    glDisable (GL_DEPTH_TEST);
    glDepthMask (GL_FALSE);
    glDisable (GL_CULL_FACE);
    glBlendFunc (GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    g_info ("GL compositing enabled (%s, buffer age %s)",
            (data->tex_type == GL_TEXTURE_2D) ? "texture 2D" : "texture rectangle",
            data->has_buffer_age ? "yes" : "no");

    return TRUE;
}

static void
free_root_texture (ScreenInfo *screen_info)
{
    XfwmGLData *data = gl_data (screen_info);
    DisplayInfo *display_info = screen_info->display_info;
    Display *dpy = myScreenGetXDisplay (screen_info);

    if (data == NULL)
    {
        return;
    }

    if (data->root_damage != None)
    {
        myDisplayErrorTrapPush (display_info);
        XDamageDestroy (dpy, data->root_damage);
        myDisplayErrorTrapPopIgnored (display_info);
        data->root_damage = None;
    }
    if (data->root_glx_pixmap != None)
    {
        myDisplayErrorTrapPush (display_info);
        if ((data->root_texture != 0) && gl_context_is_current (screen_info))
        {
            glBindTexture (data->root_tex_type, data->root_texture);
            glXReleaseTexImageEXT (dpy, data->root_glx_pixmap, GLX_FRONT_EXT);
            glBindTexture (data->root_tex_type, 0);
        }
        glXDestroyPixmap (dpy, data->root_glx_pixmap);
        myDisplayErrorTrapPopIgnored (display_info);
        data->root_glx_pixmap = None;
    }
    if (data->root_egl_image != NULL)
    {
        eglDestroyImageKHR (data->egl_display, (EGLImageKHR) data->root_egl_image);
        data->root_egl_image = NULL;
    }
    if (data->root_texture != 0 && gl_context_is_current (screen_info))
    {
        glDeleteTextures (1, &data->root_texture);
    }
    data->root_texture = 0;
    data->root_repeat = FALSE;
    data->root_pixmap = None;
    data->root_dirty = FALSE;
    data->root_missing = FALSE;
}

gboolean
xfwmGLDamageRootPixmap (ScreenInfo *screen_info, Drawable drawable)
{
    XfwmGLData *data;

    g_return_val_if_fail (screen_info != NULL, FALSE);

    data = gl_data (screen_info);
    if (data == NULL || data->root_pixmap == None ||
        data->root_pixmap != (Pixmap) drawable)
    {
        return FALSE;
    }

    data->root_dirty = TRUE;

    return TRUE;
}

gboolean
xfwmGLRootPixmapCoversScreen (ScreenInfo *screen_info)
{
    XfwmGLData *data;

    g_return_val_if_fail (screen_info != NULL, FALSE);

    data = gl_data (screen_info);

    return (data != NULL) &&
           (data->root_width >= screen_info->width) &&
           (data->root_height >= screen_info->height);
}

void
xfwmGLInvalidateRootTexture (ScreenInfo *screen_info)
{
    g_return_if_fail (screen_info != NULL);

    free_root_texture (screen_info);
}

static void
free_fbo (ScreenInfo *screen_info)
{
    XfwmGLData *data = gl_data (screen_info);

    if (data->fbo != 0)
    {
        glDeleteFramebuffers (1, &data->fbo);
        data->fbo = 0;
    }
    if (data->fbo_texture != 0)
    {
        glDeleteTextures (1, &data->fbo_texture);
        data->fbo_texture = 0;
    }
    data->fbo_width = 0;
    data->fbo_height = 0;
    data->fbo_filter = 0;
}

static void
free_blur_buffers (ScreenInfo *screen_info)
{
    XfwmGLData *data = gl_data (screen_info);

    if (data->blur_source_fbo != 0)
    {
        glDeleteFramebuffers (1, &data->blur_source_fbo);
        data->blur_source_fbo = 0;
    }
    if (data->blur_source_texture != 0)
    {
        glDeleteTextures (1, &data->blur_source_texture);
        data->blur_source_texture = 0;
    }
    if (data->blur_original_texture != 0)
    {
        glDeleteTextures (1, &data->blur_original_texture);
        data->blur_original_texture = 0;
    }
    glDeleteFramebuffers (3, data->blur_fbo);
    glDeleteTextures (3, data->blur_texture);
    memset (data->blur_fbo, 0, sizeof (data->blur_fbo));
    memset (data->blur_texture, 0, sizeof (data->blur_texture));
    memset (data->blur_width, 0, sizeof (data->blur_width));
    memset (data->blur_height, 0, sizeof (data->blur_height));
    data->blur_source_width = 0;
    data->blur_source_height = 0;
}


static void
free_cursor_texture (ScreenInfo *screen_info)
{
    XfwmGLData *data = gl_data (screen_info);

    if (data->cursor_texture != 0)
    {
        glDeleteTextures (1, &data->cursor_texture);
        data->cursor_texture = 0;
        data->cursor_serial = 0;
    }
}

void
xfwmGLScreenFinish (ScreenInfo *screen_info)
{
    XfwmGLData *data;
    guint i;

    g_return_if_fail (screen_info != NULL);
    TRACE ("entering");

    data = gl_data (screen_info);
    if (data == NULL)
    {
        return;
    }

    if (data->animation_timer)
    {
        g_source_remove (data->animation_timer);
        data->animation_timer = 0;
    }
    compositorClearPeek (screen_info);
    xfwmGLCancelAnimations (screen_info);

    free_root_texture (screen_info);

    if (gl_context_is_current (screen_info))
    {
        free_fbo (screen_info);
        free_blur_buffers (screen_info);
        free_cursor_texture (screen_info);

        if (data->black_texture != 0)
        {
            glDeleteTextures (1, &data->black_texture);
        }
        if (data->program_win != 0)
        {
            glDeleteProgram (data->program_win);
        }
        if (data->program_2d != 0 && data->program_2d != data->program_win)
        {
            glDeleteProgram (data->program_2d);
        }
        if (data->program_shadow_profile != 0)
        {
            glDeleteProgram (data->program_shadow_profile);
        }
        if (data->program_blur_down != 0)
        {
            glDeleteProgram (data->program_blur_down);
        }
        if (data->program_blur_up != 0)
        {
            glDeleteProgram (data->program_blur_up);
        }
        if (data->program_blur_mix != 0)
        {
            glDeleteProgram (data->program_blur_mix);
        }
        if (data->shadow_profile != 0)
        {
            glDeleteTextures (1, &data->shadow_profile);
        }
    }

    for (i = 0; i < GL_DAMAGE_HISTORY; i++)
    {
        if (data->damage_history[i] != NULL)
        {
            cairo_region_destroy (data->damage_history[i]);
        }
    }

    egl_screen_finish (screen_info);

    g_free (data->renderer);
    g_free (data);
    screen_info->gl_data = NULL;
}

const gchar *
xfwmGLGetRendererName (ScreenInfo *screen_info)
{
    XfwmGLData *data;

    g_return_val_if_fail (screen_info != NULL, NULL);

    data = gl_data (screen_info);

    return (data != NULL) ? data->renderer : NULL;
}

void
xfwmGLScreenSizeChanged (ScreenInfo *screen_info)
{
    g_return_if_fail (screen_info != NULL);

    if (screen_info->gl_data == NULL || !gl_context_is_current (screen_info))
    {
        return;
    }

    xfwmGLCancelAnimations (screen_info);
    free_root_texture (screen_info);
    free_fbo (screen_info);
    gl_data (screen_info)->fbo_failed = FALSE;
    gl_data (screen_info)->full_repaint = TRUE;
}

void
xfwmGLFreeWindowData (CWindow *cw)
{
    ScreenInfo *screen_info;
    Display *dpy;

    g_return_if_fail (cw != NULL);

    screen_info = cw->screen_info;
    if (screen_info->gl_data == NULL)
    {
        return;
    }
    dpy = myScreenGetXDisplay (screen_info);

    if (cw->gl_pixmap != None)
    {
        if (cw->gl_texture_bound && (cw->gl_texture != 0) &&
            gl_context_is_current (screen_info))
        {
            glBindTexture (gl_data (screen_info)->tex_type, cw->gl_texture);
            glXReleaseTexImageEXT (dpy, cw->gl_pixmap, GLX_FRONT_EXT);
            glBindTexture (gl_data (screen_info)->tex_type, 0);
        }
        cw->gl_texture_bound = FALSE;
        glXDestroyPixmap (dpy, cw->gl_pixmap);
        cw->gl_pixmap = None;
    }
    if (cw->egl_image != NULL)
    {
        eglDestroyImageKHR (gl_data (screen_info)->egl_display,
                            (EGLImageKHR) cw->egl_image);
        cw->egl_image = NULL;
        cw->gl_texture_bound = FALSE;
    }
    if (cw->gl_texture != 0 && gl_context_is_current (screen_info))
    {
        glDeleteTextures (1, &cw->gl_texture);
        cw->gl_texture = 0;
    }
    cw->gl_pixmap_width = 0;
    cw->gl_pixmap_height = 0;
}

void
xfwmGLFreeWindowShadow (CWindow *cw)
{
    g_return_if_fail (cw != NULL);

    if (cw->screen_info->gl_data == NULL ||
        !gl_context_is_current (cw->screen_info))
    {
        return;
    }

    if (cw->gl_shadow_texture != 0)
    {
        glDeleteTextures (1, &cw->gl_shadow_texture);
        cw->gl_shadow_texture = 0;
    }
    cw->shadow_width = 0;
    cw->shadow_height = 0;
    cw->gl_shadow_opacity = 0.0f;
}

void
xfwmGLUpdateWindowShadow (CWindow *cw, gdouble opacity, gint width, gint height)
{
    ScreenInfo *screen_info;
    XfwmGLData *data;
    XImage *image;
    gint gaussian_size, shadow_width = 0, shadow_height = 0;

    g_return_if_fail (cw != NULL);

    screen_info = cw->screen_info;
    data = gl_data (screen_info);
    g_return_if_fail (data != NULL);
    g_return_if_fail (screen_info->gaussianMap != NULL);
    if (!gl_context_is_current (screen_info))
    {
        return;
    }

    xfwmGLFreeWindowShadow (cw);

    gaussian_size = screen_info->gaussianMap->size;
    shadow_size (screen_info, width, height, &shadow_width, &shadow_height);

    if ((data->program_shadow_profile != 0) &&
        (shadow_width >= 2 * gaussian_size) &&
        (shadow_height >= 2 * gaussian_size) &&
        build_shadow_profile (screen_info))
    {
        cw->shadow_width = shadow_width;
        cw->shadow_height = shadow_height;
        cw->gl_shadow_opacity = (gfloat) opacity * data->shadow_profile_peak;

        return;
    }

    image = make_shadow (screen_info, opacity, width, height);
    if (image == NULL)
    {
        return;
    }

    glGenTextures (1, &cw->gl_shadow_texture);
    glBindTexture (GL_TEXTURE_2D, cw->gl_shadow_texture);
    set_tex_params (GL_TEXTURE_2D, GL_LINEAR);
    glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei (GL_UNPACK_ROW_LENGTH, image->bytes_per_line);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_ALPHA,
                  image->width, image->height, 0,
                  GL_ALPHA, GL_UNSIGNED_BYTE, image->data);
    glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
    glBindTexture (GL_TEXTURE_2D, 0);

    cw->shadow_width = image->width;
    cw->shadow_height = image->height;
    XDestroyImage (image);
}

static gboolean
build_shadow_profile (ScreenInfo *screen_info)
{
    XfwmGLData *data = gl_data (screen_info);
    XImage *image;
    guchar *profile;
    guchar peak;
    gint gaussian_size, box, row, i;

    if (data->shadow_profile != 0)
    {
        return TRUE;
    }

    g_return_val_if_fail (screen_info->gaussianMap != NULL, FALSE);

    gaussian_size = screen_info->gaussianMap->size;
    if (gaussian_size < 2)
    {
        return FALSE;
    }

    box = 4 * gaussian_size;
    image = make_shadow (screen_info, 1.0, box, box);
    if (image == NULL)
    {
        return FALSE;
    }
    if (image->width < 2 * gaussian_size || image->height < 1)
    {
        XDestroyImage (image);
        return FALSE;
    }

    row = image->height / 2;
    peak = (guchar) image->data[row * image->bytes_per_line + image->width / 2];
    if (peak == 0)
    {
        XDestroyImage (image);
        return FALSE;
    }

    profile = g_malloc (gaussian_size);
    for (i = 0; i < gaussian_size; i++)
    {
        gint v = (guchar) image->data[row * image->bytes_per_line + i] * 255 / peak;

        profile[i] = (guchar) MIN (v, 255);
    }

    glGenTextures (1, &data->shadow_profile);
    glBindTexture (GL_TEXTURE_2D, data->shadow_profile);
    set_tex_params (GL_TEXTURE_2D, GL_LINEAR);
    glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_ALPHA, gaussian_size, 1, 0,
                  GL_ALPHA, GL_UNSIGNED_BYTE, profile);
    glBindTexture (GL_TEXTURE_2D, 0);

    data->shadow_profile_peak = (gfloat) peak / 255.0f;

    {
        GLint current = 0;

        glGetIntegerv (GL_CURRENT_PROGRAM, &current);
        glUseProgram (data->program_shadow_profile);
        glUniform1f (data->u_prof_ramp, (gfloat) gaussian_size);
        glUseProgram ((GLuint) current);
    }

    g_free (profile);
    XDestroyImage (image);

    DBG ("Shadow profile built, %i samples, peak %.3f",
         gaussian_size, data->shadow_profile_peak);

    return TRUE;
}

static void
give_up_on_depth (XfwmGLData *data, gint depth)
{
    if (!data->give_up)
    {
        g_warning ("A window of depth %i cannot be bound as a texture, "
                   "falling back to XRender.", depth);
        data->give_up = TRUE;
    }
}

static void
wait_for_pixmap (DisplayInfo *display_info, Pixmap pixmap)
{
    XImage *image;

    myDisplayErrorTrapPush (display_info);
    image = XGetImage (display_info->dpy, pixmap,
                       0, 0, 1, 1, AllPlanes, ZPixmap);
    if (image != NULL)
    {
        XDestroyImage (image);
    }
    myDisplayErrorTrapPopIgnored (display_info);
}

static gboolean
bind_window_texture (CWindow *cw)
{
    ScreenInfo *screen_info = cw->screen_info;
    XfwmGLData *data = gl_data (screen_info);
    DisplayInfo *display_info = screen_info->display_info;
    Display *dpy = myScreenGetXDisplay (screen_info);
    XfwmGLDepth *dc;
    gboolean new_pixmap = FALSE;

    if (ensure_name_window_pixmap (cw) == None)
    {
        return FALSE;
    }

    cw->gl_y_inverted = TRUE;
    if (screen_info->use_egl_backend)
    {
        if (cw->egl_image == NULL)
        {
            cw->egl_image =
                eglCreateImageKHR (data->egl_display, EGL_NO_CONTEXT,
                                   EGL_NATIVE_PIXMAP_KHR,
                                   (EGLClientBuffer) (guintptr)
                                   cw->name_window_pixmap, preserved_image);
            if (cw->egl_image == NULL)
            {
                data->retry_paint = TRUE;

                return FALSE;
            }
            new_pixmap = TRUE;
        }
    }
    else
    {
        dc = depth_config (screen_info, cw->attr.depth);
        if (dc == NULL || !dc->usable)
        {
            give_up_on_depth (data, cw->attr.depth);

            return FALSE;
        }
        cw->gl_y_inverted = dc->y_inverted;

        if (cw->gl_pixmap == None)
        {
            const gint attribs[] = {
                GLX_TEXTURE_TARGET_EXT, (gint) data->tex_target,
                GLX_TEXTURE_FORMAT_EXT, (cw->attr.depth == 32)
                                         ? GLX_TEXTURE_FORMAT_RGBA_EXT
                                         : GLX_TEXTURE_FORMAT_RGB_EXT,
                None
            };

            myDisplayErrorTrapPush (display_info);
            cw->gl_pixmap = glXCreatePixmap (dpy, dc->fbconfig,
                                             cw->name_window_pixmap, attribs);
            if (myDisplayErrorTrapPop (display_info) != Success)
            {
                cw->gl_pixmap = None;
            }
            if (cw->gl_pixmap == None)
            {
                data->retry_paint = TRUE;

                return FALSE;
            }
            new_pixmap = TRUE;
        }
    }

    if (cw->gl_texture == 0)
    {
        glGenTextures (1, &cw->gl_texture);
        glBindTexture (data->tex_type, cw->gl_texture);
        set_tex_params (data->tex_type, GL_NEAREST);
    }
    else
    {
        glBindTexture (data->tex_type, cw->gl_texture);
    }

    if (!cw->gl_texture_bound || cw->gl_content_dirty)
    {
        gdouble prof_at = data->profile ? thread_cpu_ms () : 0.0;

        if (screen_info->use_egl_backend)
        {
            glEGLImageTargetTexture2DOES (GL_TEXTURE_2D, cw->egl_image);
        }
        else
        {
            if (cw->gl_texture_bound)
            {
                glXReleaseTexImageEXT (dpy, cw->gl_pixmap, GLX_FRONT_EXT);
                cw->gl_texture_bound = FALSE;
            }
            glXBindTexImageEXT (dpy, cw->gl_pixmap, GLX_FRONT_EXT, NULL);
        }
        cw->gl_texture_bound = TRUE;

        if (data->profile)
        {
            data->prof_bind += thread_cpu_ms () - prof_at;
            data->prof_binds += 1.0;
        }

        if (new_pixmap && data->wait_new_pixmap)
        {
            wait_for_pixmap (display_info, cw->name_window_pixmap);
        }
    }
    cw->gl_content_dirty = FALSE;

    return TRUE;
}

static void
draw_quad (ScreenInfo *screen_info, GLenum tex_type, gboolean y_inverted,
           gint sx, gint sy, gint tex_width, gint tex_height,
           gint dx, gint dy, gint width, gint height,
           cairo_region_t *clip)
{
    XfwmGLData *data = gl_data (screen_info);
    gint nrects, i;
    cairo_rectangle_int_t canvas = { 0, 0, screen_info->width, screen_info->height };

    if (data->snapshot_active)
    {
        canvas = data->snapshot_bounds;
    }

    if (width <= 0 || height <= 0 || tex_width <= 0 || tex_height <= 0)
    {
        return;
    }

    nrects = cairo_region_num_rectangles (clip);
    if (nrects == 0)
    {
        return;
    }

    if (data->profile)
    {
        data->prof_quads += 1.0;
        data->prof_rects += nrects;
    }
    if (data->no_paint)
    {
        return;
    }

    glBegin (GL_QUADS);
    for (i = 0; i < nrects; i++)
    {
        cairo_rectangle_int_t r;
        gint x1, y1, x2, y2;
        gfloat vx1, vy1, vx2, vy2;
        gfloat u1, v1, u2, v2;

        cairo_region_get_rectangle (clip, i, &r);

        x1 = MAX (dx, r.x);
        y1 = MAX (dy, r.y);
        x2 = MIN (dx + width, r.x + r.width);
        y2 = MIN (dy + height, r.y + r.height);
        if (x1 >= x2 || y1 >= y2)
        {
            continue;
        }

        vx1 = 2.0f * (gfloat) (x1 - canvas.x) / (gfloat) canvas.width - 1.0f;
        vx2 = 2.0f * (gfloat) (x2 - canvas.x) / (gfloat) canvas.width - 1.0f;
        vy1 = 1.0f - 2.0f * (gfloat) (y1 - canvas.y) / (gfloat) canvas.height;
        vy2 = 1.0f - 2.0f * (gfloat) (y2 - canvas.y) / (gfloat) canvas.height;

        u1 = (gfloat) (sx + x1 - dx);
        u2 = (gfloat) (sx + x2 - dx);
        v1 = (gfloat) (sy + y1 - dy);
        v2 = (gfloat) (sy + y2 - dy);
        if (!y_inverted)
        {
            v1 = (gfloat) tex_height - v1;
            v2 = (gfloat) tex_height - v2;
        }
        if (tex_type != GL_TEXTURE_RECTANGLE_ARB)
        {
            u1 /= (gfloat) tex_width;
            u2 /= (gfloat) tex_width;
            v1 /= (gfloat) tex_height;
            v2 /= (gfloat) tex_height;
        }

        glTexCoord2f (u1, v1);
        glVertex2f (vx1, vy1);
        glTexCoord2f (u2, v1);
        glVertex2f (vx2, vy1);
        glTexCoord2f (u2, v2);
        glVertex2f (vx2, vy2);
        glTexCoord2f (u1, v2);
        glVertex2f (vx1, vy2);
    }
    glEnd ();
}

static void
use_program (GLuint program, GLint u_opacity, gfloat opacity)
{
    glUseProgram (program);
    glUniform1f (u_opacity, opacity);
}

static void
draw_window_part (CWindow *cw, gint sx, gint sy, gint dx, gint dy,
                  gint width, gint height, cairo_region_t *clip)
{
    ScreenInfo *screen_info = cw->screen_info;
    XfwmGLData *data = gl_data (screen_info);
    gint tex_width, tex_height;

    get_window_pixmap_size (cw, &tex_width, &tex_height);

    draw_quad (screen_info, data->tex_type, cw->gl_y_inverted,
               sx, sy, tex_width, tex_height,
               dx, dy, width, height, clip);
}

static gboolean
ensure_blur_buffers (ScreenInfo *screen_info, gint width, gint height)
{
    XfwmGLData *data = gl_data (screen_info);
    gint w, h;
    guint i;

    if (data->blur_failed || data->program_blur_down == 0 ||
        data->program_blur_up == 0 || width <= 0 || height <= 0)
    {
        return FALSE;
    }
    if (data->blur_source_width == width && data->blur_source_height == height &&
        data->blur_source_fbo != 0 && data->blur_fbo[0] != 0)
    {
        return TRUE;
    }

    free_blur_buffers (screen_info);

    glGenTextures (1, &data->blur_original_texture);
    glBindTexture (GL_TEXTURE_2D, data->blur_original_texture);
    set_tex_params (GL_TEXTURE_2D, GL_LINEAR);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                  GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    glGenTextures (1, &data->blur_source_texture);
    glBindTexture (GL_TEXTURE_2D, data->blur_source_texture);
    set_tex_params (GL_TEXTURE_2D, GL_LINEAR);
    glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                  GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers (1, &data->blur_source_fbo);
    glBindFramebuffer (GL_FRAMEBUFFER, data->blur_source_fbo);
    glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                            GL_TEXTURE_2D, data->blur_source_texture, 0);
    if (glCheckFramebufferStatus (GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    {
        goto failed;
    }

    glGenTextures (3, data->blur_texture);
    glGenFramebuffers (3, data->blur_fbo);
    w = width;
    h = height;
    for (i = 0; i < 3; i++)
    {
        w = MAX (1, (w + 1) / 2);
        h = MAX (1, (h + 1) / 2);
        data->blur_width[i] = w;
        data->blur_height[i] = h;
        glBindTexture (GL_TEXTURE_2D, data->blur_texture[i]);
        set_tex_params (GL_TEXTURE_2D, GL_LINEAR);
        glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                      GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glBindFramebuffer (GL_FRAMEBUFFER, data->blur_fbo[i]);
        glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                GL_TEXTURE_2D, data->blur_texture[i], 0);
        if (glCheckFramebufferStatus (GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        {
            goto failed;
        }
    }
    data->blur_source_width = width;
    data->blur_source_height = height;
    return TRUE;

failed:
    g_warning ("Cannot create Dual Kawase blur buffers; backdrop blur disabled.");
    glBindFramebuffer (GL_FRAMEBUFFER, 0);
    free_blur_buffers (screen_info);
    data->blur_failed = TRUE;
    return FALSE;
}


static void
draw_full_texture (GLuint texture)
{
    glBindTexture (GL_TEXTURE_2D, texture);
    glBegin (GL_QUADS);
    glTexCoord2f (0.0f, 0.0f); glVertex2f (-1.0f, -1.0f);
    glTexCoord2f (1.0f, 0.0f); glVertex2f ( 1.0f, -1.0f);
    glTexCoord2f (1.0f, 1.0f); glVertex2f ( 1.0f,  1.0f);
    glTexCoord2f (0.0f, 1.0f); glVertex2f (-1.0f,  1.0f);
    glEnd ();
}


static gboolean
window_wants_backdrop_blur (CWindow *cw)
{
    if (cw->gl_animation || !cw->screen_info->params->blur_enabled)
    {
        return FALSE;
    }
    if (cw->blur_type != XFWM_BLUR_OTHER)
    {
        gboolean enabled = FALSE;
        switch (cw->blur_type)
        {
            case XFWM_BLUR_PREVIEW:
                enabled = cw->screen_info->params->blur_previews;
                break;
            case XFWM_BLUR_TABWIN:
                enabled = cw->screen_info->params->blur_alt_tab;
                break;
            case XFWM_BLUR_MENU:
                enabled = cw->screen_info->params->blur_menus;
                break;
            case XFWM_BLUR_TOOLTIP:
                enabled = cw->screen_info->params->blur_tooltips;
                break;
            case XFWM_BLUR_NOTIFICATION:
                enabled = cw->screen_info->params->blur_notifications;
                break;
            default:
                break;
        }
        return enabled && !WIN_IS_OPAQUE (cw);
    }
    /* Managed menus and notifications can also carry EWMH types. Keep
     * their blur switches independent of generic translucent clients. */
    if (WIN_HAS_CLIENT (cw) && (cw->c->type & WINDOW_MENU))
        return cw->screen_info->params->blur_menus && !WIN_IS_OPAQUE (cw);
    if (WIN_HAS_CLIENT (cw) && (cw->c->type & WINDOW_NOTIFICATION))
        return cw->screen_info->params->blur_notifications && !WIN_IS_OPAQUE (cw);
    if (WIN_IS_DOCK (cw))
    {
        return cw->screen_info->params->blur_docks;
    }
    return cw->screen_info->params->blur_transparent_windows &&
           WIN_HAS_CLIENT (cw) && !WIN_IS_OPAQUE (cw);
}


/* Complete a region against the current blur consumers. This is applied to
 * CURRENT frame damage before buffer-age history is added, and independently
 * to repair damage: yesterday's backbuffer repairs are not today's changes. */
static gboolean
expand_blur_damage (ScreenInfo *screen_info, cairo_region_t *region)
{
    XfwmGLData *data = gl_data (screen_info);
    GArray *samples = g_array_new (FALSE, FALSE, sizeof (cairo_rectangle_int_t));
    gboolean active;
    if (data->program_blur_down && data->program_blur_up)
        for (GList *list = screen_info->cwindows; list; list = list->next)
        {
            CWindow *cw = list->data;
            cairo_rectangle_int_t win, influence;
            gint radius;
            if (!WIN_IS_PAINTABLE (cw) || !window_wants_backdrop_blur (cw))
                continue;
            win.x = cw->attr.x;
            win.y = cw->attr.y;
            window_painted_size (cw, &win.width, &win.height);
            radius = CLAMP (screen_info->params->blur_radius, 1, 64);
            influence.x = MAX (0, win.x - radius);
            influence.y = MAX (0, win.y - radius);
            influence.width = MIN (screen_info->width, win.x + win.width + radius) - influence.x;
            influence.height = MIN (screen_info->height, win.y + win.height + radius) - influence.y;
            if (influence.width > 0 && influence.height > 0)
                g_array_append_val (samples, influence);
        }
    active = samples->len > 0;
    xfwm_damage_expand (region, (const cairo_rectangle_int_t *) samples->data, samples->len);
    g_array_free (samples, TRUE);
    return active;
}

/* Blur only the surface, never an app-drawn shadow in its transparent
 * window margin. _GTK_FRAME_EXTENTS covers GTK client-side decorations, and
 * _XFWM4_BLUR_CONTENT_RECT identifies the content of tasklist/switcher popups.
 * Unhinted applications preserve the original behaviour: arbitrary alpha
 * pixels cannot reliably be classified as shadow versus useful content. */
static cairo_region_t *
backdrop_content_clip (CWindow *cw, cairo_region_t *clip)
{
    cairo_region_t *result = cairo_region_copy (clip);
    cairo_rectangle_int_t surface;
    gint width, height, border;

    window_painted_size (cw, &width, &height);
    border = cw->attr.border_width;
    surface.x = cw->attr.x + border;
    surface.y = cw->attr.y + border;
    surface.width = MAX (0, width - 2 * border);
    surface.height = MAX (0, height - 2 * border);

    if (cw->blur_content_rect_valid)
    {
        gint left = MIN (surface.width, cw->blur_content_rect[0]);
        gint top = MIN (surface.height, cw->blur_content_rect[1]);

        surface.x += left;
        surface.y += top;
        surface.width = MIN (surface.width - left, cw->blur_content_rect[2]);
        surface.height = MIN (surface.height - top, cw->blur_content_rect[3]);
    }
    else
    {
        gint left = MIN (surface.width, cw->blur_gtk_extents[0]);
        gint right = MIN (surface.width, cw->blur_gtk_extents[1]);
        gint top = MIN (surface.height, cw->blur_gtk_extents[2]);
        gint bottom = MIN (surface.height, cw->blur_gtk_extents[3]);

        surface.x += left;
        surface.y += top;
        surface.width = MAX (0, surface.width - left - right);
        surface.height = MAX (0, surface.height - top - bottom);
    }
    /* An untrusted X11 hint must never extend the framebuffer footprint. */
    cairo_region_intersect_rectangle (result, &surface);
    return result;
}

static void
paint_backdrop_blur_gl (CWindow *cw, cairo_region_t *clip)
{
    ScreenInfo *screen_info = cw->screen_info;
    XfwmGLData *data = gl_data (screen_info);
    cairo_rectangle_int_t win, sample;
    cairo_region_t *content_clip;
    GLint previous_fbo = 0;
    GLint previous_viewport[4];
    gint radius, width, height, gl_y;
    gint src_w, src_h;
    GLuint src_texture;
    gfloat step, strength;
    guint i;

    if (!window_wants_backdrop_blur (cw) || data->program_blur_down == 0 ||
        data->program_blur_up == 0 || clip == NULL || cairo_region_is_empty (clip))
    {
        return;
    }

    /* A backdrop belongs to its window. Fade the filtered scene together
     * with _NET_WM_WINDOW_OPACITY, rather than leaving a full-strength blur
     * rectangle behind a disappearing preview or other translucent UI. */
    strength = (gfloat) screen_info->params->blur_strength / 100.0f *
               (gfloat) cw->opacity / (gfloat) NET_WM_OPAQUE;
    if (strength <= 0.0f)
        return;

    win.x = cw->attr.x;
    win.y = cw->attr.y;
    window_painted_size (cw, &win.width, &win.height);
    if (win.width <= 0 || win.height <= 0)
    {
        return;
    }
    content_clip = (cw->blur_content_rect_valid || cw->blur_gtk_extents_valid)
                   ? backdrop_content_clip (cw, clip) : clip;
    if (cairo_region_is_empty (content_clip))
    {
        if (content_clip != clip)
            cairo_region_destroy (content_clip);
        return;
    }

    radius = CLAMP (screen_info->params->blur_radius, 1, 64);
    sample.x = MAX (0, win.x - radius);
    sample.y = MAX (0, win.y - radius);
    sample.width = MIN (screen_info->width, win.x + win.width + radius) - sample.x;
    sample.height = MIN (screen_info->height, win.y + win.height + radius) - sample.y;
    width = sample.width;
    height = sample.height;

    glGetIntegerv (GL_FRAMEBUFFER_BINDING, &previous_fbo);
    glGetIntegerv (GL_VIEWPORT, previous_viewport);
    if (!ensure_blur_buffers (screen_info, width, height))
    {
        glBindFramebuffer (GL_FRAMEBUFFER, previous_fbo);
        glViewport (previous_viewport[0], previous_viewport[1],
                    previous_viewport[2], previous_viewport[3]);
        if (content_clip != clip)
            cairo_region_destroy (content_clip);
        return;
    }
    glBindFramebuffer (GL_FRAMEBUFFER, previous_fbo);
    glViewport (previous_viewport[0], previous_viewport[1],
                previous_viewport[2], previous_viewport[3]);

    /* Capture a clean, already composited backdrop.  With blur enabled the
     * renderer uses the persistent scene FBO, so this never samples the
     * previously presented translucent window itself. */
    glBindTexture (GL_TEXTURE_2D, data->blur_original_texture);
    gl_y = screen_info->height - sample.y - sample.height;
    glCopyTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0,
                         sample.x, gl_y, width, height);

    glDisable (GL_BLEND);
    step = MAX (0.75f, (gfloat) radius / 12.0f);

    /* Dual Kawase downsample chain. */
    src_texture = data->blur_original_texture;
    src_w = width;
    src_h = height;
    glUseProgram (data->program_blur_down);
    for (i = 0; i < 3; i++)
    {
        glBindFramebuffer (GL_FRAMEBUFFER, data->blur_fbo[i]);
        glViewport (0, 0, data->blur_width[i], data->blur_height[i]);
        glUniform2f (data->u_blur_pixel_down,
                     step / (gfloat) src_w, step / (gfloat) src_h);
        draw_full_texture (src_texture);
        src_texture = data->blur_texture[i];
        src_w = data->blur_width[i];
        src_h = data->blur_height[i];
    }

    /* Upsample back through the same pyramid, then to full resolution. */
    glUseProgram (data->program_blur_up);
    for (i = 3; i-- > 0; )
    {
        GLuint target_fbo;
        gint target_w, target_h;

        if (i > 0)
        {
            target_fbo = data->blur_fbo[i - 1];
            target_w = data->blur_width[i - 1];
            target_h = data->blur_height[i - 1];
        }
        else
        {
            target_fbo = data->blur_source_fbo;
            target_w = width;
            target_h = height;
        }

        glBindFramebuffer (GL_FRAMEBUFFER, target_fbo);
        glViewport (0, 0, target_w, target_h);
        glUniform2f (data->u_blur_pixel_up,
                     step / (gfloat) src_w, step / (gfloat) src_h);
        draw_full_texture (src_texture);
        src_texture = (i > 0) ? data->blur_texture[i - 1]
                              : data->blur_source_texture;
        src_w = target_w;
        src_h = target_h;
    }

    glBindFramebuffer (GL_FRAMEBUFFER, previous_fbo);
    glViewport (previous_viewport[0], previous_viewport[1],
                previous_viewport[2], previous_viewport[3]);
    if (data->program_blur_mix != 0 && strength < 1.0f)
    {
        glUseProgram (data->program_blur_mix);
        glUniform1f (data->u_blur_mix_strength, strength);
        glActiveTexture (GL_TEXTURE1);
        glBindTexture (GL_TEXTURE_2D, data->blur_original_texture);
        glActiveTexture (GL_TEXTURE0);
        glBindTexture (GL_TEXTURE_2D, data->blur_source_texture);
        draw_quad (screen_info, GL_TEXTURE_2D, FALSE,
                   win.x - sample.x, win.y - sample.y, width, height,
                   win.x, win.y, win.width, win.height, content_clip);
        glActiveTexture (GL_TEXTURE1);
        glBindTexture (GL_TEXTURE_2D, 0);
        glActiveTexture (GL_TEXTURE0);
    }
    else
    {
        if (strength < 1.0f)
            glEnable (GL_BLEND);
        use_program (data->program_2d, data->u_opacity_2d, strength);
        glBindTexture (GL_TEXTURE_2D, data->blur_source_texture);
        draw_quad (screen_info, GL_TEXTURE_2D, FALSE,
                   win.x - sample.x, win.y - sample.y, width, height,
                   win.x, win.y, win.width, win.height, content_clip);
    }
    glBindTexture (GL_TEXTURE_2D, 0);
    if (content_clip != clip)
        cairo_region_destroy (content_clip);
}


/* Client-owned normal windows are faded; dock/popup UI stays legible and
 * above the selected target. Neither X11 opacity nor window state changes. */
static gboolean
peek_overlay_window (CWindow *cw)
{
    return WIN_IS_DOCK (cw) || WIN_IS_OVERRIDE (cw);
}

static gfloat
peek_window_factor (CWindow *cw)
{
    XfwmGLData *data = gl_data (cw->screen_info);

    /* Minimize snapshots must capture the real unmodified window pixels,
     * even when a GL Peek effect happens to be active at iconification. */
    if (data->snapshot_active)
        return 1.0f;
    if (data->peek_overlay_paint)
        return data->peek_overlay_alpha;
    return compositorPeekFactor (cw);
}

static gboolean
paint_window_gl (CWindow *cw, gboolean solid_part, cairo_region_t *clip)
{
    ScreenInfo *screen_info = cw->screen_info;
    XfwmGLData *data = gl_data (screen_info);
    gfloat opacity;

    if (!bind_window_texture (cw))
    {
        return FALSE;
    }

    opacity = solid_part ? 1.0f : (gfloat) cw->opacity / (gfloat) NET_WM_OPAQUE;
    if (!solid_part)
        opacity *= peek_window_factor (cw);

    if (WIN_HAS_TRANSLUCENT_FRAME(cw))
    {
        gint frame_top, frame_bottom, frame_left, frame_right;
        gint frame_width, frame_height, pixmap_width, pixmap_height;

        window_painted_size (cw, &pixmap_width, &pixmap_height);
        frame_width = pixmap_width - 2 * cw->attr.border_width;
        frame_height = pixmap_height - 2 * cw->attr.border_width;
        frame_top = frameTop (cw->c);
        frame_bottom = frameBottom (cw->c);
        frame_left = frameLeft (cw->c);
        frame_right = frameRight (cw->c);

        if (!solid_part)
        {
            use_program (data->program_win, data->u_opacity_win,
                         opacity * (gfloat) screen_info->params->frame_opacity / 100.0f);

            draw_window_part (cw, 0, 0, cw->attr.x, cw->attr.y,
                              frame_width, frame_top, clip);
            draw_window_part (cw, 0, frame_height - frame_bottom,
                              cw->attr.x, cw->attr.y + frame_height - frame_bottom,
                              frame_width, frame_bottom, clip);
            draw_window_part (cw, 0, frame_top,
                              cw->attr.x, cw->attr.y + frame_top,
                              frame_left, frame_height - frame_top - frame_bottom,
                              clip);
            draw_window_part (cw, frame_width - frame_right, frame_top,
                              cw->attr.x + frame_width - frame_right,
                              cw->attr.y + frame_top,
                              frame_right, frame_height - frame_top - frame_bottom,
                              clip);
        }

        use_program (data->program_win, data->u_opacity_win, opacity);
        draw_window_part (cw, frame_left, frame_top,
                          cw->attr.x + frame_left, cw->attr.y + frame_top,
                          frame_width - frame_left - frame_right,
                          frame_height - frame_top - frame_bottom,
                          clip);
    }
    else
    {
        gint width, height;

        window_painted_size (cw, &width, &height);
        use_program (data->program_win, data->u_opacity_win, opacity);
        draw_window_part (cw, 0, 0, cw->attr.x, cw->attr.y, width, height,
                          clip);
    }

    return TRUE;
}

static void
paint_shadow_gl (CWindow *cw, cairo_region_t *clip)
{
    ScreenInfo *screen_info = cw->screen_info;
    XfwmGLData *data = gl_data (screen_info);

    gfloat factor = peek_window_factor (cw);

    if (cw->shadow_width <= 0 || cairo_region_is_empty (clip) ||
        factor < 0.001f)
    {
        return;
    }

    glEnable (GL_BLEND);

    if (cw->gl_shadow_texture == 0)
    {
        use_program (data->program_shadow_profile, data->u_prof_opacity,
                     cw->gl_shadow_opacity * factor);
        glUniform2f (data->u_prof_size,
                     (gfloat) cw->shadow_width, (gfloat) cw->shadow_height);
        glBindTexture (GL_TEXTURE_2D, data->shadow_profile);
    }
    else
    {
        use_program (data->program_2d, data->u_opacity_2d, factor);
        glBindTexture (GL_TEXTURE_2D, cw->gl_shadow_texture);
    }

    draw_quad (screen_info, GL_TEXTURE_2D, TRUE,
               0, 0, cw->shadow_width, cw->shadow_height,
               cw->attr.x + cw->shadow_dx, cw->attr.y + cw->shadow_dy,
               cw->shadow_width, cw->shadow_height, clip);

    glBindTexture (GL_TEXTURE_2D, 0);
}

/* A snapshot survives the minimized state but never the window or GL context. */
static gboolean
animation_make_current (ScreenInfo *screen_info)
{
    XfwmGLData *data = gl_data (screen_info);

    if (!data)
    {
        return FALSE;
    }
    if (gl_context_is_current (screen_info))
    {
        return TRUE;
    }
    if (screen_info->use_egl_backend)
    {
        return eglMakeCurrent (data->egl_display, data->egl_surface,
                               data->egl_surface, data->egl_context);
    }
    return screen_info->glx_context != None &&
           glXMakeCurrent (myScreenGetXDisplay (screen_info),
                            screen_info->glx_window, screen_info->glx_context);
}

static void
animation_free (ScreenInfo *screen_info, XfwmGLAnimation *animation, gboolean repaint)
{
    if (!animation)
    {
        return;
    }
    if (repaint)
    {
        compositorDamageRegion (screen_info, &animation->bounds);
    }
    if (animation->texture != 0 && animation_make_current (screen_info))
    {
        glDeleteTextures (1, &animation->texture);
    }
    if (g_getenv ("XFWM4_ANIMATION_DEBUG"))
    {
        g_message ("animation 0x%lx end: effect=%d frames=%u%s",
                   animation->window, animation->effect, animation->frames,
                   repaint ? " cancelled" : "");
    }
    if (animation->above_windows)
        g_array_free (animation->above_windows, TRUE);
    g_free (animation);
}

static void
animation_release (CWindow *cw, gboolean repaint)
{
    animation_free (cw->screen_info, cw->gl_animation, repaint);
    cw->gl_animation = NULL;
}

void
xfwmGLFreeCloseSnapshot (CWindow *cw)
{
    animation_free (cw->screen_info, cw->gl_close_snapshot, FALSE);
    cw->gl_close_snapshot = NULL;
}

void
xfwmGLCancelWindowAnimation (CWindow *cw)
{
    if (cw)
    {
        animation_release (cw, TRUE);
    }
}

void
xfwmGLConfigureAnimation (CWindow *cw)
{
    /* Initial placement/configure events precede the first client repaint.
     * Keep a pending open request; its snapshot uses the final geometry. */
    if (cw->gl_animation && cw->gl_animation->phase == GL_ANIMATION_OPEN &&
        (cw->gl_animation->pending || cw->gl_animation->frames == 0))
        cw->gl_animation->requested = g_get_monotonic_time ();
    else
        xfwmGLCancelWindowAnimation (cw);
    xfwmGLFreeCloseSnapshot (cw);
}

void
xfwmGLCancelAnimations (ScreenInfo *screen_info)
{
    GList *list;

    if (!screen_info || !screen_info->gl_data)
    {
        return;
    }
    for (list = screen_info->cwindows; list; list = list->next)
    {
        xfwmGLCancelWindowAnimation (list->data);
        xfwmGLFreeCloseSnapshot (list->data);
    }
    XfwmGLData *data = gl_data (screen_info);
    for (list = data->closing_animations; list; list = list->next)
        animation_free (screen_info, list->data, TRUE);
    g_list_free (data->closing_animations);
    data->closing_animations = NULL;
}

/* Retain previews across unmap, but never let many minimized 4K windows
 * consume arbitrary amounts of GPU memory.  Evict the oldest cached images;
 * restored windows without a cached texture still use the safe delayed path. */
static guint64
animation_cache_bytes (ScreenInfo *screen_info)
{
    guint64 bytes = 0;
    GList *list;

    for (list = screen_info->cwindows; list; list = list->next)
    {
        CWindow *cw = list->data;
        XfwmGLAnimation *a = cw->gl_animation;
        if (a && a->texture)
            bytes += (guint64) a->source.width * a->source.height * 4;
        a = cw->gl_close_snapshot;
        if (a && a->texture)
            bytes += (guint64) a->source.width * a->source.height * 4;
    }
    XfwmGLData *data = gl_data (screen_info);
    for (list = data->closing_animations; list; list = list->next)
    {
        XfwmGLAnimation *a = list->data;
        bytes += (guint64) a->source.width * a->source.height * 4;
    }
    return bytes;
}

/* Reserve before allocating, including active effects and the old texture
 * during replacement. Background close images never evict another entry:
 * otherwise an over-budget mapped working set recaptures itself every frame. */
static gboolean
animation_reserve (ScreenInfo *screen_info, guint64 additional,
                   XfwmGLAnimation *protected, gboolean evict)
{
    const guint64 budget = 64ULL * 1024ULL * 1024ULL;
    guint64 bytes = animation_cache_bytes (screen_info);
    XfwmGLData *data = gl_data (screen_info);
    GList *list;

    if (additional > budget)
        return FALSE;
    while (bytes > budget - additional)
    {
        CWindow *oldest_cw = NULL;
        XfwmGLAnimation *oldest = NULL;
        GList *oldest_ghost = NULL;
        gboolean close_snapshot = FALSE;
        gint64 oldest_time = G_MAXINT64;
        if (!evict)
            return FALSE;
        for (list = screen_info->cwindows; list; list = list->next)
        {
            CWindow *cw = list->data;
            XfwmGLAnimation *a = cw->gl_animation;
            if (a && a != protected && a->cached && a->started < oldest_time)
            {
                oldest_time = a->started;
                oldest_cw = cw;
                oldest = a;
                close_snapshot = FALSE;
            }
            a = cw->gl_close_snapshot;
            if (a && a != protected && a->texture && a->started < oldest_time)
            {
                oldest_time = a->started;
                oldest_cw = cw;
                oldest = a;
                close_snapshot = TRUE;
            }
        }
        if (!oldest)
            for (list = data->closing_animations; list; list = list->next)
            {
                XfwmGLAnimation *a = list->data;
                if (a != protected && a->started < oldest_time)
                {
                    oldest_time = a->started;
                    oldest = a;
                    oldest_ghost = list;
                }
            }
        if (!oldest)
            for (list = screen_info->cwindows; list; list = list->next)
            {
                CWindow *cw = list->data;
                XfwmGLAnimation *a = cw->gl_animation;
                if (a && a != protected && a->texture && a->started < oldest_time)
                {
                    oldest_time = a->started;
                    oldest = a;
                    oldest_cw = cw;
                }
            }
        if (!oldest) return FALSE;
        bytes -= (guint64) oldest->source.width * oldest->source.height * 4;
        if (oldest_ghost)
        {
            animation_free (screen_info, oldest, TRUE);
            data->closing_animations = g_list_delete_link (data->closing_animations, oldest_ghost);
        }
        else if (close_snapshot)
            xfwmGLFreeCloseSnapshot (oldest_cw);
        else
            animation_release (oldest_cw, TRUE);
    }
    return TRUE;
}

static void
animation_limit_cache (ScreenInfo *screen_info)
{
    animation_reserve (screen_info, 0, NULL, TRUE);
}

/* Peek is a visual-only taskbar effect. Never map, activate, restack, or
 * change the real X11 opacity of a client. The selected window can be drawn
 * from the minimization snapshot while it remains genuinely unmapped. */
static CWindow *
peek_find_window (ScreenInfo *screen_info, Window window)
{
    return compositorPeekWindow (screen_info, window);
}

void
xfwmGLSetPeek (ScreenInfo *screen_info, Window client, gint mode,
               gint other_opacity, gint duration_ms)
{
    compositorSetPeek (screen_info, client, mode, other_opacity, duration_ms);
}

static double
animation_progress (const XfwmGLAnimation *animation, gint64 now)
{
    if (animation->whisker)
        return xfwm_popup_progress (animation->from, animation->to,
                                    now - animation->started, animation->duration);
    return xfwm_animation_progress (animation->from, animation->to,
                                    now - animation->started, animation->duration);
}

static void
animation_update_bounds (XfwmGLAnimation *animation)
{
    gint right, bottom;

    animation->bounds.x = MIN (animation->source.x, animation->icon.x) - 2;
    animation->bounds.y = MIN (animation->source.y, animation->icon.y) - 2;
    right = MAX (animation->source.x + animation->source.width,
                 animation->icon.x + animation->icon.width) + 2;
    bottom = MAX (animation->source.y + animation->source.height,
                  animation->icon.y + animation->icon.height) + 2;
    animation->bounds.width = right - animation->bounds.x;
    animation->bounds.height = bottom - animation->bounds.y;
}

static void
animation_icon_geometry (CWindow *cw, XfwmGLAnimation *animation)
{
    compositorIconTarget (cw, &animation->icon, &animation->edge);
}

static gboolean
animation_snapshot (CWindow *cw, XfwmGLAnimation *animation, gboolean evict)
{
    ScreenInfo *screen_info = cw->screen_info;
    XfwmGLData *data = gl_data (screen_info);
    Display *dpy = myScreenGetXDisplay (screen_info);
    GLuint fbo = 0, texture = 0;
    GLint old_fbo, old_program, old_active, old_2d, old_rect = 0, max_texture;
    XserverRegion extents;
    cairo_region_t *shadow_clip;
    cairo_rectangle_int_t source;
    guint64 additional, reserved_bytes;
    gboolean success = FALSE;
    gboolean reuse;

    extents = win_extents (cw, &source);
    if (extents != None)
        XFixesDestroyRegion (dpy, extents);
    glGetIntegerv (GL_MAX_TEXTURE_SIZE, &max_texture);
    if (source.width <= 0 || source.height <= 0 ||
        source.width > max_texture || source.height > max_texture ||
        (guint64) source.width * source.height > 16 * 1024 * 1024)
        return FALSE;
    reuse = animation->texture && source.width == animation->source.width &&
            source.height == animation->source.height;
    additional = reuse ? 0 : (guint64) source.width * source.height * 4;
    if (!animation_reserve (screen_info, additional, animation, evict))
        return FALSE;
    reserved_bytes = animation_cache_bytes (screen_info) + additional;

    glGetIntegerv (GL_FRAMEBUFFER_BINDING, &old_fbo);
    glGetIntegerv (GL_CURRENT_PROGRAM, &old_program);
    glGetIntegerv (GL_ACTIVE_TEXTURE, &old_active);
    glActiveTexture (GL_TEXTURE0);
    glGetIntegerv (GL_TEXTURE_BINDING_2D, &old_2d);
    if (data->tex_type != GL_TEXTURE_2D)
    {
        glGetIntegerv (GL_TEXTURE_BINDING_RECTANGLE_ARB, &old_rect);
    }
    glPushAttrib (GL_COLOR_BUFFER_BIT | GL_ENABLE_BIT | GL_VIEWPORT_BIT | GL_SCISSOR_BIT);
    glDisable (GL_SCISSOR_TEST);

    XSync (dpy, False);
    if (screen_info->use_egl_backend)
    {
        eglWaitNative (EGL_CORE_NATIVE_ENGINE);
    }
    if (!bind_window_texture (cw))
    {
        goto out;
    }
    if (reuse)
        texture = animation->texture;
    else
        glGenTextures (1, &texture);
    glBindTexture (GL_TEXTURE_2D, texture);
    set_tex_params (GL_TEXTURE_2D, GL_LINEAR);
    if (!reuse)
        glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, source.width,
                      source.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers (1, &fbo);
    glBindFramebuffer (GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    if (glCheckFramebufferStatus (GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    {
        goto out;
    }
    glViewport (0, 0, source.width, source.height);
    glClearColor (0, 0, 0, 0);
    glClear (GL_COLOR_BUFFER_BIT);
    glDisable (GL_BLEND);
    data->snapshot_active = TRUE;
    data->snapshot_bounds = source;
    shadow_clip = cairo_region_create_rectangle (&source);
    cairo_region_subtract (shadow_clip, window_shape (cw));
    paint_shadow_gl (cw, shadow_clip);
    cairo_region_destroy (shadow_clip);
    /* Window pixels, shape and decoration opacity are captured without the
     * desktop behind them. No screenshot readback or XUnmap delay is needed. */
    success = paint_window_gl (cw, FALSE, window_shape (cw));

out:
    data->snapshot_active = FALSE;
    glBindFramebuffer (GL_FRAMEBUFFER, old_fbo);
    if (fbo)
    {
        glDeleteFramebuffers (1, &fbo);
    }
    if (success)
    {
        animation->texture = texture;
        animation->source = source;
        cw->gl_close_snapshot_dirty = FALSE;
        animation_update_bounds (animation);
        if (g_getenv ("XFWM4_ANIMATION_DEBUG"))
            g_message ("snapshot 0x%lx %s: bytes=%" G_GUINT64_FORMAT,
                       cw->id, reuse ? "refresh" : "allocate",
                       reserved_bytes);
    }
    else if (texture && !reuse)
    {
        glDeleteTextures (1, &texture);
    }
    glUseProgram (old_program);
    glBindTexture (GL_TEXTURE_2D, old_2d);
    if (data->tex_type != GL_TEXTURE_2D)
    {
        glBindTexture (data->tex_type, old_rect);
    }
    glActiveTexture (old_active);
    glPopAttrib ();
    return success;
}

static gboolean
animation_tick (gpointer user_data)
{
    ScreenInfo *screen_info = user_data;
    XfwmGLData *data = gl_data (screen_info);
    GList *list;
    gboolean active = FALSE;
    gint64 now = g_get_monotonic_time ();

    if (!data || !screen_info->use_gl_render || !screen_info->compositor_active)
    {
        if (data)
        {
            data->animation_timer = 0;
        }
        return G_SOURCE_REMOVE;
    }
    for (list = screen_info->cwindows; list; list = list->next)
    {
        CWindow *cw = list->data;
        XfwmGLAnimation *animation = cw->gl_animation;
        if (!animation || animation->cached)
        {
            continue;
        }
        /* Do not leave a timer/snapshot alive if damage is fully suppressed
         * by a direct fullscreen window, or if a newly mapped client stalls. */
        if ((animation->pending && now - animation->requested > 250000) ||
            (!animation->pending && now - animation->started > animation->duration + 200000))
        {
            animation_release (cw, TRUE);
            continue;
        }
        compositorDamageRegion (screen_info, &animation->bounds);
        active = TRUE;
    }
    for (list = data->closing_animations; list; )
    {
        GList *next = list->next;
        XfwmGLAnimation *a = list->data;
        if (now - a->started > a->duration + 200000)
        {
            animation_free (screen_info, a, TRUE);
            data->closing_animations = g_list_delete_link (data->closing_animations, list);
        }
        else
        {
            compositorDamageRegion (screen_info, &a->bounds);
            active = TRUE;
        }
        list = next;
    }
    if (!active)
    {
        data->animation_timer = 0;
    }
    return active ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

static gboolean
window_animation_allowed (CWindow *cw)
{
    ScreenInfo *screen_info = cw->screen_info;
    if (cw->whisker_menu)
        return screen_info->compositor_active && screen_info->use_gl_render &&
               WIN_IS_REDIRECTED (cw);
    return screen_info->compositor_active && screen_info->use_gl_render &&
           screen_info->params->window_effect != 0 && cw->c &&
           !(cw->c->type & (WINDOW_DESKTOP | WINDOW_DOCK)) &&
           !WIN_IS_OVERRIDE (cw) && WIN_IS_REDIRECTED (cw) &&
           !screen_info->show_desktop &&
           !FLAG_TEST (cw->c->flags, CLIENT_FLAG_ICONIFIED) &&
           (FLAG_TEST (cw->c->flags, CLIENT_FLAG_STICKY) ||
            cw->c->win_workspace == screen_info->current_ws);
}

static void
window_animation_geometry (ScreenInfo *screen_info, XfwmGLAnimation *a)
{
    compositorWindowTarget (screen_info, &a->source, a->whisker, &a->icon);
    animation_update_bounds (a);
}

static void
animation_start_timer (ScreenInfo *screen_info)
{
    XfwmGLData *data = gl_data (screen_info);
    if (g_getenv ("XFWM4_ANIMATION_TEST_SESSION") && g_getenv ("XFWM4_ANIMATION_CAPTURE_DIR"))
    {
        data->capture_until = g_get_monotonic_time () + 1500000;
        GList *list;
        for (list = screen_info->cwindows; list; list = list->next)
        {
            CWindow *cw = list->data;
            if (cw->gl_animation && !cw->gl_animation->cached)
                data->capture_bounds = cw->gl_animation->bounds;
        }
        if (data->closing_animations)
            data->capture_bounds = ((XfwmGLAnimation *) g_list_last (data->closing_animations)->data)->bounds;
    }
    if (!data->animation_timer)
        data->animation_timer = g_timeout_add_full (G_PRIORITY_DEFAULT, 8, animation_tick, screen_info, NULL);
}

void
xfwmGLAnimateOpen (CWindow *cw)
{
    if (!window_animation_allowed (cw) || !animation_make_current (cw->screen_info))
        return;
    if (cw->whisker_menu)
    {
        XfwmGLData *data = gl_data (cw->screen_info);
        for (GList *li = data->closing_animations; li; )
        {
            GList *next = li->next;
            XfwmGLAnimation *old = li->data;
            if (old->whisker && old->window == cw->id)
            {
                animation_free (cw->screen_info, old, TRUE);
                data->closing_animations = g_list_delete_link (data->closing_animations, li);
            }
            li = next;
        }
    }
    animation_release (cw, TRUE);
    XfwmGLAnimation *a = g_new0 (XfwmGLAnimation, 1);
    a->phase = GL_ANIMATION_OPEN;
    a->whisker = cw->whisker_menu;
    a->window = cw->id;
    a->effect = a->whisker ? XFWM_ANIMATION_SLIDE : XFWM_ANIMATION_SCALE_FADE;
    a->from = a->progress = 1.0;
    a->to = 0.0;
    a->pending = TRUE;
    a->requested = a->started = g_get_monotonic_time ();
    a->duration = 1000LL * (a->whisker ? 180 : cw->screen_info->params->window_duration);
    a->source = (cairo_rectangle_int_t) {cw->attr.x, cw->attr.y, cw->attr.width, cw->attr.height};
    window_animation_geometry (cw->screen_info, a);
    cw->gl_animation = a;
    compositorDamageRegion (cw->screen_info, &a->bounds);
    animation_start_timer (cw->screen_info);
    if (g_getenv ("XFWM4_ANIMATION_DEBUG"))
        g_message ("animation 0x%lx open: duration=%" G_GINT64_FORMAT "us", cw->id, a->duration);
}

void
xfwmGLAnimateClose (CWindow *cw)
{
    ScreenInfo *screen_info = cw->screen_info;
    XfwmGLData *data = gl_data (screen_info);
    XfwmGLAnimation *a;
    double from = 0.0;
    GList *list;

    if (!window_animation_allowed (cw) || !WIN_IS_VIEWABLE (cw) || !data ||
        (!cw->whisker_menu && compositorPeek (screen_info)->phase > 0.001f) || !animation_make_current (screen_info))
        return;
    /* Do not turn an interrupted restore into a full-sized closing flash. */
    if (cw->gl_animation && cw->gl_animation->phase == GL_ANIMATION_MINIMIZE &&
        !cw->gl_animation->cached)
        return;
    if (cw->gl_animation && cw->gl_animation->phase == GL_ANIMATION_OPEN &&
        cw->gl_animation->texture)
    {
        a = cw->gl_animation;
        from = animation_progress (a, g_get_monotonic_time ());
        cw->gl_animation = NULL;
        xfwmGLFreeCloseSnapshot (cw);
    }
    else
    {
        a = cw->gl_close_snapshot;
        cw->gl_close_snapshot = NULL;
    }
    if (!a || !a->texture)
    {
        animation_free (screen_info, a, FALSE);
        return;
    }
    a->phase = GL_ANIMATION_CLOSE;
    a->window = cw->id;
    a->whisker = cw->whisker_menu;
    a->workspace = cw->c ? cw->c->win_workspace : screen_info->current_ws;
    a->sticky = a->whisker || FLAG_TEST (cw->c->flags, CLIENT_FLAG_STICKY);
    a->cached = a->pending = FALSE;
    a->effect = a->whisker ? XFWM_ANIMATION_SLIDE : XFWM_ANIMATION_SCALE_FADE;
    a->from = a->progress = from;
    a->to = 1.0;
    a->started = g_get_monotonic_time ();
    a->duration = xfwm_animation_duration (a->whisker ? 180 : screen_info->params->window_duration, from, 1);
    a->frames = 0;
    window_animation_geometry (screen_info, a);
    a->above_windows = g_array_new (FALSE, FALSE, sizeof (Window));
    for (list = screen_info->cwindows; list && list->data != cw; list = list->next)
    {
        Window id = ((CWindow *) list->data)->id;
        g_array_append_val (a->above_windows, id);
    }
    data->closing_animations = g_list_append (data->closing_animations, a);
    compositorDamageRegion (screen_info, &a->bounds);
    animation_limit_cache (screen_info);
    animation_start_timer (screen_info);
    if (g_getenv ("XFWM4_ANIMATION_DEBUG"))
        g_message ("animation 0x%lx close: detached snapshot", cw->id);
}

/* Capture the last painted client pixels before X11 can destroy its children.
 * Reuse an existing FBO texture between damage events; no CPU readback. */
static void
cache_close_snapshot (CWindow *cw)
{
    if (!window_animation_allowed (cw) || !WIN_IS_PAINTABLE (cw) || cw->gl_animation)
        return;
    if (cw->gl_close_snapshot && !cw->gl_close_snapshot_dirty)
        return;
    XfwmGLAnimation *a = cw->gl_close_snapshot;
    if (!a)
        a = g_new0 (XfwmGLAnimation, 1);
    GLuint previous = a->texture;
    if (!animation_snapshot (cw, a, FALSE))
    {
        if (!previous) g_free (a);
        return;
    }
    if (previous && previous != a->texture) glDeleteTextures (1, &previous);
    a->window = cw->id;
    a->cached = TRUE;
    a->started = g_get_monotonic_time ();
    cw->gl_close_snapshot = a;
    animation_limit_cache (cw->screen_info);
}

void
xfwmGLAnimateWindow (CWindow *cw, gboolean restore)
{
    ScreenInfo *screen_info = cw->screen_info;
    XfwmGLData *data = gl_data (screen_info);
    XfwmGLAnimation *animation = cw->gl_animation;
    gint64 now = g_get_monotonic_time ();

    if (!data || !screen_info->use_gl_render || !screen_info->compositor_active ||
        !cw->c || (cw->c->type & (WINDOW_DESKTOP | WINDOW_DOCK)) ||
        WIN_IS_OVERRIDE (cw) || !WIN_IS_REDIRECTED (cw) ||
        screen_info->show_desktop || !animation_make_current (screen_info))
    {
        return;
    }
    /* Preserve the frame texture for GL Peek even if minimize effects are
     * disabled. Caching is visual-only: no extra XMap or delayed XUnmap and
     * no animation timer. The same 64 MiB budget bounds memory consumption. */
    if (animation && animation->phase != GL_ANIMATION_MINIMIZE)
    {
        animation_release (cw, TRUE);
        animation = NULL;
    }
    xfwmGLFreeCloseSnapshot (cw);
    if (screen_info->params->minimize_effect == XFWM_ANIMATION_NONE)
    {
        if (restore)
        {
            animation_release (cw, FALSE);
            return;
        }

        animation_release (cw, FALSE);
        animation = g_new0 (XfwmGLAnimation, 1);
        animation->window = cw->id;
        animation->effect = XFWM_ANIMATION_NONE;
        animation->source = (cairo_rectangle_int_t) {
            cw->attr.x, cw->attr.y, cw->attr.width, cw->attr.height
        };
        animation->started = now;
        animation_icon_geometry (cw, animation);
        animation_update_bounds (animation);
        cw->gl_animation = animation;
        if (!WIN_IS_VIEWABLE (cw) || !animation_snapshot (cw, animation, TRUE))
        {
            animation_release (cw, FALSE);
            return;
        }
        animation->cached = TRUE;
        animation_limit_cache (screen_info);
        return;
    }

    if (animation && animation->texture)
    {
        /* A finished minimize owns a complete snapshot of the window, even
         * though the real X11 window is unmapped.  Use that image for every
         * frame of the restore, rather than the freshly mapped pixmap which
         * can contain only the frame before the client repaints. */
        animation->from = animation->cached ? 1.0 : animation_progress (animation, now);
        if (animation->cached && g_getenv ("XFWM4_ANIMATION_DEBUG"))
            g_message ("animation 0x%lx restoring cached window texture", cw->id);
        animation->cached = FALSE;
        animation->pending = FALSE;
    }
    else
    {
        animation_release (cw, FALSE);
        animation = g_new0 (XfwmGLAnimation, 1);
        animation->window = cw->id;
        animation->effect = screen_info->params->minimize_effect;
        animation->from = restore ? 1.0 : 0.0;
        animation->pending = restore;
        animation->requested = now;
        animation->source = (cairo_rectangle_int_t) {
            cw->attr.x, cw->attr.y, cw->attr.width, cw->attr.height
        };
        animation_icon_geometry (cw, animation);
        animation_update_bounds (animation);
        cw->gl_animation = animation;
        if (!restore && (!WIN_IS_VIEWABLE (cw) || !animation_snapshot (cw, animation, TRUE)))
        {
            animation_release (cw, FALSE);
            return;
        }
    }
    /* An iconified window may have been cached when animations were
     * disabled. If effects have since been enabled, use the currently
     * selected restore animation rather than the old NONE setting. */
    animation->effect = screen_info->params->minimize_effect;
    animation->to = restore ? 0.0 : 1.0;
    animation->started = g_get_monotonic_time ();
    animation->progress = animation->from;
    animation->duration = xfwm_animation_duration (screen_info->params->minimize_duration,
                                                  animation->from, animation->to);
    compositorDamageRegion (screen_info, &animation->bounds);
    if (!data->animation_timer)
    {
        data->animation_timer = g_timeout_add_full (G_PRIORITY_DEFAULT, 8,
                                                    animation_tick, screen_info, NULL);
    }
    if (g_getenv ("XFWM4_ANIMATION_DEBUG"))
    {
        g_message ("animation 0x%lx %s: effect=%d duration=%" G_GINT64_FORMAT
                   "us target=%d,%d %dx%d", cw->id, restore ? "restore" : "minimize",
                   animation->effect, animation->duration, animation->icon.x,
                   animation->icon.y, animation->icon.width, animation->icon.height);
    }
}

static void
advance_animations (ScreenInfo *screen_info, cairo_region_t *damage)
{
    GList *list;
    gint64 now = g_get_monotonic_time ();

    for (list = screen_info->cwindows; list; list = list->next)
    {
        CWindow *cw = list->data;
        XfwmGLAnimation *animation = cw->gl_animation;
        if (!animation || animation->cached)
        {
            continue;
        }
        /* Include both the old and new image, even on the completion frame. */
        cairo_region_union_rectangle (damage, &animation->bounds);
        if ((!cw->c && !animation->whisker) ||
            (!animation->whisker && (screen_info->show_desktop ||
            (animation->phase == GL_ANIMATION_MINIMIZE
             ? screen_info->params->minimize_effect == XFWM_ANIMATION_NONE
             : screen_info->params->window_effect == 0) ||
            (!FLAG_TEST (cw->c->flags, CLIENT_FLAG_STICKY) &&
              cw->c->win_workspace != screen_info->current_ws))))
        {
            animation_release (cw, FALSE);
            continue;
        }
        if (animation->pending)
        {
            if (WIN_IS_PAINTABLE (cw) && animation_snapshot (cw, animation, TRUE))
            {
                animation->pending = FALSE;
                /* Binding/copying the first pixmap can block on the driver.
                 * Start the timeline after capture, not before that wait. */
                now = g_get_monotonic_time ();
                animation->started = now;
                if (g_getenv ("XFWM4_ANIMATION_TEST_SESSION") && g_getenv ("XFWM4_ANIMATION_CAPTURE_DIR"))
                    gl_data (screen_info)->capture_until = now + animation->duration + 500000;
                if (animation->phase == GL_ANIMATION_OPEN)
                    window_animation_geometry (screen_info, animation);
                cairo_region_union_rectangle (damage, &animation->bounds);
                animation_limit_cache (screen_info);
                if (cw->gl_animation == NULL)
                    continue;
            }
            else
            {
                if (now - animation->requested > 250000)
                {
                    animation_release (cw, FALSE);
                }
                continue;
            }
        }
        /* Opening uses current client content, including late first paints.
         * Refresh on damage instead of holding a decoration-only snapshot. */
        if (animation->phase == GL_ANIMATION_OPEN && cw->gl_close_snapshot_dirty)
        {
            GLuint previous = animation->texture;
            if (animation_snapshot (cw, animation, TRUE))
            {
                if (previous && previous != animation->texture) glDeleteTextures (1, &previous);
                window_animation_geometry (screen_info, animation);
            }
        }
        animation->progress = animation_progress (animation, now);
        if (now - animation->started >= animation->duration)
        {
            gl_data (screen_info)->capture_final = TRUE;
            if (animation->phase == GL_ANIMATION_OPEN &&
                (animation->whisker || screen_info->params->window_effect != 0))
            {
                xfwmGLFreeCloseSnapshot (cw);
                animation->cached = TRUE;
                cw->gl_close_snapshot = animation;
                cw->gl_animation = NULL;
                animation_limit_cache (screen_info);
            }
            else if (cw->c && animation->to >= 1.0 &&
                FLAG_TEST (cw->c->flags, CLIENT_FLAG_ICONIFIED) &&
                animation->texture != 0)
            {
                /* Cached textures are kept only for genuinely minimized
                 * windows; the renderer and window destruction paths delete
                 * them on compositor shutdown, geometry changes, or closing. */
                animation->cached = TRUE;
                animation->progress = 1.0;
                animation_limit_cache (screen_info);
                if (g_getenv ("XFWM4_ANIMATION_DEBUG") && cw->gl_animation != NULL)
                    g_message ("animation 0x%lx cached minimized window texture", cw->id);
            }
            else
            {
                animation_release (cw, FALSE);
            }
        }
    }
    XfwmGLData *data = gl_data (screen_info);
    for (list = data->closing_animations; list; )
    {
        GList *next = list->next;
        XfwmGLAnimation *a = list->data;
        cairo_region_union_rectangle (damage, &a->bounds);
        if ((!a->whisker && (screen_info->show_desktop || screen_info->params->window_effect == 0 ||
            compositorPeek (screen_info)->phase > 0.001f)) || (!a->sticky && a->workspace != screen_info->current_ws) ||
            now - a->started >= a->duration)
        {
            data->capture_final = TRUE;
            if (g_getenv ("XFWM4_ANIMATION_DEBUG"))
                g_message ("animation 0x%lx close finished: frames=%u", a->window, a->frames);
            animation_free (screen_info, a, FALSE);
            data->closing_animations = g_list_delete_link (data->closing_animations, list);
        }
        else
            a->progress = animation_progress (a, now);
        list = next;
    }
}

static void
paint_animation_gl (ScreenInfo *screen_info, XfwmGLAnimation *animation, cairo_region_t *clip)
{
    XfwmGLData *data = gl_data (screen_info);
    XfwmAnimationRect source, icon;
    gint nx, ny, ix, iy, r;
    gboolean old_scissor;
    GLint old_scissor_box[4];
    double x, y;

    if (!animation->texture || animation->pending || cairo_region_is_empty (clip))
    {
        return;
    }
    source = (XfwmAnimationRect) { animation->source.x, animation->source.y,
                                  animation->source.width, animation->source.height };
    icon = (XfwmAnimationRect) { animation->icon.x, animation->icon.y,
                                animation->icon.width, animation->icon.height };
    nx = animation->effect == XFWM_ANIMATION_MAGIC_LAMP ? CLAMP ((gint) ceil (source.width / 32.0), 2, 64) : 1;
    ny = animation->effect == XFWM_ANIMATION_MAGIC_LAMP ? CLAMP ((gint) ceil (source.height / 32.0), 2, 64) : 1;
    glActiveTexture (GL_TEXTURE0);
    glBindTexture (GL_TEXTURE_2D, animation->texture);
    glEnable (GL_BLEND);
    glBlendFunc (GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    use_program (data->program_2d, data->u_opacity_2d,
                 xfwm_animation_opacity (animation->effect, animation->progress));
    old_scissor = glIsEnabled (GL_SCISSOR_TEST);
    glGetIntegerv (GL_SCISSOR_BOX, old_scissor_box);
    glEnable (GL_SCISSOR_TEST);
    for (r = 0; r < cairo_region_num_rectangles (clip); ++r)
    {
        cairo_rectangle_int_t rect;
        cairo_region_get_rectangle (clip, r, &rect);
        glScissor (rect.x, screen_info->height - rect.y - rect.height, rect.width, rect.height);
        for (iy = 0; iy < ny; ++iy)
        {
            glBegin (GL_TRIANGLE_STRIP);
            for (ix = 0; ix <= nx; ++ix)
            {
                gint side;
                for (side = 0; side < 2; ++side)
                {
                    double u = (double) ix / nx, v = (double) (iy + side) / ny;
                    xfwm_animation_vertex (animation->effect, animation->edge, &source, &icon,
                                            animation->progress, u, v, &x, &y);
                    glTexCoord2f (u, 1.0 - v);
                    glVertex2f (2.0 * x / screen_info->width - 1.0,
                                1.0 - 2.0 * y / screen_info->height);
                }
            }
            glEnd ();
        }
    }
    glScissor (old_scissor_box[0], old_scissor_box[1], old_scissor_box[2], old_scissor_box[3]);
    if (!old_scissor)
    {
        glDisable (GL_SCISSOR_TEST);
    }
    glBindTexture (GL_TEXTURE_2D, 0);
    animation->frames++;
    if (g_getenv ("XFWM4_ANIMATION_DEBUG") && animation->phase != GL_ANIMATION_MINIMIZE)
        g_message ("animation 0x%lx frame: phase=%d progress=%.3f", animation->window,
                   animation->phase, animation->progress);
}

static CWindow *
animation_find_window (ScreenInfo *screen_info, Window id)
{
    GList *list;
    for (list = screen_info->cwindows; list; list = list->next)
    {
        CWindow *cw = list->data;
        if (cw->id == id) return cw;
    }
    return NULL;
}

/* XGetImage(root) returns the untransformed X11 clients, not the GL scene.
 * Private regression sessions can read the actual composed framebuffer.
 * No readback/allocation happens in ordinary production sessions. */
static void
animation_capture_frame (ScreenInfo *screen_info)
{
    XfwmGLData *data = gl_data (screen_info);
    const gchar *directory = g_getenv ("XFWM4_ANIMATION_CAPTURE_DIR");
    gint64 now = g_get_monotonic_time ();
    gboolean whole_scene = g_getenv ("XFWM4_ANIMATION_CAPTURE_ALL") != NULL;
    if (!directory || !g_getenv ("XFWM4_ANIMATION_TEST_SESSION") ||
        (!whole_scene && now > data->capture_until) ||
        (!data->capture_final && now - data->capture_last < 50000))
        return;
    data->capture_final = FALSE;
    data->capture_last = now;
    GList *list;
    for (list = screen_info->cwindows; list; list = list->next)
    {
        CWindow *cw = list->data;
        if (cw->gl_animation && !cw->gl_animation->cached && !cw->gl_animation->pending)
            data->capture_bounds = cw->gl_animation->bounds;
    }
    if (data->closing_animations)
        data->capture_bounds = ((XfwmGLAnimation *) g_list_last (data->closing_animations)->data)->bounds;
    if (whole_scene)
        data->capture_bounds = (cairo_rectangle_int_t) {0, 0, screen_info->width, screen_info->height};
    gint x = MAX (0, data->capture_bounds.x), y = MAX (0, data->capture_bounds.y);
    gint width = MIN (screen_info->width, data->capture_bounds.x + data->capture_bounds.width) - x;
    gint height = MIN (screen_info->height, data->capture_bounds.y + data->capture_bounds.height) - y;
    if (width <= 0 || height <= 0) return;
    guchar *pixels = g_malloc_n ((gsize) width * height, 4);
    GLint alignment;
    glGetIntegerv (GL_PACK_ALIGNMENT, &alignment);
    glPixelStorei (GL_PACK_ALIGNMENT, 1);
    glReadPixels (x, screen_info->height - y - height, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glPixelStorei (GL_PACK_ALIGNMENT, alignment);
    GdkPixbuf *image = gdk_pixbuf_new_from_data (pixels, GDK_COLORSPACE_RGB, TRUE, 8,
                                               width, height, width * 4, NULL, NULL);
    GdkPixbuf *upright = gdk_pixbuf_flip (image, FALSE);
    gchar *path = g_strdup_printf ("%s/frame-%06u.png", directory, data->capture_sequence++);
    gdk_pixbuf_save (upright, path, "png", NULL, "compression", "0", NULL);
    g_free (path);
    g_object_unref (upright);
    g_object_unref (image);
    g_free (pixels);
}

/* Paint detached snapshots in their old stacking slot, below surviving UI.
 * Only the solid-prepass path culls opaque foreground pixels; bottom-up blur
 * keeps them available for sampling. Never retain a dead CWindow. */
static void
paint_closing_before (ScreenInfo *screen_info, Window above, cairo_region_t *damage,
                      gboolean backdrop_scene)
{
    XfwmGLData *data = gl_data (screen_info);
    GList *list;
    if (!damage) return;
    for (list = data->closing_animations; list; list = list->next)
    {
        XfwmGLAnimation *a = list->data;
        Window anchor = None;
        guint i;
        for (i = a->above_windows->len; i > 0; --i)
        {
            Window id = g_array_index (a->above_windows, Window, i - 1);
            CWindow *cw = animation_find_window (screen_info, id);
            if (cw && WIN_IS_PAINTABLE (cw)) { anchor = id; break; }
        }
        if (anchor != above) continue;
        cairo_region_t *clip = cairo_region_copy (damage);
        cairo_region_intersect_rectangle (clip, &a->bounds);
        /* Bottom-up blur needs even the covered pixels in its sample margin.
         * Foreground windows will cover them later in the same scene pass. */
        for (i = 0; !backdrop_scene && i < a->above_windows->len; ++i)
        {
            CWindow *cw = animation_find_window (screen_info, g_array_index (a->above_windows, Window, i));
            if (!cw || !WIN_IS_PAINTABLE (cw) || cw->gl_animation) continue;
            if (WIN_IS_OPAQUE (cw) && !WIN_HAS_TRANSLUCENT_FRAME (cw))
                cairo_region_subtract (clip, window_shape (cw));
            else if (cw->opacity == NET_WM_OPAQUE)
            {
                cairo_region_t *opaque = window_opaque_region (cw);
                if (opaque) cairo_region_subtract (clip, opaque);
            }
        }
        paint_animation_gl (screen_info, a, clip);
        cairo_region_destroy (clip);
    }
}

static gboolean
bind_root_texture (ScreenInfo *screen_info)
{
    XfwmGLData *data = gl_data (screen_info);
    DisplayInfo *display_info = screen_info->display_info;
    Display *dpy = myScreenGetXDisplay (screen_info);
    XfwmGLDepth *dc;
    GLXFBConfig fbconfig;
    GLenum target;
    Pixmap pixmap;
    Window root_ret;
    gint x_ret, y_ret;
    gint tiles_x, tiles_y;
    guint width_ret, height_ret, border_ret, depth_ret;
    gint attribs[] = {
        GLX_TEXTURE_TARGET_EXT, 0,
        GLX_TEXTURE_FORMAT_EXT, 0,
        None
    };

    if (data->root_texture != 0)
    {
        glBindTexture (data->root_tex_type, data->root_texture);
        if (data->root_dirty || data->root_damage == None)
        {
            if (data->root_glx_pixmap != None)
            {
                glXReleaseTexImageEXT (dpy, data->root_glx_pixmap, GLX_FRONT_EXT);
                glXBindTexImageEXT (dpy, data->root_glx_pixmap, GLX_FRONT_EXT, NULL);
                data->root_dirty = FALSE;
            }
            else if (data->root_egl_image != NULL)
            {
                glEGLImageTargetTexture2DOES (GL_TEXTURE_2D,
                                              data->root_egl_image);
                data->root_dirty = FALSE;
            }
        }

        return TRUE;
    }

    if (data->root_missing)
    {
        return FALSE;
    }

    pixmap = root_background_pixmap (screen_info);
    if (pixmap == None)
    {
        data->root_missing = TRUE;

        return FALSE;
    }

    myDisplayErrorTrapPush (display_info);
    if (!XGetGeometry (dpy, pixmap, &root_ret, &x_ret, &y_ret,
                       &width_ret, &height_ret, &border_ret, &depth_ret))
    {
        myDisplayErrorTrapPopIgnored (display_info);
        data->root_missing = TRUE;

        return FALSE;
    }
    myDisplayErrorTrapPopIgnored (display_info);

    fbconfig = NULL;
    target = data->tex_target;
    data->root_tex_type = data->tex_type;
    data->root_repeat = FALSE;
    data->root_y_inverted = TRUE;

    if (!screen_info->use_egl_backend)
    {
        dc = depth_config (screen_info, (gint) depth_ret);
        if (dc == NULL || !dc->usable)
        {
            data->root_missing = TRUE;

            return FALSE;
        }
        fbconfig = dc->fbconfig;
        data->root_y_inverted = dc->y_inverted;
    }

    if (((gint) width_ret < screen_info->width) ||
        ((gint) height_ret < screen_info->height))
    {
        GLXFBConfig fbconfig_2d;
        gboolean y_inverted_2d;

        if (data->tex_target == GLX_TEXTURE_2D_EXT)
        {
            data->root_repeat = TRUE;
        }
        else if (!screen_info->use_egl_backend &&
                 find_fbconfig (screen_info, (gint) depth_ret,
                                GLX_TEXTURE_2D_EXT, &fbconfig_2d,
                                &y_inverted_2d))
        {
            fbconfig = fbconfig_2d;
            target = GLX_TEXTURE_2D_EXT;
            data->root_tex_type = GL_TEXTURE_2D;
            data->root_repeat = TRUE;
            data->root_y_inverted = y_inverted_2d;
        }
    }

    tiles_x = screen_info->width / (gint) width_ret + 2;
    tiles_y = screen_info->height / (gint) height_ret + 2;
    if (!data->root_repeat &&
        (tiles_x > GL_MAX_ROOT_TILES || tiles_y > GL_MAX_ROOT_TILES ||
         tiles_x * tiles_y > GL_MAX_ROOT_TILES))
    {
        g_warning ("The desktop background is a %ux%u tile and this driver "
                   "cannot repeat it, so the desktop is drawn black.",
                   width_ret, height_ret);
        data->root_missing = TRUE;

        return FALSE;
    }

    data->root_width = (gint) width_ret;
    data->root_height = (gint) height_ret;

    if (screen_info->use_egl_backend)
    {
        data->root_egl_image =
            eglCreateImageKHR (data->egl_display, EGL_NO_CONTEXT,
                               EGL_NATIVE_PIXMAP_KHR,
                               (EGLClientBuffer) (guintptr) pixmap,
                               preserved_image);
        if (data->root_egl_image == NULL)
        {
            data->root_missing = TRUE;

            return FALSE;
        }
    }
    else
    {
        attribs[1] = (gint) target;
        attribs[3] = (depth_ret == 32) ? GLX_TEXTURE_FORMAT_RGBA_EXT
                                       : GLX_TEXTURE_FORMAT_RGB_EXT;

        myDisplayErrorTrapPush (display_info);
        data->root_glx_pixmap = glXCreatePixmap (dpy, fbconfig, pixmap, attribs);
        if (myDisplayErrorTrapPop (display_info) != Success)
        {
            data->root_glx_pixmap = None;
        }
        if (data->root_glx_pixmap == None)
        {
            data->root_missing = TRUE;

            return FALSE;
        }
    }

    glGenTextures (1, &data->root_texture);
    glBindTexture (data->root_tex_type, data->root_texture);
    set_tex_params (data->root_tex_type, GL_NEAREST);
    if (data->root_repeat)
    {
        glTexParameteri (data->root_tex_type, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri (data->root_tex_type, GL_TEXTURE_WRAP_T, GL_REPEAT);
    }
    if (screen_info->use_egl_backend)
    {
        glEGLImageTargetTexture2DOES (GL_TEXTURE_2D, data->root_egl_image);
    }
    else
    {
        glXBindTexImageEXT (dpy, data->root_glx_pixmap, GLX_FRONT_EXT, NULL);
    }
    wait_for_pixmap (display_info, pixmap);

    data->root_pixmap = pixmap;
    data->root_dirty = FALSE;
    myDisplayErrorTrapPush (display_info);
    data->root_damage = XDamageCreate (dpy, pixmap, XDamageReportNonEmpty);
    if (myDisplayErrorTrapPop (display_info) != Success)
    {
        data->root_damage = None;
    }

    return TRUE;
}

static void
paint_root_gl (ScreenInfo *screen_info, cairo_region_t *clip)
{
    XfwmGLData *data = gl_data (screen_info);

    if (cairo_region_is_empty (clip))
    {
        return;
    }

    glDisable (GL_BLEND);

    if (bind_root_texture (screen_info))
    {
        gint tex_width = data->root_width;
        gint tex_height = data->root_height;

        if (data->root_repeat)
        {
            use_program (data->program_2d, data->u_opacity_2d, 1.0f);
            draw_quad (screen_info, GL_TEXTURE_2D, data->root_y_inverted,
                       0, 0, tex_width, tex_height,
                       0, 0, screen_info->width, screen_info->height, clip);
        }
        else
        {
            cairo_rectangle_int_t area;
            gint x, y, first_x, first_y;

            use_program (data->program_win, data->u_opacity_win, 1.0f);
            cairo_region_get_extents (clip, &area);
            first_x = (area.x / tex_width) * tex_width;
            first_y = (area.y / tex_height) * tex_height;

            for (y = first_y; y < area.y + area.height; y += tex_height)
            {
                for (x = first_x; x < area.x + area.width; x += tex_width)
                {
                    draw_quad (screen_info, data->root_tex_type,
                               data->root_y_inverted,
                               0, 0, tex_width, tex_height,
                               x, y, tex_width, tex_height, clip);
                }
            }
        }
    }
    else
    {
        use_program (data->program_2d, data->u_opacity_2d, 1.0f);
        glBindTexture (GL_TEXTURE_2D, data->black_texture);
        draw_quad (screen_info, GL_TEXTURE_2D, TRUE, 0, 0, 1, 1,
                   0, 0, screen_info->width, screen_info->height, clip);
        glBindTexture (GL_TEXTURE_2D, 0);
    }
}

static void
paint_cursor_gl (ScreenInfo *screen_info)
{
    XfwmGLData *data = gl_data (screen_info);
    cairo_rectangle_int_t rect;
    cairo_region_t *clip;

    if (screen_info->cursorSerial == 0)
    {
        return;
    }

    if (data->cursor_texture == 0 || data->cursor_serial != screen_info->cursorSerial)
    {
        XFixesCursorImage *cursor;
        guint32 *pixels;
        gboolean same_size;

        cursor = XFixesGetCursorImage (myScreenGetXDisplay (screen_info));
        if (cursor == NULL)
        {
            return;
        }

        pixels = cursor_pixels_to_argb32 (cursor);

        same_size = (data->cursor_texture != 0 &&
                     data->cursor_width == cursor->width &&
                     data->cursor_height == cursor->height);
        if (data->cursor_texture == 0)
        {
            glGenTextures (1, &data->cursor_texture);
        }
        glBindTexture (GL_TEXTURE_2D, data->cursor_texture);
        glPixelStorei (GL_UNPACK_ALIGNMENT, 4);
        if (same_size)
        {
            glTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0,
                             cursor->width, cursor->height,
                             GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, pixels);
        }
        else
        {
            set_tex_params (GL_TEXTURE_2D, GL_LINEAR);
            glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, cursor->width, cursor->height,
                          0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, pixels);
        }
        g_free (pixels);

        data->cursor_width = cursor->width;
        data->cursor_height = cursor->height;
        data->cursor_serial = screen_info->cursorSerial;
        XFree (cursor);
    }
    else
    {
        glBindTexture (GL_TEXTURE_2D, data->cursor_texture);
    }

    rect.x = screen_info->cursorLocation.x;
    rect.y = screen_info->cursorLocation.y;
    rect.width = screen_info->cursorLocation.width;
    rect.height = screen_info->cursorLocation.height;
    clip = cairo_region_create_rectangle (&rect);

    glEnable (GL_BLEND);
    use_program (data->program_2d, data->u_opacity_2d, 1.0f);
    draw_quad (screen_info, GL_TEXTURE_2D, TRUE,
               0, 0, data->cursor_width, data->cursor_height,
               screen_info->cursorLocation.x, screen_info->cursorLocation.y,
               screen_info->cursorLocation.width, screen_info->cursorLocation.height,
               clip);
    cairo_region_destroy (clip);
}

static gboolean
bind_zoom_fbo (ScreenInfo *screen_info)
{
    XfwmGLData *data = gl_data (screen_info);

    if (data->fbo_failed)
    {
        return FALSE;
    }

    if (data->fbo_width != screen_info->width ||
        data->fbo_height != screen_info->height)
    {
        free_fbo (screen_info);
    }

    if (data->fbo == 0)
    {
        glGenFramebuffers (1, &data->fbo);
        glGenTextures (1, &data->fbo_texture);
        glBindTexture (GL_TEXTURE_2D, data->fbo_texture);
        set_tex_params (GL_TEXTURE_2D, GL_LINEAR);
        glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA,
                      screen_info->width, screen_info->height, 0,
                      GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        data->fbo_width = screen_info->width;
        data->fbo_height = screen_info->height;

        glBindFramebuffer (GL_FRAMEBUFFER, data->fbo);
        glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                GL_TEXTURE_2D, data->fbo_texture, 0);
        if (glCheckFramebufferStatus (GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        {
            g_warning ("Incomplete frame buffer object, magnifier disabled.");
            glBindFramebuffer (GL_FRAMEBUFFER, 0);
            free_fbo (screen_info);
            data->fbo_failed = TRUE;

            return FALSE;
        }
    }
    else
    {
        glBindFramebuffer (GL_FRAMEBUFFER, data->fbo);
    }

    return TRUE;
}

static void
draw_zoomed_scene (ScreenInfo *screen_info)
{
    XfwmGLData *data = gl_data (screen_info);
    gdouble zoom, x_offset, y_offset;
    GLenum filter;

    glBindFramebuffer (GL_FRAMEBUFFER, 0);

    zoom = XFixedToDouble (screen_info->transform.matrix[0][0]);
    if (zoom <= 0.0)
    {
        zoom = 1.0;
    }
    x_offset = XFixedToDouble (screen_info->transform.matrix[0][2]);
    y_offset = XFixedToDouble (screen_info->transform.matrix[1][2]);

    filter = ZOOM_SMOOTHING_WANTED (zoom) ? GL_LINEAR : GL_NEAREST;

    glDisable (GL_BLEND);
    glBindTexture (GL_TEXTURE_2D, data->fbo_texture);
    if (data->fbo_filter != filter)
    {
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint) filter);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint) filter);
        data->fbo_filter = filter;
    }
    use_program (data->program_2d, data->u_opacity_2d, 1.0f);

    {
        gfloat u1 = (gfloat) (x_offset / screen_info->width);
        gfloat u2 = u1 + (gfloat) zoom;
        gfloat v2 = 1.0f - (gfloat) (y_offset / screen_info->height);
        gfloat v1 = v2 - (gfloat) zoom;

        glBegin (GL_QUADS);
        glTexCoord2f (u1, v2);
        glVertex2f (-1.0f, 1.0f);
        glTexCoord2f (u2, v2);
        glVertex2f (1.0f, 1.0f);
        glTexCoord2f (u2, v1);
        glVertex2f (1.0f, -1.0f);
        glTexCoord2f (u1, v1);
        glVertex2f (-1.0f, -1.0f);
        glEnd ();
    }
}

static cairo_region_t *
fetch_damage (Display *dpy, XserverRegion damage)
{
    cairo_region_t *region;
    XRectangle *rects;
    gint nrects = 0;

    rects = XFixesFetchRegion (dpy, damage, &nrects);
    if (rects == NULL)
    {
        return cairo_region_create ();
    }

    region = region_from_rects (rects, nrects, 0, 0);
    XFree (rects);

    return region;
}

static cairo_region_t *
screen_region (ScreenInfo *screen_info)
{
    cairo_rectangle_int_t r = { 0, 0, screen_info->width, screen_info->height };

    return cairo_region_create_rectangle (&r);
}

static cairo_region_t *
get_paint_region (ScreenInfo *screen_info, cairo_region_t *damage,
                  gboolean *whole)
{
    XfwmGLData *data = gl_data (screen_info);
    Display *dpy = myScreenGetXDisplay (screen_info);
    cairo_region_t *region;
    guint age = 0;

    *whole = FALSE;

    if ((data->present_mode == GL_PRESENT_FBO) && !data->full_repaint)
    {
        return cairo_region_copy (damage);
    }

    if (((data->present_mode == GL_PRESENT_SWAP) ||
         (data->present_mode == GL_PRESENT_SCENE)) &&
        data->has_buffer_age && !data->full_repaint)
    {
        if (screen_info->use_egl_backend)
        {
            EGLint egl_age = 0;

            eglQuerySurface (data->egl_display, data->egl_surface,
                             EGL_BUFFER_AGE_EXT, &egl_age);
            age = (egl_age > 0) ? (guint) egl_age : 0;
        }
        else
        {
            glXQueryDrawable (dpy, screen_info->glx_window,
                              GLX_BACK_BUFFER_AGE_EXT, &age);
        }
    }

    if (data->profile)
    {
        data->prof_age_frames += 1.0;
        if (age > data->prof_age_max)
        {
            data->prof_age_max = age;
        }
        data->prof_age_hist[MIN (age, GL_PROF_AGE_BUCKETS - 1)] += 1.0;
        if (age > GL_DAMAGE_HISTORY)
        {
            data->prof_age_over += 1.0;
        }
        else if (age == 0)
        {
            data->prof_full_repaint += 1.0;
        }
    }

    cairo_rectangle_int_t screen = {0, 0, screen_info->width, screen_info->height};
    region = xfwm_damage_repair (damage, data->damage_history, data->damage_index,
                                GL_DAMAGE_HISTORY, age, &screen, whole);

    return region;
}

static void
debug_damage_region (const gchar *kind, const cairo_region_t *region)
{
    if (g_getenv ("XFWM4_ANIMATION_TEST_SESSION") && g_getenv ("XFWM4_DAMAGE_DEBUG"))
    {
        GString *text = g_string_new (NULL);
        for (gint i = 0; i < cairo_region_num_rectangles (region); i++)
        {
            cairo_rectangle_int_t r;
            cairo_region_get_rectangle (region, i, &r);
            g_string_append_printf (text, "%d,%d,%d,%d;", r.x, r.y, r.width, r.height);
        }
        g_message ("damage %s: %s", kind, text->str);
        g_string_free (text, TRUE);
    }
}

static void
record_damage (ScreenInfo *screen_info, cairo_region_t *damage)
{
    XfwmGLData *data = gl_data (screen_info);

    xfwm_damage_record (data->damage_history, &data->damage_index, GL_DAMAGE_HISTORY, damage);
    debug_damage_region ("history", damage);
}

static void
egl_swap (ScreenInfo *screen_info, cairo_region_t *frame_damage,
          gboolean whole)
{
    XfwmGLData *data = gl_data (screen_info);
    EGLint rects[4 * GL_MAX_PRESENT_RECTS];
    gint nrects;

    nrects = (frame_damage != NULL)
              ? cairo_region_num_rectangles (frame_damage) : 0;
    if (frame_damage)
        debug_damage_region ("swap", frame_damage);
    if (whole || nrects == 0 || data->egl_swap_with_damage == NULL)
    {
        eglSwapBuffers (data->egl_display, data->egl_surface);

        return;
    }

    nrects = xfwm_damage_swap_rects (frame_damage, screen_info->height,
                                    GL_MAX_PRESENT_RECTS, rects);

    data->egl_swap_with_damage (data->egl_display, data->egl_surface,
                                rects, nrects);
}

gboolean
xfwmGLTakeRetryPaint (ScreenInfo *screen_info)
{
    XfwmGLData *data;

    g_return_val_if_fail (screen_info != NULL, FALSE);

    data = gl_data (screen_info);
    if (data == NULL || !data->retry_paint)
    {
        return FALSE;
    }
    data->retry_paint = FALSE;

    return TRUE;
}

static gdouble
thread_cpu_ms (void)
{
    struct timespec ts;

    if (clock_gettime (CLOCK_THREAD_CPUTIME_ID, &ts) != 0)
    {
        return 0.0;
    }

    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

#define PROF_MARK(field)                                        \
    G_STMT_START {                                              \
        if (data->profile)                                      \
        {                                                       \
            gdouble prof_now = thread_cpu_ms ();                \
            data->field += prof_now - prof_mark;                \
            prof_mark = prof_now;                               \
        }                                                       \
    } G_STMT_END

static void
prof_gpu_begin (XfwmGLData *data)
{
    guint i;

    if (data->prof_query[0] == 0)
    {
        glGenQueries (GL_PROF_QUERIES, data->prof_query);
    }

    for (i = 0; i < GL_PROF_QUERIES; i++)
    {
        GLuint ready = GL_FALSE;

        if (!data->prof_query_busy[i])
        {
            continue;
        }
        glGetQueryObjectuiv (data->prof_query[i], GL_QUERY_RESULT_AVAILABLE,
                             &ready);
        if (ready)
        {
            GLuint64 ns = 0;

            glGetQueryObjectui64v (data->prof_query[i], GL_QUERY_RESULT, &ns);
            data->prof_gpu_ns += (gdouble) ns;
            data->prof_gpu_frames += 1.0;
            data->prof_query_busy[i] = FALSE;
        }
    }

    data->prof_query_active = -1;
    for (i = 0; i < GL_PROF_QUERIES; i++)
    {
        guint slot = (data->prof_query_next + i) % GL_PROF_QUERIES;

        if (!data->prof_query_busy[slot])
        {
            data->prof_query_active = (gint) slot;
            data->prof_query_next = (slot + 1) % GL_PROF_QUERIES;
            glBeginQuery (GL_TIME_ELAPSED, data->prof_query[slot]);
            break;
        }
    }
}

static void
prof_gpu_end (XfwmGLData *data)
{
    if (data->prof_query_active >= 0)
    {
        glEndQuery (GL_TIME_ELAPSED);
        data->prof_query_busy[data->prof_query_active] = TRUE;
        data->prof_query_active = -1;
    }
}

void
xfwmGLNoteFenceWait (ScreenInfo *screen_info)
{
    XfwmGLData *data = gl_data (screen_info);

    if (data != NULL && data->profile)
    {
        data->prof_fence_waits += 1.0;
    }
}

static void
stats_note_paint (ScreenInfo *screen_info, cairo_region_t *present_region,
                  gint64 prof_wall)
{
    XfwmGLData *data = gl_data (screen_info);
    gint64 t = g_get_monotonic_time ();

    if (data->profile)
    {
        data->prof_wall += (t - prof_wall) / 1000.0;
    }
    data->stat_frames++;
    if (present_region != NULL)
    {
        cairo_rectangle_int_t r;
        gint i;

        for (i = 0; i < cairo_region_num_rectangles (present_region); i++)
        {
            cairo_region_get_rectangle (present_region, i, &r);
            data->stat_pixels += (gdouble) r.width * r.height;
        }
    }
    else
    {
        data->stat_pixels += (gdouble) screen_info->width * screen_info->height;
    }

    if (t - data->stat_since >= G_USEC_PER_SEC * 5)
    {
        gdouble s = (t - data->stat_since) / 1e6;

        g_message ("paints %.1f/s, %.1f Mpix/s presented, "
                   "%.1f Mpix/s painted for %.1f Mpix/s damaged",
                   data->stat_frames / s, data->stat_pixels / s / 1e6,
                   data->stat_paint_pixels / s / 1e6,
                   data->stat_damage_pixels / s / 1e6);
        if (data->profile && data->stat_frames > 0)
        {
            gdouble n = data->stat_frames;
            gdouble cpu = data->prof_damage + data->prof_pass1 +
                          data->prof_root + data->prof_pass2 +
                          data->prof_present;

            g_message ("  per paint (ms): damage %.3f  windows %.3f  "
                       "root %.3f  blended %.3f  present %.3f  "
                       "= %.3f cpu, %.3f wall",
                       data->prof_damage / n, data->prof_pass1 / n,
                       data->prof_root / n, data->prof_pass2 / n,
                       data->prof_present / n, cpu / n,
                       data->prof_wall / n);
            g_message ("  per paint: %.1f quads over %.1f clip rects, "
                       "%.1f windows blended, %.1f shadows, "
                       "%.1f texture rebinds costing %.3f ms, "
                       "%.1f fence waits",
                       data->prof_quads / n, data->prof_rects / n,
                       data->prof_windows / n, data->prof_shadows / n,
                       data->prof_binds / n, data->prof_bind / n,
                       data->prof_fence_waits / n);
            {
                GString *h = g_string_new ("  age histogram:");
                gdouble an = (data->prof_age_frames > 0.0)
                             ? data->prof_age_frames : n;
                guint k;

                for (k = 0; k < GL_PROF_AGE_BUCKETS; k++)
                {
                    if (data->prof_age_hist[k] > 0.0)
                    {
                        g_string_append_printf (h, "  %u%s:%.0f%%", k,
                                                (k == GL_PROF_AGE_BUCKETS - 1) ? "+" : "",
                                                data->prof_age_hist[k] / an * 100.0);
                    }
                    data->prof_age_hist[k] = 0.0;
                }
                g_message ("%s", h->str);
                g_string_free (h, TRUE);
            }
            g_message ("  buffer age: %.1f%% of paints older than the "
                       "%d frame history and so repainted whole, "
                       "%.1f%% owed everything anyway, deepest age %u",
                       data->prof_age_over /
                       ((data->prof_age_frames > 0.0) ? data->prof_age_frames : n)
                       * 100.0,
                       GL_DAMAGE_HISTORY,
                       data->prof_full_repaint /
                       ((data->prof_age_frames > 0.0) ? data->prof_age_frames : n)
                       * 100.0,
                       data->prof_age_max);
            if (data->prof_gpu_frames > 0.0)
            {
                gdouble per = data->prof_gpu_ns / data->prof_gpu_frames;

                g_message ("  GPU spends %.3f ms drawing each paint, "
                           "%.2f%% of the card over %.0f measured frames",
                           per / 1e6,
                           data->prof_gpu_ns / 1e6 / (s * 1000.0) * 100.0,
                           data->prof_gpu_frames);
                data->prof_gpu_ns = data->prof_gpu_frames = 0.0;
            }
            data->prof_damage = data->prof_pass1 = data->prof_root =
                data->prof_pass2 = data->prof_present =
                data->prof_wall = data->prof_quads = data->prof_rects =
                data->prof_windows = data->prof_shadows =
                data->prof_bind = data->prof_binds =
                data->prof_fence_waits = data->prof_age_over =
                data->prof_age_frames = data->prof_full_repaint = 0.0;
        }
        data->stat_frames = 0;
        data->stat_pixels = 0.0;
        data->stat_damage_pixels = 0.0;
        data->stat_paint_pixels = 0.0;
        data->stat_since = t;
    }
}

/* Render the hovered window visually on top of ordinary applications,
 * without changing the X server's stacking order or WM state. */
static void
peek_draw_window (ScreenInfo *screen_info, Window window, gfloat opacity)
{
    XfwmGLData *data = gl_data (screen_info);
    CWindow *cw = peek_find_window (screen_info, window);
    cairo_rectangle_int_t screen = {0, 0, screen_info->width, screen_info->height};
    cairo_region_t *clip;

    if (opacity <= 0.001f || !cw)
        return;

    if (cw->gl_animation && cw->gl_animation->texture &&
        (cw->gl_animation->cached || !WIN_IS_PAINTABLE (cw)))
    {
        XfwmGLAnimation *animation = cw->gl_animation;
        cairo_rectangle_int_t rect = animation->source;
        clip = cairo_region_create_rectangle (&rect);
        cairo_region_intersect_rectangle (clip, &screen);
        if (!cairo_region_is_empty (clip))
        {
            glEnable (GL_BLEND);
            use_program (data->program_2d, data->u_opacity_2d, opacity);
            glBindTexture (GL_TEXTURE_2D, animation->texture);
            draw_quad (screen_info, GL_TEXTURE_2D, FALSE,
                       0, 0, rect.width, rect.height,
                       rect.x, rect.y, rect.width, rect.height, clip);
        }
        cairo_region_destroy (clip);
        return;
    }

    if (!WIN_IS_PAINTABLE (cw))
        return;
    clip = cairo_region_copy (window_shape (cw));
    cairo_region_intersect_rectangle (clip, &screen);
    if (!cairo_region_is_empty (clip))
    {
        data->peek_overlay_paint = TRUE;
        data->peek_overlay_alpha = opacity;
        glEnable (GL_BLEND);
        paint_window_gl (cw, FALSE, clip);
        data->peek_overlay_paint = FALSE;
    }
    cairo_region_destroy (clip);
}

static void
peek_draw_selected (ScreenInfo *screen_info)
{
    XfwmPeek *peek = compositorPeek (screen_info);
    gfloat switching = peek->previous != None ? peek->switching : 1.0f;

    if (peek->phase <= 0.001f)
        return;

    if (peek->previous != None)
        peek_draw_window (screen_info, peek->previous, peek->phase * (1.0f - switching));
    peek_draw_window (screen_info, peek->window, peek->phase * switching);
}

/* Dock windows, tasklist previews, menus, and tooltips should retain their
 * normal X11 stacking ABOVE a raised Peek window; do not fade any of them. */
static void
peek_draw_ui (ScreenInfo *screen_info)
{
    GList *list;
    cairo_rectangle_int_t screen = {0, 0, screen_info->width, screen_info->height};

    if (compositorPeek (screen_info)->phase < 0.001f)
        return;

    for (list = g_list_last (screen_info->cwindows); list; list = list->prev)
    {
        CWindow *cw = list->data;
        cairo_region_t *clip, *shadow_clip;
        cairo_region_t *shape;

        if (!peek_overlay_window (cw) || !WIN_IS_PAINTABLE (cw))
            continue;

        shape = window_shape (cw);
        clip = cairo_region_copy (shape);
        cairo_region_intersect_rectangle (clip, &screen);

        if (cw->shadow_width > 0)
        {
            cairo_rectangle_int_t shadow_rect = {
                cw->attr.x + cw->shadow_dx,
                cw->attr.y + cw->shadow_dy,
                cw->shadow_width, cw->shadow_height
            };
            shadow_clip = cairo_region_create_rectangle (&shadow_rect);
            cairo_region_intersect_rectangle (shadow_clip, &screen);
            cairo_region_subtract (shadow_clip, shape);
            paint_shadow_gl (cw, shadow_clip);
            cairo_region_destroy (shadow_clip);
        }
        if (!cairo_region_is_empty (clip))
        {
            paint_backdrop_blur_gl (cw, clip);
            glEnable (GL_BLEND);
            paint_window_gl (cw, FALSE, clip);
        }
        cairo_region_destroy (clip);
    }
}

gboolean
xfwmGLPaintAll (ScreenInfo *screen_info, XserverRegion damage)
{
    gdouble prof_mark = 0.0;
    gint64 prof_wall = 0;
    XfwmGLData *data;
    DisplayInfo *display_info;
    Display *dpy;
    cairo_region_t *frame_damage;
    cairo_region_t *paint_region;
    cairo_region_t *closing_damage = NULL;
    cairo_region_t *present_region;
    cairo_region_t *clip;
    GList *list;
    CWindow *cw;
    gboolean zoomed;
    gboolean was_full_repaint;
    gboolean painted_whole;
    gboolean backdrop_scene = FALSE;

    g_return_val_if_fail (screen_info != NULL, FALSE);
    TRACE ("entering");

    data = gl_data (screen_info);
    if (data == NULL)
    {
        return FALSE;
    }

    display_info = screen_info->display_info;
    dpy = myScreenGetXDisplay (screen_info);

    if (screen_info->use_egl_backend)
    {
        if (!gl_context_is_current (screen_info) &&
            !eglMakeCurrent (data->egl_display, data->egl_surface,
                             data->egl_surface, data->egl_context))
        {
            g_warning ("Cannot make the GL context current, GL compositing disabled.");
            return FALSE;
        }
    }
    else if ((glXGetCurrentContext () != screen_info->glx_context ||
              glXGetCurrentDrawable () != screen_info->glx_window) &&
             !glXMakeCurrent (dpy, screen_info->glx_window, screen_info->glx_context))
    {
        g_warning ("Cannot make the GL context current, GL compositing disabled.");
        return FALSE;
    }

    myDisplayErrorTrapPush (display_info);

    if (data->profile)
    {
        prof_mark = thread_cpu_ms ();
        prof_wall = g_get_monotonic_time ();
    }

    frame_damage = fetch_damage (dpy, damage);
    advance_animations (screen_info, frame_damage);
    if (compositorPeek (screen_info)->phase > 0.001f)
    {
        /* A translucent Peek window exposes desktop pixels far outside the
         * original damage. Rebuild the full scene while the effect is active
         * rather than displaying stale tiles from the normal opaque path. */
        cairo_rectangle_int_t all = {0, 0, screen_info->width, screen_info->height};
        cairo_region_union_rectangle (frame_damage, &all);
        data->full_repaint = TRUE;
    }
    /* Store and submit expanded CURRENT output changes. get_paint_region()
     * adds old buffer repairs to a separate region, never to frame_damage. */
    backdrop_scene = expand_blur_damage (screen_info, frame_damage);
    debug_damage_region ("current", frame_damage);
    paint_region = get_paint_region (screen_info, frame_damage, &painted_whole);
    debug_damage_region ("repair", paint_region);

    if (cairo_region_is_empty (paint_region))
    {
        cairo_region_destroy (paint_region);
        if (frame_damage != NULL)
        {
            cairo_region_destroy (frame_damage);
        }
        myDisplayErrorTrapPopIgnored (display_info);

        return TRUE;
    }

    present_region = NULL;
    if ((data->present_mode == GL_PRESENT_FBO))
    {
        present_region = cairo_region_copy (paint_region);
    }
    else if (data->present_mode == GL_PRESENT_SCENE)
    {
        present_region = paint_region;
        paint_region = (!data->full_repaint && frame_damage != NULL)
                       ? cairo_region_copy (frame_damage)
                       : screen_region (screen_info);
    }
    else if (data->present_mode == GL_PRESENT_COPY)
    {
        if (data->full_repaint)
        {
            present_region = screen_region (screen_info);
        }
        else
        {
            present_region = cairo_region_copy (frame_damage);
        }
    }

    /* Blur needs a bottom-to-top scene, not the opaque foreground prepass. */
    if (backdrop_scene)
    {
        expand_blur_damage (screen_info, paint_region);
        if (present_region)
            cairo_region_union (present_region, paint_region);
    }

    was_full_repaint = data->full_repaint;
    data->full_repaint = FALSE;
    data->retry_paint = FALSE;

    if (data->stats)
    {
        gint i;

        for (i = 0; frame_damage != NULL &&
                    i < cairo_region_num_rectangles (frame_damage); i++)
        {
            cairo_rectangle_int_t r;

            cairo_region_get_rectangle (frame_damage, i, &r);
            data->stat_damage_pixels += (gdouble) r.width * r.height;
        }
        for (i = 0; i < cairo_region_num_rectangles (paint_region); i++)
        {
            cairo_rectangle_int_t r;

            cairo_region_get_rectangle (paint_region, i, &r);
            data->stat_paint_pixels += (gdouble) r.width * r.height;
        }
    }

    zoomed = screen_info->zoomed;
    if ((zoomed || (data->present_mode == GL_PRESENT_FBO) ||
         (data->present_mode == GL_PRESENT_SCENE)) &&
        !bind_zoom_fbo (screen_info))
    {
        zoomed = FALSE;
        if ((data->present_mode == GL_PRESENT_FBO) ||
            (data->present_mode == GL_PRESENT_SCENE))
        {
            g_warning ("No frame buffer object, presenting with swaps instead.");
            data->present_mode = GL_PRESENT_SWAP;
            data->full_repaint = TRUE;
            data->retry_paint = TRUE;
            cairo_region_destroy (present_region);
            cairo_region_destroy (paint_region);
            if (frame_damage != NULL)
            {
                cairo_region_destroy (frame_damage);
            }
            myDisplayErrorTrapPopIgnored (display_info);

            return TRUE;
        }
    }
    if (!screen_info->zoomed)
    {
        if (data->fbo != 0 && (data->present_mode != GL_PRESENT_FBO) &&
            (data->present_mode != GL_PRESENT_SCENE))
        {
            free_fbo (screen_info);
        }
        free_cursor_texture (screen_info);
    }

    if (zoomed && present_region != NULL)
    {
        cairo_region_destroy (present_region);
        present_region = screen_region (screen_info);
    }

    PROF_MARK (prof_damage);

    if (screen_info->use_egl_backend)
    {
        gboolean fresh = data->root_dirty ||
                         (!data->root_missing && data->root_texture == 0) ||
                         (data->root_egl_image != NULL &&
                          data->root_damage == None);

        for (list = screen_info->cwindows; !fresh && list;
             list = g_list_next (list))
        {
            cw = (CWindow *) list->data;
            if (!WIN_IS_PAINTABLE(cw))
            {
                continue;
            }
            fresh = cw->gl_content_dirty || !cw->gl_texture_bound;
        }
        if (fresh)
        {
            eglWaitNative (EGL_CORE_NATIVE_ENGINE);
        }
    }

    if (data->prof_gpu)
    {
        prof_gpu_begin (data);
    }

    glViewport (0, 0, screen_info->width, screen_info->height);
    glUseProgram (data->program_win);
    glActiveTexture (GL_TEXTURE0);

    if (data->closing_animations)
        closing_damage = cairo_region_copy (paint_region);
    /* Do not add background snapshot copies to an active animation's critical
     * path. Its own source image is already retained for immediate closing. */
    gboolean animating = data->closing_animations != NULL;
    for (list = screen_info->cwindows; !animating && list; list = list->next)
    {
        CWindow *window = list->data;
        animating = window->gl_animation && !window->gl_animation->cached;
    }
    if (!animating)
        for (list = screen_info->cwindows; list; list = list->next)
            cache_close_snapshot (list->data);

    for (list = screen_info->cwindows; list; list = g_list_next (list))
    {
        cairo_region_t *shape;
        gboolean opaque_window;

        cw = (CWindow *) list->data;

        if (cw->gl_paint_clip != NULL)
        {
            cairo_region_destroy (cw->gl_paint_clip);
            cw->gl_paint_clip = NULL;
        }

        if (cw->gl_animation && !cw->gl_animation->cached)
        {
            cw->gl_paint_clip = cairo_region_copy (paint_region);
            cairo_region_intersect_rectangle (cw->gl_paint_clip, &cw->gl_animation->bounds);
            cw->skipped = FALSE;
            continue;
        }

        if (!WIN_IS_PAINTABLE(cw))
        {
            cw->skipped = TRUE;
            continue;
        }

        if (compositorPeek (screen_info)->phase > 0.001f &&
            (compositorEffectWindow (cw) || peek_overlay_window (cw)))
        {
            /* Do not let the solid pass consume regions behind windows that
             * are faded or visually elevated by Peek. */
            cw->gl_paint_clip = cairo_region_copy (paint_region);
            cw->skipped = FALSE;
            continue;
        }

        if (cw->extents == None)
        {
            cw->extents = win_extents (cw, NULL);
        }

        if (backdrop_scene)
        {
            cw->gl_paint_clip = cairo_region_copy (paint_region);
            cw->skipped = FALSE;
            continue;
        }

        shape = window_shape (cw);
        opaque_window = WIN_IS_OPAQUE(cw);

        if (opaque_window)
        {
            gboolean painted = TRUE;
            cairo_rectangle_int_t bounds;

            cairo_region_get_extents (shape, &bounds);
            if (cairo_region_contains_rectangle (paint_region, &bounds) != CAIRO_REGION_OVERLAP_OUT)
            {
                clip = cairo_region_copy (paint_region);
                cairo_region_intersect (clip, shape);
                if (!cairo_region_is_empty (clip))
                {
                    glDisable (GL_BLEND);
                    painted = paint_window_gl (cw, TRUE, clip);
                }
                cairo_region_destroy (clip);
            }

            if (!painted)
            {
                cw->skipped = TRUE;
                continue;
            }

            if (WIN_HAS_TRANSLUCENT_FRAME(cw))
            {
                cairo_rectangle_int_t client;

                window_client_area (cw, &client);
                clip = cairo_region_create_rectangle (&client);
                cairo_region_intersect (clip, shape);
                cairo_region_subtract (paint_region, clip);
                cairo_region_destroy (clip);
            }
            else
            {
                cairo_region_subtract (paint_region, shape);
            }
        }

        if ((cw->shadow_width > 0) || !opaque_window ||
            WIN_HAS_TRANSLUCENT_FRAME(cw))
        {
            cairo_rectangle_int_t blended;
            gboolean any = FALSE;

            if (cw->shadow_width > 0)
            {
                blended.x = cw->attr.x + cw->shadow_dx;
                blended.y = cw->attr.y + cw->shadow_dy;
                blended.width = cw->shadow_width;
                blended.height = cw->shadow_height;
                any = TRUE;
            }
            if (!opaque_window || WIN_HAS_TRANSLUCENT_FRAME(cw))
            {
                cairo_rectangle_int_t win;

                cairo_region_get_extents (shape, &win);
                if (any)
                {
                    gint x2 = MAX (blended.x + blended.width, win.x + win.width);
                    gint y2 = MAX (blended.y + blended.height, win.y + win.height);

                    blended.x = MIN (blended.x, win.x);
                    blended.y = MIN (blended.y, win.y);
                    blended.width = x2 - blended.x;
                    blended.height = y2 - blended.y;
                }
                else
                {
                    blended = win;
                    any = TRUE;
                }
            }

            if (any &&
                cairo_region_contains_rectangle (paint_region, &blended) !=
                CAIRO_REGION_OVERLAP_OUT)
            {
                cw->gl_paint_clip = cairo_region_copy (paint_region);
            }
        }

        if (!opaque_window && (cw->opacity == NET_WM_OPAQUE) && !WIN_IS_SHADED(cw))
        {
            cairo_region_t *opaque = window_opaque_region (cw);

            if (opaque != NULL)
            {
                cairo_region_subtract (paint_region, opaque);
            }
        }

        cw->skipped = FALSE;
    }

    PROF_MARK (prof_pass1);

    paint_root_gl (screen_info, paint_region);

    PROF_MARK (prof_root);

    for (list = g_list_last (screen_info->cwindows); list; list = g_list_previous (list))
    {
        cairo_region_t *shape;

        cw = (CWindow *) list->data;
        paint_closing_before (screen_info, cw->id, closing_damage, backdrop_scene);
        if (cw->gl_paint_clip == NULL)
        {
            continue;
        }

        if (compositorPeek (screen_info)->phase > 0.001f && peek_overlay_window (cw))
        {
            /* Defer the taskbar and popup until AFTER the raised client. */
            cairo_region_destroy (cw->gl_paint_clip);
            cw->gl_paint_clip = NULL;
            continue;
        }

        if (cw->gl_animation)
        {
            paint_animation_gl (screen_info, cw->gl_animation, cw->gl_paint_clip);
            cairo_region_destroy (cw->gl_paint_clip);
            cw->gl_paint_clip = NULL;
            continue;
        }

        shape = window_shape (cw);

        if (cw->shadow_width > 0)
        {
            cairo_rectangle_int_t sr;

            sr.x = cw->attr.x + cw->shadow_dx;
            sr.y = cw->attr.y + cw->shadow_dy;
            sr.width = cw->shadow_width;
            sr.height = cw->shadow_height;

            clip = cairo_region_create_rectangle (&sr);
            cairo_region_intersect (clip, cw->gl_paint_clip);
            cairo_region_subtract (clip, shape);
            if (data->profile && !cairo_region_is_empty (clip))
            {
                data->prof_shadows += 1.0;
            }
            paint_shadow_gl (cw, clip);
            cairo_region_destroy (clip);
        }

        if (backdrop_scene || !WIN_IS_OPAQUE(cw) || WIN_HAS_TRANSLUCENT_FRAME(cw) ||
            (compositorPeek (screen_info)->phase > 0.001f && compositorEffectWindow (cw)))
        {
            cairo_region_intersect (cw->gl_paint_clip, shape);
            if (!cairo_region_is_empty (cw->gl_paint_clip))
            {
                if (data->profile)
                {
                    data->prof_windows += 1.0;
                }
                if (compositorPeek (screen_info)->phase <= 0.001f || !compositorEffectWindow (cw))
                    paint_backdrop_blur_gl (cw, cw->gl_paint_clip);
                glEnable (GL_BLEND);
                paint_window_gl (cw, FALSE, cw->gl_paint_clip);
            }
        }

        cairo_region_destroy (cw->gl_paint_clip);
        cw->gl_paint_clip = NULL;
    }

    paint_closing_before (screen_info, None, closing_damage, backdrop_scene);
    if (closing_damage)
        cairo_region_destroy (closing_damage);

    if (compositorPeek (screen_info)->phase > 0.001f)
    {
        peek_draw_selected (screen_info);
        peek_draw_ui (screen_info);
    }

    animation_capture_frame (screen_info);

    if (screen_info->zoomed && screen_info->cursor_is_zoomed)
    {
        paint_cursor_gl (screen_info);
    }
    if (zoomed)
    {
        draw_zoomed_scene (screen_info);
    }

    PROF_MARK (prof_pass2);

    if (data->prof_gpu)
    {
        prof_gpu_end (data);
    }

    glUseProgram (0);
    glBindTexture (data->tex_type, 0);

    if (data->give_up || data->retry_paint)
    {
        if (data->retry_paint && !data->give_up &&
            (++data->bind_failures > GL_MAX_BIND_RETRIES))
        {
            g_warning ("A window pixmap could not be bound as a texture in %i "
                       "frames in a row, falling back to XRender.",
                       GL_MAX_BIND_RETRIES + 1);
            data->give_up = TRUE;
        }
        data->full_repaint = TRUE;
        if (frame_damage != NULL)
        {
            cairo_region_destroy (frame_damage);
        }
        if (present_region != NULL)
        {
            cairo_region_destroy (present_region);
        }
        cairo_region_destroy (paint_region);
        myDisplayErrorTrapPopIgnored (display_info);

        return !data->give_up;
    }
    data->bind_failures = 0;

    if (data->present_mode == GL_PRESENT_SWAP)
    {
        if (screen_info->use_egl_backend)
        {
            egl_swap (screen_info, frame_damage,
                      was_full_repaint || zoomed || painted_whole);
        }
        else
        {
            glXSwapBuffers (dpy, screen_info->glx_window);
        }
    }
    else if (data->present_mode == GL_PRESENT_SCENE)
    {
        if (!zoomed)
        {
            cairo_rectangle_int_t r;
            gint i, nrects;

            glBindFramebuffer (GL_READ_FRAMEBUFFER, data->fbo);
            glBindFramebuffer (GL_DRAW_FRAMEBUFFER, 0);

            nrects = cairo_region_num_rectangles (present_region);
            if (nrects > GL_MAX_PRESENT_RECTS)
            {
                cairo_region_get_extents (present_region, &r);
                cairo_region_destroy (present_region);
                present_region = cairo_region_create_rectangle (&r);
                nrects = 1;
            }

            for (i = 0; i < nrects; i++)
            {
                gint gl_y;

                cairo_region_get_rectangle (present_region, i, &r);
                gl_y = screen_info->height - r.y - r.height;
                glBlitFramebuffer (r.x, gl_y, r.x + r.width, gl_y + r.height,
                                   r.x, gl_y, r.x + r.width, gl_y + r.height,
                                   GL_COLOR_BUFFER_BIT, GL_NEAREST);
            }
            glBindFramebuffer (GL_FRAMEBUFFER, 0);
        }

        if (screen_info->use_egl_backend)
        {
            egl_swap (screen_info, frame_damage,
                      was_full_repaint || zoomed || painted_whole);
        }
        else
        {
            glXSwapBuffers (dpy, screen_info->glx_window);
        }
    }
    else
    {
        cairo_rectangle_int_t r;
        gboolean from_fbo = (data->present_mode == GL_PRESENT_FBO) && !zoomed;
        gint i, nrects;

        if (from_fbo)
        {
            glBindFramebuffer (GL_READ_FRAMEBUFFER, data->fbo);
            glBindFramebuffer (GL_DRAW_FRAMEBUFFER, 0);
        }

        nrects = cairo_region_num_rectangles (present_region);
        if (nrects > GL_MAX_PRESENT_RECTS)
        {
            cairo_region_get_extents (present_region, &r);
            cairo_region_destroy (present_region);
            present_region = cairo_region_create_rectangle (&r);
            nrects = 1;
        }

        for (i = 0; i < nrects; i++)
        {
            gint gl_y;

            cairo_region_get_rectangle (present_region, i, &r);
            gl_y = screen_info->height - r.y - r.height;

            if (from_fbo)
            {
                glBlitFramebuffer (r.x, gl_y, r.x + r.width, gl_y + r.height,
                                   r.x, gl_y, r.x + r.width, gl_y + r.height,
                                   GL_COLOR_BUFFER_BIT, GL_NEAREST);
            }
            glXCopySubBufferMESA (dpy, screen_info->glx_window,
                                  r.x, gl_y, r.width, r.height);
        }

        if (from_fbo)
        {
            glBindFramebuffer (GL_FRAMEBUFFER, 0);
        }
    }

    if (frame_damage != NULL && ((data->present_mode == GL_PRESENT_SWAP) ||
                                 (data->present_mode == GL_PRESENT_SCENE)))
    {
        record_damage (screen_info, frame_damage);
    }
    else if (frame_damage != NULL)
    {
        cairo_region_destroy (frame_damage);
    }
    PROF_MARK (prof_present);

    if (data->stats)
    {
        stats_note_paint (screen_info, present_region, prof_wall);
    }

    if (present_region != NULL)
    {
        cairo_region_destroy (present_region);
    }

    if (screen_info->has_ext_arb_sync)
    {
#if defined (glDeleteSync)
        if (screen_info->gl_sync)
        {
            glDeleteSync (screen_info->gl_sync);
            screen_info->gl_sync = 0;
        }
#endif
#if defined (glFenceSync)
        if (data->use_fence)
        {
            screen_info->gl_sync = glFenceSync (GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        }
#endif
    }

    cairo_region_destroy (paint_region);
    myDisplayErrorTrapPopIgnored (display_info);

    return TRUE;
}

#endif /* HAVE_EPOXY */

#endif /* HAVE_COMPOSITOR */
