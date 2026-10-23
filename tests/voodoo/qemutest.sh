#!/bin/sh
# V3TEST.COM in vmdos (voodoo3) under QEMU, screenshots of what it draws:
#   tests/voodoo/qemutest.sh [outdir]      (from the top of the tree; needs
#   nasm, qemu-system-i386, python3; builds dos.img with C=ram; QARGS='-smp 2' adds to QEMU)
# out/triangle.ppm: the fill, the Gouraud triangle and a white band at rows 20-29; out/swap.ppm: after
# the command FIFO filled the back buffer and the swap showed it (green).
set -e
out=${1:-build/v3test}
mkdir -p "$out" build/v3extra
nasm -f bin -o build/v3extra/V3TEST.COM tests/voodoo/v3test.asm
make -s C=ram EXTRA=build/v3extra vmdos.elf dos.img
mon=$out/qmon
rm -f "$mon" "$out/serial.log"
qemu-system-i386 -kernel vmdos.elf -initrd dos.img -append "c=ram voodoo3 v3log $KARGS" -m 256 \
    -serial file:"$out/serial.log" -display none -monitor unix:"$mon",server,nowait -nic none $QARGS 2>/dev/null &
qpid=$!
trap 'kill $qpid 2>/dev/null' EXIT
python3 - "$mon" "$out" <<'EOF'
import socket, sys, time
mon, out = sys.argv[1], sys.argv[2]
def cmd(c):
    s = socket.socket(socket.AF_UNIX); s.connect(mon); s.recv(4096)
    s.send((c + '\n').encode()); time.sleep(0.3); s.close()
def log():
    try: return open(out + '/serial.log', errors='replace').read()
    except OSError: return ''
def wait(text, t):
    end = time.time() + t
    while text not in log():
        if time.time() > end: sys.exit('timed out waiting for: ' + text)
        time.sleep(0.5)
wait('hd: VMHD drives', 400)
for attempt in range(4):                       # typed before the prompt is up: lost, type again
    time.sleep(5)
    for ch in 'v3test\n':
        cmd('sendkey ' + {'\n': 'ret'}.get(ch, ch)); time.sleep(0.05)
    end = time.time() + 150
    while "picture on (640x480)" not in log() and time.time() < end: time.sleep(0.5)
    if "picture on (640x480)" in log(): break
else: sys.exit('V3TEST never drew')
def px(f, x, y):
    d = open(f, 'rb').read(); hdr = d.split(b'\n', 3); w = int(hdr[1].split()[0]); o = len(b'\n'.join(hdr[:3])) + 1
    return tuple(d[o + (y * w + x) * 3: o + (y * w + x) * 3 + 3])
def near(got, want): return all(abs(a - b) < 12 for a, b in zip(got, want))
def shot_when(name, x, y, want, t):            # screenshots until (x, y) is want
    end = time.time() + t
    while time.time() < end:
        cmd('screendump ' + out + '/' + name + '.ppm'); time.sleep(1)
        try:
            if near(px(out + '/' + name + '.ppm', x, y), want): return
        except (OSError, ValueError, IndexError): pass
shot_when('triangle', 100, 25, (255, 255, 255), 120)   # the white band: drawn after the triangle
shot_when('swap', 100, 100, (0, 255, 0), 120)
time.sleep(12)
print(''.join(l + '\n' for l in log().splitlines() if ('voodoo3' in l or 'Voodoo' in l) and ': io ' not in l and ': cfg ' not in l))
checks = [('triangle', 10, 10, (0, 0, 132)), ('triangle', 100, 25, (255, 255, 255)), ('triangle', 320, 60, None), ('swap', 100, 100, (0, 255, 0))]
bad = 0
for n, x, y, want in checks:
    got = px(out + '/' + n + '.ppm', x, y)
    ok = want is None and got[0] > 200 and got[1] < 40 or want is not None and near(got, want)
    bad += not ok
    print(n, (x, y), got, 'ok' if ok else 'WRONG')
sys.exit(1 if bad else 0)
EOF
