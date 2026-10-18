# MGSV DLSS changelog

## 1.0.0

Initial release.

- Replace the game's FXAA with NVIDIA DLAA, or upscale with DLSS Quality, Balanced, Performance,
  Ultra Performance or a custom render scale. The game renders its 3D scene at the lower
  resolution itself; the HUD, menus and subtitles stay at full resolution.
- Add the camera jitter and motion vectors DLSS needs, which the game has no use for itself.
- Give characters motion vectors whatever the game's motion blur setting, and leave the blur out
  while DLSS runs, so it does not smear the DLSS image.
- Keep DLSS running with the game's post processing turned down, which would otherwise turn off the
  FXAA that DLSS replaces.
- Keep texture detail in the upscaling modes with a matching texture detail bias.
- Add an **Unlock frame rate** option that lifts the game's 60 fps cap, in any mode.
- Let the game start under ReShade: on its own it closes at start-up under any Direct3D wrapper.
- Configure the mode, DLSS preset, sharpening (AMD FidelityFX RCAS) and texture detail bias in the
  ReShade overlay. Settings are saved beside the add-on, with a log for troubleshooting.
- Target the Steam release of the game, version 1.0.15.4 (title screen "Ver. 1.15"), on NVIDIA RTX
  GPUs.
