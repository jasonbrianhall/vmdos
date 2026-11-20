#!/bin/sh
# x87 and MMX stores and loads on an unchained 256-color screen (mode X), as
# games clear and copy theirs, under QEMU:
#   tests/modex/modextest.sh [outdir]      (from the top of the tree)
# Builds X87MODEX.EXE with DJGPP (fetched by tests/voodoo/glide/build.sh
# into build/glide/djgpp if it isn't there), puts it on C: (C=ram), boots
# vmdos (its own DPMI host, so not VMHV), runs it and wants "DPMI: client
# exits (0)". KARGS adds to the kernel command line, QARGS to QEMU.
# outdir (default build/modextest): serial.log and a screenshot.
set -e
out=${1:-build/modextest}
dj=build/glide/djgpp/djgpp
if [ ! -x $dj/bin/i586-pc-msdosdjgpp-gcc ]; then
    mkdir -p build/glide/djgpp
    curl -fsSL https://github.com/andrewwutw/build-djgpp/releases/download/v3.4/djgpp-linux64-gcc1220.tar.bz2 |
        tar xjf - -C build/glide/djgpp
fi
mkdir -p build/modex "$out"
$dj/bin/i586-pc-msdosdjgpp-gcc -O2 -o build/modex/X87MODEX.EXE tests/modex/x87modex.c
make -s C=ram EXTRA=build/modex vmdos.elf dos.img
rm -f "$out/qmon" "$out/serial.log"
accel=; [ -w /dev/kvm ] && accel=-enable-kvm
qemu-system-i386 $accel -kernel vmdos.elf -initrd dos.img -append "c=ram $KARGS" -m 256 -cpu max \
    -serial file:"$out/serial.log" -display none -monitor unix:"$out/qmon",server,nowait -nic none $QARGS 2>/dev/null &
qpid=$!
trap 'kill $qpid 2>/dev/null' EXIT
python3 - "$out" <<'EOF'
import re, socket, sys, time
out = sys.argv[1]
def cmd(c):
    s = socket.socket(socket.AF_UNIX); s.connect(out + '/qmon'); s.recv(4096)
    s.send((c + '\n').encode()); time.sleep(0.2); s.close()
def log():
    try: return open(out + '/serial.log', errors='replace').read()
    except OSError: return ''
end = time.time() + 600
while 'hd: VMHD drives' not in log():
    if time.time() > end: sys.exit('vmdos never booted')
    time.sleep(0.5)
time.sleep(8)
for ch in 'x87modex\n':
    cmd('sendkey ' + {'\n': 'ret'}.get(ch, ch)); time.sleep(0.05)
end = time.time() + 180
while not re.search(r'DPMI: client exits \(\d+\)', log()):
    if time.time() > end:
        cmd('screendump %s/screen.ppm' % out)
        sys.exit('X87MODEX never exited (hung?); see %s' % out)
    time.sleep(0.5)
time.sleep(2)
cmd('screendump %s/screen.ppm' % out)
code = re.findall(r'DPMI: client exits \((\d+)\)', log())[-1]
for l in log().splitlines():
    if "can't emulate" in l or "can't do" in l: print(l)
print('X87MODEX exit code', code, '- ok' if code == '0' else '- FAILED (screen.ppm has its message)')
sys.exit(0 if code == '0' else 1)
EOF
