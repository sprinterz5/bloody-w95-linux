#!/usr/bin/env bash
# Run a Windows program in the Bloody Wine prefix with hidraw packet logging.
#   ./bloody-wine.sh setup              create the prefix and enable hidraw for 09da:1434
#   ./bloody-wine.sh install <exe>      run the Bloody installer
#   ./bloody-wine.sh run [exe]          start Bloody (default: Program Files (x86)/BloodyWorkShop8)
#   ./bloody-wine.sh mark <name>        put a marker into the log (between actions)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
export WINEPREFIX="${WINEPREFIX:-$HOME/.local/share/wineprefixes/bloody}"
export WINEDEBUG="${WINEDEBUG:--all}"
LOG="$HERE/captures/session.log"
mkdir -p "$HERE/captures"

sniff() {
    # Bloody once flashed the mouse into its bootloader: never talk to it unguarded.
    [ -f "$HERE/hidsniff.so" ] || { echo "hidsniff.so missing — refusing to run"; exit 1; }
    [ "${HIDSNIFF_GUARD:-1}" != 0 ] || { echo "HIDSNIFF_GUARD=0 is not allowed here"; exit 1; }
    export HIDSNIFF_LOG="$LOG" LD_PRELOAD="$HERE/hidsniff.so"
    # under the preload too: starting wine may itself launch autostart entries
    wine reg delete 'HKCU\Software\Microsoft\Windows\CurrentVersion\Run' /v Bloody2 /f >/dev/null 2>&1 || true
    "$@"
}

case "${1:-}" in
setup)
    wineboot -i
    wine reg add 'HKLM\System\CurrentControlSet\Services\WineBus\Devices\09da/1434' /v Hidraw /t REG_DWORD /d 1 /f
    wineserver -w
    ;;
install)
    wineserver -k || true
    sniff wine "$2"
    ;;
run)
    exe="${2:-$WINEPREFIX/drive_c/Program Files (x86)/BloodyWorkShop8/BloodyWorkShop8/BloodyWorkShop8.exe}"
    [ -f "$exe" ] || { echo "Bloody executable not found: $exe"; exit 1; }
    echo "running $exe"
    wineserver -k || true
    echo "logging to $LOG"
    sniff wine "$exe"
    ;;
mark)
    echo "$(date +%s.%N) MARK ${2:?name}" >> "$LOG"
    ;;
*)
    sed -n '2,6p' "$0"; exit 1
    ;;
esac
