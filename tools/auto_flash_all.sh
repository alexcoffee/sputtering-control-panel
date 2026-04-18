#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MAP_FILE="${SCP_BOARD_MAP_FILE:-${ROOT_DIR}/tools/.board_module_map}"
UF2_DIR="${ROOT_DIR}/cmake-build-debug-eabi/modules"

extract_module_name() {
    local arg="$1"
    local module=""

    if [[ "${arg}" == *"/modules/"* ]]; then
        module="${arg#*"/modules/"}"
        module="${module%%/*}"
    fi

    if [[ -n "${module}" ]]; then
        printf '%s\n' "${module}"
    else
        printf '%s\n' "${arg}"
    fi
}

normalize_id() {
    printf '%s\n' "$1" | tr '[:lower:]' '[:upper:]'
}

parse_board_id_from_info_output() {
    local info_out="$1"
    local board_id=""

    board_id="$(printf '%s\n' "${info_out}" | sed -n 's/^Tracking device serial number \([0-9A-Fa-f]\+\) for reboot$/\1/p' | head -n 1)"
    if [[ -z "${board_id}" ]]; then
        board_id="$(printf '%s\n' "${info_out}" | sed -n 's/^[[:space:]]*flash id:[[:space:]]*0x\([0-9A-Fa-f]\+\)$/\1/p' | head -n 1)"
    fi

    if [[ -n "${board_id}" ]]; then
        normalize_id "${board_id}"
    fi
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

reboot_board_to_application() {
    local board_id="$1"
    picotool reboot -a --ser "${board_id}" >/dev/null 2>&1 || true
}

discover_board_ids() {
    local info_out=""
    local forced_out=""
    local board_id=""
    local bus_addr_lines=()
    local line=""
    local bus=""
    local addr=""
    declare -A seen_ids=()
    local id=""

    if ! info_out="$(picotool info -d 2>&1 || true)"; then
        :
    fi

    if printf '%s\n' "${info_out}" | grep -q "Failed to initialise libUSB"; then
        printf '%s\n' "${info_out}" >&2
        return 1
    fi

    while IFS= read -r id; do
        id="$(normalize_id "${id}")"
        if [[ -n "${id}" ]]; then
            seen_ids["${id}"]=1
        fi
    done < <(printf '%s\n' "${info_out}" | sed -n 's/^[[:space:]]*flash id:[[:space:]]*0x\([0-9A-Fa-f]\+\)$/\1/p')

    mapfile -t bus_addr_lines < <(printf '%s\n' "${info_out}" | sed -n 's/^RP[0-9A-Za-z]* device at bus \([0-9][0-9]*\), address \([0-9][0-9]*\) appears.*$/\1 \2/p')

    for line in "${bus_addr_lines[@]}"; do
        bus="${line%% *}"
        addr="${line##* }"
        if [[ -z "${bus}" || -z "${addr}" ]]; then
            continue
        fi

        if ! forced_out="$(picotool info -d -F --bus "${bus}" --address "${addr}" 2>&1)"; then
            printf 'WARN: unable to probe device at bus %s address %s\n' "${bus}" "${addr}" >&2
            printf '%s\n' "${forced_out}" >&2
            continue
        fi

        board_id="$(parse_board_id_from_info_output "${forced_out}")"
        if [[ -n "${board_id}" ]]; then
            seen_ids["${board_id}"]=1
        fi
    done

    for id in "${!seen_ids[@]}"; do
        printf '%s\n' "${id}"
    done | sort
}

flash_module_to_board() {
    local board_id="$1"
    local module="$2"
    local uf2_path="${UF2_DIR}/scp_${module}.uf2"

    if [[ ! -f "${uf2_path}" ]]; then
        echo "UF2 file not found for module '${module}': ${uf2_path}" >&2
        return 1
    fi

    echo "Board ID: ${board_id} -> ${module}"
    picotool load -x "${uf2_path}" --ser "${board_id}"
}

main() {
    local explicit_module=""
    local module=""
    local ok_count=0
    local fail_count=0
    local board_id=""
    mapfile -t board_ids < <(discover_board_ids)

    if [[ ${#board_ids[@]} -eq 0 ]]; then
        echo "No RP-series devices were discovered." >&2
        return 1
    fi

    if [[ $# -ge 1 ]]; then
        explicit_module="$(extract_module_name "$1")"
    fi

    for board_id in "${board_ids[@]}"; do
        module=""

        if [[ -n "${explicit_module}" ]]; then
            module="${explicit_module}"
        else
            if ! module="$(lookup_module_by_id "${board_id}")"; then
                echo "WARN: no mapping for board ${board_id}; skipping." >&2
                reboot_board_to_application "${board_id}"
                fail_count=$((fail_count + 1))
                continue
            fi
        fi

        if flash_module_to_board "${board_id}" "${module}"; then
            persist_mapping "${board_id}" "${module}"
            ok_count=$((ok_count + 1))
        else
            echo "WARN: flashing failed for board ${board_id}." >&2
            reboot_board_to_application "${board_id}"
            fail_count=$((fail_count + 1))
        fi
    done

    echo "Completed: ${ok_count} succeeded, ${fail_count} failed."
    if [[ ${fail_count} -gt 0 ]]; then
        return 1
    fi
}

main "$@"
