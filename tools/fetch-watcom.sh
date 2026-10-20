#!/bin/sh
# Fetch Open Watcom v2 (Linux host, 16-bit DOS targets) into .watcom/, for
# make net (net/: VMTCP, DHCP, PING, NSLOOKUP, VMIPXBOX). Only needed to
# rebuild net/bin; vmdos itself builds without it. An installed Open Watcom
# works too: make net WATCOM=/usr/bin/watcom
set -e
dest=${1:-.watcom}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
curl -fL -o "$tmp/ow.tar.xz" https://github.com/open-watcom/open-watcom-v2/releases/download/Current-build/ow-snapshot.tar.xz
mkdir -p "$dest"
tar -xJf "$tmp/ow.tar.xz" -C "$dest"
echo "Open Watcom is in $dest/ (make net)"
