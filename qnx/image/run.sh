#!/bin/bash
# Boots Titra's image in QEMU, headless, with user-mode networking.
#
#   qnx/image/run.sh [IMAGE_DIR] [MODELS_VOLUME]
#
# The guest reaches the host's bench twin at 10.0.2.2. The host reaches the guest's ssh at 127.0.0.1:2222 and the
# nurse station at http://127.0.0.1:8080. The serial console is written to IMAGE_DIR/output/console.log. MODELS_VOLUME is
# the image qnx/llm/make_models_volume.sh writes; without one the advisor runs on its deterministic flags alone.
set -euo pipefail
IMAGE_DIR="${1:-$HOME/titra-image}"
MODELS="${2:-}"
cd "$IMAGE_DIR"
rm -f output/console.log
DISK2=()
if [ -n "$MODELS" ]; then
    DISK2=(-drive "file=$MODELS,if=ide,index=1,media=disk,format=raw,snapshot=on")
fi
# KVM when it works, software emulation otherwise. The model needs KVM to answer at usable speed.
exec qemu-system-x86_64 -machine accel=kvm:tcg -cpu max -smp 4 -m 8G \
    -drive file=output/disk-qemu.vmdk,if=ide,id=drv0 "${DISK2[@]}" \
    -netdev user,id=net0,hostfwd=tcp:127.0.0.1:2222-:22,hostfwd=tcp:127.0.0.1:8080-:8080 \
    -device virtio-net-pci,netdev=net0 \
    -object rng-random,filename=/dev/urandom,id=rng0 -device virtio-rng-pci,rng=rng0 \
    -kernel output/ifs.bin -display none -vga none \
    -serial file:output/console.log -pidfile output/qemu.pid
