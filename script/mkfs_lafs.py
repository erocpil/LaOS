#!/usr/bin/env python3
"""
mkfs_lafs.py — 生成 LaFS 只读文件系统磁盘镜像

LaFS 格式（lafs.h）:
  Superblock (sector 0):
    byte 0-4   magic "LaFS\x01"
    byte 5-8   inode_count  (uint32 LE)
    byte 9-12  inode_start  (uint32 LE, sector)
    byte 13-16 data_start   (uint32 LE, sector)
    byte 17-20 root_inode   (uint32 LE, always 0)

  Inode (128 bytes):
    byte 0     type   (0=free 1=file 2=dir)
    byte 1-32  name   (char[32], null-padded)
    byte 33-36 size   (uint32 LE)
    byte 37-116 blocks[20] (uint32 LE, direct block pointers)

  Directory entry:
    bytes 0-3  inode  (uint32 LE)
    byte 4     nlen   (name length)
    bytes 5+   name   (no null terminator)

用法: python3 mkfs_lafs.py -o disk.img
"""

import struct
import argparse
import os


def pack_u32(v):
    return struct.pack("<I", v)


def write_inode(img, inode_offset, inode_type, name, size, *blocks):
    """写一个 128 字节 inode 到 img[inode_offset:]"""
    img[inode_offset] = inode_type
    name_bytes = name.encode("utf-8")[:31]
    img[inode_offset + 1 : inode_offset + 1 + len(name_bytes)] = name_bytes
    img[inode_offset + 33 : inode_offset + 37] = pack_u32(size)
    for i, blk in enumerate(blocks[:20]):
        img[inode_offset + 37 + i * 4 : inode_offset + 37 + i * 4 + 4] = pack_u32(blk)


def write_dir_entry(img, offset, inode, name):
    """写一条目录项到 img[offset:]"""
    name_bytes = name.encode("utf-8")
    img[offset : offset + 4] = pack_u32(inode)
    img[offset + 4] = len(name_bytes)
    img[offset + 5 : offset + 5 + len(name_bytes)] = name_bytes
    return offset + 5 + len(name_bytes)


def make_lafs_image(output_path):
    """
    布局：
      Sector 0: Superblock
      Sector 1: Inode table (4 inodes × 128B = 512B)
      Sector 2: Root directory entries
      Sector 3: /etc directory entries
      Sector 4: /etc/motd data
      Sector 5: /etc/version data
    """
    SECTOR = 512
    total_sectors = 32  # 16 KB 够用
    img = bytearray(total_sectors * SECTOR)

    # ── Superblock ──
    img[0:5] = b"LaFS\x01"
    img[5:9] = pack_u32(4)       # inode_count
    img[9:13] = pack_u32(1)       # inode_start
    img[13:17] = pack_u32(2)      # data_start
    img[17:21] = pack_u32(0)      # root_inode

    INODE_SIZE = 128
    inode_base = 1 * SECTOR  # sector 1

    # ── 文件内容 ──
    motd = b"Welcome to LaOS!\nA teaching OS kernel.\n"
    version = b"0.1.0\n"

    # ── Inode 0: root dir (/) ──
    # entries: "etc" → inode 1
    # size = 1 entry: 4(inode) + 1(nlen) + 3("etc") = 8
    # But we'll compute actual size from dir entries written
    write_inode(img, inode_base + 0 * INODE_SIZE, 2, "", 0, 2)

    # ── Inode 1: /etc dir ──
    # entries: "motd" → inode 2, "version" → inode 3
    write_inode(img, inode_base + 1 * INODE_SIZE, 2, "etc", 0, 3)

    # ── Inode 2: /etc/motd ──
    write_inode(img, inode_base + 2 * INODE_SIZE, 1, "motd", len(motd), 4)

    # ── Inode 3: /etc/version ──
    write_inode(img, inode_base + 3 * INODE_SIZE, 1, "version", len(version), 5)

    # ── Root directory entries (sector 2) ──
    pos = 2 * SECTOR
    pos = write_dir_entry(img, pos, 1, "etc")
    # Update root dir inode size
    root_entry_size = pos - 2 * SECTOR
    img[inode_base + 0 * INODE_SIZE + 33 : inode_base + 0 * INODE_SIZE + 37] = pack_u32(root_entry_size)

    # ── /etc directory entries (sector 3) ──
    pos = 3 * SECTOR
    pos = write_dir_entry(img, pos, 2, "motd")
    pos = write_dir_entry(img, pos, 3, "version")
    etc_entry_size = pos - 3 * SECTOR
    img[inode_base + 1 * INODE_SIZE + 33 : inode_base + 1 * INODE_SIZE + 37] = pack_u32(etc_entry_size)

    # ── File data ──
    img[4 * SECTOR : 4 * SECTOR + len(motd)] = motd
    img[5 * SECTOR : 5 * SECTOR + len(version)] = version

    # Write image
    os.makedirs(os.path.dirname(output_path) or ".", exist_ok=True)
    with open(output_path, "wb") as f:
        f.write(img)

    print(
        f"[mkfs-lafs] {output_path}: {total_sectors * SECTOR} bytes, "
        f"4 inodes, 2 files"
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Generate LaFS disk image")
    parser.add_argument("-o", "--output", required=True, help="Output image path")
    args = parser.parse_args()
    make_lafs_image(args.output)
