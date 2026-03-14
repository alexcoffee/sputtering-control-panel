#!/usr/bin/env bash
set -euo pipefail

extract_module_name() {
    local arg="$1"
    local module=""

    # Support paths from CLion external tools (absolute or relative), e.g.
    # ".../modules/pirani/src/main.c" -> "pirani".
    if [[ "${arg}" == *"/modules/"* ]]; then
        module="${arg#*"/modules/"}"
        module="${module%%/*}"
    fi

    if [[ -n "${module}" ]]; then
        printf '%s\n' "${module}"
    else
        # Fall back to using the raw argument as a module name.
        printf '%s\n' "${arg}"
    fi
}

canonical_path() {
    local path="$1"
    local dir
    dir="$(cd "$(dirname "${path}")" && pwd -P)"
    printf '%s/%s\n' "${dir}" "$(basename "${path}")"
}

MODULE="${1:-roughing_pump}"
if [[ -n "${1:-}" ]]; then
    if [[ "$1" == "$0" ]]; then
        MODULE="roughing_pump"
    elif [[ -e "$1" ]] && [[ "$(canonical_path "$1")" == "$(canonical_path "$0")" ]]; then
        MODULE="roughing_pump"
    else
        MODULE="$(extract_module_name "$1")"
    fi
fi

UF2_PATH="../cmake-build-debug-eabi/modules/scp_${MODULE}.uf2"
echo "Flashing $UF2_PATH"

picotool load -x "${UF2_PATH}" -F
