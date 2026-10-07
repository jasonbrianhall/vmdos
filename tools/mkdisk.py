#!/usr/bin/env python3
"""Make a bootable FreeDOS hard-disk image: MBR + one FAT32 partition with the
FreeDOS FAT32 (LBA) boot sector, then copy files into its root.
Needs mkfs.fat (dosfstools) and mcopy (mtools).

  mkdisk.py OUT.img SIZE_MB BOOTSECTOR.bin FILE... [DIR/...]
Files are copied to the root; a directory is copied recursively."""
import os, struct, subprocess, sys

START = 2048                      # first partition sector (1 MiB aligned)


def chs(lba, heads=255, spt=63):
    c = lba // (heads * spt)
    if c > 1023:
        return bytes([254, 255, 255])
    h = (lba // spt) % heads
    s = lba % spt + 1
    return bytes([h, s | ((c >> 2) & 0xC0), c & 0xFF])


def main():
    out, size_mb, bootbin, files = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4:]
    total = size_mb * 2048
    plen = total - START
    if plen < 66600 * 1:          # FAT32 wants >= 65525 clusters
        sys.exit("mkdisk: %d MiB is too small for FAT32 (use 34 or more)" % size_mb)
    with open(out, "wb") as f:
        f.truncate(total * 512)

    mbr = bytearray(512)
    mbr[0:2] = b"\xCD\x18"        # no MBR code: INT 18h
    e = bytes([0x80]) + chs(START) + bytes([0x0C]) + chs(total - 1) + struct.pack("<II", START, plen)
    mbr[446:462] = e
    mbr[510:512] = b"\x55\xAA"
    with open(out, "r+b") as f:
        f.write(mbr)

    subprocess.run(["mkfs.fat", "-F", "32", "-S", "512", "-s", "1", "-h", str(START),
                    "--offset", str(START), "-D", "0x80", "-n", "VMDOS", "-i", "766D646F",
                    out, str(plen // 2)], check=True, stdout=subprocess.DEVNULL)

    code = open(bootbin, "rb").read()
    assert len(code) == 512 and code[0] == 0xEB and code[1] == 0x58, "unexpected boot sector"
    with open(out, "r+b") as f:
        f.seek(START * 512)
        old = f.read(512)
        new = code[0:3] + old[3:0x5A] + code[0x5A:510] + b"\x55\xAA"
        for sec in (START, START + 6):            # boot sector and its backup
            f.seek(sec * 512)
            f.write(new)

    img = "%s@@%d" % (out, START * 512)
    env = dict(os.environ, MTOOLS_SKIP_CHECK="1")
    for p in files:
        if os.path.isdir(p):
            subprocess.run(["mcopy", "-s", "-o", "-i", img, p, "::/"], check=True, env=env)
        else:
            subprocess.run(["mcopy", "-o", "-i", img, p, "::/" + os.path.basename(p).upper()],
                           check=True, env=env)


main()
