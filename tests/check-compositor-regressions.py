#!/usr/bin/python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Real WM regressions: bounded snapshots, fresh ghosts, blur layering and GTK geometry.

Read GL framebuffer captures, not XGetImage(root)'s untransformed clients.
Run through check-compositor-regressions in its disposable display/profile.
"""
import os
import pathlib
import re
import subprocess
import sys
import time

from PIL import Image, ImageChops
from Xlib import X, Xatom, Xutil, display, error, protocol

assert os.environ.get("XFWM4_ANIMATION_TEST_SESSION") == "1"
wm_path, suite, output = sys.argv[1:]
out = pathlib.Path(output)
d = display.Display()
root = d.screen().root
windows = []


def setting(name, value, kind="int"):
    subprocess.check_call(["xfconf-query", "-c", "xfwm4", "-p", "/general/" + name,
                           "-n", "-t", kind, "-s", str(value)])


def wait(callback, message, seconds=10):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        d.sync()
        result = callback()
        if result:
            return result
        assert wm.poll() is None, (out / "wm.log").read_text()
        time.sleep(.04)
    raise AssertionError(message)


def prop(window, name):
    return window.get_full_property(d.intern_atom(name), X.AnyPropertyType)


def frame(window):
    return window.query_tree().parent


def create(x, y, width, height, color, managed=True):
    window = root.create_window(x, y, width, height, 0, d.screen().root_depth,
                               X.InputOutput, X.CopyFromParent, background_pixel=color,
                               override_redirect=not managed,
                               event_mask=X.ExposureMask | X.StructureNotifyMask)
    window.set_wm_name("compositor-regression")
    window.set_wm_class("compositor-regression", "CompositorRegression")
    if width > d.screen().width_in_pixels:
        window.set_wm_normal_hints(flags=Xutil.PMinSize, min_width=width, min_height=height)
    window.map()
    d.flush()
    windows.append(window)
    if managed:
        wait(lambda: frame(window).id != root.id, "Client not framed")
        subprocess.check_call(["wmctrl", "-ir", hex(window.id), "-e",
                               "0,%d,%d,%d,%d" % (x, y, width, height)])
    time.sleep(.15)
    return window


def repaint(window, color):
    window.change_attributes(background_pixel=color)
    window.clear_area(0, 0, 0, 0)
    d.flush()


def opacity(window, value):
    window.change_property(d.intern_atom("_NET_WM_WINDOW_OPACITY"), Xatom.CARDINAL,
                           32, [int(value * 0xffffffff)])
    d.flush()


def captures():
    return sorted((out / "frames").glob("frame-*.png"))


def image_after(previous):
    path = wait(lambda: captures()[-1] if len(captures()) > previous else None,
                "No composed framebuffer capture")
    # gdk_pixbuf_save can still be finishing the latest file.
    for _ in range(50):
        try:
            with Image.open(path) as image:
                return image.convert("RGB")
        except (OSError, ValueError, SyntaxError):
            time.sleep(.02)
    raise AssertionError("Incomplete capture: " + str(path))


def full_image():
    count = len(captures())
    root.send_event(protocol.event.Expose(window=root, x=0, y=0,
                    width=d.screen().width_in_pixels, height=d.screen().height_in_pixels,
                    count=0), event_mask=X.ExposureMask)
    d.flush()
    time.sleep(.12)
    return image_after(count)


def log():
    return (out / "wm.log").read_text()


def refresh_count(xid):
    return log().count("snapshot 0x%x refresh:" % xid)


def snapshot_tests():
    setting("blur_enabled", "false", "bool")
    setting("window_effect", 0)
    large = [create(20, 20, 2100, 2100, 0x303030 + n * 0x101010) for n in range(4)]
    sizes = [(w.get_geometry().width, w.get_geometry().height) for w in large]
    assert all(size == (2100, 2100) for size in sizes), sizes
    setting("window_effect", 1)
    repaint(large[-1], 0x505050)
    time.sleep(.6)
    allocated = log().count(" allocate:")
    assert 0 < allocated < len(large), ("Cache pressure not reached", allocated)
    for n in range(8):
        repaint(large[-1], 0x505050 + n)
        time.sleep(.12)
    assert log().count(" allocate:") == allocated, "Over-budget close cache reallocates unchanged windows"
    print("PASS close-cache admission: no allocations during eight over-budget repaints", flush=True)

    setting("minimize_duration", 1000)
    for w in large:
        root.send_event(protocol.event.ClientMessage(window=w, client_type=d.intern_atom("WM_CHANGE_STATE"),
                        data=(32, [3, 0, 0, 0, 0])),
                        event_mask=X.SubstructureRedirectMask | X.SubstructureNotifyMask)
    d.flush()
    wait(lambda: log().count(" minimize:") >= 4, "Batch minimize did not start")
    time.sleep(1.4)
    sizes = [int(n) for n in re.findall(r"snapshot .*bytes=(\d+)", log())]
    assert sizes and max(sizes) <= 64 * 1024 * 1024, ("Active snapshot budget exceeded", sizes)
    for w in large:
        w.destroy()
        windows.remove(w)
    d.flush()
    time.sleep(.3)
    print("PASS active batch minimize: all admitted snapshot allocations stay within 64 MiB", flush=True)

    setting("window_effect", 0)
    b = create(100, 100, 240, 160, 0x2040dd)
    setting("window_effect", 1)
    setting("window_duration", 800)
    full_image()
    b_frame = frame(b).id
    before = refresh_count(b_frame)
    a = create(700, 100, 120, 100, 0x20dd40)
    repaint(b, 0xdd2020)
    time.sleep(1.1)
    assert refresh_count(b_frame) > before, "Content update lost while another window animated"
    before = refresh_count(b_frame)
    opacity(b, .25)
    time.sleep(.25)
    assert refresh_count(b_frame) > before, "Native opacity did not refresh close snapshot"
    normal = full_image()
    count = len(captures())
    b.destroy()
    windows.remove(b)
    d.flush()
    ghost = image_after(count)
    normal_red = normal.getpixel((180, 180))[0]
    ghost_red = ghost.getpixel((180, 180))[0]
    assert 20 < normal_red < 90 and 0 < ghost_red <= normal_red + 8, (normal_red, ghost_red)
    assert ghost.getpixel((180, 180))[0] > ghost.getpixel((180, 180))[2], "Close used old blue content"
    print("PASS deferred content and opacity freshness: closing pixels retain current red/25% opacity", flush=True)


def mark_blur(window):
    window.change_property(d.intern_atom("_XFWM4_PREVIEW_BLUR"), Xatom.CARDINAL, 32, [1])
    d.flush()


def blur_tests():
    setting("window_effect", 0)
    setting("show_frame_shadow", "false", "bool")
    setting("show_popup_shadow", "false", "bool")
    setting("blur_radius", 32)
    setting("blur_strength", 100)
    base = create(0, 0, 1200, 740, 0x205020, managed=False)
    gc = base.create_gc(foreground=0x80d080)
    for x in range(0, 1200, 16):
        base.fill_rectangle(gc, x, 0, 8, 740)
    gc.free()
    lower = create(160, 150, 320, 320, 0x303030, managed=False)
    mark_blur(lower)
    opacity(lower, .35)
    blurred = full_image()
    setting("blur_strength", 0)
    unblurred = full_image()
    difference = ImageChops.difference(blurred.crop((200, 200, 280, 280)),
                                      unblurred.crop((200, 200, 280, 280)))
    assert max(channel.getextrema()[1] for channel in difference.split()) > 5, "Blur fixture has no visible filtering"
    setting("blur_strength", 100)
    foreground = create(320, 200, 180, 180, 0xff0000, managed=False)
    first = full_image()
    repaint(foreground, 0x0000ff)
    time.sleep(.2)
    second = full_image()
    # Exposed pixels next to the foreground edge must not sample it.
    area = (300, 240, 318, 340)
    difference = ImageChops.difference(first.crop(area), second.crop(area))
    assert max(channel.getextrema()[1] for channel in difference.split()) <= 2, "Foreground bled into lower backdrop"
    print("PASS stacking-correct blur: exposed lower surface unaffected by red/blue foreground", flush=True)
    foreground.destroy()
    windows.remove(foreground)
    # Upper surface is traversed first for damage expansion; original damage
    # only reaches the lower surface. Its expansion must reach upper's samples.
    upper = create(450, 150, 320, 320, 0x303030, managed=False)
    mark_blur(upper)
    opacity(upper, .35)
    full_image()
    count = len(captures())
    gc = base.create_gc(foreground=0xe0e020)
    base.fill_rectangle(gc, 170, 200, 20, 80)
    d.flush()
    time.sleep(.2)
    partial = image_after(count)
    whole = full_image()
    difference = ImageChops.difference(partial.crop((160, 150, 770, 470)),
                                      whole.crop((160, 150, 770, 470)))
    assert max(channel.getextrema()[1] for channel in difference.split()) <= 2, "Transitive blur differs from full repaint"
    gc.free()
    print("PASS transitive blur closure: partial repaint matches full recomposition", flush=True)


def switcher_window():
    for w in root.query_tree().children:
        try:
            if w.get_attributes().map_state == X.IsViewable and prop(w, "_XFWM4_TABWIN_BLUR"):
                return w
        except error.BadWindow:
            pass
    return None


def damage_tests():
    setting("window_effect", 0)
    setting("show_popup_shadow", "false", "bool")
    setting("blur_radius", 32)
    base = create(0, 0, 1200, 740, 0x205020, managed=False)
    popup = create(160, 150, 320, 320, 0x303030, managed=False)
    mark_blur(popup)
    opacity(popup, .35)
    dummy = create(1000, 700, 10, 10, 0x101010, managed=False)
    for n in range(5):
        repaint(dummy, 0x202020 + n)
        time.sleep(.15)
    gc = base.create_gc(foreground=0xe0e020)
    base.fill_rectangle(gc, 170, 200, 20, 80)
    d.flush()
    time.sleep(.25)
    gc.free()
    def last_region(kind):
        text = re.findall(r"damage " + kind + r": ([0-9,;\-]*)", log())[-1]
        return [tuple(map(int, rect.split(","))) for rect in text.split(";") if rect]
    def includes(region, point):
        return any(x <= point[0] < x + w and y <= point[1] < y + h for x, y, w, h in region)
    current, repair, history = map(last_region, ("current", "repair", "history"))
    assert includes(current, (500, 480)), ("Expanded current output missing", current)
    assert history == current, ("Recorded damage differs from expanded current", current, history)
    assert not includes(current, (1005, 705)), ("Old repairs polluted current", current)
    # The separate staged test build can force age=3. Production never accepts
    # that override; pure damage tests also exercise age 1/2/3 and EGL encoding.
    if os.environ.get("XFWM4_TEST_BUFFER_AGE"):
        assert includes(repair, (1005, 705)), ("Age history not accumulated", repair)
    print("PASS damage pipeline: blur-expanded CURRENT recorded; historical repair kept separate", flush=True)


def closing_blur_tests():
    setting("window_effect", 0)
    setting("show_frame_shadow", "false", "bool")
    setting("blur_radius", 32)
    setting("blur_strength", 100)
    closing = create(100, 100, 600, 450, 0xff0000)
    setting("window_effect", 2)
    setting("window_duration", 1000)
    popup = create(200, 200, 350, 220, 0x303030, managed=False)
    mark_blur(popup)
    opacity(popup, .25)
    foreground = create(400, 200, 150, 300, 0x0000ff, managed=False)
    full_image()
    count = len(captures())
    closing.destroy()
    windows.remove(closing)
    d.flush()
    wait(lambda: "close: detached snapshot" in log(), "No retained closing image")
    covered = image_after(count)
    count = len(captures())
    opacity(foreground, 0)
    uncovered = image_after(count)
    # Only the pixels hidden by foreground changed. Exposed blur samples beside
    # its edge must still include the closing red image UNDER that foreground.
    area = (392, 245, 399, 345)
    assert covered.crop(area).getextrema()[0][0] > 80, "Closing image absent beside foreground"
    delta = ImageChops.difference(covered.crop(area), uncovered.crop(area))
    assert max(channel.getextrema()[1] for channel in delta.split()) <= 20, "Closing pixels culled from blur sample margin"
    print("PASS closing + blur + opaque foreground: exposed backdrop retains covered closing pixels", flush=True)


def switcher_tests():
    setting("window_effect", 0)
    setting("cycle_preview", "true", "bool")
    setting("cycle_peek_mode", 0)
    setting("blur_alt_tab", "true", "bool")
    setting("cycle_content_padding", int(os.environ.get("XFWM4_REGRESSION_CONTENT_PADDING", "24")))
    setting("cycle_selection_padding", int(os.environ.get("XFWM4_REGRESSION_SELECTION_PADDING", "5")))
    setting("cycle_spacing", int(os.environ.get("XFWM4_REGRESSION_SPACING", "4")))
    # Fifty clients reach the preview-to-icon boundary; eighty additionally
    # exercise another shrinking iteration (set the optional environment key).
    count = int(os.environ.get("XFWM4_REGRESSION_WINDOW_COUNT", "50"))
    for n in range(count):
        create(50, 50, 80, 60, 0x404040 + n)
    subprocess.check_call(["xdotool", "keydown", "Alt_L", "key", "Tab"])
    popup = wait(switcher_window, "Switcher not mapped")
    time.sleep(.4)
    geometry = popup.get_geometry()
    position = root.translate_coords(popup, 0, 0)
    assert position.x >= 0 and position.y >= 0, (position.x, position.y)
    assert position.x + geometry.width <= 640 and position.y + geometry.height <= 480, (
        "Icon fallback grid exceeds monitor", geometry.width, geometry.height, position.x, position.y)
    rect = list(prop(popup, "_XFWM4_BLUR_CONTENT_RECT").value)
    assert rect == [0, 0, geometry.width, geometry.height], ("Layout padding excluded from painted blur bounds", rect)
    requisitions = re.findall(r"tabwin fit: min=(\d+),(\d+) natural=(\d+),(\d+) limit=(\d+),(\d+)", log())
    assert requisitions, "Final GTK requisition was not checked"
    mw, mh, nw, nh, lw, lh = map(int, requisitions[-1])
    assert mw <= lw and mh <= lh and nw <= lw and nh <= lh, requisitions[-1]
    subprocess.check_call(["xdotool", "key", "--repeat", str(count - 2), "--delay", "10", "Tab"])
    time.sleep(.15)
    geometry = popup.get_geometry()
    assert geometry.width <= 640 and geometry.height <= 480, "Keyboard selection expanded scrolling popup"
    subprocess.check_call(["xdotool", "keyup", "Alt_L"])
    print("PASS %d-window switcher: actual GTK requisition and keyboard traversal bounded; full painted blur bounds" % count, flush=True)


def switcher_normal_tests():
    setting("window_effect", 0)
    setting("blur_alt_tab", "true", "bool")
    setting("cycle_preview", "false", "bool")
    setting("cycle_peek_mode", 0)
    setting("cycle_content_padding", 24)
    setting("cycle_selection_padding", 5)
    setting("cycle_spacing", 4)
    clients = [create(40 + n * 30, 40 + n * 30, 100, 70, 0x404040 + n)
               for n in range(3)]
    for mode in [1, 0]:
        setting("cycle_tabwin_mode", mode)
        subprocess.check_call(["xdotool", "windowactivate", "--sync", str(clients[-1].id)])
        subprocess.check_call(["xdotool", "mousemove", "10", "10"])
        time.sleep(.2)
        subprocess.check_call(["xdotool", "keydown", "Alt_L", "key", "Tab"])
        popup = wait(switcher_window, "Ordinary switcher not mapped")
        time.sleep(.3)
        geometry = popup.get_geometry()
        # Three ordinary clients must not be reduced to the scrollbar gadget's
        # minimum (94x94). The legacy list requests half a monitor per column;
        # the icon grid needs all three minimum-size icons and their insets.
        if mode == 1:
            assert geometry.width >= 320 and geometry.height >= 96, (mode, geometry.width, geometry.height)
        else:
            assert geometry.width >= 158, (mode, geometry.width, geometry.height)
        subprocess.check_call(["xdotool", "keyup", "Alt_L"])
        wait(lambda: int(prop(root, "_NET_ACTIVE_WINDOW").value[0]) == clients[1].id,
             "Alt+Tab did not activate the second-most-recent client")
        print("PASS ordinary three-window switcher mode %d: full content and selected-client activation" % mode,
              flush=True)


log_file = (out / "wm.log").open("w")
wm = subprocess.Popen([wm_path, "--compositor=on", "--vblank=off", "--sm-client-disable"],
                      stdout=log_file, stderr=subprocess.STDOUT)
try:
    wait(lambda: prop(root, "_XFWM4_RENDER_BACKEND"), "Backend unavailable")
    backend = prop(root, "_XFWM4_RENDER_BACKEND").value.decode()
    if suite not in ("switcher", "switcher-normal"):
        assert backend.startswith("opengl"), "Use an accelerated or test-only WM: " + backend
    print("Backend:", backend, flush=True)
    {"snapshot": snapshot_tests, "blur": blur_tests, "damage": damage_tests, "closing-blur": closing_blur_tests,
      "switcher": switcher_tests, "switcher-normal": switcher_normal_tests}[suite]()
finally:
    subprocess.run(["xdotool", "keyup", "Alt_L"], check=False)
    for window in windows:
        try:
            window.destroy()
        except error.BadWindow:
            pass
    d.flush()
    wm.terminate()
    try:
        wm.wait(timeout=5)
    except subprocess.TimeoutExpired:
        wm.kill()
        wm.wait()
    d.close()
    log_file.close()
