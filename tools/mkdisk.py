#!/usr/bin/env python3
"""Make a bootable FreeDOS hard-disk image: MBR + one FAT32 partition with the
FreeDOS FAT32 (LBA) boot sector, then copy files into its root.
Needs mkfs.fat (dosfstools) and mcopy (mtools).

  mkdisk.py [--update] [--type=0C] [--boot16=FAT16BOOT.bin] OUT.img SIZE_MB BOOTSECTOR.bin|- FILE... [DIR/...] [SRC=DEST/PATH]
Files are copied to the root; a directory is copied recursively; SRC=DEST
puts a file at DEST (directories are created); --contents=DIR copies
everything inside DIR to the root. BOOTSECTOR "-" leaves the
mkfs.fat boot code; --type sets the partition type (EF: EFI system partition).

With --boot16, disks up to 2 GiB are FAT16 with 8-32 KiB clusters and that
boot sector: FreeDOS walks a file's cluster chain on every seek, which with
FAT32's 512-byte clusters (all a small FAT32 disk can have) makes seeking in
a big file (DOOM's WAD) very slow. Otherwise FAT32 with BOOTSECTOR.

--update: OUT.img exists (made by this script) and is kept: files are copied
in, but AUTOEXEC.BAT, FDCONFIG.SYS and anything from --contents / folders
that is already there is left alone (it may have been changed in DOS)."""
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
    args = sys.argv[1:]
    ptype, boot16, update = None, None, False
    while args and args[0].startswith("--"):
        a = args.pop(0)
        if a.startswith("--type="): ptype = int(a[7:], 16)
        elif a.startswith("--boot16="): boot16 = a[9:]
        elif a == "--update": update = True
        else: sys.exit("mkdisk: unknown option " + a)
    out, size_mb, bootbin, files = args[0], int(args[1]), args[2], args[3:]
    if update and os.path.exists(out):
        return copy_files(out, files, True)
    total = size_mb * 2048
    plen = total - START
    fat16 = boot16 is not None and plen < 4 * 1024 * 1024 - 8192
    if fat16:
        spc = 16                  # 8 KiB clusters; more on bigger disks (FAT16 has < 65525 clusters)
        while plen // spc > 65000: spc *= 2
        while spc > 1 and plen // spc < 4200: spc //= 2
        if plen // spc < 4200:
            sys.exit("mkdisk: %d MiB is too small" % size_mb)
        if ptype is None: ptype = 0x0E           # FAT16, LBA
        bootbin, spc_fat, bpb_end = boot16, ["-F", "16", "-s", str(spc)], 0x3E
    else:
        if plen < 66600 * 1:          # FAT32 wants >= 65525 clusters
            sys.exit("mkdisk: %d MiB is too small for FAT32 (use 34 or more)" % size_mb)
        spc = 1
        while spc < 64 and plen // (spc * 2) >= 66600 and spc < 8: spc *= 2   # up to 4 KiB clusters
        if ptype is None: ptype = 0x0C
        spc_fat, bpb_end = ["-F", "32", "-s", str(spc)], 0x5A
    with open(out, "wb") as f:
        f.truncate(total * 512)

    mbr = bytearray(512)
    mbr[0:2] = b"\xCD\x18"        # no MBR code: INT 18h
    e = bytes([0x80]) + chs(START) + bytes([ptype]) + chs(total - 1) + struct.pack("<II", START, plen)
    mbr[446:462] = e
    mbr[510:512] = b"\x55\xAA"
    with open(out, "r+b") as f:
        f.write(mbr)

    subprocess.run(["mkfs.fat"] + spc_fat + ["-S", "512", "-h", str(START),
                    "--offset", str(START), "-D", "0x80", "-n", "VMDOS", "-i", "766D646F",
                    out, str(plen // 2)], check=True, stdout=subprocess.DEVNULL)

    if bootbin != "-":
        code = open(bootbin, "rb").read()
        assert len(code) == 512 and code[0] == 0xEB and code[1] == bpb_end - 2, "unexpected boot sector"
        with open(out, "r+b") as f:
            f.seek(START * 512)
            old = f.read(512)
            new = code[0:3] + old[3:bpb_end] + code[bpb_end:510] + b"\x55\xAA"
            for sec in ((START,) if fat16 else (START, START + 6)):   # FAT32 keeps a backup
                f.seek(sec * 512)
                f.write(new)

    copy_files(out, files, False)


KEEP = {"AUTOEXEC.BAT", "FDCONFIG.SYS"}


def copy_files(out, files, update):
    img = "%s@@%d" % (out, START * 512)
    keep = ["-D", "s"] if update else ["-o"]
    env = dict(os.environ, MTOOLS_SKIP_CHECK="1")
    made = set()
    expanded = []
    user = set()
    for p in files:
        if p.startswith("--contents="):
            d = p[len("--contents="):]
            if not os.path.isdir(d):
                sys.exit("mkdisk: %s is not a folder" % d)
            more = [os.path.join(d, n) for n in sorted(os.listdir(d))]
            expanded += more
            user.update(more)
        else:
            expanded.append(p)
    for p in expanded:
        if "=" in p and not os.path.exists(p):
            src, dest = p.split("=", 1)
            parts = dest.strip("/").split("/")
            for i in range(1, len(parts)):
                d = "/".join(parts[:i])
                if d not in made:
                    subprocess.run(["mmd", "-i", img, "::/" + d], env=env,
                                   stderr=subprocess.DEVNULL)
                    made.add(d)
            subprocess.run(["mcopy", "-o", "-i", img, src, "::/" + "/".join(parts)], check=True, env=env)
        elif os.path.isdir(p):
            subprocess.run(["mcopy", "-s"] + keep + ["-i", img, p, "::/"], check=not update, env=env)
        else:
            name = os.path.basename(p).upper()
            mode = keep if (p in user or name in KEEP) else ["-o"]
            subprocess.run(["mcopy"] + mode + ["-i", img, p, "::/" + name], check=mode == ["-o"], env=env)


main()
