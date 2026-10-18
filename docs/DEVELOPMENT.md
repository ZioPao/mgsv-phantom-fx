# MGSV DLSS development

## Build

Requires Visual Studio 2022 (or Build Tools) with the C++ workload and CMake 3.25 or later.
From a x64 developer prompt:

```
cmake --preset release
cmake --build --preset release
```

CMake downloads MinHook, the DLSS SDK v310.9.1 headers, loader and `nvngx_dlss.dll`, and the
ReShade 6.8.0 add-on headers with the Dear ImGui header, each pinned by commit and checked by
SHA-256. The add-on is written to `build/release/MGSVDLSS.addon64` together with `nvngx_dlss.dll`.
It builds with `/W4 /WX`. The version comes from `project()` in `CMakeLists.txt`: it fills the
add-on's version resource (`src/version.rc.in`), which ReShade shows and logs, and the first line
of `MGSVDLSS.log`.

`cmake --install build/release --component addon --prefix <dir>` collects the add-on, the DLSS
DLL, player documentation and licenses for distribution. The two DLLs are at the install root;
documentation and licenses are in `MGSVDLSS/`.

## Test

```
ctest --test-dir build/release
```

This runs `tests/upscaler_tests.cpp`, which needs neither the game nor a GPU: settings (defaults,
round trip, clamping, invalid files and values), the mode maths (render scales, texture LOD bias)
and the camera maths (main-view recognition, jitter direction and units, reprojection, camera cuts,
jitter phases and the Halton sequence). Everything else is checked in the game.

## Package a release

From the repository root, after building and testing:

```
cpack --config build/release/CPackConfig.cmake -B build/release
```

This produces `build/release/MGSVDLSS-1.0.0.zip` and its `.sha256` checksum:

| Path in the ZIP | What |
| --- | --- |
| `MGSVDLSS.addon64`, `nvngx_dlss.dll` | The add-on and the DLSS model, beside `mgsvtpp.exe` |
| `MGSVDLSS/README.md`, `CHANGELOG.md`, `LICENSE` | Player documentation and the add-on's license |
| `MGSVDLSS/*-LICENSE.txt` | NVIDIA DLSS, MinHook, ReShade, Dear ImGui, AMD FidelityFX (RCAS) |

CPack packages only the `addon` component, so SDK headers, static libraries, tests, debug symbols
and local settings are left out. ReShade is installed separately by the player.

## Sources

| File | Purpose |
| --- | --- |
| `src/upscaler_addon.cpp` | ReShade add-on: exports, events, build line, MinHook set-up, NGX's wrapped context |
| `src/upscaler.cpp`, `src/upscaler.hpp` | DLSS in place of FXAA: pass recognition, jitter, frames, render size plan, NGX, composite, texture LOD bias |
| `src/render_scale.cpp`, `src/render_scale.hpp` | Hook on the renderer's per-frame step: the engine renders at the requested size |
| `src/ui_redirect.cpp`, `src/ui_redirect.hpp` | The 2D pass and `GammaControl` on an output-size target while the engine renders below it |
| `src/upscaler_shaders.hpp` | HLSL: DLSS inputs and motion vectors, composite with RCAS and debug views, stretch fallback |
| `src/camera.cpp`, `src/camera.hpp` | Scene constants: main-view recognition, jitter, reprojection, camera cuts |
| `src/upscaler_settings.cpp`, `src/upscaler_settings.hpp` | Mode, custom scale, preset, texture bias, sharpening, frame rate unlock; `MGSVDLSS.ini` |
| `src/upscaler_menu.cpp`, `src/menu_style.hpp` | The "MGSV DLSS" overlay section |
| `src/device_check.cpp`, `src/device_check.hpp` | Lets the game start under ReShade (see below); the build check |
| `src/motion_blur.cpp`, `src/motion_blur.hpp` | Hooks on the motion blur plugin: velocity passes every frame, no blur |
| `src/fxaa.cpp`, `src/fxaa.hpp` | Hook on the FXAA plugin: the FXAA pass at any post processing setting |
| `src/frame_rate.cpp`, `src/frame_rate.hpp` | Hook on the frame wait setter: the optional 60 fps unlock |
| `src/frame_log.cpp`, `src/frame_log.hpp` | Development diagnostics, off by default: shader log, F10 frame record, constant and texture dumps |
| `src/resource_dump.cpp`, `src/resource_dump.hpp` | Format names, constant rows, DDS saving for the dumps |
| `src/shader_bytecode.cpp`, `src/shader_bytecode.hpp` | DXBC checksum (bytes 4 to 20 of the container) |
| `src/log.cpp`, `src/log.hpp` | `MGSVDLSS.log` beside the add-on, one per game session |
| `src/version.rc.in` | Version resource template: name, author and version from `CMakeLists.txt` |

## Game version

Steam app 287700, `mgsvtpp.exe`, title screen "Ver. 1.15", file version 1.0.15.4,
`version_info.txt` `Tpp_steam_mst_en_day3900Mgo_patch_0707_1632`. The add-on logs the build line:

| Field | Value |
| --- | --- |
| TimeDateStamp | `0x6A4CB898` (2026-07-07) |
| SizeOfImage | `0xA01D000` |
| Image base | `0x140000000`, no ASLR, so the RVAs below are fixed for this build |

Direct3D 11 at feature level 11.0. The game creates 24 deferred contexts and records almost every
draw on them; the immediate context mostly executes command lists.

## Install for testing

Into the game folder (`steamapps\common\MGS_TPP`), beside `mgsvtpp.exe`:

| File | From |
| --- | --- |
| `dxgi.dll` | `ReShade64.dll` from the official `ReShade_Setup_6.8.0_Addon.exe` (6.8.0.2155, add-on build; the setup is a zip) |
| `ReShade.ini` | Minimal: no effects, Generic Depth and Effect Runtime Sync disabled |
| `MGSVDLSS.addon64` | This build |
| `nvngx_dlss.dll` | This build (the DLSS SDK's, copied beside the add-on) |

ReShade writes `ReShade.log`, the add-on `MGSVDLSS.log` and `MGSVDLSS.ini`. Uninstall by deleting
these files.
The game can be started directly while Steam runs (`steam_appid.txt` is present), with the game
folder as working directory.

The graphics settings live in `Steam\userdata\<id>\287700\local\TPP_GRAPHICS_CONFIG` (JSON;
`window_mode`, `width`, `height`, `motion_blur_amount`, ...). Editing it by hand works.

## Start-up device check

**Symptom.** With any Direct3D wrapper (ReShade as `dxgi.dll` or `d3d11.dll`, with or without
add-ons, or RenderDoc) the game exits silently about 1.3 s after start: no window, no error, no
crash report. ReShade's log shows the device and the 24 deferred contexts, no swap chain, then
"Exiting". Without a wrapper the game starts normally.

**Cause.** After creating the device, the renderer start-up (`0x2BA1B0`) calls `0x2B9BA0` at
`0x2BA60B`. That function enumerates the process modules (`K32EnumProcessModules`,
`GetModuleBaseNameA`), finds `d3d11.dll` (string at `0x21178F0`), takes its range
(`GetModuleInformation`) and checks 97 vtable entries, from vtable + 0x18, of a Direct3D object
reached through the global at `0x2B9D0A8` (`[[[g]+8]]`). Wrapper objects have their vtables in
the wrapper's DLL, so it returns false and the caller (`je 0x2BAA37`) quits through
`PostQuitMessage` (`0x7082A`), and `WinMain` returns 0. Traced with Frida: `ExitProcess(0)` from
the CRT, `PostQuitMessage` from `mgsvtpp.exe+0x7082A`, under both ReShade and RenderDoc.
It is a local check; nothing is sent anywhere.

**Fix.** ReShade loads add-ons inside `D3D11CreateDevice`, before the check runs.
`passDeviceCheck()` (`src/device_check.cpp`) verifies the build and the function's first bytes
(`40 53 48 81 EC 60 04 00 00`) and writes `mov al, 1; ret` (`B0 01 C3`) over its start, in memory
only. The game file is never changed. Without the add-on, ReShade alone cannot run the game.

## Frame rate

**What the game does.** `framerate_control` in `TPP_GRAPHICS_CONFIG` (`videoout_setting`) is
`Auto`, `Limited30` or `Variable`; the game's menu does not offer it. Traced statically, checked in
game:

- The config loader (`0x52BC30`) stores each video-out value as a 48-bit name hash: `window_mode`
  at video out + 0x10, `vsync` + 0x18, `framerate_control` + 0x20. `Auto` is `0x373A4C315329`
  (read in game). The apply code tests `0xC5A0FE4F5128` (`Limited30`) and `0x9E7769C788E4`
  (`Variable`).
- Applying the video-out settings (`0x24BB40`) sets the frame period at `[[0x2B641C8] + 0x60]`
  (1/59.94 s, 1/29.97 s with `Limited30`) and calls `SetGameFrameWaitType` (`0x81090`, through
  `0x80EE0`) with a wait type hash: `0xA5D6397A8E12` for `Auto` and `Limited30`, `0xA29EF1CEFC2`
  for `Variable`. Lua can call it too (`FoxGameFrame.SetGameFrameWaitType`).
- `SetGameFrameWaitType` keeps the hash at `0x29E3D68` and sets the wait callback of the main
  loop's clock `__game_main` (`0x1A46790`). Each main loop frame ends in that wait (`0x7FDB0`,
  `0x1A464D0`, `0x1A46D80`). The `Auto` wait (`0x807E0`) sleeps until the next period tick: the
  60 fps cap. The `Variable` wait (`0x803F0`) does not wait; it returns the measured frame time,
  averaged over up to 30 frames.
- Game time: the frame start (`0x7F840`) steps by the clock's measured frame time unless the frame
  controller's mode (`+0x64`) is 1 (fixed step). In game it is 0, so unlocked frames take shorter
  steps (0.0104 s at 96 fps) and game speed does not change. Lua's `SetFrameRateMode` (`0x17AA70`)
  can switch the mode; the settings do not.

**Unlock.** `src/frame_rate.cpp` verifies the build and the setter's first bytes
(`40 53 48 83 EC 20 48 B8 12 8E 7A 39 D6 A5 00 00`, which hold the `Auto` wait hash) and hooks it
with MinHook at `AddonInit`. While "Unlock frame rate" is on, a request for the `Auto` wait gets the
`Variable` wait; every other type passes through, so `Variable` in the config and Lua's choices are
kept. Switching the option calls the setter at once with the last requested type, so it applies
live. When a check fails nothing is hooked and the config decides. Checked in game: 60 fps off,
about 95 fps at once when turned on (the CPU limit on the test PC), 60 again when turned off; with
the option saved on, the game's first requests at start-up get the `Variable` wait.

## Diagnostics

RenderDoc cannot capture this game (see "Known limitations of the tools"), so the add-on has its
own recorder, `src/frame_log.cpp`. It is off unless a `MGSVDLSS-frame-log` folder exists beside the
add-on when the game starts; without the folder it registers no event and costs nothing. It only
reads state; the frame renders unchanged. Its files go into that folder:

- Every shader is logged once to `MGSVDLSS.log` (stage, checksum, size). With a `shaders` folder
  inside, the bytecode is saved there. The game's shaders keep reflection and debug info (cbuffer
  and variable names, `#line` source paths), so `D3DDisassemble` names every pass.
- F10 records a frame to `frame-<n>.txt`, after one warm-up frame, in GPU order: command lists
  recorded on deferred contexts appear, indented, where the immediate context executes them. Each
  call lists shaders, targets, depth, viewport, blend, depth state, shader resources, UAVs and
  constant buffers. `frame-<n>-writers.txt` lists the call stacks of constant buffer maps by size.
- `dump.txt` (`<checksum> <count>` per line) makes the first calls with those shaders add their
  constant buffer contents (as the game wrote them on that thread) and save their textures as DDS
  in `frame-<n>/` once their command list has run.

The overlay's Diagnostics section has, for everyone: debug views of the motion vectors and of
where the engine's object velocity was used, jitter off, the game's FXAA for comparison (DLSS stays
ready), and the frame, fallback and reset counts. All are off by default and not saved. Every 1800
frames with DLSS on, `MGSVDLSS.log` gets a status line with the same counts.

## How Fox Engine renders

Recorded in Afghanistan free roam at 1920x1080 windowed, all settings Extra High, depth of field
on, motion blur Off and Low. Resource names are the pointers of one session; formats and sizes
are stable. Pass names are the shaders' source files.

1. Clears; volumetric fog volume (`VolFog_TppVolFog`, 512x256 `R16G16B16A16_FLOAT`).
2. G-buffer, reversed-Z: terrain depth prepass (`NewTerrain_Depth`), objects (`fox3ddf_*`, skinned
   `fox3ddf_skin*`), terrain (`NewTerrain_PackedMaterial*`), decals. Three `B8G8R8A8_UNORM`
   targets and depth `D32_FLOAT_S8X24_UINT`, cleared to 0, tested `greater_equal`.
3. With motion blur on only (the add-on forces it on, see "Velocity without motion blur"):
   velocity. Dynamic meshes, including Snake, soldiers and D-Horse, are drawn again with
   `GBufferVelocity` / `GBufferMaskedVelocity` (stencil ref 1) into a velocity target, then
   `MotionBlurCameraVelocity` adds camera motion from depth into a second target.
4. SSAO at half size (960x540) with a separable compute blur, screen-space and SH lighting,
   shadow maps (one 8192x8192 `D24_UNORM_S8_UINT` atlas), sun light into two
   `R16G16B16A16_FLOAT` 1920x1080 targets, then a separable compute filter over the lighting that
   reads depth and normals (shaders embedded in the exe, `04ef76054aae7a8f2d30c0386c915839` and
   `960e322bda3a4d1ca1a48bc3569c2840`; they have their own `cCameraCenterOffset` constant).
5. **Tonemap early:** `DR_VolFog_TppTonemap` writes the scene target `B8G8R8A8` 1920x1080 through
   an `_SRGB` view. Everything after is LDR, stored sRGB-encoded. `CopyDepthBuffer` makes the depth
   copy the later passes test against.
6. Sky, forward and transparent objects (`*3dfw*`), screen-space reflections (half size, blurred
   mip chain, `LocalReflectionAddBuffer`), particles (`Prim_Poly_*`).
7. Exposure (`Tonemap_Lum_Lacc`), bloom and lens flare at 480x270 (`TonemapBrightpass`,
   `SSLensFlarePass1/2`, `blur_kawase`), depth of field (`DOF_Scatter*`, composited into the scene
   target every frame with depth of field on).
8. A 960x540 layer blended over the scene (`CopyRenderBuffer`, blend ONE / SRC_ALPHA).
9. With motion blur on: `MotionBlurTile`, `MotionBlurTileNeighbour`, `MotionBlurBakeVelocityDepth`,
   two `MotionBlurMcGuire` passes at 960x540, blended over the scene (`CopyRenderBuffer`, SRC_ALPHA
   / INV_SRC_ALPHA).
10. Colour grading: `ColorCorrection_BlndLut_Tex_Gen` builds a 256x16 LUT; the scene target is
    copied to its twin; `Tonemap` applies the LUT from the copy into the scene target.
11. **FXAA:** the scene target is copied to its twin again; `Fxaa` reads the copy and writes the
    scene target.
12. **UI:** `Draw2D*` (HUD, prompts, menus) draws straight into the scene target, alpha-blended.
13. `GammaControl` reads the scene target through an `_SRGB` view and writes the back buffer
    (`R8G8B8A8_UNORM_SRGB`). Present.

Film grain, chromatic aberration and the camera filters exist (`Draw2D_TppNoiseFilter`,
`Draw2D_TppDirDisp_CA`, `Draw2D_TppCameraWaterDrops`, ...) but did not run in the recorded frames;
they are situational. `TemporalAA_CopyVelocity` exists and never ran.

### Resources

| Resource | Format | Size | Role |
| --- | --- | --- | --- |
| Back buffer | `R8G8B8A8_UNORM_SRGB`, 3 buffers, DISCARD | window | Only `GammaControl` writes it |
| Scene target | `B8G8R8A8_TYPELESS` (UNORM and UNORM_SRGB views) | render | LDR scene from step 5 on; FXAA output; UI |
| Scene copy | `B8G8R8A8_TYPELESS` | render | `CopyResource` of the scene target; FXAA and LUT input |
| G-buffer | 3 x `B8G8R8A8_TYPELESS` | render | Albedo, normal, specular/material (content per Courrèges, not checked) |
| Depth | `R32G8X24_TYPELESS`: DSV `D32_FLOAT_S8X24_UINT`, SRV `R32_FLOAT_X8X24_TYPELESS` | render | Reversed-Z, the G-buffer's |
| Depth copy | same | render | Made by `CopyDepthBuffer`; used from step 5 |
| Dynamic velocity | `B8G8R8A8_TYPELESS` | render | R = 255 where a dynamic mesh drew, G = 0, B/A = velocity |
| Velocity | `B8G8R8A8_TYPELESS` | render | R = G = 0, B/A = camera and object velocity |
| Lighting | 2 x `R16G16B16A16_FLOAT` | render | HDR light accumulation |

### Shaders

Checksums are the DXBC header checksum (bytes 4 to 20) as lowercase hex.

| Pass | Vertex shader | Pixel shader |
| --- | --- | --- |
| `Fxaa` (FXAA 3.11, `fox\source\system\Gr\Dg\shader\Fxaa.hlsl`) | `dd1e3d1240e73daa7e53e861845472f6` | `deef1a5e3462ab091b5612a37662d15d` |
| `Tonemap` (applies the LUT) | `3deca0fd1a4b712a99b1419546693ce1` | `422ba739df267d9f92d4d62b0d2b4e09` |
| `ColorCorrection_BlndLut_Tex_Gen` | `d6d688ebb016cfb9133b359f233af9f9` | `82a954d0fe3755bb0a79aaaded5cce74` |
| `GammaControl` | `a3a7925d91d305283bb6e2ccc51504e8` | `12790f061c5f1460aa40c264e4bc9f07` |
| `DR_VolFog_TppTonemap` | `11a3b2f867ec3409c20a2f58c0bb11dd` | `00a609370c2e8b47a645263f91f6d3e8` |
| `GBufferVelocity` | `653764512e1d19ffe60c3db8c1c2f2d2` | `89e3198a5285240f75a44e714406b36c` |
| `GBufferMaskedVelocity` | `b93ed456c7b3d164c14ff09f15d5144d` | `66b808b723708f6cf72b98f690f0aa44` |
| `MotionBlurCameraVelocity` | `ea21bed4add8b4f64839b58145cf3cb4` | `5d0241dd9804f41c780cd36669c71198` |
| `MotionBlurTile` | `c0d86c3bea9821b283fe78493614fa05` | `519f12495c42e4b6a27d56a05514b0c5` |
| `MotionBlurTileNeighbour` | `0d214bed7b73fea12f2cf5e22039a103` | `890e554ba4f340d1d82ddf218d3e339e` |
| `MotionBlurBakeVelocityDepth` | `0f934876edd8b1120384905cd7e3477f` | `d89d8bcef5a286dee3f697d3aae068fa` |
| `MotionBlurMcGuire` | `926f38e58184c050456e4bdb9bfe184a` | `475283f0d10c84d3c1b47e2ba153c916` |
| `CopyRenderBuffer` (generic copy, used many times) | `da1404ad7d2b4c9d6dc7b57828737141` | `9f93a957f1e21656e0d86bf112b71d6d` |
| `CopyDepthBuffer` | `33c68dfbe259d05896722b16de33562d` | `b61f24562a2421162537203cc932a8e4` |
| `Draw2D` | `49fdf6e5a7ec2e0b1461ef43eaed943e` | `ceb72d101a7b403a4e9ad06d5e55f02d` |
| `Draw2D_Ui2_BL` | `7e254e83e180ad51690ed8cb3b9206db` | `670bf1eb7e25589eeec593f59ba2ce69` |
| `NewTerrain_Depth` (first G-buffer pass) | `19db120cc51b6a18587f315dc7135436` | `45bc53d23272dbfcd9108a611174c65a` |
| `fox3ddf_blin` (common object) | `2d242c4792e9edde4e8b12ca2d3810f5` | `3942f4da227e6ac7c19daf284d4d7024` |
| `fox3ddf_skin` (skinned) | `feec72cb73d30137114267d174e4ae14` | `0ab79b77cd13e962000f47e9afe333cc` |
| `ModelForward_ShadowOpaque` | `f241bee6390b176e669aabd0eceada09` | `4b5d963e9de0def40f7aaff71e62ba9a` |

### FXAA pass

- Pixel shader: `t0 g_Image` (the scene copy, `B8G8R8A8_UNORM` view, so values are sRGB-encoded),
  sampler `s11 g_samplerLinear_Clamp`, `cb0 cPSSystem`; `fxaaQualityRcpFrame` is
  `cb0[2].zw` = `m_renderBuffer.zw`.
- Writes the scene target (`B8G8R8A8_UNORM` view) with the depth copy bound and depth off.
- The copy's alpha varies (0 to 253 in a dump), consistent with luma in alpha for FXAA.
- It runs in a deferred command list together with the LUT pass, before the UI's list.

### Constant buffers

`cPSSystem`, slot `b0` (64 bytes):

| Register | Field | Value seen |
| --- | --- | --- |
| c0 | `m_param` | 0 |
| c1 | `m_renderInfo` | (1920, 1080, 0, 0) |
| c2 | `m_renderBuffer` | (1920, 1080, 1/1920, 1/1080) |
| c3 | `m_dominantLightDir` | sun direction, w = 1 |

`cVSScene` / `cPSScene`, slot `b2` (480 bytes, dynamic, written with Map `WRITE_DISCARD`). Matrices
are rows, applied as `dp4(row, v)`:

| Registers | Field | Main view, G-buffer and velocity passes |
| --- | --- | --- |
| c0-c3 | `m_projectionView` | P x V |
| c4-c7 | `m_projection` | `(-1.75,0,0,0) (0,3.111,0,0) (0,0,A,B) (0,0,1,0)`: A = -1.725e-5, B = 0.069 |
| c8-c11 | `m_view` | world to view |
| c12-c15 | `m_shadowProjection` | shadow |
| c16-c19 | `m_shadowProjection2` | **previous frame's P x V** |
| c20 | `m_eyepos` | camera position, w = 1 |
| c21 | `m_projectionParam` | (1/P00, 1/P11, B, A) |
| c22 | `m_viewportSize` | (1920, 1080, near 0.069, far 4000) |
| c23 | `m_exposure` | |
| c24-c26 | `m_fogParam[3]` | |
| c27 | `m_fogColor` | |
| c28 | `m_cameraCenterOffset` | (0, 0, 0, 0) |
| c29 | `m_shadowMapResolutions` | (8192, 4096, 8192, 4096) |

- The projection is reversed-Z with a finite far plane: depth = A + B / z, 1 at z = 0.069, 0 at
  z = 4000. Clip w is view z. P00 is negative (the view's x axis is mirrored). P11 = 28/9 here,
  vertical FOV about 35.6 degrees; it changes with the camera.
- The game fills several instances per frame from different buffers: shadow views (`c4-c7`
  orthographic, `c7 = (0,0,0,1)`), the main view, and the post-processing passes. They are told
  apart by content, not by buffer.
- Slots are reused per pass. In `MotionBlurCameraVelocity`, `c8-c11` hold the previous P x V and
  `c16-c19` the inverse view. In post passes `c16-c19` hold fog colours. In shadow passes
  `c16-c19` hold the main view's P x V.
- With motion blur Off, the main view's `c16-c19` equalled `c0-c3` in the one frame recorded. The
  velocity passes the add-on forces on (motion blur Off) do get the previous P x V: with the camera
  following Snake on a sprint, his body's velocity is near zero while the ground's is over 10 px.

`cVSObject` / `cPSObject`, slot `b5` (208 bytes): `m_viewWorld` c0-c3, `m_world` c4-c7,
`m_useWeightCount` c8, `m_localParam[4]` c9-c12. `VSBones`, slot `b6` (3072 bytes):
`g_vsBone` (32 x `float4x3`) and **`g_vsPrevBone`** (32 x `float4x3`).

### Velocity

- Encoding, the same in `GBufferVelocity` and `MotionBlurCameraVelocity`: v = (current NDC -
  previous NDC) x `m_renderInfo.xy` x 0.5 / 64, i.e. pixels / 64; length clamped to 1; y negated;
  stored as v x 0.5 + 0.5 in B (x) and A (y), 8-bit. So: 0.5 pixel steps, at most 64 pixels,
  screen-space x right, y down, pointing from the previous position to the current one.
- `MotionBlurCameraVelocity` reconstructs view position from depth (`t2`) with
  `m_projectionParam` and `clip.xy - m_cameraCenterOffset.xy`, goes to world with `c16-c19` and to
  the previous clip position with `c8-c11`, and keeps the dynamic velocity (`t3`) where its mask is
  set.
- The game runs the velocity passes only with motion blur Low or High (config values `Small` and
  `Large`). They cover skinned characters with per-bone previous positions: dumps show Snake's legs
  and arms with their own motion and the standing horse with camera motion only. Nearby trees are
  drawn too (wind); grass and bushes are not.
- With the add-on's jitter, the current position is projected with the jittered matrices and the
  previous one with the engine's unjittered previous P x V (`c16-c19`, which the add-on leaves
  alone), so the stored velocity includes this frame's jitter.
- The vertex shader projects the current position with `cVSObject.m_viewWorld` and `c4-c7`, and the
  previous one from `g_vsPrevBone` with `c12-c15` (not checked) and then `c16-c19` (previous P x V). The scale
  (pixels / 64) is a literal in the pixel shader, so it does not depend on the blur amount.

### Velocity without motion blur

What decides whether the velocity passes run, traced statically in this build:

- `GrPluginMotionBlur` (class info `0x2B75C20`, 0x80 bytes) has its vtable at `0x20F8830`. Slot 6
  (`0x1E2D80`) runs before the frame's geometry and slot 8 (`0x1E2E20`) draws the blur; they are
  called PreExec and Exec here, after `fox::gr::PluginDeferredGeometry::PostExec`, the one name
  the string table gives. Both take (plugin, pass context, view).
- PreExec: the shutter is the camera's (`[[view+0x570]+0x128]`, or the plugin's own at `+0x6C`
  when `+0x68` is set), at least `[0x2B75AC8]`. If the dword `[0x2B75AC4]` (motion blur on) is
  set and shutter x `[0x2B75ACC]` exceeds the constant at `0x20F732C`, it sets bit 0 of the
  64-bit frame flags at `[[context+0xD0]]` (`or qword [rax], 1` at `0x1E2DDE`).
- `PluginDeferredGeometry::PostExec` (`0x1C54D0`, GPU marker `MakeVelocityBuffer`) tests that bit
  (`test byte [rcx], 1` at `0x1C558F`) and only then draws the two velocity lists (`[plugin+0x70]`
  and `[plugin+0x78]`, created unconditionally at `0x1C5050`) and `MotionBlurCameraVelocity`.
- Exec repeats the test and draws the blur through three renderer calls (vtable `+0x468`, `+0x470`,
  `+0x478`): tiles, neighbours, bake, the McGuire passes and the composite. It does nothing else.
- The flag is cleared every frame: when the add-on stops setting it, the next frame has no
  velocity passes. The three globals have no RIP-relative writers; they are written through
  pointers, presumably from the graphics settings. The add-on does not touch them.

Leads not needed in the end: the graphics settings object is the singleton at `[0x2B74910]`
(0x1E0 bytes; the current settings at `+0x38`, the next at `+0x108`, 0xD0 bytes each: video out
0x38, then quality, each value a 48-bit string hash, `motion_blur_amount` first).
`0x1D38F0` stores new settings and `0x1BCD50` notifies the listeners in the array at `0x2B735E0`
(count at `0x2B735D8`, vtable slot 2). The `GrPluginSettings` entity (reflection at `0x1C55CA0`)
has `isMotionBlur`, `isPatchVelocity` and `isTemporalAA` bits in its `flags`, set from level data.
Some functions jump into the `.xdata` section (`0x2F03000`, 111 MB, entropy 6.7), which holds
virtualised code (for example `0x1D3850`); static analysis stops there.

### FXAA with post processing down

Lower post processing settings turn the FXAA pass off, and with it DLSS, which replaces that pass.
Traced statically in this build:

- `GrPluginFxaa` (class info `0x2B799A0`, name string `0x21032D0`) has its vtable at `0x2103220`.
  Slot 8 (`0x2155A0`, Exec) takes (plugin, pass context, view), like the motion blur plugin's.
- Exec runs the FXAA filter at `[plugin+0x68]` (its vtable `+0x20`, with the view in `rdx` and
  `r8`) only when bit 7 of `[[view+0x570]+0x100]` is set (the view has post effects) and the dword
  `[0x2B79984]` is non-zero. Exec is that dword's only reader; the post processing setting clears
  it. Exec does nothing else.


All screen-space reconstruction shaders read `m_cameraCenterOffset` (`cb2[28]`) and subtract it
from the clip position: lighting (`Lighting_*`, `SSLighting2_*`, `SunLight_*`), SSAO, screen-space
decals, subsurface scattering, the rain filter and `MotionBlurCameraVelocity` (42 pixel shaders,
plus the embedded compute shaders through `cCameraCenterOffset`). The engine therefore supports an
off-centre projection and passes the offset to the reconstruction. Units and sign, from
`MotionBlurCameraVelocity`: `clipSpacePosXY = inBasePos.zw - cb2[28].xy`, then
`viewPos.xy = clipSpacePosXY * (1/P00, 1/P11) * viewZ`, so the offset is the NDC shift the
projection adds, and the shader removes it. In the game's own frames it is 0.

The two embedded compute shaders (`04ef7605...`, `960e322b...`) turned out to be the skin
subsurface-scattering filter: they only process pixels whose light buffer alpha is set (skin) and
read the offset from their own constant buffer, which the add-on does not set.

How the scene constants reach the GPU, from the call stacks of their Map calls:

| RVA | What |
| --- | --- |
| `0x2B9E30` | Per-pass "apply": updates the bound constant buffers (`0x2486E0` for the scene buffer) and binds |
| `0x2486E0` | Uploads the scene buffer object (`[ctx+0x130]`) through `0x2C5A10` |
| `0x2C5A10` | Generic upload: for each slot `Map(WRITE_DISCARD)`, `memcpy` from the CPU copy at `[slot+8]` (`[slot+0x18]` x 16 bytes), `Unmap`, bind |
| `0x2D664F`, `0x2B9E60`, `0x29190D`, `0x291EB0` | Most frequent callers of the upload for 480-byte buffers (157 of the 234 maps in the warm-up and recorded frames came from `0x2D664F`) |

Every scene-constant upload is a Map / memcpy / Unmap, and ReShade's `unmap_buffer_region` event
fires before the real `Unmap` with the data still mapped, on deferred contexts too. The code that
builds the projection itself was not found. Many functions store to offsets `0x150`, `0x160` and
`0x1C0` together (the candidates `0x1EB190`, `0x206630`, `0x20AA00`, `0x272390` and a dozen more);
the ones read fill other structures, so the scene block has no single writer to hook.

### Render size

The renderer keeps the size it renders at apart from the display's. Traced statically, then
confirmed by setting the wanted size in game.

- The renderer object (vtable `0x21086E0`) holds the display size at `+0x180`, the wanted render
  size at `+0x188`, the current one at `+0x190` and flags at `+0x198`.
- A display mode request (`0x2496A0`, slot 10) stores the display size and sets the wanted size to
  it, at least 512 and rounded up to even, with flag 2 when it differs from the current size.
  Applying the video-out settings (`0x24BB40`) sets flag 4 instead.
- The per-frame step (`0x24C220`, slot 6, on the render thread): with flag 4 it first takes the
  wanted size from the display object (`[+0xF0] + 0x30`); with flag 2 and a wanted size that differs
  from the current one it copies it and calls `0x24B2A0`. That rebuilds every size-dependent target:
  five groups through `0x242AC0` (group, width, height), three objects through their virtual
  `+0x10`, `0x27B4E0`, a generation counter at `0x2B9404C`, and `0x1BE4C0`, which lets the plugins
  resize theirs. Call stacks recorded during development show the same path rebuilding the
  targets when the game's window goes from its first 1024x576 to the configured size.
- The swap chain is not part of this: the display object resizes it on its own (`0x24AD80`,
  `ResizeBuffers` through `0x19F59A0`), and the window keeps the display size.
- With the wanted size set to 960x540 for a 1920x1080 window, the whole frame follows: G-buffer,
  lighting, SSAO and the depth-of-field and bloom chains at half size and below, the half-size
  layer (480x270). An F10 record had no 1920x1080 target apart from the back buffer.
  `GammaControl` stretches the scene target over the back buffer: it samples at
  `uv x m_renderInfo.xy x m_renderBuffer.zw` with a linear clamp sampler, the console-style
  sub-rectangle mapping, so any scene size fills the screen. FXAA's `fxaaQualityRcpFrame` and the
  colour grading follow by themselves.
- `cPSSystem`'s `m_renderInfo` and `m_renderBuffer` are set per pass to that pass's target size
  (data breakpoints caught 512 for the fog volume pass): `0x2C586E` and `0x2C5F75` copy them into a
  per-context copy of the buffer with `memcpy`.
- `ChangeMainViewportSize` (`0x1C658E0`) stays an empty stub; nothing else was needed.

### The 2D pass

- The HUD, menus, subtitles and camera filters are drawn with the `Draw2D*` shaders. Their vertex
  shaders (67 in a full shader dump, up to 73 created in a session) are generated as
  `...\Draw2D*_vs.hlsl`, and the game's shaders keep that path in their debug data; many other
  shaders also contain the word `Draw2D` there, so only the path identifies them. In
  the recorded frames they draw into the scene target in the command list after FXAA's, with the
  scene's depth-stencil bound: the stencil is cleared to 1, a mask written with reference 0 and
  later draws tested against 1; depth testing is off. The viewport is the scene size and positions
  arrive in clip space through `cVSWork`'s matrix, so the same draws work at any target size.
- Their pixel shaders read nothing from `cPSSystem` but `m_renderInfo` (`c1`) and `m_renderBuffer`
  (`c2`): the UI shaders for a screen-space mask (`blendScreen`), the filters (radial blur, blood
  and water drops, the video player) for screen coordinates.
- The only `Draw2D` draws before FXAA in the records are lens flares into the half-size layer.
- `GrPlugin2D`, `GrPlugin2DFrontmost` (vtables `0x20F2EB0`, `0x20F2F50`, Exec `0x1C0AD0`) and
  `GrPlugin2DShrink` (`0x20F2FF0`, Exec `0x1C0C70`) only queue the 2D work: a flag set around their
  Exec saw none of the draws, which the worker threads record later.

### Engine addresses

| RVA | What |
| --- | --- |
| `0x2B9BA0` | Start-up device check (returns `al`) |
| `0x2BA60B` | Its call in the renderer start-up `0x2BA1B0`; fail path `0x2BAA37` |
| `0x70810` | Quit helper (`PostQuitMessage` at `0x7082A`) |
| `0x70430` | Window procedure (`WM_DESTROY` posts quit at `0x7051D`) |
| `0x19F4F29` | `D3D11CreateDevice` call |
| `0x2C5A10` | Constant buffer upload (Map at `0x2C5B02`) |
| `0x1E2D80`, `0x1E2E20` | `GrPluginMotionBlur` PreExec and Exec (vtable `0x20F8830`, slots 6 and 8), hooked |
| `0x1C54D0` | `PluginDeferredGeometry::PostExec`, `MakeVelocityBuffer`; velocity test at `0x1C558F` |
| `0x2B75AC4`, `0x2B75AC8`, `0x2B75ACC` | Motion blur on, minimum shutter, amount (read by PreExec and Exec) |
| `0x2155A0` | `GrPluginFxaa` Exec (vtable `0x2103220`, slot 8), hooked |
| `0x2B79984` | FXAA on, cleared by lower post processing settings (read only by the FXAA Exec) |
| `0x1C658E0` | Script binding `ChangeMainViewportSize`: an empty stub (`xor eax, eax; ret`) in this build |
| `0x24BB40` | Applies the video-out settings: frame period and wait type from `framerate_control` |
| `0x81090` | `SetGameFrameWaitType` (wait type by hash), hooked; the current type at `0x29E3D68` |
| `0x807E0`, `0x803F0` | The `Auto` (60 fps) and `Variable` frame waits of the `__game_main` clock |
| `0x7F840`, `0x7FDB0` | Main loop frame start (game time step) and end (frame wait) |
| `0x24C220` | Renderer per-frame step (vtable `0x21086E0`, slot 6), hooked: applies the wanted render size |
| `0x2496A0` | Renderer display mode request (slot 10): display and wanted render size |
| `0x24B2A0` | Rebuilds the size-dependent targets at the current render size |
| `0x24AD80` | Display object: swap chain resize |
| `0x1C0AD0`, `0x1C0C70` | Exec of the 2D plugins (`GrPlugin2D`, `GrPlugin2DFrontmost`; `GrPlugin2DShrink`) |

## What the add-on changes

DLSS replaces FXAA, at native resolution (DLAA) or from a lower render size (Quality, Balanced,
Performance, Ultra Performance, Custom). Everything runs from ReShade events, apart from the
start-up device check, two hooks on the motion blur plugin, the hook on the renderer's per-frame
step and the optional frame rate unlock (see "Frame rate").

- **Shaders.** `init_pipeline` identifies three pixel shaders by DXBC checksum: `Fxaa`,
  `MotionBlurCameraVelocity` and `GammaControl`. All are drawn as one full-screen triangle, so `draw`
  only looks at the bound pixel shader for 3-vertex draws. The 2D pass's vertex shaders are known
  by their source path (see "The 2D pass").
- **Command lists.** The game records a whole frame on its worker threads, finishes about 44
  command lists, and only then executes them on the immediate context in one burst, followed by
  Present (measured over 200 frames: never a finish after the first execute of a frame). What a
  deferred context records that matters (the FXAA pass, the velocity texture) is kept by context,
  moved to the command list at `FinishCommandList` (ReShade's `execute_secondary_command_list`
  with the new list and the deferred context) and taken at its `ExecuteCommandList`. That event
  fires before the real call, so DLSS runs at the immediate context's *next* execute, the UI's
  list: by then the FXAA list has run and the UI has not.
- **FXAA stays.** FXAA reads a copy of the scene target (`CopyResource` just before it) and writes
  the scene target. The copy still holds the graded, pre-FXAA image after the list has run, so
  DLSS reads the copy and overwrites FXAA's output. Nothing is skipped in the recorded list: a
  frame DLSS cannot handle simply keeps FXAA's result. FXAA costs well under a millisecond.
- **Frames and jitter.** A frame starts with the first main-view scene upload after the immediate
  context executed anything; all of its main-view uploads get that frame's jitter. The FXAA pass
  carries the frame it was recorded in, and DLSS runs only if that is still the latest frame and
  directly follows the last evaluated one (otherwise it falls back or resets).
- **Jitter.** At `unmap_buffer_region`, with the data still mapped, 480-byte constant buffers are
  checked by content (`camera.cpp`): a perspective `c4-c7` (`c7 = 0,0,1,0`), aspect matching
  `c22.xy`, `c28 = 0`. Those uploads get `c4.z += jx`, `c5.z += jy`, and `c28.xy = (jx, jy)` in NDC.
  Where `c0-c3` is P x (something affine), `c0 += jx·c3` and `c1 += jy·c3` as well (motion blur's
  pass keeps other matrices in `c8-c11` and is still jittered). Shadow views are orthographic and
  untouched. Halton(2, 3), 8 phases at DLAA; DLSS gets (jx, jy) in render pixels, x right, y down,
  confirmed with 4x jitter (only that sign is sharp). The first upload whose `c0-c3` is `P x V`
  with `V` in `c8-c11` is the frame's camera, captured before the jitter. A frame DLSS did not run
  on leaves the next unjittered, so menus and 3D views without FXAA never shake.
- **Motion vectors.** A compute shader before DLSS: camera motion from the depth copy (the FXAA
  pass's depth target, `R32_FLOAT_X8X24_TYPELESS` view; the G-buffer depth plus forward geometry)
  and one clip-to-previous-clip matrix, `prev P x V x inverse(cur P x V)`, built on the CPU in
  double, so the camera's absolute position cancels. The pixel's NDC has the jitter removed, so
  the vectors carry none. Where the engine's velocity mask is set, the motion is clamped to within
  half a quantisation step (64/255 px) of the engine's decoded velocity less this frame's jitter,
  unless the engine's value is saturated at 64 px: object motion at 8-bit precision, static parts
  of masked objects at full precision. Checked in game:
  zero on every frame with a still camera; the previous DLSS output warped by the vectors aligns
  with the current one at offset 0 (searched in 1/8 px) on a pan; within 0.25 px of
  `MotionBlurCameraVelocity`'s output.
- **Camera cuts.** Reset when the camera moves more than 2 m, turns more than 25 degrees or
  changes P11 by more than 10% in one frame (gameplay stays far below: 2 m is 120 m/s at 60 fps).
  The title screen alternates between two cameras 1197.7 m apart with identical orientation
  every 2 to 5 frames, so it resets that often there; gameplay showed none. In three launches
  this happened only in the title's first 15 seconds (about 40 resets), never later while the
  menu showed.
- **Evaluation.** In the add-on's own `ID3DDeviceContextState`: the inputs pass copies the colour
  (sRGB-encoded LDR, `R8G8B8A8_UNORM`), depth (`R32_FLOAT`) and motion (`R16G16_FLOAT`); NGX
  evaluates through ReShade's wrapped immediate context with `DepthInverted | MVLowRes`, no
  `IsHDR`, MV scale 1 and a 1x1 exposure texture holding 1. NGX is only ever called on the
  immediate context.
- **Composite.** A full-screen draw writes the DLSS output with RCAS (AMD FidelityFX, MIT; 33% by
  default) and alpha 0, as FXAA writes: at DLAA into the scene target's `B8G8R8A8_UNORM` view, where
  the UI then draws unchanged; when upscaling into the UI target (below).
- **Render size.** `render_scale.cpp` checks the build and the per-frame step's first bytes
  (`40 53 48 83 EC 20 48 8B D9 8B 89 98 01 00 00`) and detours it with MinHook. Before the step runs
  it writes the requested size to the wanted size and sets flag 2, so the engine rebuilds its targets
  itself, on its own thread, between frames. A wanted size the hook did not write is the engine's own
  choice (start-up, a display change); it is restored when no size is requested (Off, DLAA). A
  request only applies while the display size matches the one it was planned for, and with flag 4
  set the step is left alone and corrected on the next frame. A mode change therefore costs one
  rebuild, like a resolution change, and Off renders at native resolution again from the next frame.
- **Plan.** At each present, for the upscaling modes: NGX's optimal render size for the swap chain
  size and the mode (Custom: output x scale), rounded up to even as the engine rounds its own. Only
  the mode, the custom scale and the output size make a new plan; the overlay applies the custom
  scale and the texture bias when their sliders are released, so a drag does not rebuild the
  targets every frame. The feature is created for render -> output with the mode's quality value.
  Jitter phases follow the ratio, `ceil(8 x (output / render)^2)` (32 at Performance).
- **The UI at the output size** (`ui_redirect.cpp`). While the engine's targets are smaller than
  the swap chain, the add-on keeps a `B8G8R8A8_TYPELESS` target at the output size with a
  `D24_UNORM_S8_UINT` depth-stencil:
  - Before the first command list that draws on it runs, it is filled: DLSS composites into it, or
    when DLSS did not run this frame (menus without a 3D view, loading screens, fallbacks) the
    render-size scene target is stretched into it bilinearly, as `GammaControl` would have. The UI
    never draws over a stale frame.
  - A draw with a `Draw2D` vertex shader bound (tracked per context from `bind_pipeline`) into the
    render-size scene target is made by the add-on on the output target instead, with viewports and
    scissors scaled by output / render and its own `cPSSystem` (`c1` and `c2` at the output size)
    in slot b0, then the game's bindings are restored and the game's draw is skipped. A clear of the
    scene target from such a context goes to the output target.
  - Every clear of a render-size depth-stencil also clears the output-size one, so the 2D pass's
    stencil masks work.
  - `GammaControl` reads the output target's `_SRGB` view in place of the scene target.
  - The window, swap chain and mouse are untouched; the engine never learns the output size differs.

  Checked in game: at 1920x1080 windowed, Performance (960x540) on the title menu and in free roam
  with the menu text and HUD at full resolution; at 3840x2160 borderless, Quality 2560x1440,
  Balanced 2228x1254, Performance 1920x1080 and Ultra Performance 1280x720, the HUD pixel for pixel
  the same in Performance and Ultra Performance.
  Modes switched at runtime many times: over 22,000 frames had 4 fallbacks, each the frame right
  after a mode change. With the 60 fps cap lifted, every mode ran at 93 to 96 fps at 4K: the game is
  CPU-limited there on an RTX 5080.
- **Texture LOD bias.** Material samplers get `log2(render / output)` plus the overlay's offset
  (-1 at Performance), through biased copies bound after the game's (`push_descriptors`), on every
  context, except for the 2D pass. The game creates 16 distinct samplers (logged in development):
  point, bilinear, trilinear and anisotropic in wrap and clamp variants, and two comparison
  samplers. The rule takes the mipmapped, non-point wrap or mirror samplers; screen-space passes
  use the clamped and point ones, and a bias would also move their explicit mip reads.
- **Motion blur.** DLSS needs the engine's velocity for characters, which the game draws only
  with its motion blur on, but not the blur, which would only smear its output. `motion_blur.cpp`
  checks the build and the first bytes of PreExec (`80 79 68 00 49 8B 80 70 05 00 00`) and Exec
  (`48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20`) and detours both with MinHook at `AddonInit`.
  While DLSS replaces FXAA (any DLSS mode, not the FXAA comparison), PreExec runs the game's code
  and then sets the velocity bit, and Exec returns without drawing. So the velocity passes run
  every frame whatever the game's setting, and the blur passes do not run at all. Off and the
  comparison give both back to the game's setting from the next frame. When a check fails nothing
  is hooked and the game's setting stays in charge. `AddonUninit` removes all engine hooks with
  `MH_Uninitialize`. Checked with F10 records:

  | Motion blur | Add-on | Velocity passes | Blur passes |
  | --- | --- | --- | --- |
  | Off | DLAA | yes | none |
  | Off | Off | none | none |
  | Low | DLAA | yes | none |
  | Low | Off, or the FXAA comparison | yes | tiles and both McGuire passes |

  With motion blur Off, the object motion debug view covers Snake standing and sprinting and the
  nearby trees; the ghosting of limbs moving toward the camera, seen without the hooks, is gone.
  The still-camera motion vectors stay zero, still-camera stability and a pan match the build
  without the hooks, and 10,000 frames had no fallback.
- **FXAA with post processing down.** `fxaa.cpp` checks the build and the first bytes of the FXAA
  plugin's Exec (`49 8B 80 70 05 00 00 8B 90 00 01 00 00`) and detours it with MinHook at
  `AddonInit`. While DLSS replaces FXAA, the detour keeps the view test and runs the filter without
  testing the post processing dword, so DLSS has its pass at every setting. Off and the comparison
  give the pass back to the game's setting from the next frame. Checked on the title menu with post
  processing Low and DLAA: DLSS ran on every frame.
- **Fallbacks.** Missing or mismatched FXAA bindings, sizes that differ, a frame recorded out of
  step, a second FXAA pass in a frame, no list after FXAA, no plan yet, a render size the engine has
  not reached yet after a mode change, or an FXAA pass sharing a list with the UI keep FXAA (stretched
  into the UI target when upscaling) and log the reason once. An exception or NGX error stops DLSS
  until a mode is chosen again; the engine then renders at native resolution. A size change
  recreates the feature and textures.

Not handled:

- The skin subsurface-scattering filter gets no jitter offset (see "Jitter"). Its shaders only use
  the offset for the view direction of a pixel, which the jitter (at most half a pixel) turns by
  about 0.01 degrees, so it is left alone.
- Heat haze (`Primitive_TppHeatHaze`, `hazeMirage*`), particles, and grass and bushes moving in the
  wind have no motion vectors. None was seen to ghost in the tested scenes; a DLSS
  bias-current-colour mask drawn with the same vertex shaders would be the fix if one does.
- The title screen's resets (see "Camera cuts"): only in its first 15 seconds, so left alone.
- A `Draw2D` draw into the scene target before FXAA would be redirected and then covered by the
  DLSS image. None was seen (see "The 2D pass"); the situational camera filters were not recorded.
- 2D filters that redraw the whole screen from the scene copy (chromatic aberration, radial blur)
  read the render-size copy, so while one is on, the image under it has the render size.

## Known limitations of the tools

- **RenderDoc 1.46** captures nothing: with the device check passed (patched in memory through
  Frida during `--opt-delay-for-debugger`), the game stays on frame 1 at about 0.4 FPS. Thread
  stacks show every game thread waiting and none inside `renderdoc.dll`; ReShade with the same
  patch runs normally. Without the patch RenderDoc hits the device check like ReShade. Not pursued;
  the frame log and dumps replace it.
- **Frida must attach at spawn.** Attaching to the running game and scanning its memory crashed it
  in `frida-agent.dll` (`0xC0000409`). Spawning with the game folder as working directory works.
