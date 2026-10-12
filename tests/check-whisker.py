#!/usr/bin/python3
"""Isolated real XRender slide lifecycle, including immediate unmap."""
import os
import subprocess
import time
from PIL import Image, ImageChops
from Xlib import X, display

assert os.environ.get('XFWM4_ANIMATION_TEST_SESSION') == '1'
import sys
wm = subprocess.Popen([sys.argv[1], '--compositor=on', '--vblank=off', '--sm-client-disable'])
d = display.Display()
root = d.screen().root
window = None
def sample():
    raw = root.get_image(0, 0, 1280, 800, X.ZPixmap, 0xffffffff)
    image = Image.frombytes('RGB', (1280, 800), raw.data, 'raw', 'BGRX')
    r, g, b = image.split()
    mask = ImageChops.subtract(g, ImageChops.lighter(r, b)).point(lambda p: 255 if p > 50 else 0)
    return mask.getbbox()
def record():
    boxes = []
    end = time.monotonic() + 1.2
    while time.monotonic() < end:
        boxes.append(sample()); time.sleep(.025)
    return boxes
try:
    time.sleep(1)
    for name, value in [('window_effect', 0), ('window_duration', 600)]:
        subprocess.run(['xfconf-query', '-c', 'xfwm4', '-p', '/general/' + name,
                        '-n', '-t', 'int', '-s', str(value)], check=True)
    time.sleep(.1)
    for iteration in range(2):
        if window is None:
            window = root.create_window(80, 300, 300, 450, 0, d.screen().root_depth,
                X.InputOutput, X.CopyFromParent, override_redirect=1, background_pixel=0x20ee20)
            window.set_wm_name('Whisker Menu')
            window.set_wm_class('wrapper-2.0', 'Wrapper-2.0')
        window.map(); d.flush()
        opening = record()
        assert any(box and 310 < box[1] < 790 for box in opening), opening
        assert opening[-1] == (80, 300, 380, 750), opening
        window.unmap(); d.sync()
        assert window.get_attributes().map_state == X.IsUnmapped
        closing = record()
        assert any(box and 310 < box[1] < 790 for box in closing), closing
        assert closing[-1] is None, closing
        print('PASS Whisker slide map/unmap', iteration, opening, closing, flush=True)
finally:
    if window: window.destroy(); d.flush()
    wm.terminate(); wm.wait(timeout=5)
