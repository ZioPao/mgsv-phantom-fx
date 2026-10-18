# MGSV DLSS

Adds NVIDIA DLSS to Metal Gear Solid V: The Phantom Pain: DLAA in place of the game's FXAA, or
upscaling from a lower render resolution. It can also lift the game's 60 fps cap.

## Requirements

- Metal Gear Solid V: The Phantom Pain, Steam version **1.0.15.4** (title screen "Ver. 1.15").
  Other versions are not supported.
- An **NVIDIA RTX GPU** and an up-to-date driver.
- [ReShade 6.8+ with full add-on support](https://reshade.me/#download).

## Install

1. Install ReShade for `mgsvtpp.exe` (in `steamapps\common\MGS_TPP`), choosing **DirectX 10/11/12**.
   No effect packs are needed.
2. Extract the ZIP into the game folder. `MGSVDLSS.addon64` and `nvngx_dlss.dll` must sit beside
   `mgsvtpp.exe`.
3. Start the game, open the ReShade overlay (Home by default) and find **MGSV DLSS**.
   **DLAA is on by default.**

ReShade alone cannot run this game: the game closes at start-up under any Direct3D wrapper. The
add-on gets it past that check, so keep both installed together.

## Use

The overlay has the mode, the DLSS preset, sharpening, the texture detail bias and **Unlock frame
rate**. Settings are saved in `MGSVDLSS.ini` beside the add-on.

| Mode | Renders the 3D scene at |
| --- | --- |
| Off | Native resolution, with the game's FXAA |
| DLAA | Native resolution |
| Quality | About 67% of the output resolution per axis |
| Balanced | About 58% |
| Performance | 50% |
| Ultra Performance | 33% |
| Custom | The **Render scale** slider, 33% to 100%; applies when you release it |

- **Upscaling modes:** only the 3D scene renders smaller; the HUD, menus and subtitles stay at full
  resolution. The section's first line shows the render and output resolution. Changing the
  mode rebuilds the game's render targets, which takes a moment, like changing the resolution.
  Keep the game's own resolution at your display's: it is the output resolution.
- **DLSS preset:** Default lets DLSS choose. K and J are transformer models (J ghosts less); M and
  L are made for Performance and Ultra Performance.
- **Sharpening:** AMD FidelityFX RCAS on the DLSS image, 33% by default. 0% turns it off.
- **Texture detail bias:** textures get a level-of-detail bias that matches the render resolution,
  so they keep their detail. The slider adds to it when you release it: lower is sharper but can
  shimmer.
- **Unlock frame rate:** lifts the 60 fps cap at once, in any mode, including Off. Game speed stays
  the same. It works like `framerate_control` `"Variable"` in the game's settings file, without
  editing that file. Turning it off gives the game's own setting back.

Where the game is limited by the CPU, for example at the 60 fps cap, upscaling lowers the GPU's load
and power draw instead of raising the frame rate.

## Notes

- **FXAA:** DLSS takes the place of the game's FXAA: it starts from the image FXAA would have
  smoothed and writes its result where FXAA's went. It runs at any post processing setting: the
  add-on keeps the game's FXAA pass on, even where the setting would turn it off.
- **Motion blur:** the game's setting does not matter while DLSS runs. The add-on has the game draw
  motion vectors for characters, which keeps them sharp, and leaves the blur out.
- **Off** leaves the game as it is: native resolution, FXAA and its own motion blur setting.

To **update**, close the game and replace the ZIP's files. Keep your INI to keep your settings.

To **uninstall**, delete `MGSVDLSS.addon64` and `nvngx_dlss.dll`, and uninstall ReShade too,
since ReShade alone cannot run the game. You can also remove `MGSVDLSS.ini`, `MGSVDLSS.log` and
the `MGSVDLSS` documentation folder.

## Problems

If the game closes by itself right after starting, the add-on could not get it past its start-up
check: check the game version, that ReShade has full add-on support, and that `MGSVDLSS.addon64`
sits beside `mgsvtpp.exe`. For DLSS errors, read the first line of the **MGSV DLSS** section and
check your GPU driver.

Report issues with `MGSVDLSS.log` and `ReShade.log` from the game folder, plus your GPU, driver,
resolution, window mode, DLSS mode and steps to reproduce.

## Technical reference

See [DEVELOPMENT.md](docs/DEVELOPMENT.md) for build instructions and how the add-on works.

## License

Code and documentation: Bustanity, MIT. NVIDIA DLSS, MinHook, ReShade, Dear ImGui and AMD
FidelityFX (the sharpening filter) retain their own licenses, included in the ZIP's `MGSVDLSS`
folder.
