#!/bin/bash
# Makes tests/data/ntfs-1746m.vkfix: an empty NTFS volume of 1746 MiB, made with mkntfs (ntfs-3g)
# for a partition starting at sector 616448 (301 MiB) of a disk with 512-byte sectors, the data
# partition of the disk image test (see the elevated test script and "image: the test disk" in
# tests/test_image.cpp). Run on Linux with ntfs-3g, python3 and zstd installed.
#
# Format (little-endian): "VKFIX001", u64 volume size, u32 chunk size, u32 chunk count, then the
# offset (u64) of every chunk that is not all zeros, then u64 length of those chunks together and
# the zstd-compressed bytes of those chunks one after the other. The rest of the volume is zeros.
set -eu
out="${1:?usage: make_ntfs_fixture.sh <output file>}"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
truncate -s 1746M "$work/ntfs.img"
mkntfs -F -Q -s 512 -c 4096 -p 616448 -H 255 -S 63 -L VaultikTest "$work/ntfs.img" >/dev/null
python3 - "$work/ntfs.img" "$work/chunks.bin" "$work/head.bin" <<'EOF'
import struct, sys
image, chunks, head = sys.argv[1:4]
size = 1746 << 20
chunk = 64 << 10
offsets = []
with open(image, 'rb') as f, open(chunks, 'wb') as c:
    off = 0
    while off < size:
        data = f.read(chunk)
        if data.count(0) != len(data):
            offsets.append(off)
            c.write(data)
        off += chunk
with open(head, 'wb') as h:
    h.write(b'VKFIX001')
    h.write(struct.pack('<QII', size, chunk, len(offsets)))
    for o in offsets:
        h.write(struct.pack('<Q', o))
    h.write(struct.pack('<Q', len(offsets) * chunk))
print(f'{len(offsets)} chunks of {chunk} bytes are not zeros')
EOF
zstd -q -19 -c "$work/chunks.bin" > "$work/chunks.zst"
cat "$work/head.bin" "$work/chunks.zst" > "$out"
ls -l "$out"
