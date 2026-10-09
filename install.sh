#!/usr/bin/env bash
# Installs fs25vr (stereoscopic 6DOF VR for Farming Simulator 25) for the Steam version under
# Proton on Linux. The Linux counterpart of install.ps1:
#
#   Copies the bridge (dinput8.dll, openxr_loader.dll, fs25vr.ini) into <game>/x64 and the
#   FS25_VR mod into the mods folder inside the game's Proton prefix (honours the in-game
#   mods-folder override). Works from a release package (x64/ + mod/ folders) or from a source
#   checkout after the cross build (build-mingw/).
#
# Usage:
#   bash install.sh                        install
#   bash install.sh --resolution 2568x2584 also switch the game to a 2568x2584 window (vsync off)
#   bash install.sh --auto-resolution      ... with the size fs25vr.log recommends for your headset
#   bash install.sh --uninstall            remove the bridge and the mod, restore game.xml
#   bash install.sh --game-dir DIR         game folder, if it is not found automatically
#
# The game must be started with these Steam launch options (Properties > Launch options):
#   WINEDLLOVERRIDES="dinput8=n,b" PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1 %command%
set -euo pipefail

APPID=2300320
LAUNCH_OPTIONS='WINEDLLOVERRIDES="dinput8=n,b" PRESSURE_VESSEL_IMPORT_OPENXR_1_RUNTIMES=1 %command%'
root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

game_dir=""
uninstall=0
resolution=""
auto_resolution=0

die() { echo "Error: $*" >&2; exit 1; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --game-dir) game_dir="${2:-}"; shift 2 ;;
        --resolution) resolution="${2:-}"; shift 2 ;;
        --auto-resolution) auto_resolution=1; shift ;;
        --uninstall) uninstall=1; shift ;;
        -h|--help) awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "$0"; exit 0 ;;
        *) die "unknown option: $1 (see --help)" ;;
    esac
done
if [[ -n "$resolution" && ! "$resolution" =~ ^[0-9]+x[0-9]+$ ]]; then
    die "--resolution expects WIDTHxHEIGHT, e.g. 2568x2584"
fi

# --- find the game ------------------------------------------------------------------------------

# Steam library folders (native and Flatpak Steam), one per line.
steam_libraries() {
    local s vdf
    for s in "$HOME/.local/share/Steam" "$HOME/.steam/steam" "$HOME/.steam/root" \
             "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam"; do
        [[ -d "$s/steamapps" ]] || continue
        echo "$s"
        vdf="$s/steamapps/libraryfolders.vdf"
        [[ -f "$vdf" ]] && sed -n 's/^[[:space:]]*"path"[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' "$vdf"
    done | while read -r lib; do
        [[ -d "$lib" ]] && realpath "$lib"
    done | awk '!seen[$0]++'
}

is_game_dir() { [[ -n "$1" && -f "$1/x64/FarmingSimulator2025Game.exe" ]]; }

# read the list once: leaving a loop over a still-running pipeline early makes it print broken pipes
mapfile -t libraries < <(steam_libraries)

if [[ -z "$game_dir" ]]; then
    for lib in "${libraries[@]}"; do
        if is_game_dir "$lib/steamapps/common/Farming Simulator 25"; then
            game_dir="$lib/steamapps/common/Farming Simulator 25"
            break
        fi
    done
fi
while ! is_game_dir "$game_dir"; do
    if [[ -n "$game_dir" ]]; then
        echo "Farming Simulator 25 was not found in: $game_dir"
    else
        echo "Could not find Farming Simulator 25 automatically."
    fi
    echo "Paste the game's install folder (the one that contains x64/FarmingSimulator2025Game.exe)"
    read -r -p "and press Enter, or just press Enter to cancel: " game_dir || true
    [[ -n "$game_dir" ]] || die "cancelled - nothing was installed"
    game_dir="${game_dir%/}"
done
game_dir="$(realpath "$game_dir")"
x64="$game_dir/x64"

# The game's process is the exe itself (argv[0], a Windows or Unix path to it). Matching whole command
# lines (pgrep -f) would also find any shell command that merely mentions the exe's name.
game_running() {
    local f a0
    for f in /proc/[0-9]*/cmdline; do
        IFS= read -r -d '' a0 < "$f" 2>/dev/null || continue
        [[ "${a0##*[\\/]}" =~ ^FarmingSimulator2025(Game)?\.exe$ ]] && return 0
    done
    return 1
}

if game_running; then
    die "Farming Simulator 25 is running. Close the game first, then try again."
fi

# --- the game's Proton prefix -------------------------------------------------------------------

# compatdata normally sits in the same library as the game; otherwise look through all libraries
prefix=""
for c in "$game_dir/../../compatdata/$APPID/pfx" "${libraries[@]/%//steamapps/compatdata/$APPID/pfx}"; do
    if [[ -d "$c/drive_c" ]]; then
        prefix="$(realpath "$c")"
        break
    fi
done
[[ -n "$prefix" ]] || die "the game's Proton prefix (compatdata/$APPID) was not found. Start the game once with Proton, then try again."
profile="$prefix/drive_c/users/steamuser/Documents/My Games/FarmingSimulator2025"

# Windows path in the prefix ("C:/Temp/mods") -> Linux path (via dosdevices, like Wine resolves it)
wine_path() {
    local p="${1//\\//}" letter rest
    if [[ "$p" =~ ^([A-Za-z]):(.*)$ ]]; then
        letter="${BASH_REMATCH[1],,}"
        rest="${BASH_REMATCH[2]}"
        echo "$prefix/dosdevices/$letter:$rest"
    else
        echo "$p"
    fi
}

mods="$profile/mods"
settings="$profile/gameSettings.xml"
if [[ -f "$settings" ]]; then
    override="$(grep -o '<modsDirectoryOverride[^>]*active="true"[^>]*>' "$settings" | head -n1 || true)"
    dir="$(sed -n 's/.*directory="\([^"]*\)".*/\1/p' <<< "$override")"
    [[ -n "$dir" ]] && mods="$(wine_path "$dir")"
fi
zip="$mods/FS25_VR.zip"
game_xml="$profile/game.xml"
backup="$game_xml.fs25vr-backup"

# --- uninstall ----------------------------------------------------------------------------------

if [[ $uninstall -eq 1 ]]; then
    for f in dinput8.dll dinput8.pdb openxr_loader.dll fs25vr.ini fs25vr.log fs25vr_profile.csv; do
        rm -f "$x64/$f"
    done
    rm -f "$zip"
    if [[ -f "$backup" ]]; then
        cp -f "$backup" "$game_xml" && rm -f "$backup"
        echo "game.xml restored"
    fi
    echo "fs25vr removed from $game_dir"
    echo "Remove the launch options in Steam as well (Properties > Launch options)."
    exit 0
fi

# --- install ------------------------------------------------------------------------------------

# Where the files come from: a release package or a source checkout.
if [[ -f "$root/x64/dinput8.dll" ]]; then
    release=1
    dll="$root/x64/dinput8.dll"
    loader="$root/x64/openxr_loader.dll"
    ini="$root/x64/fs25vr.ini"
else
    release=0
    dll="$root/build-mingw/dinput8.dll"
    loader="$root/third_party/openxr/x64/bin/openxr_loader.dll"
    ini="$root/dist/fs25vr.ini"
    [[ -f "$dll" ]] || die "build first (see README, 'Cross build on Linux'), or run this from a release package"
    [[ -f "$loader" ]] || die "openxr_loader.dll not found at $loader (copy it there from a release package, see README 'Cross build on Linux')"
fi

cp -f "$dll" "$loader" "$x64/"
[[ -f "$x64/fs25vr.ini" ]] || cp "$ini" "$x64/"   # keep the player's settings

mkdir -p "$mods"
rm -f "$zip"
if [[ $release -eq 1 ]]; then
    cp "$root/mod/FS25_VR.zip" "$zip"
else
    command -v python3 >/dev/null || die "python3 is needed to pack the mod from a source checkout"
    # entry names relative to the mod folder, with forward slashes (what the game expects)
    python3 - "$root/mod/FS25_VR" "$zip" <<'PY'
import os, sys, zipfile
src, out = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for d, _, files in os.walk(src):
        for f in sorted(files):
            path = os.path.join(d, f)
            z.write(path, os.path.relpath(path, src).replace(os.sep, "/"))
PY
fi

# --- render size --------------------------------------------------------------------------------

if [[ $auto_resolution -eq 1 ]]; then
    log="$x64/fs25vr.log"
    size="$( [[ -f "$log" ]] && grep -o 'recommended render size for the centred frustum: [0-9]*x[0-9]*' "$log" | tail -n1 | grep -o '[0-9]*x[0-9]*$' || true)"
    [[ -n "$size" ]] || die "no recommended size yet. Play once in VR (load a savegame with the headset on), quit the game, then run this again."
    resolution="$size"
    echo "Recommended size for your headset: $resolution"
fi

if [[ -n "$resolution" ]]; then
    [[ -f "$game_xml" ]] || die "game.xml not found at $game_xml. Start the game once, then try again."
    [[ -f "$backup" ]] || cp "$game_xml" "$backup"
    w="${resolution%x*}"
    h="${resolution#*x}"
    sed -i -e "s|<width>[0-9]*</width>|<width>$w</width>|" \
           -e "s|<height>[0-9]*</height>|<height>$h</height>|" \
           -e "s|<fullscreenMode>[^<]*</fullscreenMode>|<fullscreenMode>windowed</fullscreenMode>|" \
           -e "s|\(<vsync[^>]*>\)[^<]*</vsync>|\1false</vsync>|" \
           -e "s|<renderer>[^<]*</renderer>|<renderer>D3D_12</renderer>|" "$game_xml"
    echo "game.xml: windowed ${w}x${h}, vsync off, D3D12 (backup: $backup)"
fi

echo "fs25vr installed:"
echo "  $x64/dinput8.dll, openxr_loader.dll, fs25vr.ini"
echo "  $zip  (enable 'VR (OpenXR, stereoscopic 6DOF)' when starting a savegame)"
echo
echo "Set these launch options for the game in Steam (Properties > Launch options):"
echo "  $LAUNCH_OPTIONS"
echo "Start your OpenXR runtime (e.g. the WiVRn server) and connect the headset before the game."
