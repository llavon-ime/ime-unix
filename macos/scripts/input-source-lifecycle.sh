#!/bin/sh

# Input-source lifecycle for bundle replacement. The caller provides
# llavon_tis(), which runs the helper in the console user's GUI session.
llavon_prepare_input_source() {
    llavon_restore_source=""
    llavon_current_source="$(llavon_tis current)" || return 1
    case "${llavon_current_source}" in
        "${1}"|"${1}".*)
            llavon_restore_source="${llavon_current_source}"
            llavon_tis select-ascii || return 1
            for llavon_attempt in 1 2 3 4 5; do
                llavon_current_source="$(llavon_tis current)" || return 1
                case "${llavon_current_source}" in
                    "${1}"|"${1}".*) sleep 0.2 ;;
                    *) return 0 ;;
                esac
            done
            echo "Could not switch away from the input method before replacing it." >&2
            return 1
            ;;
    esac
}

llavon_restore_input_source() {
    [ -n "${llavon_restore_source:-}" ] || return 0
    for llavon_attempt in 1 2 3 4 5; do
        if llavon_tis select "${llavon_restore_source}" &&
           [ "$(llavon_tis current)" = "${llavon_restore_source}" ]; then
            llavon_restore_source=""
            return 0
        fi
        sleep 1
    done
    echo "Could not restore ${llavon_restore_source}; the system keyboard remains selected." >&2
    return 1
}
