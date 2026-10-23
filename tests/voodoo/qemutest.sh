#!/bin/sh
# V3TEST.COM in vmdos (voodoo3) under QEMU, screenshots of what it draws:
#   tests/voodoo/qemutest.sh [outdir]      (from the top of the tree; needs
#   nasm, qemu-system-i386, python3; builds dos.img with C=ram; QARGS='-smp 2' adds to QEMU)
# out/triangle.ppm: the fill and the Gouraud triangle; out/swap.ppm: after
# the command FIFO filled the back buffer and the swap showed it (green).
set -e
out=${1:-build/v3test}
mkdir -p "$out" build/v3extra
nasm -f bin -o build/v3extra/V3TEST.COM tests/voodoo/v3test.asm
make -s C=ram EXTRA=build/v3extra vmdos.elf dos.img
mon=$out/qmon
rm -f "$mon"
qemu-system-i386 -kernel vmdos.elf -initrd dos.img -append "c=ram voodoo3" -m 256 \
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
wait('hd: VMHD drives', 120)
for attempt in range(4):                       # typed before the prompt is up: lost, type again
    time.sleep(5)
    for ch in 'v3test\n':
        cmd('sendkey ' + {'\n': 'ret'}.get(ch, ch)); time.sleep(0.05)
    end = time.time() + 30
    while "the Voodoo 3's picture on" not in log() and time.time() < end: time.sleep(0.5)
    if "the Voodoo 3's picture on" in log(): break
else: sys.exit('V3TEST never drew')
time.sleep(1)
cmd('screendump ' + out + '/triangle.ppm')
wait('overlay on', 60)
time.sleep(2)
cmd('screendump ' + out + '/swap.ppm')
wait('client exits', 120)
print(''.join(l + '\n' for l in log().splitlines() if 'voodoo3' in l or 'Voodoo' in l or 'DPMI' in l))
EOF
