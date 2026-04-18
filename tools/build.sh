#!/usr/bin/env bash
set -euo pipefail

cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

BUILD_DIR="./cmake-build-debug-eabi/"
TARGET="${1:-monitor}"

export PICO_SDK_PATH="${PICO_SDK_PATH:-$(pwd)/pico-sdk}"

if [[ "${TARGET}" == "all" || "${TARGET}" == "--all" ]]; then
    cmake -S . -B "${BUILD_DIR}" -DSCP_BUILD_ALL_MODULES=ON
else
    cmake -S . -B "${BUILD_DIR}" -DSCP_BUILD_ALL_MODULES=OFF -DSCP_MODULE_TARGET="${TARGET}"
fi

cmake --build "${BUILD_DIR}"
