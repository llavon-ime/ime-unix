#!/usr/bin/env bash
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "${ROOT_DIR}/macos/scripts/input-source-lifecycle.sh"

bundle=com.llavon.inputmethod.LlavonIME
current="${bundle}.Default"
events=""
fail_ascii=0
fail_restore=0
sleep() { :; }
llavon_tis() {
    case "$1" in
        current) echo "${current}" ;;
        select-ascii)
            events="${events} ascii"
            [[ "${fail_ascii}" != 2 ]] || return 0
            [[ "${fail_ascii}" == 0 ]] || return 1
            current=com.apple.keylayout.ABC ;;
        select)
            events="${events} restore"
            [[ "${fail_restore}" == 0 ]] || return 1
            current="$2" ;;
        *) return 1 ;;
    esac
}

# Active input method switches away before replacement and is restored later.
llavon_prepare_input_source "${bundle}"
[[ "${current}" == com.apple.keylayout.ABC ]]
[[ "${llavon_restore_source}" == "${bundle}.Default" ]]
llavon_restore_input_source
[[ "${current}" == "${bundle}.Default" && "${events}" == ' ascii restore' ]]

# A different selected source is not changed by the install.
current=com.apple.inputmethod.TCIM.Zhuyin
events=""
llavon_prepare_input_source "${bundle}"
llavon_restore_input_source
[[ "${current}" == com.apple.inputmethod.TCIM.Zhuyin && -z "${events}" ]]

# Failure to switch prevents destructive replacement.
current="${bundle}.Default"
fail_ascii=1
if llavon_prepare_input_source "${bundle}" 2>/dev/null; then
    echo 'expected preparation failure' >&2
    exit 1
fi
[[ "${current}" == "${bundle}.Default" ]]

# A successful API return without a real selection change is not sufficient.
fail_ascii=2
if llavon_prepare_input_source "${bundle}" 2>/dev/null; then
    echo 'expected unchanged-selection failure' >&2
    exit 1
fi
[[ "${current}" == "${bundle}.Default" ]]

# Failure to restore is reported and leaves a working system layout selected.
fail_ascii=0
fail_restore=1
llavon_prepare_input_source "${bundle}"
if llavon_restore_input_source 2>/dev/null; then
    echo 'expected restoration failure' >&2
    exit 1
fi
[[ "${current}" == com.apple.keylayout.ABC ]]
echo 'input-source lifecycle tests passed'
