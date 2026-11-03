"""Write the game's PE section checksum marker to a built experimental module."""

from __future__ import annotations

import argparse
import os
import struct
from pathlib import Path

from verify_bootstrap_integrity import inspect_module


PROJECT_ROOT = Path(__file__).resolve().parents[2]
OUTPUT_ROOT = PROJECT_ROOT / "out"


def finalize_marker(module_path: Path) -> str:
    resolved_module = module_path.resolve(strict=True)
    if not resolved_module.is_relative_to(OUTPUT_ROOT.resolve()):
        raise ValueError("Only modules inside this emulator's out directory may be finalized")
    if resolved_module.name.lower() != "x3_x64.xem":
        raise ValueError("The experimental module must be named x3_x64.xem")

    inspection = inspect_module(resolved_module)
    marker = int(str(inspection["computed_marker"]), 16)
    data = bytearray(resolved_module.read_bytes())
    struct.pack_into("<H", data, 0x1C, marker >> 16)
    struct.pack_into("<H", data, 0x1E, marker & 0xFFFF)

    staging_path = resolved_module.with_name(resolved_module.name + ".marker-staged")
    if staging_path.exists():
        raise FileExistsError(staging_path)

    staging_path.write_bytes(data)
    os.replace(staging_path, resolved_module)
    if not inspect_module(resolved_module)["marker_matches"]:
        raise ValueError("The finalized module did not pass the bootstrap checksum check")

    return f"0x{marker:08x}"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("module", type=Path, help="built x3_x64.xem inside Emulator/out")
    arguments = parser.parse_args()

    marker = finalize_marker(arguments.module)
    print(f"Finalized bootstrap marker: {marker}")


if __name__ == "__main__":
    main()
