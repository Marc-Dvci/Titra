#!/bin/bash
# Builds Titra's QNX 8.0 image for QEMU x86_64 with mkqnximage.
#
#   qnx/image/build.sh [IMAGE_DIR] [--secpol=no|develop|secure] [--policy=FILE]
#
# The image directory (default ~/titra-image) gets local/snippets from qnx/image/snippets, with the build folder of
# the Titra binaries filled in. Run `make -C qnx` first.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
IMAGE_DIR="${1:-$HOME/titra-image}"
shift || true
source "$HOME/qnx800/qnxsdp-env.sh" >/dev/null
mkdir -p "$IMAGE_DIR/local/snippets"
for f in "$HERE"/snippets/*.custom; do
    sed "s#__TITRA_BUILD__#$REPO/qnx/build/x86_64#g" "$f" >"$IMAGE_DIR/local/snippets/$(basename "$f")"
done
test -f "$HOME/.ssh/id_ed25519.pub" || ssh-keygen -q -t ed25519 -N '' -f "$HOME/.ssh/id_ed25519"
cd "$IMAGE_DIR"
mkqnximage --noprompt --force --build --type=qemu --arch=x86_64 --graphics=no --hostname=titra --cpu=4 --ram=2G \
    --ssh-ident="$HOME/.ssh/id_ed25519.pub" "$@"
ls -la output/ifs.bin output/disk-qemu.vmdk
