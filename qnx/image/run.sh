#!/bin/bash
# Boots Titra's image in QEMU, headless, with user-mode networking.
#
#   qnx/image/run.sh [IMAGE_DIR]
#
# The guest reaches the host's bench twin at 10.0.2.2. The host reaches the guest's ssh at 127.0.0.1:2222 and the
# nurse station at http://127.0.0.1:8080. The serial console is written to IMAGE_DIR/output/console.log.
set -euo pipefail
IMAGE_DIR="${1:-$HOME/titra-image}"
cd "$IMAGE_DIR"
rm -f output/console.log
# KVM when it works, software emulation otherwise.
exec qemu-system-x86_64 -machine accel=kvm:tcg -cpu max -smp 4 -m 2G \
    -drive file=output/disk-qemu.vmdk,if=ide,id=drv0 \
    -netdev user,id=net0,hostfwd=tcp:127.0.0.1:2222-:22,hostfwd=tcp:127.0.0.1:8080-:8080 \
    -device virtio-net-pci,netdev=net0 \
    -object rng-random,filename=/dev/urandom,id=rng0 -device virtio-rng-pci,rng=rng0 \
    -kernel output/ifs.bin -display none -vga none \
    -serial file:output/console.log -pidfile output/qemu.pid
