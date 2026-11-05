"""Cut a README GIF from a screen recording: one action, cropped, looped, small.

python scripts/make_gif.py <recording.mp4> <docs/out.gif> [--start 2] [--duration 10] [--width 900] [--fps 12]
                           [--crop W:H:X:Y] [--colors 96] [--dither none]

Two-pass palette (ffmpeg palettegen/paletteuse) for clean colours. Aim for 8 to 15 seconds and under 5 MB: about
900 px wide for a landscape screen, 400 px for a tall one shown side by side. A moving camera changes every pixel
of every frame; if the file is too big, lower --colors, --fps or --width, or crop tighter. No dithering keeps 3D
scenes and soft gradients small and clean; try --dither bayer:bayer_scale=5 if flat areas show bands.
Needs ffmpeg on the PATH.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

LIMIT_MB = 5.0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("video", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--start", type=float, default=0.0)
    ap.add_argument("--duration", type=float, default=10.0)
    ap.add_argument("--width", type=int, default=900)
    ap.add_argument("--fps", type=int, default=12)
    ap.add_argument("--crop", help="W:H:X:Y in source pixels, to keep only the panel that matters")
    ap.add_argument("--colors", type=int, default=96, help="palette size, 2 to 256")
    ap.add_argument("--dither", default="none", help="ffmpeg paletteuse dither, e.g. none or bayer:bayer_scale=5")
    a = ap.parse_args()
    steps = ([f"crop={a.crop}"] if a.crop else []) + [f"fps={a.fps}", f"scale={a.width}:-1:flags=lanczos"]
    graph = (
        ",".join(steps)
        + f",split[x][y];[x]palettegen=max_colors={a.colors}:stats_mode=diff[p];"
        + f"[y][p]paletteuse=dither={a.dither}:diff_mode=rectangle"
    )
    cmd = ["ffmpeg", "-y", "-loglevel", "error", "-ss", str(a.start), "-t", str(a.duration), "-i", str(a.video)]
    subprocess.run([*cmd, "-filter_complex", graph, "-loop", "0", str(a.out)], check=True)
    size = a.out.stat().st_size / 1e6
    print(f"wrote {a.out} ({size:.1f} MB)")
    if size > LIMIT_MB:
        print(f"over {LIMIT_MB:.0f} MB: lower --colors, --fps or --width, shorten it, or crop tighter")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
