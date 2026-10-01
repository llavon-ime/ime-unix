#!/usr/bin/env bash
# Build in a dedicated Linux scratch directory using cached, patched sources.
# The repository itself can be mounted read-only. No network downloads needed.
set -euo pipefail
REPO="${1:?usage: build-linux.sh REPO SCRATCH}"
SCRATCH="${2:?usage: build-linux.sh REPO SCRATCH}"
PREFIX="${SCRATCH}/prefix"
mkdir -p "${SCRATCH}/sources" "${PREFIX}"
copy_source() {
    local port="$1" directory="$2"
    if [[ ! -d "${SCRATCH}/sources/${port}" ]]; then
        cp -R "${REPO}/vcpkg/buildtrees/${port}/src/${directory}" "${SCRATCH}/sources/${port}"
    fi
}
copy_source ggml v0.11.1-71fe00c170.clean
copy_source llama-cpp b9030-e7f1e8d5e1.clean
copy_source reflectcpp v0.25.0-4459a3d10b.clean
copy_source utfcpp v4.1.1-a4242085af.clean
COMMON=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="${CC:-clang}" -DCMAKE_CXX_COMPILER="${CXX:-clang++}"
        -DCMAKE_CXX_STANDARD=23 -DCMAKE_CXX_SCAN_FOR_MODULES=OFF
        -DCMAKE_INSTALL_PREFIX="${PREFIX}" -DCMAKE_PREFIX_PATH="${PREFIX}")
cmake -S "${SCRATCH}/sources/ggml" -B "${SCRATCH}/ggml" "${COMMON[@]}" \
    -DBUILD_SHARED_LIBS=OFF -DGGML_STATIC=ON -DGGML_NATIVE=ON -DGGML_OPENMP=OFF \
    -DGGML_METAL=OFF -DGGML_VULKAN=OFF -DGGML_BUILD_TESTS=OFF -DGGML_BUILD_EXAMPLES=OFF \
    -DGGML_ALL_WARNINGS=ON -DGGML_CCACHE=OFF
cmake --build "${SCRATCH}/ggml" --parallel 4
cmake --install "${SCRATCH}/ggml"
cmake -S "${SCRATCH}/sources/llama-cpp" -B "${SCRATCH}/llama" "${COMMON[@]}" \
    -DBUILD_SHARED_LIBS=OFF -DLLAMA_USE_SYSTEM_GGML=ON -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_COMMON=OFF \
    -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_TOOLS=OFF -DLLAMA_BUILD_SERVER=OFF \
    -DLLAMA_CURL=OFF -DLLAMA_ALL_WARNINGS=ON
cmake --build "${SCRATCH}/llama" --parallel 4
cmake --install "${SCRATCH}/llama"
cmake -S "${SCRATCH}/sources/reflectcpp" -B "${SCRATCH}/reflectcpp" "${COMMON[@]}" \
    -DREFLECTCPP_BUILD_SHARED=OFF -DREFLECTCPP_BUILD_TESTS=OFF -DREFLECTCPP_INSTALL=ON \
    -DREFLECTCPP_STRICT_WARNINGS=ON -DREFLECTCPP_USE_BUNDLED_DEPENDENCIES=ON -DREFLECTCPP_USE_VCPKG=OFF
cmake --build "${SCRATCH}/reflectcpp" --parallel 4
cmake --install "${SCRATCH}/reflectcpp"
cmake -S "${SCRATCH}/sources/utfcpp" -B "${SCRATCH}/utfcpp" "${COMMON[@]}"
cmake --install "${SCRATCH}/utfcpp"
cmake -S "${REPO}/scripts/inference-bench" -B "${SCRATCH}/bench" "${COMMON[@]}" \
    -DLLAVON_IME_WARNINGS_AS_ERRORS=ON
cmake --build "${SCRATCH}/bench" --target inference_bench --parallel 4
