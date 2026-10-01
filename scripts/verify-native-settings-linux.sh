#!/usr/bin/env bash
# Ubuntu/X11 fixture: run inside a fresh 1280x1024 Xvfb and dbus-run-session.
# Requires the installed addon + native app, fcitx5-config-qt, xdotool,
# ImageMagick, openbox, trayer, xterm, x11-utils, and libatspi2.0-dev.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PREFIX="${1:?pass the isolated installation prefix}"
ARTIFACTS="${2:?pass an empty test artifacts directory}"
mkdir -p "$ARTIFACTS"
export XDG_CONFIG_HOME="$ARTIFACTS/config" XDG_STATE_HOME="$ARTIFACTS/state"
export FCITX_ADDON_DIRS="$PREFIX/lib/fcitx5:/usr/lib/$(gcc -dumpmachine)/fcitx5"
export FCITX_DATA_DIRS="$PREFIX/share/fcitx5:/usr/share/fcitx5"
export PATH="$PREFIX/bin:$PATH" QT_LINUX_ACCESSIBILITY_ALWAYS_ON=1 LC_ALL=C.UTF-8
mkdir -p "$XDG_CONFIG_HOME/fcitx5" "$XDG_STATE_HOME"
mkdir -p "$XDG_CONFIG_HOME/fcitx5/conf"
printf '# Legacy configuration\nThreadCount=7\nFutureSetting=keep\n' > "$XDG_CONFIG_HOME/fcitx5/conf/llavon-ime.conf"
g++ -std=c++23 -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
    "$ROOT/ime-unix-service/tests/native_entry_probe.cpp" \
    $(pkg-config --cflags --libs atspi-2 gobject-2.0) -o "$ARTIFACTS/probe"
probe() { "$ARTIFACTS/probe" "$@"; }
capture() { import -window root "$ARTIFACTS/$1.png"; }
settings() { probe expect '鍵盤、選字與推論，依照你的習慣調整。'; }
gui_pid() { pgrep -n -f "^$PREFIX/bin/llavon-ime-lora-gui"; }
trap 'kill ${fcitx_pid:-} ${config_pid:-} ${gui:-} ${wm_pid:-} ${tray_pid:-} ${xterm_pid:-} 2>/dev/null || true' EXIT
cat > "$XDG_CONFIG_HOME/fcitx5/profile" <<'PROFILE'
[Groups/0]
Name=Default
Default Layout=us
DefaultIM=llavon-ime
[Groups/0/Items/0]
Name=keyboard-us
Layout=
[Groups/0/Items/1]
Name=llavon-ime
Layout=
[GroupOrder]
0=Default
PROFILE
fcitx5 --disable all --enable dbus,llavon-ime,keyboard,xcb,xim,classicui > "$ARTIFACTS/fcitx.log" 2>&1 &
fcitx_pid=$!
sleep 2
gdbus call --session --dest org.fcitx.Fcitx5 --object-path /controller \
    --method org.fcitx.Fcitx.Controller1.GetConfig fcitx://config/addon/llavon-ime > "$ARTIFACTS/schema.txt"
grep -q llavon-ime-settings "$ARTIFACTS/schema.txt"
grep -q llavon-ime-phrases "$ARTIFACTS/schema.txt"
if grep -Eq "ThreadCount|BopomofoKeyboardLayout|AccessibilityStatus|Entries" "$ARTIFACTS/schema.txt"; then
    echo 'Old platform settings fields are still exposed' >&2; exit 1
fi
fcitx5-config-qt > "$ARTIFACTS/configtool.log" 2>&1 &
config_pid=$!
sleep 2
# The fixture's original input-method list/gear is not exposed with unique
# action names. After this click, use the real accessible controls by name.
xdotool mousemove 100 129 click 1
xdotool mousemove 400 317 click 1
probe expect '開啟拉風設定與個人化:'
capture original-config
probe click Configure 1
settings; gui=$(gui_pid)
echo 'PASS original input-method list/gear cold launches settings'
capture launched-settings
probe click Configure 2
probe expect '讓常用詞，優先用你想要的寫法。'
probe click Configure 3
probe expect '留下值得學習的句子，讓選字更懂你。'
probe click Configure 1
settings
echo 'PASS original phrase/training/settings buttons select their pages'
probe set '執行緒數' 3
probe click '儲存並套用'
sleep 1
gdbus call --session --dest org.fcitx.Fcitx5 --object-path /controller \
    --method org.fcitx.Fcitx.Controller1.GetConfig fcitx://config/addon/llavon-ime > "$ARTIFACTS/launcher.txt"
gdbus call --session --dest org.fcitx.Fcitx5 --object-path /llavon/update \
    --method org.llavon.IME.Update1.Status > "$ARTIFACTS/reloaded.txt"
grep -q '"thread_count":3' "$ARTIFACTS/reloaded.txt"
grep -q 'FutureSetting=keep' "$XDG_CONFIG_HOME/fcitx5/conf/llavon-ime.conf"
echo 'PASS app save updates running addon without manual reload'
cp "$XDG_CONFIG_HOME/fcitx5/conf/llavon-ime.conf" "$ARTIFACTS/config-before-old-submit.txt"
gdbus call --session --dest org.fcitx.Fcitx5 --object-path /controller \
    --method org.fcitx.Fcitx.Controller1.SetConfig fcitx://config/addon/llavon-ime "<{'ThreadCount': <'99'>}>"
cmp "$ARTIFACTS/config-before-old-submit.txt" "$XDG_CONFIG_HOME/fcitx5/conf/llavon-ime.conf"
echo 'PASS obsolete platform form submissions cannot overwrite app-owned settings'
llavon-ime-lora-gui --page about
probe expect '目前執行版本：0.2.1'
capture migrated-host-status
llavon-ime-settings
llavon-ime-phrases
probe expect '讓常用詞，優先用你想要的寫法。'
test "$(gui_pid)" = "$gui"
echo 'PASS installed wrappers reuse the existing process'
openbox > "$ARTIFACTS/openbox.log" 2>&1 &
wm_pid=$!
trayer --edge top --align right --widthtype pixel --width 64 --height 32 > "$ARTIFACTS/tray.log" 2>&1 &
tray_pid=$!
XMODIFIERS=@im=fcitx xterm -name entry-client -xrm 'XTerm*inputMethod: fcitx' > "$ARTIFACTS/xterm.log" 2>&1 &
xterm_pid=$!
sleep 2
client=$(xdotool search --onlyvisible --class XTerm | head -1)
xdotool windowactivate --sync "$client"
xdotool mousemove --window "$client" 80 80 click 1
fcitx5-remote -s llavon-ime
fcitx5-remote -o
sleep 1
fcitx5-remote -n > "$ARTIFACTS/active-inputmethod.txt"
grep -q llavon-ime "$ARTIFACTS/active-inputmethod.txt"
tray_open() { xdotool mousemove 1258 16 click 3; sleep 0.3; }
tray_select() { xdotool mousemove 1140 105 click 1; settings; }
tray_open; capture original-tray; tray_select
test "$(gui_pid)" = "$gui"
window=
# A short-lived warm launcher can disappear during XQueryTree; retry the
# lookup, then use the stable primary window ID for the minimize assertion.
for attempt in {1..20}; do
    if windows=$(xdotool search --all --onlyvisible --pid "$gui" --name '^拉風 ·' 2>/dev/null); then
        window=${windows%%$'\n'*}; break
    fi
    sleep 0.1
done
test -n "$window"
wait_map() {
    for attempt in {1..30}; do
        if xwininfo -id "$window" | grep -q "Map State: $1"; then return 0; fi
        sleep 0.1
    done
    echo "Window did not reach $1" >&2; return 1
}
xdotool windowminimize "$window"
wait_map IsUnMapped
tray_open; capture minimized-tray; tray_select
wait_map IsViewable
echo 'PASS original tray action switches page and restores minimized window'
kill "$gui"; sleep 1
tray_open; tray_select; gui=$(gui_pid)
echo 'PASS original tray action cold launches after termination'
llavon-ime-phrases
probe expect '讓常用詞，優先用你想要的寫法。'
gio launch "$PREFIX/share/applications/llavon-ime-lora.desktop"
settings; test "$(gui_pid)" = "$gui"
echo 'PASS desktop application entry reuses the existing process'
kill "$fcitx_pid"; wait "$fcitx_pid" || true
cmp "$ARTIFACTS/config-before-old-submit.txt" "$XDG_CONFIG_HOME/fcitx5/conf/llavon-ime.conf"
echo 'PASS Fcitx shutdown cannot truncate or rewrite app-owned configuration'
