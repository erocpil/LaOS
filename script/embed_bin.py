#!/usr/bin/env python3
"""Generate C embed header from a binary file.

Usage: python3 embed_bin.py <binary_file> <output_header> <var_prefix>

Produces:
  unsigned char <var_prefix>[] = { ... };
  unsigned int <var_prefix>_len = ...;
"""

import sys


def main():
    if len(sys.argv) != 4:
        print(f'Usage: {sys.argv[0]} <binary> <output.h> <var_prefix>',
              file=sys.stderr)
        sys.exit(1)

    bin_path = sys.argv[1]
    out_path = sys.argv[2]
    prefix = sys.argv[3]

    with open(bin_path, 'rb') as f:
        data = f.read()

    with open(out_path, 'w') as f:
        f.write(f'unsigned char {prefix}[] = {{\n')
        for i in range(0, len(data), 12):
            chunk = data[i:i+12]
            f.write('  ' + ', '.join('0x%02x' % b for b in chunk) + ',\n')
        f.write('};\n')
        f.write(f'unsigned int {prefix}_len = {len(data)};\n')

    print(f'{out_path}: {len(data)} bytes (var: {prefix})')


if __name__ == '__main__':
    main()
