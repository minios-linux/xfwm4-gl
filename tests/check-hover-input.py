#!/usr/bin/python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise pointer routing with an interactive popup under a new wireframe."""
import subprocess
import sys
import time
from Xlib import X, Xatom, display, error
from Xlib.ext import xtest

wm_path, backend = sys.argv[1:]
assert backend in ('xrender', 'off'), backend
subprocess.check_call(['xfconf-query', '-c', 'xfwm4', '-p', '/general/use_gl_compositing',
                       '-n', '-t', 'bool', '-s', 'false'])
dpy = display.Display()
screen = dpy.screen()
root = screen.root
hover = dpy.intern_atom('_XFWM4_HOVER_FRAME')

def wait_for(callback, message):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        dpy.sync()
        result = callback()
        if result:
            return result
        time.sleep(.02)
    raise AssertionError(message)

wm = subprocess.Popen([wm_path, '--replace', '--sm-client-disable',
                       '--compositor=' + ('on' if backend == 'xrender' else 'off'),
                       '--vblank=off'])
client = popup = None
try:
    check = dpy.intern_atom('_NET_SUPPORTING_WM_CHECK')
    wait_for(lambda: root.get_full_property(check, Xatom.WINDOW), 'WM did not start')
    client = root.create_window(160, 100, 820, 500, 0, screen.root_depth,
                                X.InputOutput, X.CopyFromParent, background_pixel=0xffffff,
                                event_mask=X.StructureNotifyMask)
    client.set_wm_name('Hover input regression client')
    client.map()
    frame = wait_for(lambda: client.query_tree().parent if client.query_tree().parent.id != root.id else None,
                     'Client was not framed')
    bounds = frame.get_geometry()
    position = root.translate_coords(frame, 0, 0)
    px, py = position.x + 40, position.y + bounds.height - 80
    popup = root.create_window(px, py, 320, 160, 0, screen.root_depth,
                               X.InputOutput, X.CopyFromParent, override_redirect=True,
                               background_pixel=0xcccccc, event_mask=X.ButtonPressMask)
    popup.map()
    dpy.sync()

    def mapped_wireframes():
        result = []
        for window in root.query_tree().children:
            try:
                if (window.get_wm_class() == ('xfwm4-wireframe', 'Xfwm4') and
                        window.get_attributes().map_state == X.IsViewable):
                    result.append(window)
            except error.BadWindow:
                # A changed fill/target destroys the old transient frame.
                continue
        return result
    for fill in (0, 1):
        previous = {window.id for window in mapped_wireframes()}
        root.change_property(hover, Xatom.CARDINAL, 32, [client.id, fill], X.PropModeReplace)
        dpy.flush()
        wait_for(lambda: [window for window in mapped_wireframes() if window.id not in previous],
                 'Hover frame did not map')
        # Cover both the ARGB interior and the shaped Xlib bottom edge.
        for label, y in [('interior', py + 20), ('edge', position.y + bounds.height - 1)]:
            xtest.fake_input(dpy, X.MotionNotify, x=px + 50, y=y)
            dpy.sync()
            assert root.query_pointer().child.id == popup.id, (backend, fill, label, 'wireframe intercepted pointer')
            xtest.fake_input(dpy, X.ButtonPress, detail=1)
            xtest.fake_input(dpy, X.ButtonRelease, detail=1)
            dpy.flush()
            def clicked():
                while dpy.pending_events():
                    event = dpy.next_event()
                    if event.type == X.ButtonPress and event.window.id == popup.id:
                        return True
                return False
            wait_for(clicked, 'Popup did not receive click')
            print('PASS: %s fill=%d %s popup receives click' % (backend, fill, label), flush=True)
finally:
    root.delete_property(hover)
    if popup is not None:
        popup.destroy()
    if client is not None:
        client.destroy()
    dpy.sync()
    wm.terminate()
    try:
        wm.wait(timeout=3)
    except subprocess.TimeoutExpired:
        wm.kill()
        wm.wait()
    dpy.close()
