#!/usr/bin/python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Real X11 removal, intermediate compositor pixels and snapshot lifetime."""
import os,pathlib,re,subprocess,sys,time
from PIL import Image,ImageChops
from Xlib import X,display
assert os.environ.get('XFWM4_ANIMATION_TEST_SESSION')=='1'
wm_path,probe_path,backend,output=sys.argv[1:]
out=pathlib.Path(output); out.mkdir(parents=True,exist_ok=True)
def run(*args): return subprocess.check_output(args,text=True).strip()
def setting(name,value,kind='int'):
    subprocess.check_call(['xfconf-query','-c','xfwm4','-p','/general/'+name,'-n','-t',kind,'-s',str(value)])
log=(out/'wm.log').open('w')
wm=subprocess.Popen([wm_path,'--compositor=on','--vblank=off','--sm-client-disable'],stdout=log,stderr=subprocess.STDOUT)
probe=None
def wait_for(callback,message,seconds=6):
    deadline=time.monotonic()+seconds
    while time.monotonic()<deadline:
        if callback(): return
        assert wm.poll() is None,(out/'wm.log').read_text()
        time.sleep(.02)
    raise AssertionError(message)
try:
    wait_for(lambda:backend in run('xprop','-root','_XFWM4_RENDER_BACKEND'),'Backend unavailable')
    setting('blur_enabled','false','bool'); setting('window_duration',600)
    d=display.Display(); root=d.screen().root; size=(d.screen().width_in_pixels,d.screen().height_in_pixels)
    def capture():
        image=root.get_image(0,0,*size,X.ZPixmap,0xffffffff)
        return Image.frombytes('RGB',size,image.data,'raw','BGRX')
    def red_area(image):
        r,g,b=image.split()
        return ImageChops.subtract(r,ImageChops.lighter(g,b)).point(lambda p:255 if p>32 else 0).histogram()[255]
    def red_energy(image):
        r,g,b=image.split()
        return sum(i*n for i,n in enumerate(ImageChops.subtract(r,ImageChops.lighter(g,b)).histogram()))
    def managed():
        text=run('xprop','-root','_NET_CLIENT_LIST')
        return re.findall(r'0x[0-9a-f]+',text)
    def own_client():
        for xid in managed():
            if 'xfwm4-animation-probe' in run('xprop','-id',xid,'WM_NAME'): return xid
        return None
    def record(label,seconds=.9):
        frames=[]; areas=[]; energies=[]; started=time.monotonic()
        while time.monotonic()-started<seconds:
            frame=capture(); frames.append(frame); areas.append(red_area(frame)); energies.append(red_energy(frame)); time.sleep(.02)
        for n in (0,len(frames)//3,2*len(frames)//3,len(frames)-1): frames[n].save(out/(label+'-%d.png'%n))
        print(label,areas,flush=True); return areas,energies
    for effect in (1,2):
        setting('window_effect',effect)
        probe=subprocess.Popen([probe_path])
        opened,opening_energy=record('effect-%d-open'%effect)
        wid=own_client(); assert wid
        full_image=capture(); full=red_area(full_image); full_energy=red_energy(full_image); assert full>20000
        assert any(0<a<full_energy*.96 for a in opening_energy),('No opening fade frames',opening_energy)
        subprocess.check_call(['wmctrl','-ic',wid])
        closed,closing_energy=record('effect-%d-close'%effect)
        probe.wait(timeout=3); probe=None
        assert wid not in managed(),'Closed client retained in X11'
        assert closed[-1]==0,('Closing residue',closed)
        assert any(0<a<full_energy*.96 for a in closing_energy),('No closing fade frames',closing_energy)
        if backend=='opengl':
            assert 'close finished: frames=' in (out/'wm.log').read_text()
        else:
            assert 'xrender animation' in (out/'wm.log').read_text()
        print('PASS lifecycle effect',effect,backend,flush=True)
    # Disabled mode uses immediate native opening/closing, no retained ghost.
    setting('window_effect',0)
    before=len(re.findall(r'animation .* (?:open|close):',(out/'wm.log').read_text()))
    probe=subprocess.Popen([probe_path]); wait_for(lambda:own_client() is not None,'Disabled-mode client missing')
    time.sleep(.2); subprocess.check_call(['wmctrl','-ic',own_client()]); probe.wait(timeout=3); probe=None
    time.sleep(.15); assert red_area(capture())==0
    assert len(re.findall(r'animation .* (?:open|close):',(out/'wm.log').read_text()))==before
    print('PASS disabled lifecycle effect',flush=True)
    setting('window_effect',1)
    for _ in range(10):
        probe=subprocess.Popen([probe_path]); wait_for(lambda:own_client() is not None,'Rapid client missing')
        time.sleep(.05); subprocess.check_call(['wmctrl','-ic',own_client()]); probe.wait(timeout=3); probe=None
    time.sleep(.9); assert red_area(capture())==0
    print('PASS rapid open/close',backend,flush=True)
    if backend=='opengl':
        probe=subprocess.Popen([probe_path]); wait_for(lambda:own_client() is not None,'GL toggle client missing')
        time.sleep(.1); setting('use_gl_compositing','false','bool')
        wait_for(lambda:'xrender' in run('xprop','-root','_XFWM4_RENDER_BACKEND'),'GL disable failed')
        subprocess.check_call(['wmctrl','-ic',own_client()]); probe.wait(timeout=3); probe=None
        time.sleep(.3); assert wm.poll() is None
        print('PASS backend switch during lifecycle animation',flush=True)
finally:
    if probe and probe.poll() is None: probe.terminate(); probe.wait(timeout=3)
    if wm.poll() is None: wm.terminate(); wm.wait(timeout=3)
    log.close()
