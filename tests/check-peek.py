#!/usr/bin/python3
"""Isolated Xvfb regression for shared Peek. Never runs on the user display."""
import os, time, subprocess, pathlib, math, argparse
from Xlib import display, X, Xatom
from PIL import Image

assert os.environ.get("XFWM4_ANIMATION_TEST_SESSION") == "1"
assert os.environ.get("DISPLAY", "").startswith(":")

args_parser=argparse.ArgumentParser()
args_parser.add_argument('--wm', required=True)
args_parser.add_argument('--backend', choices=['opengl','xrender'], required=True)
args_parser.add_argument('--output', required=True)
args=args_parser.parse_args()
OUT=pathlib.Path(args.output)
OUT.mkdir(parents=True,exist_ok=True)

def run(*args, check=True):
    return subprocess.run(args, text=True, capture_output=True, check=check).stdout
def wait(f,msg,timeout=10):
    stop=time.monotonic()+timeout
    while time.monotonic()<stop:
        if f(): return
        if wm.poll() is not None: raise AssertionError("WM failed "+(OUT/'wm.log').read_text()[-4000:])
        time.sleep(.06)
    raise AssertionError(msg)

log=(OUT/'wm.log').open('w')
wm=subprocess.Popen([args.wm,'--compositor=on','--vblank=off','--sm-client-disable'],stdout=log,stderr=subprocess.STDOUT)
d=None
try:
    wait(lambda: args.backend in run('xprop','-root','_XFWM4_RENDER_BACKEND',check=False),
         'Unexpected compositor backend')
    print('PASS backend',run('xprop','-root','_XFWM4_RENDER_BACKEND').strip(),flush=True)
    d=display.Display(); root=d.screen().root; scr=d.screen()
    atom=lambda s:d.intern_atom(s)
    palette=[0xdd3030,0x206edc]
    specs=[(120,120,310,250),(490,170,315,270)]
    windows=[]
    for i,(x,y,w,h) in enumerate(specs):
        win=root.create_window(x,y,w,h,0,scr.root_depth,X.InputOutput,X.CopyFromParent,
                               background_pixel=palette[i],
                               event_mask=X.ExposureMask|X.StructureNotifyMask)
        win.set_wm_name("MiniOS Peek Probe "+str(i))
        win.set_wm_class('peek-test-'+str(i),'MiniOSPeekTest')
        windows.append(win)
        win.map()
    d.flush()
    for win in windows:
        wait(lambda w=win:'Normal' in run('xprop','-id',hex(w.id),'WM_STATE',check=False), 'test window not managed')
    for i,w in enumerate(windows):
        x,y,width,height = ((120,110,310,250),(550,140,315,270))[i]
        run('wmctrl','-ir',hex(w.id),'-e',f'0,{x},{y},{width},{height}')
    time.sleep(.35)
    for w in windows:
        info=w.get_attributes()
        print('DEBUG client',hex(w.id),'map_state',info.map_state,
              'WM_STATE',run('xprop','-id',hex(w.id),'WM_STATE',check=False).strip(),flush=True)
        w.clear_area(0,0,0,0,True)
    d.flush()
    time.sleep(.45)
    g=root.get_geometry()
    size=(g.width,g.height)
    def screenshot(name):
        d.sync()
        im=root.get_image(0,0,*size,X.ZPixmap,0xffffffff)
        shot=Image.frombytes('RGB',size,im.data,'raw','BGRX')
        shot.save(OUT/(name+'.png'))
        return shot
    def sample(shot):
        return tuple(shot.getpixel(p) for p in ((220,230),(640,270),(935,600)))
    def dist(a,b):
        return math.sqrt(sum((x-y)**2 for x,y in zip(a,b)))
    def prop(target,mode,alpha=15,duration=180):
        root.change_property(atom('_XFWM4_GL_PEEK'),Xatom.CARDINAL,32,
                             [target,mode,alpha,duration],X.PropModeReplace)
        d.flush()
    def hold(target,mode,duration=.65,alpha=15):
        until=time.monotonic()+duration
        while time.monotonic()<until:
            prop(target,mode,alpha)
            time.sleep(.16)
    base=sample(screenshot('baseline'))
    print('baseline R B ROOT =',base,flush=True)
    assert base[0][0]>base[0][2]+45 and base[1][2]>base[1][0]+35, 'wrong initial colors'
    def exercise_peek():
        hold(windows[0].id,1)
        dimmed=sample(screenshot('dimmed'))
        print('dimmed R B ROOT =',dimmed,flush=True)
        assert dist(dimmed[1],base[2]) < dist(base[1],base[2])*0.8, 'Other window did not dim'
        assert dimmed[0][0]>dimmed[0][2]+25, 'Selected window missing'
        # Switching hover between two cards should crossfade, not flash the
        # entire desktop back to normal before raising the new target.
        prop(windows[1].id,1)
        time.sleep(.04)
        switch_early=sample(screenshot('switch-early'))
        time.sleep(.08)
        switch_mid=sample(screenshot('switch-middle'))
        time.sleep(.15)
        switch_final=sample(screenshot('switch-final'))
        print(args.backend,'card switch (early/mid/final):',switch_early,switch_mid,switch_final,flush=True)
        assert switch_early[0][0] > 100, 'Previous target disappeared abruptly'
        assert switch_mid[0][0] < switch_early[0][0], 'Previous image did not fade'
        assert switch_mid[1][2] > switch_early[1][2], 'New target did not fade in'
        assert switch_final[1][2] > 180, 'New target did not become visible'
        assert switch_final[0][0] < 75, 'Previous target not dimmed after switching'
        hold(windows[0].id,1,duration=.45)
        hold(windows[0].id,2)
        hidden=sample(screenshot('hidden'))
        print('hidden R B ROOT =',hidden,flush=True)
        assert dist(hidden[1],base[2])<35, 'Other window did not disappear'
        assert hidden[0][0]>hidden[0][2]+25, 'Target lost in hide mode'
        prop(0,0)
        time.sleep(.4)
        restored=sample(screenshot('restored'))
        print('restored R B ROOT =',restored,flush=True)
        assert dist(restored[0],base[0])<25 and dist(restored[1],base[1])<25, 'Normal rendering not restored'

        # Minimize a window, then Peek it without deiconifying X11 client.
        run('xfconf-query','-c','xfwm4','-p','/general/minimize_effect','-n','-t','int','-s','1')
        run('xdotool','windowminimize',str(windows[1].id))
        wait(lambda:'Iconic' in run('xprop','-id',hex(windows[1].id),'WM_STATE',check=False), 'Window failed to minimize')
        time.sleep(.52) # let the GL snapshot finish and become cached
        before=sample(screenshot('minimized'))
        assert dist(before[1],base[1])>65, 'Minimized window still visible before Peek'
        hold(windows[1].id,2)
        minimized_peek=sample(screenshot('minimized-peek'))
        print('minimized target pixels:',minimized_peek,flush=True)
        assert minimized_peek[1][2]>minimized_peek[1][0]+35, 'Minimized snapshot did not appear'
        assert 'Iconic' in run('xprop','-id',hex(windows[1].id),'WM_STATE'), 'Peek deiconified target!'
        prop(0,0)
        time.sleep(.4)
        recovered=sample(screenshot('recovered-minimized'))
        assert 'Iconic' in run('xprop','-id',hex(windows[1].id),'WM_STATE')
        assert dist(recovered[1],base[2])<35, 'Minimized target ghost remains'

        # Stop heartbeats; the renderer must recover without user cooperation.
        hold(windows[0].id,1,duration=.35)
        time.sleep(1.8)
        stale=sample(screenshot('stale-heartbeat-recovered'))
        assert dist(stale[0],base[0])<25 and dist(stale[1],base[2])<35, 'Stale Peek did not auto-clear'
        # Peek can also display a minimized window if minimize/restore animations
        # are disabled: the GL renderer retains an image without animating X11.
        setting_disabled=run('xfconf-query','-c','xfwm4','-p','/general/minimize_effect',
                             '-n','-t','int','-s','0')
        run('xdotool','windowminimize',str(windows[0].id))
        wait(lambda:'Iconic' in run('xprop','-id',hex(windows[0].id),'WM_STATE',check=False),
             'No animation minimize failed')
        time.sleep(.22)
        without_effect=sample(screenshot('none-minimized'))
        hold(windows[0].id,2,.6)
        peek_no_effect=sample(screenshot('peek-with-animations-disabled'))
        print('Peek with minimize effects OFF:',peek_no_effect,flush=True)
        assert peek_no_effect[0][0]>peek_no_effect[0][2]+45, (
            'No cached GL texture when minimize animations disabled',peek_no_effect)
        assert 'Iconic' in run('xprop','-id',hex(windows[0].id),'WM_STATE'), (
            'Peek changed window mapping with animations disabled')
        prop(0,0); time.sleep(.3)
        assert dist(sample(screenshot('none-restored'))[0],base[2])<35
        print('PASS:',args.backend,'dim, hide, crossfade switch, soft restore, minimized snapshot with effects ON/OFF, unchanged X11 iconification, stale-heartbeat recovery',flush=True)
    exercise_peek()
finally:
    if d:
        try:
            root.change_property(atom('_XFWM4_GL_PEEK'),Xatom.CARDINAL,32,[0,0,15,180],X.PropModeReplace)
            d.flush()
        except Exception: pass
        try:d.close()
        except Exception:pass
    wm.terminate()
    try:wm.wait(timeout=3)
    except subprocess.TimeoutExpired: wm.kill();wm.wait()
    log.close()
