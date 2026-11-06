#!/bin/bash
# Builds Titra's QNX 8.0 image for QEMU x86_64 with mkqnximage.
#
#   qnx/image/build.sh [IMAGE_DIR] [--secpol=no|develop|secure] [--policy=FILE]
#
# The image directory (default ~/titra-image) gets local/snippets from qnx/image/snippets, with the build folder of
# the Titra binaries filled in. Run `make -C qnx` first.
#
# startup-x86 -c advice:0xe declares CPUs 1-3 as a cluster, the partition the advisor and the model run in: on QNX 8.0 a
# runmask must match a cluster, and the system predefines only all CPUs and each single CPU.
# TITRA_STARTUP_OPTS adds options to startup-x86. Windows Hypervisor Platform needs "-f 0,0,200M": it does not report
# its 200 MHz APIC timer in a form the startup library reads, and KVM or a real board need nothing here.
# TITRA_RAM sets the memory the image expects (default 8G: the model's weights take 3.7 GB of it).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
IMAGE_DIR="${1:-$HOME/titra-image}"
shift || true
source "$HOME/qnx800/qnxsdp-env.sh" >/dev/null
mkdir -p "$IMAGE_DIR/local/snippets"
rm -f "$IMAGE_DIR"/local/snippets/*.custom
for f in "$HERE"/snippets/*.custom; do
    sed "s#__TITRA_BUILD__#$REPO/qnx/build/x86_64#g" "$f" >"$IMAGE_DIR/local/snippets/$(basename "$f")"
done
printf '#define __STARTUP_OPTS__ -c advice:0xe %s\n' "${TITRA_STARTUP_OPTS:-}" >"$IMAGE_DIR/local/snippets/definitions.custom"
test -f "$HOME/.ssh/id_ed25519.pub" || ssh-keygen -q -t ed25519 -N '' -f "$HOME/.ssh/id_ed25519"
cd "$IMAGE_DIR"
# procnto -P 22: priorities 22 and above need the priority ability, which only Titra's own cot processes hold. By default
# any process may use up to 63, which would let it outrank the controllers at 30.
mkqnximage --noprompt --force --build --type=qemu --arch=x86_64 --graphics=no --hostname=titra --cpu=4 \
    --ram="${TITRA_RAM:-8G}" --sys-size=48 --proc="procnto-smp-instr -P 22" --ssh-ident="$HOME/.ssh/id_ed25519.pub" "$@"
ls -la output/ifs.bin output/disk-qemu.vmdk
