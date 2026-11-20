#!/bin/sh
# Real 3dfx Glide on vmdos's emulated Voodoo 3 in plain DOS (no VMHV), under QEMU:
#   tests/voodoo/glide/glidetest.sh [outdir]     (from the top of the tree)
# Builds Glide 2 (DJGPP, 30 test programs) and Glide 3 (Open Watcom: DOS/4GW
# and DOS/32A) for the Voodoo 3 (build.sh), puts them on C: (C=ram), boots
# vmdos with voodoo3, runs each (run.py), checks pixels where it knows what
# the test draws, and that each exits 0 (a hang stops the run).
# outdir (default build/glidetest): serial.log and a screenshot per test.
# TESTS="TEST00 W00 WINIT" runs only those; KARGS adds to the kernel command
# line; QARGS to QEMU. Uses KVM when /dev/kvm is there (much faster).
# Needs: qemu-system-i386, python3, nasm, and what build.sh needs.
set -e
out=${1:-build/glidetest}
sh tests/voodoo/glide/build.sh
make -s C=ram EXTRA=build/glide/extra vmdos.elf dos.img
mkdir -p "$out"
rm -f "$out/qmon" "$out/serial.log" "$out"/*.ppm
accel=; [ -w /dev/kvm ] && accel=-enable-kvm
qemu-system-i386 $accel -kernel vmdos.elf -initrd dos.img -append "c=ram voodoo3 $KARGS" -m 256 -cpu max -smp 2 \
    -serial file:"$out/serial.log" -display none -monitor unix:"$out/qmon",server,nowait -nic none $QARGS 2>/dev/null &
qpid=$!
trap 'kill $qpid 2>/dev/null' EXIT
python3 tests/voodoo/glide/run.py "$out"
