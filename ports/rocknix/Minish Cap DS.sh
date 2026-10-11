#!/bin/bash
# Minish Cap — Dual Screen, ROCKNIX launcher (Anbernic RG DS Plus / RG DS).
#
# Game on the top screen, the live map / quest / items panel on the bottom
# touchscreen. Drop this script and the minishcapds/ folder into your ports
# folder (/storage/roms/ports), put your own USA ROM in minishcapds/ as
# baserom.gba, and launch from Ports.
#
# What this script does around the game:
#   - finds the two sway outputs (focused one = top screen) and powers the
#     bottom one on if ROCKNIX left it off; restores it on exit
#   - tells SDL which output is which (SDL_VIDEO_DISPLAY_PRIORITY), so the
#     game opens fullscreen on the top screen and the panel on the bottom
#   - double-checks placement through swaymsg once both windows exist
#   - starts gptokeyb (when PortMaster is installed) for the Select+Start
#     exit combo
# Touch needs nothing from sway: the game reads the bottom touchscreen
# directly (see TMC_PANEL_TOUCH* in port/port_second_screen_linux.c).

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}
if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi

if [ -f "$controlfolder/control.txt" ]; then
  # shellcheck disable=SC1091
  source "$controlfolder/control.txt"
  get_controls
fi

# ROCKNIX's profile carries WAYLAND_DISPLAY / SWAYSOCK / SDL_VIDEODRIVER;
# ports normally inherit it, but be sure when run from a bare shell.
if [ -z "$SWAYSOCK" ] && [ -f /etc/profile ]; then
  # shellcheck disable=SC1091
  . /etc/profile
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
GAMEDIR="$SCRIPT_DIR/minishcapds"
cd "$GAMEDIR" || exit 1
exec > >(tee "$GAMEDIR/log.txt") 2>&1
echo "[launcher] $(date) gamedir=$GAMEDIR device=${QUIRK_DEVICE:-unknown}"

# ---- ROM -------------------------------------------------------------------
# The game looks for baserom.gba (USA), baserom_eu.gba or baserom_jp.gba and
# checks the hash itself. A single, differently named .gba gets renamed.
if [ ! -f baserom.gba ] && [ ! -f baserom_eu.gba ] && [ ! -f baserom_jp.gba ]; then
  shopt -s nullglob
  roms=(*.gba)
  shopt -u nullglob
  if [ "${#roms[@]}" -eq 1 ]; then
    echo "[launcher] renaming '${roms[0]}' to baserom.gba"
    mv -- "${roms[0]}" baserom.gba
  else
    echo "[launcher] no ROM found: copy your Minish Cap (USA) ROM to $GAMEDIR/baserom.gba"
  fi
fi

# ---- Screens ---------------------------------------------------------------
TOP=""
BOTTOM=""
BOTTOM_ENABLED_BY_US=0
BOTTOM_POWERED_BY_US=0

if command -v swaymsg >/dev/null 2>&1 && command -v jq >/dev/null 2>&1 && swaymsg -t get_version >/dev/null 2>&1; then
  outs=$(swaymsg -t get_outputs -r)
  TOP=$(echo "$outs" | jq -r '[.[] | select(.focused)][0].name // empty')
  [ -z "$TOP" ] && TOP=$(echo "$outs" | jq -r '.[0].name // empty')
  BOTTOM=$(echo "$outs" | jq -r --arg t "$TOP" '[.[] | select(.name != $t)][0].name // empty')

  if [ -n "$BOTTOM" ]; then
    active=$(echo "$outs" | jq -r --arg b "$BOTTOM" '.[] | select(.name == $b) | .active')
    power=$(echo "$outs" | jq -r --arg b "$BOTTOM" '.[] | select(.name == $b) | if has("power") then .power else .dpms end')
    if [ "$active" = "false" ]; then
      swaymsg output "$BOTTOM" enable >/dev/null && BOTTOM_ENABLED_BY_US=1
    fi
    if [ "$power" = "false" ]; then
      swaymsg output "$BOTTOM" power on >/dev/null && BOTTOM_POWERED_BY_US=1
    fi
    [ $BOTTOM_ENABLED_BY_US -eq 1 ] || [ $BOTTOM_POWERED_BY_US -eq 1 ] && sleep 0.5

    # Top screen first: SDL's primary display (the game window) is the first
    # listed, and the panel takes the next one.
    export SDL_VIDEO_DISPLAY_PRIORITY="$TOP,$BOTTOM"
    export TMC_PANEL_DISPLAY=1
  fi
  echo "[launcher] top=$TOP bottom=${BOTTOM:-none}"
else
  echo "[launcher] sway not reachable; leaving display placement to SDL"
fi

# Sway normally honors the fullscreen output SDL asks for. If a window still
# lands on the wrong screen (or both share one), move them once.
place_windows() {
  local tree panel_out game_out i
  for i in $(seq 1 60); do
    sleep 0.5
    tree=$(swaymsg -t get_tree -r 2>/dev/null) || continue
    panel_out=$(echo "$tree" | jq -r '.. | objects | select(.type? == "output") | .name as $o
        | .. | objects | select(.name? == "Minish Cap Panel") | $o' | head -n1)
    [ -z "$panel_out" ] && continue
    game_out=$(echo "$tree" | jq -r '.. | objects | select(.type? == "output") | .name as $o
        | .. | objects | select((.name? // "") | startswith("The Minish Cap")) | $o' | head -n1)
    echo "[launcher] windows: game on ${game_out:-?}, panel on $panel_out"
    if [ "$panel_out" != "$BOTTOM" ]; then
      swaymsg "[title=\"^Minish Cap Panel\$\"] fullscreen disable, move container to output $BOTTOM, fullscreen enable" >/dev/null
    fi
    if [ -n "$game_out" ] && [ "$game_out" != "$TOP" ]; then
      swaymsg "[title=\"^The Minish Cap\"] fullscreen disable, move container to output $TOP, fullscreen enable" >/dev/null
    fi
    swaymsg "[title=\"^The Minish Cap\"] focus" >/dev/null
    return 0
  done
}

PLACER=""
if [ -n "$BOTTOM" ]; then
  place_windows &
  PLACER=$!
fi

# ---- Run -------------------------------------------------------------------
export LD_LIBRARY_PATH="$GAMEDIR/libs.aarch64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
[ -n "$sdl_controllerconfig" ] && export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"

if [ -n "$GPTOKEYB" ]; then
  $GPTOKEYB "tmc_pc" &
fi

chmod +x ./tmc_pc
./tmc_pc

# ---- Restore ---------------------------------------------------------------
[ -n "$PLACER" ] && kill "$PLACER" 2>/dev/null
if [ -n "$BOTTOM" ]; then
  [ $BOTTOM_POWERED_BY_US -eq 1 ] && swaymsg output "$BOTTOM" power off >/dev/null
  [ $BOTTOM_ENABLED_BY_US -eq 1 ] && swaymsg output "$BOTTOM" disable >/dev/null
fi
if type pm_finish >/dev/null 2>&1; then
  pm_finish
elif [ -n "$GPTOKEYB" ]; then
  pkill -f gptokeyb 2>/dev/null
fi
