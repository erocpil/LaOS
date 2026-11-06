#!/usr/bin/env python3
"""
mkcpio.py — SVR4 newc CPIO archive generator

Usage: python3 mkcpio.py [-o output] file1:name1 [file2:name2 ...]

Each argument is "local_path:archive_name". The archive name is the
path inside the CPIO (e.g. "user.elf", "task/module_foo.mo").

If -o is given, writes to file. Otherwise writes to stdout.
"""

import os, sys, struct, time

def pad4(n):
    """Round up to next multiple of 4."""
    return (n + 3) & ~3

def newc_header(name, size, *, mtime=None, ino=1):
    """Build a newc CPIO header (110 bytes including trailing NUL)."""
    mode = 0o100644  # regular file
    fields = {
        'c_magic':     '070701',
        'c_ino':       f'{ino:08X}',
        'c_mode':      f'{mode:08X}',
        'c_uid':       '00000000',
        'c_gid':       '00000000',
        'c_nlink':     '00000001',
        'c_mtime':     f'{int(time.time()) if mtime is None else mtime:08X}',
        'c_filesize':  f'{size:08X}',
        'c_devmajor':  '00000000',
        'c_devminor':  '00000000',
        'c_rdevmajor': '00000000',
        'c_rdevminor': '00000000',
        'c_namesize':  f'{len(name.encode("utf-8")) + 1:08X}',  # +1 for NUL
        'c_check':     '00000000',
    }
    hdr = ''.join(fields.values())
    assert len(hdr) == 110, f'header size {len(hdr)} != 110'
    return hdr.encode('ascii')

def write_entry(f, name, data, *, mtime=None, ino=1):
    hdr = newc_header(name, len(data), mtime=mtime, ino=ino)
    f.write(hdr)
    encoded_name = name.encode('utf-8')
    f.write(encoded_name + b'\x00')
    # Pad filename to 4-byte boundary
    name_pad = pad4(len(hdr) + len(encoded_name) + 1) - (len(hdr) + len(encoded_name) + 1)
    f.write(b'\x00' * name_pad)
    f.write(data)
    # Pad file data to 4-byte boundary
    data_pad = pad4(len(data)) - len(data)
    f.write(b'\x00' * data_pad)

def write_trailer(f, *, mtime=None, ino=1):
    """TRAILER!!! entry marks end of archive."""
    write_entry(f, 'TRAILER!!!', b'', mtime=mtime, ino=ino)

def main():
    args = sys.argv[1:]
    outfile = None

    if args and args[0] == '-o':
        outfile = args[1]
        args = args[2:]

    if not args:
        print(f'Usage: {sys.argv[0]} [-o output] file1:name1 ...', file=sys.stderr)
        sys.exit(1)

    entries = []
    for arg in args:
        if ':' in arg:
            local, name = arg.split(':', 1)
        else:
            local = arg
            name = os.path.basename(arg)
        if not os.path.isfile(local):
            print(f'Error: {local} not found', file=sys.stderr)
            sys.exit(1)
        entries.append((local, name))

    f = open(outfile, 'wb') if outfile else sys.stdout.buffer
    for local, name in entries:
        with open(local, 'rb') as inf:
            data = inf.read()
        write_entry(f, name, data)
    write_trailer(f)
    if outfile:
        f.close()

if __name__ == '__main__':
    main()
