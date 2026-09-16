#!/usr/bin/env python3
"""Splice an Elektron Machinedrum / Monomachine OS upgrade SysEx file into an 8 MiB flash image.

  elektronOsToFlash.py <stock flash .bin> <OS upgrade .syx> <output .bin>

The OS upgrade file is what the "MIDI Upgrade" boot menu accepts (stock Elektron releases and the
community X.xx / EMS firmwares). Each 0x7e message carries a flash address (six 4-bit nibbles) and
32 big-endian 16-bit words, each word packed as three 7-bit bytes (b0 << 14 | b1 << 7 | b2). The
OS occupies flash from 0x4000 upwards; the boot loader in the first 16 KiB and everything after the
OS (samples, kits, patterns, global data) are copied unchanged from the stock image.

The emulator only accepts the two stock images unless GEARMULATOR_ALLOW_ALT_OS=1 is set in the
plugin host's environment, which lets any image with the stock boot loader through.
"""
import struct
import sys

OS_START = 0x4000
FLASH_SIZE = 8 * 1024 * 1024


def sysex_messages(data):
    out = []
    i = 0
    while True:
        a = data.find(b'\xf0', i)
        if a < 0:
            return out
        b = data.find(b'\xf7', a)
        if b < 0:
            return out
        out.append(data[a:b + 1])
        i = b + 1


def decode_block(msg):
    # F0 00 20 3C <product> 00 7E <chk> <chk> <a5..a0> <words...> F7
    addr = 0
    for n in msg[9:15]:
        addr = (addr << 4) | (n & 15)
    payload = msg[15:-1]
    raw = bytearray()
    for i in range(0, len(payload) - len(payload) % 3, 3):
        raw += struct.pack('>H', ((payload[i] << 14) | (payload[i + 1] << 7) | payload[i + 2]) & 0xffff)
    return addr, bytes(raw)


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    stock = bytearray(open(sys.argv[1], 'rb').read())
    syx = open(sys.argv[2], 'rb').read()
    if len(stock) != FLASH_SIZE:
        sys.exit('stock image must be exactly 8 MiB')
    product = {2: 'Machinedrum', 3: 'Monomachine'}
    blocks = []
    for m in sysex_messages(syx):
        if len(m) < 16 or m[1:4] != b'\x00\x20\x3c' or m[6] != 0x7e:
            continue
        addr, raw = decode_block(m)
        if addr >= OS_START and raw:
            blocks.append((addr, raw))
    if not blocks:
        sys.exit('no OS blocks found in the SysEx file')
    prod = product.get(syx[4], 'unknown product %d' % syx[4])
    end = max(a + len(r) for a, r in blocks)

    # the stock OS ends where the flash turns into erased 0xff before the data area
    old_end = OS_START
    while old_end < len(stock) and stock[old_end:old_end + 256] != b'\xff' * 256:
        old_end += 256
    if end > old_end:
        # the new OS is longer than the stock one: make sure it only grows into erased space
        if any(b != 0xff for b in stock[old_end:end]):
            sys.exit('new OS (ends at 0x%x) would overwrite data after the stock OS (0x%x)' % (end, old_end))

    out = bytearray(stock)
    for a, r in blocks:
        out[a:a + len(r)] = r
    if end < old_end:
        out[end:old_end] = b'\xff' * (old_end - end)
    open(sys.argv[3], 'wb').write(out)
    print('%s OS: %d blocks, flash 0x%x..0x%x (stock OS ended at 0x%x), wrote %s'
          % (prod, len(blocks), OS_START, end, old_end, sys.argv[3]))


if __name__ == '__main__':
    main()
