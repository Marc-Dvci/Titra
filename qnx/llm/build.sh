#!/bin/bash
# Builds llama.cpp for QNX 8.0 x86_64: the official repository at the commit NVIDIA's model card pins, with Titra's patch.
#
#   qnx/llm/build.sh [DIR]          (default ~/llama-qnx)
#
# Then `make -C qnx llm` builds /dev/llm's server against it. The patch holds the QNX port (runmask and priority in ggml,
# the memory query), the per-decode loop count for looped models, and the AVX2 kernels for Q5_K weights.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
DIR="${1:-$HOME/llama-qnx}"
REV=98a74c97ea09ac72f6ceda1559808fc0a2f447ce
source "$HOME/qnx800/qnxsdp-env.sh" >/dev/null
if [ ! -d "$DIR/.git" ] && [ ! -f "$DIR/.git" ]; then
    git init -q "$DIR"
    git -C "$DIR" remote add origin https://github.com/ggml-org/llama.cpp.git
    git -C "$DIR" fetch -q origin pull/30306/head
    git -C "$DIR" checkout -q --detach "$REV"
    git -C "$DIR" apply "$HERE/patches/llama-qnx.patch"
fi
cmake -S "$DIR" -B "$DIR/build-qnx" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$HERE/toolchain-qnx-x86_64.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DLLAMA_BUILD_TOOLS=OFF -DLLAMA_BUILD_EXAMPLES=OFF \
    -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_SERVER=OFF -DLLAMA_BUILD_COMMON=OFF -DLLAMA_CURL=OFF -DGGML_OPENMP=OFF \
    -DGGML_NATIVE=OFF -DGGML_BACKEND_DL=OFF -DGGML_AVX=ON -DGGML_AVX2=ON -DGGML_FMA=ON -DGGML_F16C=ON -DGGML_BMI2=ON \
    -DGGML_SSE42=ON -DGGML_AVX_VNNI=OFF -DGGML_LLAMAFILE=ON >/dev/null
cmake --build "$DIR/build-qnx" --target llama --parallel "$(nproc)"
ls -la "$DIR/build-qnx/src/libllama.a"
