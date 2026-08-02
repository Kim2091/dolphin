# Remix video backend

A Dolphin video backend that submits GameCube/Wii geometry to the NVIDIA RTX
Remix runtime through the Remix API, so games are path-traced instead of
rasterized.

Requires an **NVIDIA RTX GPU** and Windows x64.

---

## Building

### Prerequisites

- **Visual Studio 2026** (or its Build Tools) with the C++ workload and the
  **v145** toolset.
- **CMake 4.x.** Not optional: the presets pin the `Visual Studio 18 2026`
  generator, and older CMake — including the 3.31 bundled with VS 2022 — does
  not know that generator and fails to configure. VS 2026 Build Tools ships a
  usable copy, typically at
  `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`.
  `cmake` is often not on `PATH`.

### Steps

```
git clone https://github.com/Kim2091/dolphin.git
cd dolphin
git checkout remix-backend
git submodule update --init --recursive

cmake --workflow --preset visualstudio-release-x64
```

or, as two steps:

```
cmake --preset visualstudio-release-x64
cmake --build build\release\x64 --config Release
```

Notes that save time:

- **The submodule step is required.** A `--depth 1` clone pulls no submodules,
  and configuration then fails on lz4, libspng, cubeb and Qt with errors that
  never mention submodules.
- **Run cmake from the repository root.** A relative build path from any other
  working directory fails with a confusing "is not a directory" error naming a
  nested source folder.
- `build\release` is not itself a CMake build directory. The build directory is
  `build\release\x64`.
- Output lands **flat** in `build\release\x64\Binaries\` — there is no `Release\`
  subfolder.

### Working on this backend

This directory is **CMake-only**: it has a `CMakeLists.txt` and no `.vcxproj`,
and there is no Visual Studio solution. Adding a source file means editing
`CMakeLists.txt`.

The Remix API header is vendored at `Externals/remix/remix_c.h`, so building does
**not** require the dxvk-remix source — only the runtime at run time.

---

## The runtime

The backend loads `d3d9.dll` from next to the executable. That DLL **is** the RTX
Remix runtime (~231 MB — it contains the whole path tracer), not a system file
and not a stub. It is distributed separately from this repository.

Two things worth knowing before swapping runtimes:

- **A stock RTX Remix release will not work.** This backend targets a fork
  carrying its own ABI line, `REMIXAPI_VERSION_MINOR 1000`, versus stock 0.6.x;
  a stock runtime fails at load with `INCOMPATIBLE_VERSION`. The reported ABI
  version is also not a reliable identifier — different forks have shipped the
  same `0.1000.0` from different lineages. Identify a build by the version string
  embedded in the DLL instead:
  `grep -aoE "remix-[a-zA-Z0-9._/-]*\+[0-9a-f]{7,}" d3d9.dll`.
- **Do not use a `bridge/_output/d3d9.dll`.** That is the 32-bit bridge client
  (~875 KB). Dolphin is x64 and needs the native runtime. The `.trex/` folder
  some Remix layouts ship is likewise part of the 32-bit bridge path and is not
  needed here.

---

## Running

```
Dolphin.exe --config Dolphin.Core.GFXBackend=Remix -e "path\to\game.rvz"
```

or select **Remix** under Graphics → Backend in the GUI.

**Only** `--config Dolphin.Core.GFXBackend=Remix` works on the command line.
Backend options passed as `--config GFX.Settings.X=Y` are silently ignored; they
must go in `GFX.ini`.

---

## Configuration

Options live in `%APPDATA%\Dolphin Emulator\Config\GFX.ini` under `[Settings]`.
Every knob is declared in `Source/Core/Core/Config/GraphicsSettings.cpp` with a
comment explaining what it is for; most are correctness fixes whose **off
position is exactly the behaviour before the fix existed**, so any of them can be
A/B tested cleanly against a bug.

The three that are not optional in practice:

| Key | Set to | Why |
|---|---|---|
| `CPUCull` | `False` | Dolphin's CPU culling drops draws before the backend sees them. |
| `RemixCameraRecovery` | `True` | Defaults off. GC/Wii have no separate view matrix — `xfmem.posMatrices` hold a combined modelview — so without recovery the camera sits at the origin and the world swings around it instead of the camera moving through it. |
| `RemixSkyAutoDetect` | `2` | Detect *and tag* the skybox; the default `1` only logs. Detection works from the transform (a skybox translates with the camera while its rotation holds still), so it also catches untextured domes no texture-hash list can reach. |

Correctness knobs added by the 2026-08-02 GX audit, all defaulting on, all
reverting to exactly the pre-fix behaviour when set to `False`:

| Key | Default | What it fixes |
|---|---|---|
| `RemixUiScaleToXfb` | `True` | Maps the UI overlay onto the region the console *presents* — the XFB copy's source rect — instead of onto the EFB's full 640×528. Wind Waker presents 480 rows, so the old mapping put every HUD element ~9% too high and left the bottom of the window dead. |
| `RemixGxRasChannel` | `True` | Takes the rasterized colour channel from the TEV stage that actually reads `RasColor`/`RasAlpha`, not from stage 0. GX names that channel per stage, and the consuming stage is routinely not stage 0. |
| `RemixWorldScissorSkip` | `True` | Skips world draws whose scissor result is empty. The world path read scissor state nowhere, so a draw the game hid by scissoring it away was drawn in full — and cast shadows. Only the all-or-nothing case is acted on; a path tracer has no screen-space clip. |

**Performance:** the main lever is `RemixUiOverlayScale` (default `1.0`). The UI
overlay is rasterized on the CPU and is fill-rate bound, so `0.5` quarters its
cost. GC UI is authored for a 640×528 framebuffer, so there is little real detail
to lose on a large window.

**Diagnostics:** `RemixLogStats` (on by default) emits one statistics line per
frame, which needs `Logger.ini` to have `Video = True`, `Verbosity = 4` and
`WriteToFile = True`. `RemixUiDumpFrame = <n>` dumps the composited UI overlay to
`Logs/remix-ui-overlay.bmp`, drawn over a checkerboard so transparent and black
are distinguishable. The log **appends across runs** — delete it before a run you
intend to read.

---

## Status

**Working:** geometry, textures, materials and lighting path-traced; camera
recovery from the position-matrix palette; automatic skybox detection and
suppression; GX semantics (vertex colour, texgen, blend translation, alpha test,
face winding, XF spot and distant lights); UI, HUD and menus via a software
rasterizer composited as a screen overlay.

**Known broken or missing:**

- Split-screen — `xfmem.viewport` is only partly honoured.
- BC-compressed custom texture packs, and mipmaps.
- Points and lines are not submitted.
- Skinned characters can ghost: matrix-palette draws are CPU-transformed and
  re-hash every frame, so they carry no motion vectors.
- **EFB copies are not executed.** Anything a game renders to texture and reads
  back — heat haze, pictographs, some reflections — is wrong or absent.
- Assorted per-game UI artefacts.

Games that reach 3D quickly are easiest to test with: Wind Waker, Super Monkey
Ball, F-Zero GX, Luigi's Mansion, Mario Kart: Double Dash.

---

## Design notes

Geometry is captured at `VertexManagerBase::DrawCurrentBatch`, where the default
`ResetBuffer` hands over CPU-decoded object-space vertices;
`xfmem.posMatrices` is byte-identical in layout to `remixapi_Transform` (3×4),
and textures are already XXH64 content-hashed. Setting
`bSupportsPrimitiveRestart = false` makes quads, strips and fans collapse to
plain triangle lists, so capture never has to parse restart indices.

`bSupportsGPUTextureDecoding = false` means `TextureCacheBase` CPU-decodes every
GC format — CI4/CI8/CI14 with TLUT, CMPR, I/IA — to RGBA8 before upload.

UI draws are orthographic and cannot go through the path tracer sensibly, so they
are rasterized on the CPU and submitted via `remixapi_DrawScreenOverlay`, which
composites at the present boundary with no path tracer, denoiser or camera
involved. Vertices are already CPU-decoded and ortho draws have `w = 1`, so
affine interpolation is exact and no depth buffer is needed.
