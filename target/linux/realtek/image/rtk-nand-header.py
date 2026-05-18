#!/usr/bin/env python3
import struct, sys

inpath = sys.argv[1]
outpath = sys.argv[2]
loadaddr = int(sys.argv[3], 0)
burnaddr = int(sys.argv[4], 0)

d = open(inpath, 'rb').read()
if len(d) & 1:
    d += b'\xff'
s = 0
for i in range(0, len(d), 2):
    s = (s + (d[i] | (d[i+1] << 8))) & 0xFFFF
ck = (-s) & 0xFFFF
payload = d + struct.pack('<H', ck)
sig = b'cr6c'
hd = struct.pack('>4sIII', sig, loadaddr, burnaddr, len(payload))
open(outpath, 'wb').write(hd + payload)
