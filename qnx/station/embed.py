"""Embeds the nurse view into a C header, so the station serves it from its own binary.

    python3 station/embed.py station/index.html station/page.h
"""

import sys
from pathlib import Path


def main() -> None:
    src, dst = Path(sys.argv[1]), Path(sys.argv[2])
    data = src.read_bytes()
    lines = ["/* Generated from index.html by embed.py. */", "static const char TITRA_PAGE[] ="]
    for i in range(0, len(data), 96):
        chunk = data[i : i + 96]
        lines.append('    "' + "".join(f"\\x{b:02x}" for b in chunk) + '"')
    lines[-1] += ";"
    dst.write_text("\n".join(lines) + "\n", encoding="ascii")


if __name__ == "__main__":
    main()
