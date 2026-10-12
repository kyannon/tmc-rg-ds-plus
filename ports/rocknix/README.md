# Minish Cap — Dual Screen for ROCKNIX

The dual-screen Minish Cap port, built for ROCKNIX on the **Anbernic RG DS
Plus** (and the original **RG DS**). The game runs on the top screen. The
bottom touchscreen becomes the live panel: map, dungeon automap, quest
status, tap-to-equip items, and settings.

This is the same panel the Android build draws for the AYN Thor. It is
rendered natively and decoded from your own ROM as the game runs. No game
data ships with this port.

## Install

1. Copy **`Minish Cap DS.sh`** and the **`minishcapds/`** folder into
   `/storage/roms/ports/`. You can do this over the network share or on the
   SD card's `roms/ports`.
2. Put your own *Minish Cap* ROM in `minishcapds/` as **`baserom.gba`**.
   - USA ROM SHA-1: `b4bd50e4131b027c334547b4524e2dbbd4227130`.
   - EU (`baserom_eu.gba`) and JP (`baserom_jp.gba`) also load, but text,
     menus and RNG follow the USA build in places. See the main README.
   - If the folder holds exactly one `.gba`, the launcher renames it for you.
3. Refresh the game list in EmulationStation. Launch **Minish Cap DS** from
   **Ports**.

The first launch extracts the game's assets from the ROM, which takes a
minute on the RK3568. Later launches start straight away.

## Using it

- **Buttons** play the game as on a GBA. Start opens the in-game menu.
- **Bottom screen** (stylus or finger):
  - **Tap** an item to equip it to A.
  - **Press and hold** an item to equip it to B.
  - Tap the A/B ring first to choose the slot, then tap the item.
  - On the map, ZOOM toggles the whole kingdom. Tap a region to open its
    enlarged map.
  - The settings tab can hide the top HUD, toggle widescreen, set the volume,
    and open the port menu.
- **Exit**: use your usual ROCKNIX exit combo. With PortMaster installed,
  Select + Start works too. Save in-game first (or enable autosave in
  settings).

Saves (`tmc.sav`), quick saves and `config.json` live in `minishcapds/`.

## Troubleshooting

The launcher writes `minishcapds/log.txt` on every run. Look for the
`[launcher]` and `[second_screen]` lines.

| Symptom | Try |
|---|---|
| Panel on the wrong screen, or both on one screen | On the RG DS / RG DS Plus the launcher puts the game on `DSI-2` and the panel on `DSI-1` (the touch panel). Check `top=… bottom=…` in the log; set `TMC_TOP_OUTPUT` and `TMC_BOTTOM_OUTPUT` to override. |
| Bottom screen stays black | Look for `[second_screen] panel window …` in the log. If it says "only one display", sway has the bottom output disabled. Enable it in ROCKNIX's display settings. |
| Taps do nothing | Check the `[second_screen] touch:` log line. If no touchscreen matched, run `ls -l /sys/class/input/event*/device` and set `TMC_PANEL_TOUCH_MATCH` (or `TMC_PANEL_TOUCH=/dev/input/eventN`) at the top of the launcher. |
| Taps land in the wrong place | Set `TMC_PANEL_TOUCH_ROTATE=90/180/270`, `TMC_PANEL_TOUCH_FLIPX=1` or `TMC_PANEL_TOUCH_FLIPY=1` in the launcher. |
| Game below 60 FPS | `log.txt` always has `[perf] … FPS / … TPS` every second and a `[second_screen]` panel line every 10 s. For more detail, set `TMC_PROFILE=1` (frame-phase timings every 2 s) and `TMC_SAMPLE=1` (the functions the game thread spends its time in, every 10 s). To check whether the panel is involved, run once with `TMC_PANEL=0`, which disables it. |
| Audio crackles | Raise the buffer with `TMC_AUDIO_FRAMES=3072` (default 2048, about 43 ms). |
| Battery life matters more than frame rate | `TMC_GOVERNOR=keep` leaves ROCKNIX's own CPU/GPU governor setting in charge instead of `performance`. |

To set any of these, add a line such as `export TMC_PANEL_SCALE=2` near the
top of `Minish Cap DS.sh`.

## How it works

- **Screens.** On the RG DS family the launcher uses `DSI-2` as the top
  screen and `DSI-1`, the touch panel, as the bottom. On other devices it
  takes the output sway has focused as the top. It powers the bottom output
  on if ROCKNIX left it off, then tells SDL to list the top screen first,
  so the game opens fullscreen there and the panel takes the bottom one.
- **Panel.** A second SDL window, painted 20 times a second on its own
  thread from a snapshot of the game state.
- **Touch.** The bottom touchscreen is read directly from evdev and grabbed
  while the game runs (`fe5e0000.i2c`, the RG DS family's bottom panel).
  Taps work regardless of how sway maps the touch device. The device returns
  to sway when the game exits.
- **Performance.** The RK3568 has four in-order cores shared by the music
  synth, the PPU and the panel. The launcher pins CPU, memory and GPU
  governors to `performance` while the game runs and restores them on exit.
  The music synth runs on a realtime audio thread with the cheaper LINEAR
  resampler. The panel paints at low priority, uploads only the rows that
  changed, and spreads each upload over three game frames. While the game
  runs below 60 FPS the panel halves its refresh to 10 Hz
  (`TMC_PANEL_ADAPTIVE=0` keeps it at 20). Widescreen is off by default:
  the top screen is 4:3, so it only letterboxes the picture smaller at
  a 60% higher render cost. It can still be turned on in settings.
- **Asset lookups.** The game checks on almost every memory copy whether a
  pointer lies in an asset loaded from the ROM. Those checks are a binary
  search over a sorted index; a linear scan here was what held the RK3568
  below 60 FPS.
- **Libraries.** The port links against ROCKNIX's own SDL3. It bundles only
  `libgomp.so.1`. Built against glibc 2.27, so it runs on any current
  ROCKNIX.

Source and build script: `ports/rocknix/` and
`port/port_second_screen_linux.c` in the repository. Licensed GPL-3.0; see
`LICENSE` and `THIRD-PARTY-LICENSES.md`.
