#!/bin/sh
set -eu

# Uninstalls the native input method package and cleans up the files left by
# the earlier fcitx5-based package.
#
# The earlier package bundled Fcitx5.app; this one does not. A leftover
# Fcitx5.app is only removed when the console user agrees: without a console
# user, or when the dialog cannot be shown, it is kept. This mirrors the
# package preinstall.

console_user="$(stat -f %Su /dev/console 2>/dev/null || true)"
if [ -z "${console_user}" ] || [ "${console_user}" = "root" ] || [ "${console_user}" = "loginwindow" ]; then
    console_user="${SUDO_USER:-}"
fi

user_home=""
uid=""
if [ -n "${console_user}" ] && id "${console_user}" >/dev/null 2>&1; then
    user_home="$(dscl . -read "/Users/${console_user}" NFSHomeDirectory 2>/dev/null | awk '{print $2}')"
    if [ -z "${user_home}" ] || [ ! -d "${user_home}" ]; then
        user_home="$(eval echo "~${console_user}")"
    fi
    uid="$(id -u "${console_user}")"
    /usr/bin/pkill -x -u "${uid}" LlavonIME >/dev/null 2>&1 || true
fi

rm -rf "/Library/Input Methods/LlavonIME.app"
rm -rf "/Library/Application Support/llavon-ime"
pkgutil --forget llavon-ime >/dev/null 2>&1 || true

fcitx5_system_app="/Library/Input Methods/Fcitx5.app"
fcitx5_user_app=""
if [ -n "${user_home}" ] && [ -d "${user_home}/Library/Input Methods/Fcitx5.app" ]; then
    fcitx5_user_app="${user_home}/Library/Input Methods/Fcitx5.app"
fi

if [ -d "${fcitx5_system_app}" ] || [ -n "${fcitx5_user_app}" ]; then
    answer=""
    if [ -n "${user_home}" ]; then
        # launchctl asuser puts osascript in the console user's session so the
        # dialog is visible; dropping to the user keeps the AppleEvent allowed.
        answer="$(/bin/launchctl asuser "${uid}" /usr/bin/sudo -u "${console_user}" \
            /usr/bin/osascript \
            -e 'button returned of (display dialog "偵測到先前安裝的 Fcitx5 輸入法。要一併移除它嗎？" buttons {"保留", "移除"} default button "保留" with title "拉風輸入法" with icon caution giving up after 300)' \
            2>/dev/null || true)"
    fi

    if [ "${answer}" = "移除" ]; then
        /usr/bin/pkill -x Fcitx5 >/dev/null 2>&1 || true
        rm -rf "${fcitx5_system_app}"
        if [ -n "${fcitx5_user_app}" ]; then
            rm -rf "${fcitx5_user_app}"
        fi
        echo "llavon-ime: removed the previous Fcitx5 install."
    else
        echo "llavon-ime: kept the previous Fcitx5 install."
    fi
fi

if [ -n "${user_home}" ]; then
    # Legacy files from the fcitx5-based package.
    target_root="${user_home}/Library/fcitx5"
    /usr/bin/pkill -x llavon-ime-unix-service >/dev/null 2>&1 || true
    rm -f \
        "${target_root}/bin/llavon-ime-unix-service" \
        "${target_root}/bin/llavon-ime-lora" \
        "${target_root}/bin/llavon-ime-lora-gui" \
        "${target_root}/bin/llavon-ime-lora-backend" \
        "${target_root}/lib/fcitx5/llavon-ime-addon.so" \
        "${target_root}/share/fcitx5/addon/llavon-ime.conf" \
        "${target_root}/share/fcitx5/inputmethod/llavon-ime.conf" \
        "${target_root}/plugin/llavon-ime.json"
    rm -rf "${target_root}/share/llavon-ime/tables" "${target_root}/lib/llavon-ime/tools" "${target_root}/bin/llavon-ime-lora-gui.app"
    find "${target_root}" -name '._*' -delete 2>/dev/null || true
fi

echo "llavon-ime: uninstalled."
exit 0
