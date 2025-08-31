#!/usr/bin/env python3
"""Create small, independent format-v1 seeds for libFuzzer."""
import argparse
from pathlib import Path
import struct
import zlib

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("directory", type=Path)
args = parser.parse_args()
args.directory.mkdir(parents=True, exist_ok=True)
header = b"HUFFBLK\0" + struct.pack("<HHI", 1, 0, 4096)
end = struct.pack("<B3xIIIQ", 255, 0, 0, 0, 0)
raw = bytes(range(256))
lengths = bytearray(256)
lengths[65:68] = bytes([1, 2, 2])
body = bytes(lengths) + b"\x58"
seeds = {
    "empty": header + end,
    "raw": header + struct.pack("<B3xIIIQ", 0, len(raw), len(raw), zlib.crc32(raw), len(raw) * 8) + raw + end,
    "canonical-abc": header + struct.pack("<B3xIIIQ", 1, 3, len(body), zlib.crc32(b"ABC"), 5) + body + end,
}
for name, data in seeds.items():
    (args.directory / name).write_bytes(data)
print(f"Created {len(seeds)} seeds in {args.directory}")
