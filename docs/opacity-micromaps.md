# Opacity micromaps (research)

Whether the rays' alpha test should move to DXR 1.2's opacity micromaps (OMM), what it would
take in this add-on, and what blocks it today.

**Short answer:** the shader side fits this codebase almost line for line, but the *runtime* does
not: OMM with inline `RayQuery` (the only kind of tracing this project does) needs the Agility SDK
1.619 runtime, and an add-on cannot select that runtime for a game it did not build. Measured
2026-10-30: the inbox `D3D12Core.dll` (616) reports raytracing tier 1.2 yet caps at shader model 6.8
and rejects `cs_6_9` outright; a probe executable carrying Agility 1.619.6 builds the same shader
fine. **Decision: capability-gated only** — build the DX12 path, ship it dormant, let it turn itself
on when the OS runtime reaches 619 (section 4). Bake against 0.5 mip-free alpha would also change how
distant foliage shadows look, because today the alpha test picks a mip from the ray cone and OMM
cannot.

## 1. What OMM is

Hardware (Ada's RT core "opacity micromap engine"; emulated in software on Turing and Ampere) keeps
a 1- or 2-bit opacity mask per micro-triangle of a base triangle, 4^N of them, N up to 12, and
resolves a ray/triangle hit as one of:

| state | inline `RayQuery` result |
| --- | --- |
| transparent | hit dropped, traversal continues, **no candidate returned** |
| opaque | hit committed, **no candidate returned** |
| unknown-transparent / unknown-opaque (4-state only) | candidate returned, as a non-opaque hit is today |

So every micro-triangle the mask can decide stops costing this project an `asm`-heavy shader
iteration (`CutOutBlocks` in `src/passes/ray_tracing/shaders/gi_common.hlsli:426`), a texture
fetch, a `TextureLod` (a cross product, a 2x2 determinant and a `log2`), and a loop round the
`RayQuery`. That is the whole win: fewer SM iterations per hit, and the RTCore does the coverage
test. NVIDIA measures roughly a 30% cut in traversal cost in alpha-test-heavy scenes and up to 2.3x
in path-traced titles.

This codebase is an unusually good fit: it traces **only** with inline `RayQuery` in compute
shaders, has no SBT, no any-hit shaders, and already has a hand-written per-candidate alpha test to
replace. The unknown states map one-to-one onto the existing loop: candidate -> `CutOutBlocks` ->
commit or carry on.

## 2. Where it would plug in

| Today | With OMM |
| --- | --- |
| `GI_GEOMETRY_CUT_OUT` geometry is non-opaque in the BLAS (`gi_scene.cpp:335`), so its triangles reach the candidate loop | Same geometry, but built as `D3D12_RAYTRACING_GEOMETRY_TYPE_OMM_TRIANGLES` with an OMM index buffer and an OMM array |
| `CutOutBlocks` samples the base texture's alpha at the cone's mip and compares to `alphaThreshold` (`gi_common.hlsli:426`) | Runs only for **unknown** micro-triangles; known ones never reach the shader |
| `TraceClosest` / `SurfaceRayBlocked` loops over candidates (`gi_common.hlsli:486`, `:779`) | Unchanged, minus most iterations |
| Cut-out geometry without a texture blocks stochastically (`untextured`, `blocking`) | Unchanged: no texture, no OMM, no linkage |

Nothing in `gi_renderer.cpp`'s passes changes beyond the shader template argument.

## 3. API surface (Direct3D 12)

Confirmed against the DXR functional spec (Opacity micromaps, and the OMM sections of
`GetRaytracingAccelerationStructurePrebuildInfo` / `BuildRaytracingAccelerationStructure`) and the
Windows SDK 10.0.26100 headers.

- **Feature gate:** `D3D12_FEATURE_DATA_D3D12_OPTIONS5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_2`.
  `gi_renderer.cpp:466` already queries `OPTIONS5`; it only checks 1.1.
- **OMM array** (opaque driver data, not stored in the BLAS):
  - build: `D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS{ Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_ARRAY, DescsLayout = ARRAY, NumDescs = 1, pOpacityMicromapArrayDesc = &desc }`, then `BuildRaytracingAccelerationStructure` (on the add-on's compute list).
  - `D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_DESC`: `InputBuffer` (raw OC1 bitstream, 128-byte aligned), `PerOmmDescs` (GPUVA + stride, `D3D12_RAYTRACING_OPACITY_MICROMAP_DESC{ ByteOffset, SubdivisionLevel:16, Format:16 }` each), and the CPU-side `pOmmHistogram` (`Count, SubdivisionLevel, Format` per level/format combination) that sizes the prebuild.
  - memory: `GetRaytracingAccelerationStructurePrebuildInfo` on the same inputs; result and scratch live in `D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE`, aligned to `D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_BYTE_ALIGNMENT` (128), and can share an allocation with acceleration structures. Post-build size via `EmitRaytracingAccelerationStructurePostbuildInfo` / the copy modes.
- **Linkage:** replace `D3D12_RAYTRACING_GEOMETRY_DESC.Triangles` with `OmmTriangles`:
  - `D3D12_RAYTRACING_GEOMETRY_OMM_TRIANGLES_DESC{ pTriangles, pOmmLinkage }`;
  - `D3D12_RAYTRACING_GEOMETRY_OMM_LINKAGE_DESC{ OpacityMicromapIndexBuffer (GPUVA+stride), OpacityMicromapIndexFormat, OpacityMicromapBaseLocation, OpacityMicromapArray }`.
  - Index buffer optional; without it entries map 1:1 to triangles. With it, triangles share blocks (`R8/R16/R32_UINT`) and negative entries encode uniform states (`FULLY_OPAQUE = -2`, `FULLY_TRANSPARENT = -1`, the unknowns `-3/-4`), which avoids storing blocks for the thousands of fully clear or fully solid cards.
  - Index buffer must be in `NON_PIXEL_SHADER_RESOURCE`; the array in `RAYTRACING_ACCELERATION_STRUCTURE`.
- **BLAS flags:** `ALLOW_OMM_LINKAGE_UPDATE` (0x40, named `ALLOW_OMM_UPDATE` in the 26100 headers) only if OMM linkage is re-pointed by a BLAS *update*; not needed if a re-bake rebuilds the BLAS. `ALLOW_DISABLE_OMMS` only if instances need `D3D12_RAYTRACING_INSTANCE_FLAG_DISABLE_OMMS`.
- **Shader opt-in (the blocker for the runtime):** inline tracing needs
  `RayQuery<RAY_FLAG_NONE, RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS>`, which needs **Shader Model
  6.9**, i.e. DXC 1.9 (retail `1.9.2602.16` or newer). Omitting the flag while an OMM is present is
  *undefined behaviour*, not a slow path. For shaders compiled below 6.9 there is a state-object /
  PSO flag (`D3D12_STATE_OBJECT_FLAG_RAYQUERY_ALLOW_OPACITY_MICROMAPS_ON_PRE_SM69_SHADERS`), which
  arrived with the same 1.619 runtime.
- `RAY_FLAG_FORCE_OMM_2_STATE` (0x400) demotes a 4-state mask to 2-state per ray, e.g. for soft
  shadows where precision does not matter.
- OMM only turns transparent into a miss; **`RAY_FLAG_FORCE_OPAQUE` cannot turn it back into a
  hit.** Worth knowing because `TraceClosest(..., alphaTest = false, ...)` uses that path
  (`gi_common.hlsli:517`); today no caller passes `false`, so nothing changes, but that branch would
  stop meaning "every triangle is opaque".

## 4. The blocker: which D3D12 runtime runs

Measured on this machine (2026-10-30) with a standalone probe: a compute PSO built from a `cs_6_9`
DXIL blob that declares `RayQuery<RAY_FLAG_NONE, RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS>`, compiled
by DXC 1.9.2609, run twice — once on the inbox loader, once with the probe exe exporting
`D3D12SDKVersion = 619` and an app-local `D3D12\D3D12Core.dll` (Agility SDK 1.619.6).

| | inbox | Agility 1.619.6 |
| --- | --- | --- |
| `D3D12Core.dll` | 616 (System32) | 619 (app-local `D3D12\`) |
| GPU / driver | RTX 3090, 617.42 | same |
| OS | Windows 11 25H2, build 26200 | same |
| `OPTIONS5.RaytracingTier` | 12 | 12 |
| `SHADER_MODEL` highest | **6.8** | **6.9** |
| OMM array prebuild info | works (640 B result, 896 B scratch for one level-4 4-state block) | works, same |
| PSO from `cs_6_9` + RayQuery OMM | **fails, `0x80070057`** | **builds** |

The failure is not subtle: the inbox runtime rejects the shader outright, *"Shader must be cs_6_8,
cs_6_7, ... Shader version provided: cs_6_9"* (debug layer, `ID3D12Device::CreateComputeShader`).
So the inbox 616 runtime has OMM for `TraceRay` (the 2025 release) but not SM 6.9 at all, and the
tier check alone over-promises: a device can report tier 1.2 and still refuse every shader that
could use an OMM with inline tracing.

Also measured: the Windows SDK's DXC (1.8.2502) rejects `-T cs_6_9`, and `d3d12.h` in SDK 26100 is
the pre-retail OMM shape (`ALLOW_OMM_UPDATE`, separate OMM postbuild info), with no
`RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS`. DXC 1.9.2609 from the GitHub release compiles the shader
fine, and the 619 runtime accepted the 26100 `D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_DESC` in the
prebuild probe, so the old headers can drive the new runtime.

Selecting 1.619 is the hard part. The Agility SDK runtime is chosen only through `D3D12SDKVersion` /
`D3D12SDKPath` **exported from the process's main .exe**, or by `ID3D12SDKConfiguration::SetSDKVersion`
(Developer Mode only; this machine has Developer Mode off). The game is `mgsvtpp.exe`; an add-on
cannot add exports to it, and asking players for Developer Mode is not an option. This is not a GPU
limitation: the RTX 3090 (and every RTX card) reports OMM support, Ada accelerates it in hardware and
pre-Ada resolves it in software ("lightweight ... may still produce a speedup").

**Decision (2026-10-30): capability-gated only.** Ship the DX12 path behind a runtime gate and let it
turn itself on when the OS's inbox `D3D12Core.dll` reaches 619, rather than shipping a runtime or
patching the game. For development and validation, build a *separate probe executable* that exports
the two Agility symbols and carries `D3D12\D3D12Core.dll` next to it; the add-on itself does not, so
the feature stays dormant until the OS catches up. The routes below are kept for the record.

### Routes considered

1. **NVAPI (`NvAPI_D3D12_BuildRaytracingOpacityMicromapArray` and friends, R520+).** Same OMM data,
   built and linked through `NvAPI_D3D12_*` instead of the DXR 1.2 entry points, so it runs on the
   inbox 1.616 runtime. Untested whether NVAPI OMMs work with **inline `RayQuery`** on 1.616 or only
   with `TraceRay` pipelines. *Not taken: NVIDIA-only, and the DX12 path is the one worth owning.*
2. **Ship Agility 1.619 and patch the game's exports in memory.** `D3D12` reads the exe's export
   directory when `D3D12CreateDevice` is called; nothing says the bytes have to come from the file.
   Cost: a PE writer in the add-on, a ~5 MB DLL, and a new failure mode if the loader validates the
   export directory table more strictly than it reads it. *Not taken now; the fallback if the OS takes
   too long.*
3. **Do nothing and keep the shader test.** Not a fallback that needs building, but it sets the bar:
   the toggle must fall back cleanly, and the feature is NVIDIA-only until AMD/Intel report tier 1.2
   (AMD: "future hardware", Intel "evaluating"; neither in the SM 6.9 support table).

If the runtime never arrives, a cheaper quality-neutral idea is to replace the per-candidate texture
fetch with a compact per-triangle alpha bitmask *looked up in the shader* (a software OMM). That
removes the `TextureLod` and the texture fetch from the inner loop but still pays a shader iteration
per hit, so the ceiling is far lower.

## 5. Baking

No asset pipeline, no offline tooling: OMMs have to be generated inside the add-on from what the
scene already has. Everything needed is there:

- vertices and `UV0` (`scene_description.hpp:65`, two halves in each pool vertex);
- the base texture already copied to D3D12, cut-out copies at up to 512 px
  (`gi_textures.hpp:18`, `MaxCutOutSize`), in the game's own BC format;
- the threshold: `CutOutThreshold` 0.5 for rays (`GIGeometry::alphaThreshold`).

**GPU bake, not CPU.** The textures are already BC-compressed and on the GPU (D3D11->D3D11 copy,
shared to D3D12); a CPU baker would need a readback plus its own BC decoder, and would have to match
the D3D12 sRGB view the shader samples. A compute kernel can write the OC1 bitstream straight into
the OMM array's `InputBuffer`:

1. CPU, at BLAS creation (`gi_scene.cpp:323`): per triangle, compute UV area x texture size to pick a
   subdivision level, e.g. `level = clamp(floor(log4(texelsPerTriangle / 2)), 0, 6)` — about two
   texels per micro-triangle, capped at level 6 (4096 micro-triangles, 512 B per 2-state block,
   1 KB per 4-state block) and at a per-mesh budget;
2. CPU: hash the triangle's three UVs + level + format and dedupe. Grass and bush cards in this game
   reuse UVs heavily, so most blocks are shared; a triangle whose three corners are all above the
   threshold gets the `FULLY_OPAQUE` special index and no block at all, all below gets
   `FULLY_TRANSPARENT`;
3. GPU kernel: per block, per micro-triangle, sample the alpha at the micro-triangle's centroid at
   the mip whose footprint matches the micro-triangle, write the bit at its OC1 (barycentric
   space-filling curve) position;
4. barrier, then build the OMM array (`BuildRaytracingAccelerationStructure`) and then the BLAS in
   the same list, before the BLAS build.

**2-state or 4-state.** 4-state, with the existing cone-LOD test as the unknown path. Reason:
today's alpha test is deliberately mip-aware ("the cone a pixel wide at the receiver, not spreading",
`docs/DEVELOPMENT.md`, "Alpha test"), and the project already hit a real bug from a coarse mip
reading thin bars as clear. OMM cannot do per-ray mip selection; NVIDIA's integration guide calls
this "Type 2" (per ray mip bias) and prescribes baking conservatively across mips. So: sample each
micro-triangle at its own footprint mip, mark it opaque only if the whole micro-triangle is above
the threshold at that mip, transparent only if wholly below, unknown otherwise. Classifying the
micro-triangle at the *coarsest* mip the ray cone can pick (the receiver's level) is the
conservative variant; it lowers the known/unknown ratio but cannot lose the thin-bar case. Measure
both; expose no knob, pick one.

**Invalidation.** Cut-out textures are re-copied every 300 frames when the game streams a better mip
(`gi_textures.cpp:22`). Bake against the 512-px copy the rays already see and do not re-bake on
finer streaming; if a copy is *replaced* (different source), mark the mesh's OMM dirty and rebuild
that BLAS. With `PREFER_FAST_TRACE | ALLOW_COMPACTION` statics cannot be updated, so rebuild; the
scene already has a build budget and a compaction queue for exactly this.

**Memory.** Level 6 4-state is 1 KB per triangle before dedupe; a 20k-triangle vegetation mesh with
heavy UV sharing should land in single-digit MB, but the budget must be measured from captures, and
the pool needs a new 128-byte-aligned allocation kind next to structures. Add a hard cap (e.g. 64 MB)
and fall back to the shader test per mesh above it.

## 6. Shader changes

- The only required change is the template argument:
  `RayQuery<RAY_FLAG_NONE, RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS>` in `TraceClosest` (both branches)
  and in `SurfaceRayBlocked`'s two queries.
- `CutOutBlocks` stays as the unknown resolver, unchanged. Its `untextured`/`blocking` behaviour
  only ever applies to geometry with no OMM linkage, so it is untouched.
- **Hair must not get OMMs.** `PassHair` already dims a ray by a fixed share for each hair layer it
  crosses (`gi_common.hlsli:812`, `:842`); if an opaque micro-triangle commits without returning a
  candidate, that partial transmittance silently disappears and hair goes hard-edged under lights.
  `GI_GEOMETRY_HAIR` geometry therefore gets no linkage (or `ALLOW_DISABLE_OMMS` + a disabled
  instance, which is worse: per-instance, not per-ray).
- Leaves are fine: `GI_GEOMETRY_LEAF` only changes how a hit is lit, not whether a ray passes.
- Optionally use `RAY_FLAG_FORCE_OMM_2_STATE` for the sun's shadow rays to trade edge precision for
  RTCore throughput. Start without it.

## 7. Compile and build changes

- DXC: the Windows SDK's `dxc` (1.8.2502 here) cannot compile SM 6.9; `CMakeLists.txt:165-179`
  would need DXC 1.9+ (NuGet `Microsoft.Direct3D.DXC` or a GitHub release), pinned by SHA-256 like
  every other dependency, with `-T cs_6_9`. Mixed shader models are fine: only the shaders that
  declare OMM `RayQuery`s need 6.9, so the rest can stay `cs_6_5` while the change is validated.
- Headers: the SDK 26100 `d3d12.h` is the pre-retail OMM shape. Either take the headers from the
  Agility SDK 1.619 package (which the runtime route already needs) or gate on the newer declarations
  and define the few missing constants (`RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS`,
  `RAY_FLAG_FORCE_OMM_2_STATE`) by hand.
- `ID3D12Device` -> the add-on creates its own device (`d3d12_helpers.hpp:55`), so device creation is
  already in our control; only the runtime selection is not.

## 8. Does it pay here?

Current cost of the alpha test, from this project's own log and docs:

- shadow rays over the brush's vegetation meshes **1.5 to 2.6 ms** with textures; 8 to 47 ms in the
  captured-mesh era;
- the global illumination's ray passes **0.85 ms of 5.1 ms** (probe 0.41, second bounce 0.19, hit
  lighting 0.24).

So the ceiling is a few ms in vegetation-heavy areas, not a frame-wide win, and only where the RT
feature is on. On the dev machine (RTX 3090 = Ampere) OMM is resolved in software: expect small or
no gain, possibly a loss, because driving an emulation costs work the current loop does not. On Ada
and Blackwell (RTX 40/50) the mask is free at the RTCore and the win is the one NVIDIA advertises.
That is a nasty split for a mod whose users are on a mix of cards, and the reason the tier gate and
the menu toggle are not optional.

Measurement plan: A/B in three places with the scene view (F10) and the existing pass timings
(`performance::Pass`) — Mother Base vegetation, an Afghan grass bank, and the hospital lattice at
range — plus a PIX capture with the OMM viewer to check unknown coverage. Ada hardware validation
needs a second machine, not this one.

## 9. Order of work

| # | Step | Time |
| --- | --- | --- |
| 0 | Runtime probe: tier 1.2, SM 6.9, `cs_6_9` RayQuery-OMM PSO on inbox and on a 619 probe exe (done 2026-10-30, see 4) | done |
| 1 | Bake kernel + level heuristic + dedupe + tests (bit packing round-trips, level heuristic, special-index selection) | 3-4 days |
| 2 | Scene: OMM pool (128-byte alignment, budget), linkage per BLAS, dirtiness on texture replacement | 2-3 days |
| 3 | Shaders: the template flag, hair exclusion, `cs_6_9` build wiring, menu toggle, gate (tier + SM 6.9 + trial PSO) | 2 days |
| 4 | Dev probe exe with the Agility exports + `D3D12\D3D12Core.dll`, so the path can be exercised before the OS ships it | 0.5 day |
| 5 | In-game validation, quality tuning (4-state conservative mip), Ada timing | 2-4 days |

The bake work is worth starting because the runtime gate is now measured and the DX12 API shape is
confirmed; the feature will land for users when the inbox `D3D12Core.dll` moves past 616.

## 10. Open questions

- Unknown-coverage ratio for this content at 4-state, level 4-6, conservatively baked across the
  receiver mip: if most micro-triangles come out unknown, the win evaporates.
- OMM array build cost per model load, measured on the compute queue next to the BLAS budget.
- Behaviour when a cut-out texture is replaced while the OMM array is live (the BLAS must be rebuilt,
  not refitted).
- Whether `D3D12SDKVersion` exports on the *add-on's* module are ever consulted (they are not per the
  spec; only the main exe), which decides if the feature can ever be forced on without the OS.
