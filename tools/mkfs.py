#!/usr/bin/env python3
# mkfs.py — builds a raw LiquidFS disk image from a directory of files.
#
# Layout (must match kernel/fs.h exactly):
#   LBA 0           Superblock: magic "LQFS", version (u32), file_count (u32),
#                    then free_extent_count (u32) + free extent list — left
#                    zeroed here since a freshly-formatted image has nothing
#                    to reclaim; fs.h's write_file()/delete_file() populate
#                    it at runtime as files are grown, shrunk, or deleted.
#   LBA 1..4        Directory table: up to 32 entries, 64 bytes each
#   LBA 5..end      File data, each file starting on its own sector boundary
#
# This is a host-side build tool, not kernel code — it runs at image-build
# time (via `make`), not at boot. The kernel only ever reads this format
# (mkfs.py never runs again after boot); fs.h's write_file()/delete_file()
# are what change it at runtime.
#
# TOTAL_SECTORS pads the image out to a fixed size beyond whatever
# diskfiles/ needs. This matters more than it looks: QEMU fixes an
# attached drive's reported capacity to the image file's size at the
# moment it's attached. A disk image sized EXACTLY to its baked-in files
# has no headroom at all — the first byte fs.h's write_file() ever tries
# to write past the last existing file lands on an LBA the emulated
# drive doesn't believe exists, and the command is rejected with an ATA
# error. (Found by hitting exactly this while testing write support.)

import os
import struct
import sys

SECTOR_SIZE = 512
DIR_START_LBA = 1
DIR_SECTORS = 4
DATA_START_LBA = DIR_START_LBA + DIR_SECTORS  # 5
MAX_FILES = 32
NAME_LEN = 32
ENTRY_SIZE = 64
TOTAL_SECTORS = 2048  # 1 MiB image - plenty of headroom for runtime writes


def build(files_dir, out_path):
    names = sorted(
        n for n in os.listdir(files_dir)
        if os.path.isfile(os.path.join(files_dir, n))
    )
    if len(names) > MAX_FILES:
        raise SystemExit(f"too many files ({len(names)}), max is {MAX_FILES}")

    entries = []
    data_chunks = []
    next_lba = DATA_START_LBA

    for name in names:
        if len(name) >= NAME_LEN:
            raise SystemExit(f"filename too long (>{NAME_LEN - 1} chars): {name}")
        with open(os.path.join(files_dir, name), "rb") as f:
            data = f.read()
        sectors_needed = max(1, (len(data) + SECTOR_SIZE - 1) // SECTOR_SIZE)
        entries.append((name, next_lba, len(data)))
        data_chunks.append(data)
        next_lba += sectors_needed

    with open(out_path, "wb") as out:
        sb = bytearray(SECTOR_SIZE)
        sb[0:4] = b"LQFS"
        struct.pack_into("<I", sb, 4, 1)             # version
        struct.pack_into("<I", sb, 8, len(entries))  # file_count
        out.write(sb)

        dir_bytes = bytearray(DIR_SECTORS * SECTOR_SIZE)
        for i, (name, start_lba, size) in enumerate(entries):
            offset = i * ENTRY_SIZE
            name_bytes = name.encode("ascii")
            dir_bytes[offset:offset + len(name_bytes)] = name_bytes
            struct.pack_into("<I", dir_bytes, offset + 32, start_lba)
            struct.pack_into("<I", dir_bytes, offset + 36, size)
        out.write(dir_bytes)

        for data in data_chunks:
            out.write(data)
            pad = (-len(data)) % SECTOR_SIZE
            if pad:
                out.write(b"\x00" * pad)

        if next_lba > TOTAL_SECTORS:
            raise SystemExit(
                f"diskfiles/ needs {next_lba} sectors, exceeds TOTAL_SECTORS={TOTAL_SECTORS}"
            )
        out.write(b"\x00" * ((TOTAL_SECTORS - next_lba) * SECTOR_SIZE))

    print(f"wrote {out_path}: {len(entries)} files, {next_lba} sectors used, "
          f"{TOTAL_SECTORS} sectors total ({TOTAL_SECTORS * SECTOR_SIZE} bytes)")
    for name, start_lba, size in entries:
        print(f"  {name:<20} lba={start_lba:<4} size={size}")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(f"usage: {sys.argv[0]} <files_dir> <out.img>")
    build(sys.argv[1], sys.argv[2])
