#!/usr/bin/env python3
"""Convert a PNG to vanille/extern/resources/fonts/logo.c

Usage:
  python scripts/png_to_logo_c.py path/to/your/logo.png
  python scripts/png_to_logo_c.py path/to/your/logo.png -o extern/resources/fonts/logo.c
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path


def png_to_c(data: bytes) -> str:
    lines = ["const unsigned char vanille_png[] = {"]
    row: list[str] = []
    for i, byte in enumerate(data):
        row.append(f"0x{byte:02X}")
        if len(row) == 12:
            lines.append("    " + ", ".join(row) + ",")
            row = []
    if row:
        lines.append("    " + ", ".join(row))
    lines.append("};")
    lines.append(f"const unsigned int vanille_png_len = {len(data)};")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description="PNG → logo.c for Vanille")
    parser.add_argument("png", type=Path, help="Your logo PNG file")
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=Path(__file__).resolve().parent.parent / "extern" / "resources" / "fonts" / "logo.c",
        help="Output logo.c path (default: extern/resources/fonts/logo.c)",
    )
    args = parser.parse_args()

    if not args.png.is_file():
        print(f"Error: not found: {args.png}", file=sys.stderr)
        return 1

    data = args.png.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        print("Warning: file does not look like a PNG header", file=sys.stderr)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(png_to_c(data), encoding="utf-8", newline="\n")
    print(f"Wrote {len(data)} bytes -> {args.output}")
    print("Rebuild vanille (Release|x64) and run build\\vanille.exe")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
