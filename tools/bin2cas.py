#!/usr/bin/env python3
"""bin2cas.py - make a Dragon .cas tape from a machine-code binary

    tools/bin2cas.py [-n NAME] [-l LOAD] [-e EXEC] input.bin output.cas

The input can be a DragonDOS binary (as written by asm6809 -D: a 9-byte
header with load and exec addresses) or a raw binary (then -l is
needed; -e defaults to the load address).  The tape is what CSAVEM
would write: leader, a name block (file type 2 = machine code), the data
in 255-byte blocks, an end-of-file block.  Load it with CLOADM, then
EXEC.

Copyright (C) 2026 Dylan Jones.  GNU GPL version 2 or later.
"""
import argparse
import sys


def block(btype, data):
    """One tape block: sync, type, length, data, checksum, trailing $55."""
    assert len(data) <= 255
    checksum = (btype + len(data) + sum(data)) & 0xFF
    return bytes([0x3C, btype, len(data)]) + bytes(data) + bytes([checksum, 0x55])


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-n", "--name", help="tape name (up to 8 characters)")
    ap.add_argument("-l", "--load", type=lambda v: int(v, 0), help="load address (raw binaries)")
    ap.add_argument("-e", "--exec", type=lambda v: int(v, 0), dest="exec_addr", help="exec address")
    ap.add_argument("input")
    ap.add_argument("output")
    a = ap.parse_args()

    raw = open(a.input, "rb").read()
    if len(raw) >= 9 and raw[0] == 0x55 and raw[8] == 0xAA:		# DragonDOS header
        load = raw[2] << 8 | raw[3]
        length = raw[4] << 8 | raw[5]
        exec_addr = raw[6] << 8 | raw[7]
        data = raw[9:9 + length]
    else:
        if a.load is None:
            sys.exit("raw binary: give the load address with -l")
        load, data, exec_addr = a.load, raw, a.load
    if a.load is not None:
        load = a.load
    if a.exec_addr is not None:
        exec_addr = a.exec_addr

    name = (a.name or a.input.rsplit("/", 1)[-1].split(".")[0]).upper()[:8].ljust(8)
    namefile = name.encode("ascii") + bytes([
        0x02,				# file type: machine code
        0x00,				# binary, not ASCII
        0x00,				# no gaps between blocks
        exec_addr >> 8, exec_addr & 0xFF,
        load >> 8, load & 0xFF,
    ])

    tape = bytes([0x55] * 128) + block(0x00, namefile)
    tape += bytes([0x55] * 128)		# the gap + leader before the data
    for i in range(0, len(data), 255):
        tape += bytes([0x55]) + block(0x01, data[i:i + 255])
    tape += bytes([0x55]) + block(0xFF, b"")

    open(a.output, "wb").write(tape)
    print("%s: \"%s\" load $%04X exec $%04X, %d bytes in %d blocks"
          % (a.output, name.strip(), load, exec_addr, len(data), (len(data) + 254) // 255))


if __name__ == "__main__":
    main()
