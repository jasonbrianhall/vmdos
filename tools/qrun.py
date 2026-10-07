#!/usr/bin/env python3
# qrun.py NAME SECONDS [keys...] -- qemu args...
# Runs QEMU headless with the serial log in t/NAME.log, sends keys through the
# monitor, and saves a screenshot as t/NAME.png at the end (t/ next to this
# script, or $QRUN_DIR). Keys: a QEMU key name (ret, esc, ctrl-f11), "@2.5"
# (wait seconds), "txt:DIR C:" (type text), "mon:mouse_move 20 10" (any
# monitor command). Example:
#   tools/qrun.py doom 120 @10 txt:CD\DOOM ret txt:DOOM ret -- \
#       qemu-system-i386 -kernel vmdos.elf -initrd dos.img -m 512 -append debug=1
import socket, subprocess, sys, time, os
name, secs = sys.argv[1], float(sys.argv[2])
i = sys.argv.index('--')
keys, qargs = sys.argv[3:i], sys.argv[i+1:]
D = os.environ.get('QRUN_DIR', os.path.dirname(os.path.abspath(__file__)) + '/t') + '/'
os.makedirs(D, exist_ok=True)
sock = D + name + '.sock'
if os.path.exists(sock): os.unlink(sock)
p = subprocess.Popen(qargs + ['-display', 'none', '-serial', 'file:' + D + name + '.log',
                              '-monitor', 'unix:%s,server,nowait' % sock])
time.sleep(0.5)
s = None
for _ in range(50):
    try:
        s = socket.socket(socket.AF_UNIX); s.connect(sock); break
    except Exception: time.sleep(0.2)
def cmd(c, show=False):
    s.sendall((c + '\n').encode()); time.sleep(0.15 if not show else 1.0)
    try:
        out = s.recv(65536)
        if show: print('MON:', out.decode(errors='replace')[-400:])
    except Exception: pass
s.settimeout(0.3)
t0 = time.time()
for k in keys:
    if k.startswith('@'):
        time.sleep(float(k[1:])); continue
    if k.startswith('mon:'):
        cmd(k[4:], True); continue
    if k.startswith('txt:'):
        for ch in k[4:]:
            m = {' ': 'spc', '.': 'dot', '\\': 'backslash', ':': 'shift-semicolon', '/': 'slash', '-': 'minus', '*': 'shift-8'}
            kk = m.get(ch, ch.lower() if not ch.isupper() else 'shift-' + ch.lower())
            cmd('sendkey ' + kk)
        continue
    cmd('sendkey ' + k)
rest = secs - (time.time() - t0)
if rest > 0: time.sleep(rest)
cmd('screendump ' + D + name + '.ppm')
time.sleep(0.5)
cmd('quit')
try: p.wait(5)
except Exception: p.kill()
from PIL import Image
Image.open(D + name + '.ppm').save(D + name + '.png')
print(open(D + name + '.log', errors='replace').read()[-3000:])
