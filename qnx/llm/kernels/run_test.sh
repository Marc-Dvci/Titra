#!/bin/bash
# Compares the AVX2 Q5_K kernels with ggml's generic kernels on the host: same blocks, same products.
#   qnx/llm/kernels/run_test.sh [LLAMA_DIR]   (default ~/llama-qnx; run qnx/llm/build.sh first)
# Builds ggml for this host in LLAMA_DIR/build-host if it is not there yet.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
L="${1:-$HOME/llama-qnx}"
if [ ! -f "$L/build-host/ggml/src/libggml-cpu.a" ]; then
    cmake -S "$L" -B "$L/build-host" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DLLAMA_BUILD_TOOLS=OFF \
        -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_SERVER=OFF -DLLAMA_BUILD_COMMON=OFF -DLLAMA_CURL=OFF \
        -DGGML_OPENMP=OFF -DGGML_NATIVE=OFF -DGGML_AVX=ON -DGGML_AVX2=ON -DGGML_FMA=ON -DGGML_F16C=ON -DGGML_BMI2=ON >/dev/null
fi
cmake --build "$L/build-host" --target ggml --parallel "$(nproc)" >/dev/null
OUT="$(mktemp -d)"
g++ -std=gnu++17 -O2 -mavx2 -mfma -mf16c -DGGML_VERSION=\"t\" -DGGML_COMMIT=\"t\" \
    -I"$L/ggml/include" -I"$L/ggml/src" -I"$L/ggml/src/ggml-cpu" "$HERE/test_q5k.cpp" \
    "$L/build-host/ggml/src/libggml-cpu.a" "$L/build-host/ggml/src/libggml-base.a" -lm -lpthread -o "$OUT/test_q5k"
"$OUT/test_q5k"
rm -rf "$OUT"
