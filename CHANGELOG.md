# Phantom FX changelog

## 1.3.0

- Add **Global illumination (GI-1.2)**: ray-traced indirect light after AMD's GI-1.2. Sunlight and
  skylight bounce between surfaces, also those off screen, and the sky only lights what it can
  reach. Replaces the game's baked probe light and SSAO, and GTVBAO and SSILVB while on. Needs a
  GPU that traces rays (DXR 1.1). Off by default.
- Add **Ray-traced shadows**: the sun's shadows from rays traced through the game's world, in
  place of the game's shadow map, soft shadows and sun contact shadows. They soften with distance
  as the sun's disc makes them and reach as far as you can see, where the game's end at about
  90 m. Leaves and grass keep the game's shadow map near you. Needs a GPU that traces rays
  (DXR 1.1). Off by default.
- Ray-traced shadows and global illumination move to their own overlay section, **Ray Tracing
  [EXPERIMENTAL]**.
- Add a **Character** swapper under Utility: play as Venom Snake, the avatar, a male or female
  soldier, Ocelot or Quiet, in any outfit the game has a model for. Soldiers can take any face.
- Remove **Double shadow resolution**: it cost 1.5 GB of video memory for no visible gain. The
  `double_shadow_resolution` line in `PhantomFX.ini` is now ignored.

## 1.2.0

- Add **Double shadow resolution**: the sun's shadow map at twice the width and height of the
  game's shadow quality (16384x16384 at Extra High), for sharper shadow edges and small shadows.
  Uses 768 MB more video memory. Off by default.
- Add **Haze** to the volumetric fog's physical look: from clear air to as thick as the game's own
  haze.
- Photo mode: weather and time of day changes now show while the game is paused.
- Fix the sandstorm's vignette sitting off-centre while upscaling.
- Diagnostics shows what each effect costs on the GPU: a table with a bar, milliseconds and share
  of the GPU frame for upscaling, the sun and lamp shadows, GTVBAO, SSILVB, the volumetric fog, the
  depth of field and HDR, each opening to its passes. "Show the costs over the game" keeps it in a
  corner with the overlay closed, and `PhantomFX-performance.txt` beside the log holds the same
  numbers. "Measure the real cost" times the GPU frame with everything on, then leaves out one
  effect at a time, so what the passes change in the game's own work counts too. Nothing is timed
  while the costs are not on screen.
- **Indirect lighting (SSILVB)** costs about a quarter of what it did: it now traces at half the
  render size and upscales the result along depth edges.
- **Ambient occlusion (GTVBAO)** and SSILVB read smaller copies of the depth for distant samples
  (XeGTAO's depth mips), which keeps them cheaper up close to walls and floors.
- **Lamp contact shadows** cost far less: each lamp now traces only the part of the screen it can
  light, instead of the whole screen.

## 1.1.0

- Rename the mod from MGSV DLSS to Phantom FX. Its files are now `PhantomFX.*`: delete
  `MGSVDLSS.addon64`, and rename `MGSVDLSS.ini` to `PhantomFX.ini` to keep your settings.
- Add NVIDIA DLSS Frame Generation: 2x to 6x, or Dynamic to reach the display's refresh rate. It
  runs with any DLSS mode, DLAA included, and turns on NVIDIA Reflex; the overlay shows the
  latency. The game then presents through Direct3D 12 by way of NVIDIA Streamline, so turning it
  on takes a restart. Needs an RTX 40 (2x) or RTX 50 GPU and Windows' hardware-accelerated GPU
  scheduling.
- With frame generation on, ReShade and its add-ons still see the game as Direct3D 11, so add-ons
  that follow the game's DLSS keep working.
- Add **AMD FSR** upscaling, chosen with the new **Upscaler** setting: FSR 4 on Radeon RX 7000 and
  9000, FSR 3.1 on other GPUs. It takes DLSS's place with the same modes at AMD's ratios, Native AA
  in place of DLAA, and the same sharpening.
- Add **FSR frame generation** (2x) on any GPU, with either upscaler. Like DLSS Frame Generation it
  takes a restart; it has no Reflex.
- Add **HDR10 output**: the sun, sky, lamps, fire and reflections keep their light beyond the game's
  white, up to the display's peak, while the rest looks as the game graded it. **Peak brightness**
  (the display's own by default) and **Paper white** in the overlay. Needs HDR on for the display in
  Windows; turning it on takes a restart. Off by default.
- Add **Soft shadows (PCSS)**: contact-hardening soft shadows from the sun's shadow map, sharp
  where an object meets the ground and softer further from it. **Sun size** in the overlay. Off by
  default; works in any mode and applies at once.
- Add **Sun contact shadows** and **Lamp contact shadows**: Bend Studio's screen-space shadows add
  contact shadows to the sun's shadow map (feet, rocks, grass) and give lamps, spot lights and
  flashlights short shadows of their own. The sun's are cheap; the lamps' cost one trace per light,
  so they are a separate toggle. Grass, bushes and leaves receive them but cast none, so clumps
  don't darken themselves; the shadow map still shadows them. Off by default; works in any mode and
  applies at once.
- Add **Ambient occlusion (GTVBAO)**: ground-truth visibility bitmask ambient occlusion in place of
  the game's SSAO, on the indirect light only, so it no longer darkens sunlight. Radius and strength
  in the overlay. Off (the game's own SSAO) by default; works in any mode and applies at once.
- Add **Indirect lighting (SSILVB)**: screen-space indirect lighting with visibility bitmask, one
  bounce of sunlight and lamplight between nearby surfaces, tinted by their colour. Radius and
  strength in the overlay. Off by default; works in any mode and applies at once.
- Add **Volumetric fog**: the game's own haze and fog with the sun shadowed in it, for light shafts
  through trees, buildings and dust. **Look** Physical draws the sky and the air as in nature
  (Hillaire's atmosphere, as in Unreal Engine) with height fog that follows the game's weather.
  Off by default; works in any mode and applies at once.
- Add **Depth of field**: a gather in place of the game's sprite-based depth of field, smooth at
  full resolution where the blur is slight, with blurred foreground spreading over what lies
  behind it and lights blurred before the tonemap. **Aperture** shapes out-of-focus lights with 5
  to 9 blades. Off (the game's own) by default; works in any mode and applies at once.
- The overlay is grouped into **Upscaling**, **Visuals**, **Utility** and **Diagnostics**, with
  each feature's settings indented under it.
- Sliders set away from their default get a **Reset** button.
- Add a photo mode on **F2**: the game freezes, the HUD hides, and a free camera flies with WASD,
  the mouse, Space and Ctrl (Shift for faster).
- **F3** hides the HUD, menus and subtitles at any time.
- Photo mode focuses on the centre of the screen. **f-stop** in the overlay's Utility section sets the
  blur, and **F4** turns it off and on.
- **F5** in photo mode takes a lens shot: the frozen scene rendered from points across the lens's
  opening and averaged, as a real lens forms its image. **Lens shot frames** sets how many.

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
