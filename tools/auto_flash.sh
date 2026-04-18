#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MAP_FILE="${SCP_BOARD_MAP_FILE:-${ROOT_DIR}/tools/.board_module_map}"
UF2_DIR="${ROOT_DIR}/cmake-build-debug-eabi/modules"

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

normalize_id() {
    printf '%s\n' "$1" | tr '[:lower:]' '[:upper:]'
}

detect_board_id() {
    local info_out=""
    local board_id=""

    if ! info_out="$(picotool info -d -F 2>&1)"; then
        printf '%s\n' "${info_out}" >&2
        return 1
    fi

    board_id="$(printf '%s\n' "${info_out}" | sed -n 's/^Tracking device serial number \([0-9A-Fa-f]\+\) for reboot$/\1/p' | head -n 1)"
    if [[ -z "${board_id}" ]]; then
        board_id="$(printf '%s\n' "${info_out}" | sed -n 's/^[[:space:]]*flash id:[[:space:]]*0x\([0-9A-Fa-f]\+\)$/\1/p' | head -n 1)"
    fi

    if [[ -z "${board_id}" ]]; then
        echo "Unable to determine board ID from picotool output." >&2
        printf '%s\n' "${info_out}" >&2
        return 1
    fi

    normalize_id "${board_id}"
}

lookup_module_by_id() {
    local board_id="$1"

    if [[ ! -f "${MAP_FILE}" ]]; then
        return 1
    fi

    awk -v id="${board_id}" '
        NF >= 2 && $1 == id { print $2; found = 1; exit }
        END { if (!found) exit 1 }
    ' "${MAP_FILE}"
}

reboot_board_to_application() {
    local board_id="$1"
    picotool reboot -a --ser "${board_id}" >/dev/null 2>&1 || true
}

persist_mapping() {
    local board_id="$1"
    local module="$2"
    local map_dir=""
    local tmp_file=""

    map_dir="$(dirname "${MAP_FILE}")"
    mkdir -p "${map_dir}"

    if [[ ! -f "${MAP_FILE}" ]]; then
        printf '%s\t%s\n' "${board_id}" "${module}" > "${MAP_FILE}"
        return
    fi

    tmp_file="$(mktemp)"
    awk -v id="${board_id}" -v mod="${module}" '
        BEGIN { updated = 0 }
        NF == 0 { next }
        {
            if ($1 == id) {
                printf "%s\t%s\n", id, mod
                updated = 1
            } else {
                print
            }
        }
        END {
            if (!updated) {
                printf "%s\t%s\n", id, mod
            }
        }
    ' "${MAP_FILE}" > "${tmp_file}"

    mv "${tmp_file}" "${MAP_FILE}"
}

flash_module_to_board() {
    local board_id="$1"
    local module="$2"
    local uf2_path="${UF2_DIR}/scp_${module}.uf2"

    if [[ ! -f "${uf2_path}" ]]; then
        echo "UF2 file not found for module '${module}': ${uf2_path}" >&2
        echo "Build it first (e.g. ./tools/build.sh ${module})." >&2
        reboot_board_to_application "${board_id}"
        return 1
    fi

    echo "Board ID: ${board_id}"
    echo "Flashing module '${module}' from ${uf2_path}"
    picotool load -x "${uf2_path}" --ser "${board_id}"
}

main() {
    local board_id=""
    local module=""

    board_id="$(detect_board_id)"

    if [[ $# -ge 1 ]]; then
        module="$(extract_module_name "$1")"
        flash_module_to_board "${board_id}" "${module}"
        persist_mapping "${board_id}" "${module}"
        echo "Saved mapping: ${board_id} -> ${module} (${MAP_FILE})"
        return
    fi

    if ! module="$(lookup_module_by_id "${board_id}")"; then
        echo "No module mapping found for board ID ${board_id} in ${MAP_FILE}" >&2
        echo "Run with module once to register it: ./tools/auto_flash.sh <module>" >&2
        reboot_board_to_application "${board_id}"
        return 1
    fi

    flash_module_to_board "${board_id}" "${module}"
    echo "Used saved mapping: ${board_id} -> ${module}"
}

main "$@"
