# xfwm4-gl

`xfwm4-gl` is a fork of Xfce [xfwm4](https://gitlab.xfce.org/xfce/xfwm4) that
adds OpenGL compositing. XRender, the original way, stays as a fallback.

MiniOS localization configuration, terminology and lock state live in the
parent `minios-live` workspace. Run `lokit status --target xfwm4-gl` from that
workspace root; the PO catalogs remain in this repository.

There is also a new option, on by default, that turns compositing off while a
fullscreen application has focus. It is worth about 3% more frames for
applications that do not ask for that themselves.

Compared to XRender it uses **about a third less power** for the same work and
provides **about 10% more frames per second** to applications. The gap grows
with load, up to **25%** with eight windows drawing flat out.

<table>
<tr><th>Driver</th><th>Renderer</th><th>FPS</th><th>CPU (ms/s)</th><th>CPU (W)</th><th>GPU (W)</th></tr>
<tr><td rowspan="3">AMD (Mesa radeonsi)</td><td>no compositor</td><td>180.5</td><td>-</td><td colspan="2" align="center">18.9</td></tr>
<tr><td>XRender</td><td>161.6</td><td>11.2</td><td colspan="2" align="center">21.0</td></tr>
<tr><td>OpenGL</td><td>174.2</td><td>13.2</td><td colspan="2" align="center">20.2</td></tr>
<tr><td rowspan="3">NVIDIA proprietary</td><td>no compositor</td><td>166.3</td><td>-</td><td>4.0</td><td>37.2</td></tr>
<tr><td>XRender</td><td>160.9</td><td>17.5</td><td>5.8</td><td>43.0</td></tr>
<tr><td>OpenGL</td><td>158.6</td><td>16.2</td><td>4.7</td><td>43.2</td></tr>
<tr><td rowspan="3">NVIDIA Mesa (zink)</td><td>no compositor</td><td>149.6</td><td>-</td><td>6.8</td><td>N/A</td></tr>
<tr><td>XRender</td><td>149.1</td><td>23.5</td><td>14.8</td><td>N/A</td></tr>
<tr><td>OpenGL</td><td>149.1</td><td>22.5</td><td>14.8</td><td>N/A</td></tr>
</table>

FPS is measured in a windowed benchmark running as fast as it can; CPU and
power are measured while the same benchmark is locked at 60 fps. The AMD
machine has the graphics inside the processor, so its one power figure covers
both. The numbers come from [wmbench](https://github.com/fulalas/wmbench),
which measures any window manager, so they can be reproduced or compared with
another desktop.

## Features

OpenGL compositing is on by default; if the driver cannot do it, XRender is
used instead.

OpenGL talks to the driver through EGL by default, because it was the cheapest
on the processor on every driver tested. `XFWM4_GL_BACKEND=glx` switches to
GLX, and if EGL cannot start, GLX takes over on its own. `XFWM4_PAINT_STATS`
set to `1` prints how many frames were painted per second every 5 seconds.

Only the parts of the screen that changed are painted, then the whole buffer
is swapped, which the display hardware does for free. `XFWM4_GL_PRESENT` set
to `copy` or `fbo` picks other ways of getting the frame on screen; they are
for drivers that behave differently, not for everyday use.

Adaptive vsync can be picked now: frames wait for the screen while they can
keep up, and stop waiting when they cannot, so a slow moment costs no extra
lag. It needs the GLX backend (`XFWM4_GL_BACKEND=glx`) or XRender; the EGL
default always syncs every frame. See [Settings](#settings).

To check which renderer is in use, open Window Manager Tweaks, Compositor tab:

<img src="https://github.com/user-attachments/assets/1f2c71c2-1027-4006-b626-6dea17beae0f" />

* **Use OpenGL for compositing (default on)** — enables the OpenGL renderer;
  XRender is used if it cannot start.
* **Suspend compositing for focused fullscreen windows (default on)** — lets a
  fullscreen application with focus draw straight to the screen, and nothing
  behind it is painted. Useful for games. Other monitors are composited as
  usual, and a window opened over it that needs compositing, a menu with a
  shadow or the picker of a screenshot tool for instance, brings compositing
  back for as long as it is there. While the application
  draws directly, avoiding tearing is up to it: fine for OpenGL and Vulkan,
  others may tear. An application can ask never to be bypassed, which is what
  `mpv --x11-bypass-compositor=never` does.
* **Display fullscreen overlay windows directly (default on)** — same as the
  original `xfwm4`; it only gained a tooltip. It covers quite old games and
  players that bypass the window manager without saying anything.

## Usage

All files keep the same names and paths as the original, so once installed the
session loads it automatically.

To try it without installing, replace the running window manager:

    ./path_to_new_build/src/xfwm4 --replace

## Settings

`/general/use_gl_compositing` turns the OpenGL renderer on and off. It takes
effect immediately.

`/general/blur_enabled` is the master switch for backdrop blur. It is enabled
by default and is exposed in Window Manager Tweaks under Compositor.

`/general/blur_docks` enables a frosted-glass backdrop blur behind translucent
dock windows such as `xfce4-panel`. It is enabled by default and only affects
the OpenGL compositor; the XRender fallback keeps the normal transparency.

`/general/blur_transparent_windows` applies the same backdrop blur to translucent
managed application windows (including dialogs), for example a terminal using
window opacity. It is also enabled by default. Opaque windows are unaffected.

Additional window-type switches are available in **Window Manager Tweaks →
Compositor**. `/general/blur_previews` controls MiniOS tasklist preview popups
(enabled by default); `/general/blur_menus` controls EWMH popup/dropdown menus;
`/general/blur_tooltips` controls EWMH tooltips; and
`/general/blur_notifications` controls EWMH notification windows. The last
three categories are disabled by default. These settings only affect windows
with transparent content and the OpenGL compositor; XRender remains unchanged.
The tasklist explicitly marks its popup with `_XFWM4_PREVIEW_BLUR`, separating
its interactive preview from generic tooltip blur.

Shadow opacity does not request backdrop blur. The GL renderer paints the
shadows generated by `xfwm4` separately; GTK client-side shadow margins are
excluded using `_GTK_FRAME_EXTENTS` where present. Clients can also declare
`_XFWM4_BLUR_CONTENT_RECT` as four CARDINAL values `[x, y, width, height]`
in drawable-local X11 pixels. The tasklist publishes this automatically, so
the compositor shadow stays outside the surface and cannot be included in
the backdrop blur of the preview itself.
Backdrop blur follows the window's overall X11 opacity, so a fading preview
loses its blur together with its contents. An entirely invisible window adds
no blur; the configured strength remains unchanged for fully visible windows.
Tasklist and window-switcher popups use the **same native Xfce Gaussian shadow**
as ordinary windows. `/general/show_popup_shadow` controls both, using the
standard Xfce shadow opacity and offsets. The renderer paints outside the popup
instead of clipping a GTK CSS box-shadow into a visible secondary rim.
There is no separate tasklist shadow property or strength. Late preview/type
hints invalidate the old compositor extents.
Applications that paint custom shadows inside their ARGB buffers but do not
provide content bounds cannot be reliably distinguished from ordinary
transparency; those windows retain the default whole-window blur fallback.

`/general/blur_alt_tab` independently blurs behind the translucent window
switcher while using OpenGL (enabled by default). The switcher advertises
`_XFWM4_TABWIN_BLUR` and `_XFWM4_BLUR_CONTENT_RECT`, so the blur is clipped
inside its GTK theme frame rather than into the external shadow.

`/general/blur_radius` controls the blur sampling radius from 1 to 64 pixels
(default: 14). `/general/blur_strength` blends between the original backdrop and
the blurred result from 0 to 100 percent (default: 80).

**Window-switcher appearance:** Window Manager Tweaks → Cycling → Appearance
customizes the native `#xfwm-tabwin` surface. The tasklist uses the same themed
layout and shared `xfwm-tabwin-style.h` implementation, with its own saved
preview settings. There is no alternate tasklist appearance mode.

| xfconf key under `/general/` | Default | Range / purpose |
| --- | --- | --- |
| `cycle_background_custom` | false | Use a custom RGB background instead of the GTK theme |
| `cycle_background_color` | `#30343b` | GTK color chooser value |
| `cycle_background_opacity` | 80 | 0–100%, surface only |
| `cycle_corner_radius` | 12 | 0–36 px |
| `cycle_border_width` | 0 | 0–5 px |
| `cycle_spacing` | 4 | 0–20 px between cards |
| `cycle_content_padding` | 24 | 0–64 px around cards |
| `cycle_preview_width` | 300 | 120–800 px maximum card width |
| `cycle_preview_height` | 192 | 80–340 px maximum card height, including footer and selection inset |
| `cycle_selection_padding` | 5 | 0–32 px inside the selected card |
| `cycle_selection_custom` | false | Override the theme selection color |
| `cycle_selection_color` | `#398ee7` | GTK color chooser value |
| `cycle_selection_radius` | 6 | 0–36 px |
| `cycle_selection_opacity` | 30 | 0–100%, selection background only |

These appearance options work with OpenGL, XRender and no compositing. The GTK
theme supplies the selection color unless overridden and retains application
typography; blur and native shadows remain separate compositor options.
Surface appearance updates refresh an already-open window switcher. Thumbnail
dimensions and selection settings apply when the switcher next opens. Images
preserve their aspect ratio; large grids reduce thumbnail dimensions to fit.

**Reset** in the appearance dialog restores its background,
thumbnail dimensions, spacing, padding and selection settings immediately.
It preserves window-switching behavior and Peek options. This is the only
reset button for the window switcher.

The Compositor tab has its own **Reset** button, available even
when compositing is disabled. It restores compositor/backend switches, blur,
opening/closing and minimize effects, shadows, zoom and opacity controls from
the installed `xfwm4/defaults` file. Other Window Manager Tweaks pages retain
their current settings. Resetting either section keeps the dialog open.

`/general/window_effect` selects the shared opening/closing effect: `0` is
None, `1` is Scale + Fade (default), and `2` is Fade. `/general/window_duration`
sets its duration (default 160 ms, range 50–1000). These controls are under
Window Manager Tweaks → Compositor → Open and close. Both OpenGL and XRender
support these effects; disabling compositing gives immediate opening/closing.
Closing releases the X11 client immediately and animates an independent image;
GL owns textures and XRender owns server-side pixmaps. Closing and
minimize snapshots share the 64 MiB per-screen budget.

`/general/minimize_effect` selects the minimize/restore effect:
`0` disables animations, `1` uses Scale + Fade (the default), and `2` uses
Magic Lamp (OpenGL only). Scale + Fade works on both backends.
`/general/minimize_duration` is the duration in milliseconds
(default: 180, permitted range: 50–1000). The settings are available in Window
Manager Tweaks, Compositor tab. Magic Lamp is unavailable in the XRender
selector; its saved value is preserved across backend changes. With compositing
disabled, windows minimize and restore immediately.

While a window is minimized, each backend can retain its fully rendered
image for the restore animation. This prevents the client contents from
appearing suddenly after the frame animation has already begun. The total
retained snapshot budget is 64 MiB per compositor screen; older cached images
are evicted if necessary and destroyed when their window or compositor closes.
`animation-math` and `compositor-effects` share timing, easing, motion targets,
icon geometry and Peek state; backend-specific code owns images and rendering.

**Peek:** the Window Buttons plugin can temporarily preview a client
above other windows while those are dimmed or hidden. It communicates a
transient `_XFWM4_GL_PEEK` root-window property containing the client XID,
mode, other-window opacity and transition duration. The compositor animates
only its composed output, leaving window mapping, focus, stacking and X11 opacity
unchanged. Peek automatically expires if plugin heartbeat messages stop.
The historical property name remains compatible with earlier GL clients.
The WM advertises `_XFWM4_PEEK_SUPPORTED = 1` while compositing is active;
the tasklist requires this capability before requesting XRender Peek from an
older WM. Tasklist and window-switcher Peek use Dim other windows by default,
with the selected-window outline disabled on GL and XRender.
Minimized clients can be shown through retained snapshots, subject to the
64 MiB snapshot budget and availability of a previous capture. Snapshots are
also retained with minimize/restore animations disabled, without animating the
window itself. Adjacent hovered thumbnails crossfade without flashing the
normal desktop. Stationary Peek avoids unnecessary full-scene repaints.

For development, `tests/check-peek /path/to/xfwm4 opengl|xrender output-dir`
runs against an isolated Xvfb/DBus/xfconf session. The OpenGL case requires
a compatible renderer (software GL is normally blacklisted by the production
binary); the XRender case exercises the same Peek assertions.

**Window-switcher Peek and taskbar window framing:** `/general/cycle_peek_mode`
selects Off (0), dim other windows (1, default), or hide them (2) during
window switching. `/general/cycle_peek_opacity` sets other windows' opacity for dim mode
(default 15%). The selected window is rendered through the shared Peek
path, without altering client focus or stacking until switching completes;
when Peek is enabled, the switcher's wireframe retains its native outline
but omits its semitransparent fill so it does not mask the effect.
`/general/cycle_draw_frame` controls the native window-switcher outline and
`/general/cycle_frame_fill` independently enables the selected-window
highlight inside that frame. Disabling the outline disables its fill; with
the visual effect Off, the outline remains mandatory.

The tasklist can request that **same native window-switcher outline** using
`_XFWM4_HOVER_FRAME` on the root window. The property contains two CARDINAL
values: the selected client XID (0 clears), and whether to highlight its
interior (0 for outline only, 1 for the fill). Legacy single-value requests
still work. The window manager invokes the same
`wireframeCreateWithFill`/`wireframeUpdate`/`wireframeDelete` implementation
for both the switcher and the tasklist. Its
850-ms lost-panel heartbeat watchdog removes a stale outline. The outline
works with GL, XRender and compositing disabled, and no X11 client is raised,
restored or activated merely by hovering.

`/general/suspend_compositing_fullscreen` lets a fullscreen window with focus
draw straight to the screen.

`/general/vblank_mode`, or `--vblank` on the command line, read at startup
only:

| value | description |
| --- | --- |
| `auto` | (default) sync every frame to the screen |
| `adaptive` | (new) turns vsync off when the frame rate falls below the refresh rate, avoiding stutter and input lag (needs GLX, `XFWM4_GL_BACKEND=glx`, or XRender, plus `GLX_EXT_swap_control_tear`; otherwise, and always with the EGL default, same as `auto`) |
| `off` | no sync, fastest, tears |

Two more values exist, `glx` and `xpresent`, but with the OpenGL renderer both
behave like `auto`. They only differ after falling back to XRender.

## When it falls back to XRender

Whisker Menu uses a separate bottom-edge slide on GL and XRender. It enters
upwards at its normal size and exits downwards; the general opening/closing
effect does not apply. The compositor animates an owned image after unmap,
so keyboard focus and application launch are released immediately. Reopening
the same menu cancels its old closing image. Without compositing it is immediate.

The OpenGL path is skipped, quietly and without breaking the session, if:

* `libepoxy` was missing at build time
* the driver is a software renderer such as `llvmpipe` or `swrast`
* the driver is older than OpenGL 2.0, has no frame buffer objects, or cannot
  hand windows over as textures
* a colour depth the driver cannot hand over
* a window that fails to bind as a texture several frames in a row

## Other changes to xfwm4

Everything this fork changes outside the OpenGL renderer, compared with
[xfwm4](https://gitlab.xfce.org/xfce/xfwm4/) 4.20.0.

New features:

* two new boxes in Window Manager Tweaks, under Compositor: one turns the
  OpenGL renderer on and off, one turns off the fullscreen behaviour above.
  The dialog also says which renderer is running
* a fullscreen window with focus draws straight to the screen, so games and
  video get it to themselves while the other monitors stay composited
* `--vblank` parameter now supports the `adaptive` value
* buttons in the title bar can light up under the pointer on a window that is
  not focused, from a new `inactive-prelight` image in the theme
* any tool can read the current renderer and vsync mode by running
  `xprop -root _XFWM4_RENDER_BACKEND _XFWM4_VSYNC`

Fixes:

* a window keeps its border while it is resized, instead of losing an edge for
  a moment on every step
* a window that is resized and moved at the same time, by dragging a corner,
  no longer appears to jump and come back
* a shrinking window leaves no stale strip behind along the edge it gave up
* title bar buttons are redrawn only when they really change
* the whole screen is drawn once when compositing starts, so nothing stale is
  left on it, and switching compositing off no longer risks a crash
* resizing a window no longer stops dead for half a second the first time.
  xfwm4 was asking programs to report their drawing in a way it never answered,
  so they waited for a reply that never came
* it now waits a tenth of a second, not half a second, for a program to draw
  while you resize it, and a program that is slow once keeps smooth resizing
  afterwards instead of losing it for good
* a window and its dialogs are remembered as a family instead of being worked
  out by going through every window on screen. That happened on every focus,
  raise, restack, map, unmap and workspace change
* finding the window an event belongs to is a direct lookup now, not a walk
  through the whole list
* a window opened over a fullscreen video now shows up over it. The video used
  to stay on screen and take the clicks meant for the new window
* windows are painted again once compositing comes back, instead of the screen
  keeping what was on it
* the panel and the taskbar no longer rebuild themselves every time a
  fullscreen window takes the screen and gives it back
* windows are drawn in the right order when one of them is raised or lowered
* a window that is opening is no longer covered by the border pattern of its
  own frame
* the decoration asks for fewer redraws, and a theme that ships only a
  stretched border image is used instead of being skipped

## Requirements

No new build dependency. `xfwm4` already builds against `libepoxy` for vsync
and `xfwm4-gl` uses it for the OpenGL renderer too. It is optional upstream,
so check that the configure summary says `Epoxy support: yes`.

At runtime the driver needs OpenGL 2.0 or newer, frame buffer objects, and the
ability to hand windows over as textures. Every driver of the last 15 years or
so has all three.

VirtualBox and QEMU can use the OpenGL renderer as long as they have their
3D acceleration on.

## License

Same as `xfwm4`: GNU General Public License, version 2 or later. See
[COPYING](COPYING) for the full text.
