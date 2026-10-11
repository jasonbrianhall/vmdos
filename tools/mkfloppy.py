#!/usr/bin/env python3
"""Make vmdos's fallback boot floppy: a 1.44M FAT12 FreeDOS disk with the
FreeDOS FAT12 boot sector, booted as A: when no disk has a C: for vmdos.
Needs mkfs.fat (dosfstools) and mcopy (mtools).

  mkfloppy.py OUT.img BOOTSECTOR.bin FILE... [SRC=DEST/PATH]
Files are copied to the root; SRC=DEST puts a file at DEST (directories
are created)."""
import os, subprocess, sys


def main():
    out, boot, files = sys.argv[1], sys.argv[2], sys.argv[3:]
    if os.path.exists(out):
        os.unlink(out)
    subprocess.run(['mkfs.fat', '-C', '-F', '12', '-f', '2', '-r', '224', '-M', '0xF0',
                    '-n', 'VMDOS', out, '1440'], check=True, stdout=subprocess.DEVNULL)
    img = bytearray(open(out, 'rb').read())
    b = open(boot, 'rb').read()
    if len(b) != 512 or b[510:512] != b'\x55\xaa':
        sys.exit('%s: not a boot sector' % boot)
    img[0:3] = b[0:3]                     # the jump; the BPB (3-3Dh) stays mkfs.fat's
    img[0x3E:510] = b[0x3E:510]
    img[0x24] = 0                         # drive number: A:
    open(out, 'wb').write(img)
    env = dict(os.environ, MTOOLS_SKIP_CHECK='1')
    made = set()
    for f in files:
        src, _, dest = f.partition('=')
        dest = dest or os.path.basename(src)
        parts = dest.replace('\\', '/').split('/')
        for i in range(1, len(parts)):
            d = '/'.join(parts[:i])
            if d not in made:
                subprocess.run(['mmd', '-i', out, '::/' + d], env=env, check=True)
                made.add(d)
        subprocess.run(['mcopy', '-o', '-i', out, src, '::/' + '/'.join(parts)], env=env, check=True)


if __name__ == '__main__':
    main()
