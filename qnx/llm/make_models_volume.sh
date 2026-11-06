#!/bin/bash
# Builds the read-only models volume: a QNX6 filesystem image holding the GGUF, attached to the guest as a second disk.
#
#   qnx/llm/make_models_volume.sh MODEL.gguf OUT.qnx6
#
# The model is NVIDIA Nemotron Labs Dory 1.0 4B, Q5_K_M, from Hugging Face (licence OpenMDW-1.1). It is not in this
# repository. At boot llmstore checks it against its published SHA-256 and refuses any other file.
set -euo pipefail
MODEL="$1"
OUT="$2"
source "$HOME/qnx800/qnxsdp-env.sh" >/dev/null
SIZE=$(stat -c %s "$MODEL")
BUILD="$(mktemp)"
printf '[num_sectors=%d]\n[perms=0444 uid=0 gid=0] dory.gguf=%s\n' $(((SIZE + 200 * 1024 * 1024) / 512)) "$MODEL" >"$BUILD"
mkqnx6fsimg "$BUILD" "$OUT"
rm -f "$BUILD"
ls -la "$OUT"
