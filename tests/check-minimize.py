#!/usr/bin/python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run only in a private X/DBus session (see check-minimize)."""
import argparse
import os
import pathlib
import re
import subprocess
import time
from PIL import Image, ImageChops, ImageDraw
from Xlib import display, X

if os.environ.get('XFWM4_ANIMATION_TEST_SESSION') != '1':
    raise SystemExit('Refusing to run on an ordinary desktop; use tests/check-minimize.')
args_parser = argparse.ArgumentParser()
args_parser.add_argument('--wm', required=True)
args_parser.add_argument('--probe', required=True)
args_parser.add_argument('--backend', choices=['opengl', 'xrender'], required=True)
args_parser.add_argument('--output', required=True)
args = args_parser.parse_args()
out = pathlib.Path(args.output); out.mkdir(parents=True, exist_ok=True)


def run(*cmd):
    return subprocess.run(cmd, text=True, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout


def wait_for(predicate, message, timeout=6):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        if wm.poll() is not None:
            raise AssertionError('WM exited: ' + (out / 'wm.log').read_text())
        time.sleep(.03)
    raise AssertionError(message)


def backend():
    return run('xprop', '-root', '_XFWM4_RENDER_BACKEND')


def setting(name, value, kind='int'):
    run('xfconf-query', '-c', 'xfwm4', '-p', '/general/' + name, '-n', '-t', kind, '-s', str(value))


def log_count():
    return len(re.findall(r'animation .* (?:minimize|restore):', (out / 'wm.log').read_text()))


wm_log = (out / 'wm.log').open('w')
wm = subprocess.Popen([args.wm, '--compositor=on', '--vblank=off', '--sm-client-disable'],
                      stdout=wm_log, stderr=subprocess.STDOUT)
probe = None
try:
    wait_for(lambda: args.backend in backend(), 'Unexpected compositor backend')
    print('PASS backend:', backend().strip(), flush=True)
    setting('blur_enabled', 'false', 'bool')
    setting('minimize_duration', 600)
    probe = subprocess.Popen([args.probe])
    def find_id():
        # Some wmctrl versions crash when _NET_CLIENT_LIST is not populated
        # yet. Wait for our own named client and its managed WM_STATE instead.
        result = subprocess.run(['xdotool', 'search', '--name', '^xfwm4-animation-probe$'],
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        for ident in result.stdout.split():
            state = subprocess.run(['xprop', '-id', ident, 'WM_STATE'],
                                   text=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            if 'Normal' in state.stdout:
                return hex(int(ident))
        return None
    wait_for(lambda: find_id() is not None, 'Probe not managed')
    wid = find_id()
    run('wmctrl', '-ir', wid, '-e', '0,140,100,520,330')
    run('wmctrl', '-ia', wid)
    d = display.Display(); root = d.screen().root
    geom = root.get_geometry(); size = (geom.width, geom.height)

    def capture():
        im = root.get_image(0, 0, *size, X.ZPixmap, 0xffffffff)
        return Image.frombytes('RGB', size, im.data, 'raw', 'BGRX')

    def red_area(im):
        r, g, b = im.split()
        mask = ImageChops.subtract(r, ImageChops.lighter(g, b)).point(lambda a: 255 if a > 32 else 0)
        return mask.histogram()[255]

    time.sleep(.3)
    base = capture(); base_area = red_area(base)
    assert base_area > 20000, ('Probe missing from screen', base_area)
    base.save(out / 'initial.png')

    def record(action, label, animated=True):
        before = time.monotonic()
        run(*action)
        frames = []
        while time.monotonic() - before < .85:
            frames.append(capture())
            time.sleep(.025)
        areas = [red_area(f) for f in frames]
        if animated:
            middle = sum(0 < a < base_area * .96 for a in areas)
            assert middle >= 1, (label, 'No intermediate compositor images', areas)
        samples = [frames[min(len(frames) - 1, int(i * (len(frames) - 1) / 5))] for i in range(6)]
        sheet = Image.new('RGB', (size[0], size[1] // 2 * 3))
        for i, im in enumerate(samples):
            im = im.resize((size[0] // 2, size[1] // 2))
            ImageDraw.Draw(im).text((8, 8), f'{label} frame {i}', fill='white')
            sheet.paste(im, ((i % 2) * (size[0] // 2), (i // 2) * (size[1] // 2)))
        sheet.save(out / (label + '.png'))
        print(label, 'red pixels:', areas, flush=True)
        return areas

    for effect in (1, 2):
        setting('minimize_effect', effect)
        count = log_count()
        animated = args.backend == 'opengl' or effect == 1
        down = record(('xdotool', 'windowminimize', wid), f'effect-{effect}-minimize', animated)
        assert down[-1] == 0, ('Minimize left pixels on screen', down[-1])
        assert '_NET_WM_STATE_HIDDEN' in run('xprop', '-id', wid, '_NET_WM_STATE')
        up = record(('wmctrl', '-ia', wid), f'effect-{effect}-restore', animated)
        assert up[-1] > base_area * .95, ('Restore missing content', up[-1], base_area)
        assert '_NET_WM_STATE_HIDDEN' not in run('xprop', '-id', wid, '_NET_WM_STATE')
        if args.backend == 'xrender':
            assert log_count() == count + (2 if effect == 1 else 0), 'Incorrect XRender animation policy'
        else:
            # The fully painted window must be cached BEFORE unminimizing.  A
            # brand-new snapshot of the just-remapped frame is incomplete on
            # many real applications and makes their content pop in late.
            logs = (out / 'wm.log').read_text()
            assert 'cached minimized window texture' in logs, 'Minimized texture not retained'
            assert logs.count('restoring cached window texture') >= (effect), (
                'Restore recaptured a not-yet-painted window instead of using cached contents')
        print(f'PASS effect={effect}: minimize + restore', flush=True)

    # Disabled effect must neither animate nor change the ordinary X11 state.
    setting('minimize_effect', 0); count = log_count()
    run('xdotool', 'windowminimize', wid); time.sleep(.12)
    run('wmctrl', '-ia', wid); time.sleep(.15)
    assert log_count() == count
    print('PASS None disables animations', flush=True)

    if args.backend == 'opengl':
        setting('minimize_effect', 2)
        for i in range(12):
            run('xdotool', 'windowminimize', wid); time.sleep(.05)
            run('wmctrl', '-ia', wid); time.sleep(.05)
        time.sleep(.9)
        assert red_area(capture()) > base_area * .95
        print('PASS rapid reversals', flush=True)
        run('xdotool', 'windowminimize', wid); time.sleep(.12)
        setting('use_gl_compositing', 'false', 'bool')
        wait_for(lambda: 'xrender' in backend(), 'GL disable failed')
        count = log_count()
        run('wmctrl', '-ia', wid); time.sleep(.15)
        assert log_count() == count
        assert red_area(capture()) > base_area * .95
        print('PASS GL disabled mid-animation: no XRender animation', flush=True)
        setting('use_gl_compositing', 'true', 'bool')
        wait_for(lambda: 'opengl' in backend(), 'GL re-enable failed')
        run('xdotool', 'windowminimize', wid); time.sleep(.1)
        probe.terminate(); probe.wait(timeout=5); probe = None
        time.sleep(.9)
        assert wm.poll() is None
        print('PASS destroying an animating client', flush=True)
    assert wm.poll() is None
    print('PASS lifecycle complete', flush=True)
finally:
    if probe and probe.poll() is None:
        probe.terminate(); probe.wait(timeout=5)
    if wm.poll() is None:
        wm.terminate(); wm.wait(timeout=5)
    wm_log.close()
