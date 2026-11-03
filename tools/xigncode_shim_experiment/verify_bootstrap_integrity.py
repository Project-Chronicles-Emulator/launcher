"""Reproduce the game's embedded bootstrap PE section checksum without launching it."""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path


MASK32 = 0xFFFFFFFF
CHUNK_SIZE = 0x1000


def read_u16(data: bytes, offset: int) -> int:
    if offset < 0 or offset + 2 > len(data):
        raise ValueError(f"16-bit field at {offset:#x} exceeds the file")

    return struct.unpack_from("<H", data, offset)[0]


def read_u32(data: bytes, offset: int) -> int:
    if offset < 0 or offset + 4 > len(data):
        raise ValueError(f"32-bit field at {offset:#x} exceeds the file")

    return struct.unpack_from("<I", data, offset)[0]


def hash_chunk(chunk: bytes) -> int:
    accumulator = 0
    for value in chunk:
        mixed = (value + accumulator) & MASK32
        accumulator = (mixed * 0x401) & MASK32
        accumulator ^= accumulator >> 6

    mixed = (accumulator * 9) & MASK32
    mixed ^= mixed >> 11
    return (mixed * 0x8001) & MASK32


def inspect_module(path: Path) -> dict[str, object]:
    data = path.read_bytes()
    if data[:2] != b"MZ":
        raise ValueError("Missing DOS header")

    pe_offset = read_u32(data, 0x3C)
    if data[pe_offset:pe_offset + 4] != b"PE\0\0":
        raise ValueError("Missing PE signature")

    section_count = read_u16(data, pe_offset + 6)
    timestamp = read_u32(data, pe_offset + 8)
    optional_header_size = read_u16(data, pe_offset + 0x14)
    section_table = pe_offset + 0x18 + optional_header_size
    marker = (read_u16(data, 0x1C) << 16) | read_u16(data, 0x1E)

    section_hash = 0
    sections = []
    for index in range(section_count):
        header = section_table + index * 40
        name = data[header:header + 8].split(b"\0", 1)[0].decode("ascii", errors="replace")
        raw_size = read_u32(data, header + 0x10)
        raw_offset = read_u32(data, header + 0x14)
        if raw_offset + raw_size > len(data):
            raise ValueError(f"Section {name} extends beyond the file")

        for chunk_start in range(raw_offset, raw_offset + raw_size, CHUNK_SIZE):
            chunk_end = min(chunk_start + CHUNK_SIZE, raw_offset + raw_size)
            section_hash = (section_hash + hash_chunk(data[chunk_start:chunk_end])) & MASK32

        sections.append({"name": name, "raw_offset": raw_offset, "raw_size": raw_size})

    computed_marker = timestamp ^ section_hash
    return {
        "path": str(path.resolve()),
        "sha256": hashlib.sha256(data).hexdigest(),
        "file_bytes": len(data),
        "timestamp": f"0x{timestamp:08x}",
        "stored_marker": f"0x{marker:08x}",
        "computed_marker": f"0x{computed_marker:08x}",
        "marker_matches": marker == computed_marker,
        "sections": sections,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--require-match",
        action="store_true",
        help="exit with an error when a module's stored marker is invalid",
    )
    parser.add_argument("modules", type=Path, nargs="+", help="PE modules to inspect")
    arguments = parser.parse_args()

    all_markers_match = True
    for module in arguments.modules:
        result = inspect_module(module)
        print(json.dumps(result, indent=2))
        all_markers_match = all_markers_match and bool(result["marker_matches"])

    if arguments.require_match and not all_markers_match:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
