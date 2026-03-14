#!/usr/bin/env bash
set -euo pipefail

cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

MODULE="${1:-roughing_pump}"
BUILD_DIR="./cmake-build-debug-eabi/"

export PICO_SDK_PATH="${PICO_SDK_PATH:-$(pwd)/pico-sdk}"

cmake -S . -B "${BUILD_DIR}" -DSCP_MODULE_TARGET="${MODULE}"
cmake --build "${BUILD_DIR}"
