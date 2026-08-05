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

The backend loads `d3d9-remix.dll` from next to the executable. That DLL **is**
the RTX Remix runtime (~231 MB — it contains the whole path tracer), not a system
file and not a stub. It is distributed separately from this repository.

### ⚠ Do not name the runtime `d3d9.dll`

Qt's Windows platform plugin, `QtPlugins\platforms\qwindows.dll`, has a real
import on `d3d9.dll`, and Windows resolves an implicit import out of the
**application directory** before `System32`. So a Remix runtime called `d3d9.dll`
next to `Dolphin.exe` is loaded and fully self-initialized while Qt is starting
up — measured at **1.8 seconds before the video backend runs**, before a game has
even been chosen. The runtime resolves its config layers and its file paths once,
at that moment, and refuses to redo them.

Consequences, in order of how much they matter:

- **Per-game files cannot work** (`RemixPerGamePaths`). Nothing Dolphin sets after
  Qt has started can reach the runtime, so every game silently falls back to the
  shared `rtx.conf` — the exact failure the feature exists to prevent.
- A 242 MB path tracer is loaded into **every** Dolphin process, including runs
  using Vulkan or D3D12 that will never touch it.

The fix is a file rename; nothing else. The backend loads the runtime explicitly
and drives it through its `remixapi_*` exports, so the filename carries no
meaning — it is not acting as a `d3d9` stand-in for anything. An install still
carrying `d3d9.dll` keeps working: `RemixApi::Initialize` falls back to it and
logs the warning above, it just cannot have per-game files until it is renamed.

Two more things worth knowing before swapping runtimes:

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

Every knob is editable in the GUI: **Options → Graphics → Remix**. Options are
labelled with a short descriptive phrase rather than the raw INI key, but every
tooltip is titled with the key, and the filter box matches key names as well as
labels — so a name read off a log line or an existing `GFX.ini` can be pasted
straight in to find the option it belongs to.

The same tab appears under **right-click a game → Properties → Game Config →
Graphics → Remix**, which scopes any of these to one title. Per-game values land
in `User\GameSettings\<GameID>.ini` under `[Video_Settings]`, are shown in bold,
and are cleared by right-clicking the control. Editing that file by hand works
just as well.

Global values live in `%APPDATA%\Dolphin Emulator\Config\GFX.ini` under
`[Settings]`. All 61 knobs are declared in
`Source/Core/Core/Config/RemixSettings.cpp` with a comment explaining what each
is for and why its default is what it is; the tables below are a summary, and
that file is the authority. The metadata table at the bottom of the same file is
what generates the GUI tab, so a knob cannot exist without being editable.

Two conventions hold throughout:

- **Most of these are correctness fixes whose `False` position is exactly the
  behaviour before the fix existed.** That is deliberate, so any of them can be
  A/B tested cleanly against a bug without building anything.
- **Changes take effect at backend init**, i.e. when emulation starts. Restart
  the game after editing. The exceptions are seven knobs that are re-read at
  every frame boundary (`RemixApi::RefreshLiveConfig`) and can therefore be
  changed mid-game: `RemixUiMode`, `RemixLogStats`, `RemixTraceProjections`,
  `RemixTraceModelviews`, `RemixTraceColors`, `RemixTraceEfbCopies` and
  `RemixUiDumpFrame`. Everything else is baked into meshes, materials, lights or
  classification state and the GUI greys it out while a game is running.

### Not optional in practice

| Key | Set to | Why |
|---|---|---|
| `CPUCull` | `False` | Not a Remix option — stock Dolphin. Its CPU culling drops draws before the backend ever sees them. |
| `RemixCameraRecovery` | `True` | Defaults off. GC/Wii have no separate view matrix — `xfmem.posMatrices` hold a combined modelview — so without recovery the camera sits at the origin and the world swings around it instead of the camera moving through it. |

### Runtime and scale

| Key | Type | Default | Meaning |
|---|---|---|---|
| `RemixDllPath` | string | `d3d9-remix.dll` | Passed straight to `LoadLibrary`, so a bare name resolves next to `Dolphin.exe`. **Must not be `d3d9.dll`** — see "Do not name the runtime `d3d9.dll`" above; that name is loaded by Qt at startup and breaks per-game files. Left at its default and not found, the backend falls back to `d3d9.dll` with a warning. |
| `RemixSceneScale` | float | `1.0` | Pushed to the runtime as `rtx.sceneScale` — centimetres per GC world unit. |
| `RemixLightScale` | float | `1.0` | Multiplies the radiance derived from XF lights. The lever for "the scene is too dim/bright" once light translation itself is correct. |
| `RemixLightRange` | float | `5000.0` | Stand-in for D3D9's `Light.Range`, which the ported radiance conversion needs and GX does not have. Consulted only when the distance-attenuation polynomial never falls off. |

### Files and per-game folders

Each game gets its own Remix settings, mods, captures and runtime log:

```
<Dolphin.exe dir>\
    rtx.conf                        TEMPLATE — copied into a new game, never read at play time
    user.conf                       TEMPLATE — same
    rtx-remix\mods\                 cross-game mods (see RemixPerGameMods)
    Remix\
        GZLE01\                     one folder per game ID
            rtx.conf                this game's settings, a full independent copy
            user.conf               where the dev menu saves — see below
            rtx-remix\
                mods\               point the Remix Toolkit's project wizard here
                captures\
                logs\               remix-dxvk.log lives here now, NOT in dolphin.log
```

**Why it exists.** `rtx.conf` holds mesh and texture hashes — `rtx.skyBoxTextures`,
the ignore lists, everything tagged from the dev menu. Those hashes mean nothing
outside the game they came from, so one shared file lets a tag written while
playing one title take effect in another. That is not hypothetical: a leftover
skybox tag deleted Wind Waker's sky, silently, and cost a day to find.

**How it works — the globals are templates, not layers.** Dolphin sets five
environment variables just before it loads the runtime:
`DXVK_RTX_CONFIG_FILE`, `DXVK_USER_CONFIG_FILE`, `DEFAULT_MODS_DIR`,
`DXVK_CAPTURE_PATH` and `DXVK_LOG_PATH`. The two config variables point at the
**game's own files and nothing else**.

The `rtx.conf` and `user.conf` next to `Dolphin.exe` are a starting point. The
first time a game's folder is set up they are copied into it; from then on that
game reads and writes only its own pair, and the templates are never loaded
again and never written to. So they can be curated as known-good defaults and
left alone.

Seeding only ever writes a file the game **does not already have**, which is what
makes it safe to run on every boot — re-copying would wipe out everything tagged
in-game since. Two consequences worth knowing:

- **Each game is completely independent.** Deleting something from a game's
  config actually deletes it. Nothing is re-supplied from a layer underneath on
  the next boot, and nothing one game does can reach another.
- **A later edit to a template does not reach games that already exist.** That is
  the deliberate trade for the independence above. To refresh a game from the
  template, delete that game's `rtx.conf` / `user.conf` and start it again.

**`user.conf` is the one that actually matters**, even though `rtx.conf` gets all
the attention. Every edit made through the Remix dev menu targets the *user*
layer — `dxvk_imgui.cpp` wraps them in
`RtxOptionLayerTarget(RtxOptionEditTarget::User)` — so tagging a texture in-game
writes to `user.conf`, not `rtx.conf`. That layer was the only one with no
environment variable, which meant the whole separation collapsed at the exact
moment someone used it. `DXVK_USER_CONFIG_FILE` is a **fork addition** to the
runtime (`rtx_option_layer.cpp` step 6) added for this; a stock runtime ignores
it and puts every game's tags back in one shared file.

**Do not move or rename these folders.** The RTX Remix Toolkit's project wizard
binds a mod project to one with a *pair of symbolic links* — `<project>/deps` →
the `rtx-remix` folder, and `rtx-remix/mods/<project>` → back to the project — so
renaming or deleting one breaks the user's project. Nothing here ever removes or
relocates a folder, and there is no cleanup feature by design. Symlinks also mean
NTFS: a portable build on an exFAT USB stick cannot host mod projects.

**Finding a folder.** `GZLE01` is not a name anyone recognises, so the Remix tab
has an **Open Remix Folder** button. In a game's Properties it opens that game's
folder; in the global dialog it opens the running game's, or the root when
nothing is running. It creates the folder first, which is the point — the Toolkit
has to be able to select it before the game has ever been played.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `RemixPerGamePaths` | bool | `True` | Master switch. `False` restores one shared `rtx.conf` and one shared `rtx-remix\` for every game, exactly as before. |
| `RemixPerGameRoot` | string | *(empty)* | Where the per-game folders live. Empty means `Remix\` next to `Dolphin.exe`, falling back to the Dolphin user folder if that cannot be written to. **Worth setting on a development build**, whose executable sits in `build\release\x64\Binaries\` — cleaning that directory would take mod projects with it. |
| `RemixPerGameMods` | bool | `True` | Whether the game's own `rtx-remix\mods` is the runtime's mods folder. The runtime takes exactly one, so this is a choice, not an addition: with it on, the shared `rtx-remix\mods` next to `Dolphin.exe` is **not searched at all**. Turn it off if you have mods there that every game should see. |

Four things worth knowing when this does not behave:

- **Keep hash lists out of the templates.** `rtx.skyBoxTextures`,
  `rtx.ignoreTextures` and friends are texture and mesh hashes, and a hash means
  something only in the game it was tagged in. Left in a template they get copied
  into every new game folder, where they match nothing — or something else.
  Dolphin counts them and says so in `dolphin.log` at the moment a game is
  seeded (`the template '...' carries N texture/mesh hash entr(ies)`). It cannot
  clean them up: only you know which game each came from.

  Inside a single file, note that hash lists **merge** rather than replace, and
  a `-0x…` entry is the runtime's own *removal* syntax
  (`util_hash_set_layer.h`) — never corruption to be tidied away.
- **If the runtime is still called `d3d9.dll`, none of this applies at all** —
  see the section above. `dolphin.log` says so outright
  (`was already loaded before this game started, so the per-game folders ... are
  NOT in effect`). This is the first thing to check.
- **The runtime log moved.** `remix-dxvk.log` — where `[RTX-API-Cat]` and
  everything else the *runtime* prints goes — is now under the game's
  `rtx-remix\logs\`. `dolphin.log` is unaffected and still names the resolved
  paths at startup (`Remix: DXVK_RTX_CONFIG_FILE = ...`).
- **An environment variable set outside Dolphin wins.** If any of the four is
  already set when Dolphin starts, it is left alone and a line saying so goes
  into `dolphin.log`. That is deliberate — someone who exported `DXVK_LOG_PATH`
  meant it — but it will look like the per-game folder is being ignored.
- **Booting a second game without restarting Dolphin is the risky case.** The
  runtime fixes its file paths the first time it initializes and refuses to
  redo it, so if it stays resident past `FreeLibrary` the second game reads the
  first game's folders. Dolphin checks for exactly that and warns
  (`is still loaded from an earlier game in this session`). Restart Dolphin
  between games if the warning appears.

### Camera recovery

| Key | Type | Default | Meaning |
|---|---|---|---|
| `RemixCameraRecovery` | bool | `False` | Master switch. Off means identity view: camera at the origin, world space *is* camera space. |
| `RemixCameraFromModelview` | bool | `True` | Take the camera from the dominant per-frame modelview slot (`xfmem.posMatrices[0]` on every title measured) instead of from the RANSAC delta estimator. Off restores the estimator exactly. Requires `RemixCameraRecovery`. |
| `RemixViewElectorateFix` | bool | `True` | Let every persisting draw vote on the camera delta, and drop mesh hashes submitted more than once in a frame (ocean tiles, repeated props) since they have no unique cross-frame correspondence. Off is the old 256-entry submission-order slice. |
| `RemixViewHoldOnMiss` | bool | `True` | On a consensus miss, hold the pose the frame started with rather than re-anchoring. A reset re-welds world space onto the current pose, which rotates the whole sky in one frame and can leave the horizon permanently tilted after a pitched cut. |
| `RemixViewTieBreak` | bool | `True` | Prefer the calm-camera hypothesis when two clusters are near-equal in size. |

### Sky

The game's own skybox is rendered, not replaced. Detection is by transform
signature — a skybox translates with the camera while its rotation holds still —
so it also catches untextured domes that no texture-hash list can reach.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `RemixSkyAutoDetect` | int | `1` | `0` off, `1` classify (and push/light per the knobs below), `2` classify *and tag*. **Leave at `1` to render the game's sky.** Mode `2` tags SKY/IGNORE, and on the API path both resolve to *hidden* — see the warning below. |
| `RemixSkyAtInfinity` | bool | `True` | Scale each classified sky instance about the camera position, `x → p + k(x − p)`. Directions from the eye are unchanged, so the image is identical angle-for-angle while the surface moves behind all world geometry. This is the console's "drawn first, no depth write" expressed as geometry. |
| `RemixSkyInfinityScale` | float | `16.0` | The `k` above. Needs to exceed `far_plane / dome_extent` to clear the world (≈10.5 on Wind Waker). |
| `RemixSkyEmissive` | bool | `True` | Give classified sky an unlit (emissive) material carrying the folded TEV constant, with albedo zeroed so emission is the surface's entire output. GX draws a skybox unlit, so its authored colour *is* the pixel; emissive-plus-lit is neither. |
| `RemixSkyEmissiveIntensity` | float | `1.0` | Sky radiance multiplier. `1.0` is the console's colour at face value; higher makes the sky a stronger light source and pushes it above white. This is the closest equivalent to Remix's `rtx.skyForceHDR`, which is unreachable on the API path. |
| `RemixSkyAutoFrames` | int | `30` | Consecutive *informative* frames a candidate must hold the signature before it is classified. |
| `RemixSkyAutoMinExtent` | float | `0.05` | Minimum dome extent as a fraction of the far plane. A GC skybox is a modest dome near the camera (0.09–0.16× far), **not** geometry scaled to clip distance — at `0.25` the size gate alone rejects every real skybox. |
| `RemixSkyTextures` | string | *(empty)* | Comma-separated stage-0 texture hashes to tag as sky manually. |
| `RemixSkyVetoHashes` | string | *(empty)* | Hashes — texture or mesh — never treated as sky. Highest precedence: veto beats the manual list, which beats auto-detection. |
| `RemixSkyAutoUntexturedIgnore` | bool | `True` | Only consulted in mode `2`. Untextured classified draws take IGNORE rather than SKY. |
| `RemixSkyMode` | int | `0` | Legacy depth-state heuristic (`ztest off && zwrite off`). **Superseded and best left at `0`** — it matched 41–88 draws a frame in Wind Waker and never the actual dome, which is depth-tested. |

> ⚠ **Do not tag sky on this backend.** `REMIXAPI_INSTANCE_CATEGORY_BIT_SKY`
> becomes `CameraType::Sky`, which the runtime forces to hidden; the pass that
> would draw it instead (`RtxContext::tryHandleSky`) is D3D9-raster-only and API
> draws never reach it. So SKY means *delete, with nothing in its place* — under
> every `rtx.skyMode`. The same applies to `rtx.skyBoxTextures` in `rtx.conf`,
> and note that for an **untextured** draw those lists key on the **mesh** hash,
> so a dev-menu texture tag can silently delete a dome that has no texture.

### GX semantics

All default on; `False` is the pre-fix behaviour in every case.

| Key | Default | What it fixes |
|---|---|---|
| `RemixGxColor` | `True` | Per-draw material/ambient colour sources (`xfmem.matColor`), which the backend previously dropped. |
| `RemixGxTevColor` | `True` | Evaluates the TEV **colour** chain, with the texture pinned white so the result is exactly the non-texture factor. GC titles keep colour in TEV *registers* and use the vertex colour only as the lerp weight between two of them, so without this a register-coloured surface renders greyscale or white. |
| `RemixGxTextureStage` | `True` | Read the albedo from whichever stage actually samples, plus **its** texture coordinate, when stage 0 samples nothing. Identity decisions (sky lists, untextured test) keep reading stage 0 either way. |
| `RemixGxRasChannel` | `True` | Take the rasterized colour channel from the TEV stage that reads it, not from stage 0. GX names that channel per stage. World draws only. |
| `RemixGxTexGen` | `True` | Non-trivial texture-coordinate generation. |
| `RemixGxBlend` | `True` | Translate GX blend factors to Vulkan ones for the runtime's own classifier, rather than leaving everything opaque. |
| `RemixGxLightFix` | `True` | XF light kinds by attenuation function, calibrated radiance, and the spot cone axis/angle. |
| `RemixGxLightNoFalloffDistant` | `True` | Route a Spot whose distance **and** angular attenuation are both constant to a *distant* light. GX has no directional type, so a sun is an ordinary light parked far away with falloff off; as a sphere it takes a 1/r² the console never applied, contributes nothing, and still suppresses `rtx.fallbackLightMode`. |
| `RemixGxLightDropDistant` | `False` | Drop the game's *directional* lights so an atmosphere mod owns the key light — two suns double up and the game's flat white fights the mod's. Positional lights are kept. Default off, because with no sky mod loaded this removes the scene's only key light. Watch the frame line's `dropped` count. |
| `RemixFallbackLight` | `True` | The backend's own single distant light, drawn only on frames where the scene submitted **no** lights at all — many GC titles bake lighting into vertex colours and enable none. **Not** the runtime's `rtx.fallbackLightMode`; that is a third, separate mechanism in `rtx.conf`. Turn it off whenever a sky mod provides the light; `RemixGxLightDropDistant` already implies off. |
| `RemixWorldScissorSkip` | `True` | Skip world draws whose scissor result is empty. Only the all-or-nothing case — a path tracer has no screen-space clip. |
| `RemixGpuSkinning` | `True` | Submit matrix-palette draws as **object-space** meshes with one bone per vertex plus a per-draw bone palette (`remixapi_MeshInfoSkinning` + `remixapi_InstanceInfoBoneTransformsEXT`), instead of CPU-transforming every vertex by its own `xfmem.posMatrices` row. The bake re-hashes the mesh every frame — new handle, new instance, no BLAS history, no motion vectors — which is what makes skinned characters ghost. Per-vertex bone indices are remapped to compact `0..N-1` in order of first appearance, so the hash survives the game renumbering palette slots. Off is the bake, exactly. Counter: `skinned`. |
| `RemixDynamicMeshIdentity` | `True` | Stable temporal identity for geometry the **game** regenerates every frame (game-CPU-skinned characters — no palette ever reaches this backend, so `RemixGpuSkinning` cannot help them). Identity is keyed on what a re-pose does *not* change — index bytes, per-vertex UVs and colours, vertex count, material — and a topology whose full mesh hash is seen changing across two distinct frames is promoted: its existing handle's vertex bytes are rewritten in place through the runtime's `UpdateMeshBatched`, which refits the BLAS and yields real per-vertex motion vectors, instead of minting a new handle per frame. Skinned and world-UI draws are excluded; update-path draws are kept out of the camera electorate. Degrades to off (one warning) on a runtime without `UpdateMeshBatched`. Off is the old behaviour, byte for byte. Counter: `updated`. |

### Projection and viewport

| Key | Type | Default | Meaning |
|---|---|---|---|
| `RemixProjectionFix` | bool | `True` | Fold each draw's projection difference from the frame's reference into its instance transform, including the off-centre shear terms. A dropped oblique term reads almost exactly like a small camera rotation. |
| `RemixViewportFix` | bool | `True` | The same for `xfmem.viewport`. Without it a sub-screen rect (F-Zero GX position-ladder portraits, PiP panels) renders through the reference rect and lands mid-world. Reduces term-for-term to the projection-only correction when rects match. Inert while `RemixProjectionFix` is off. |

### UI overlay

| Key | Type | Default | Meaning |
|---|---|---|---|
| `RemixUiMode` | int | `1` | `0` drop the game's 2D layer entirely, `1` software-rasterize it into a screen overlay, `2` place it as world-space geometry. Mode `2` is a look experiment — it is genuinely path-traced and swims when the camera moves. |
| `RemixUiOverlayScale` | float | `1.0` | Fraction of the window the overlay is rasterized at. **The main performance lever** — the overlay is CPU-rasterized and fill-rate bound, so `0.5` quarters its cost. GC UI is authored for 640×528, so there is little real detail to lose on a large window. |
| `RemixUiScaleToXfb` | bool | `True` | Map the overlay onto the region the console *presents* (the XFB copy's source rect) instead of the EFB's full 640×528. Wind Waker presents 480 rows, so the old mapping put every element ~9% too high. |
| `RemixUiRasChannel` | bool | `True` | The per-stage RAS-channel rule applied to ortho draws. **This is what removes Wind Waker's leaked title-screen HUD.** |
| `RemixUiDropDstAlpha` | bool | `True` | Skip UI draws whose colour blend factor is `DST_ALPHA`/`ONE_MINUS_DST_ALPHA`. A screen overlay composites at present time, so there is no EFB alpha for the factor to read — the draw is not representable, and falling through to `Over` at full weight produces an opaque wash. |
| `RemixUiDropEfbCopyTextures` | bool | `False` | Filter UI draws textured from EFB-copy RAM. Measured a no-op on Wind Waker (0 of 691,200 pixels), hence off. |
| `RemixUiDropPreWorldBlank` | bool | `True` | Drop **untextured** 2D draws that arrive before any world geometry this frame. Those are EFB clears and scratch fills, not UI: the console draws the scene over them, but this backend composites 2D *on top* of the traced image, so they paint the screen flat. The SpongeBob white-box fix. Real 2D is textured. Counter: `preworld`. |
| `RemixWorldUiDistance` | float | `2.0` | Mode `2` only: distance of the UI plane, in near-plane units. The plane scales with distance so apparent size is unchanged. |
| `RemixWorldUiFlipY` | bool | `False` | Mode `2` only: flip the plane vertically. |

### EFB emulation

The backend keeps a real CPU-side EFB — 640×528, the Software backend's store
reused unchanged. Clears, CPU pokes and CPU peeks all work against it, and every
EFB copy the game triggers is **classified** and then either executed (encoded
into game RAM out of that EFB) or **deliberately discarded**.

Discarding is not a gap; for most classes it is the point. A GameCube game's
baked shadow maps, mirrored-camera water reflections and bloom chains are
screen-space fakes of things the path tracer does natively and better, and
throwing them away is what lets the traced result show through. Discard is also
bit-for-bit what this backend did before any of this existed, so **the
fall-through arm of every classification is discard** and an unrecognised or
misclassified copy can never look worse than the previous build.

| Class | Signal | Exact? | Default | Why |
|---|---|---|---|---|
| **Xfb** | `EFBCopyFormat::XFB` | yes | discard | Presentation copy. The runtime presents; nothing here consumes the XFB image, and it is the only per-frame full-width encode in the feature. |
| **Depth** | source pixel format Z24 | signal exact, purpose inferred | discard | Almost always a shadow map, and the tracer casts real shadows. The EFB's depth plane holds only clear-Z, so executing would hand the game a *uniform* depth map — a full-screen wrong shadow test, worse than absence. |
| **Intensity** | luminance destination format | purpose inferred | discard | Bloom/glow luminance tap, usually riding with `half`. The runtime does its own bloom; a flat clear-luminance only washes the screen. |
| **Scene** | colour copy, ≥1 perspective draw already this frame | **heuristic** | discard | The rect very likely holds world pixels, which this backend never rasterizes — executing paints a flat clear-coloured rectangle where the console had the scene. |
| **Composed2D** | colour copy, **zero** perspective draws so far this frame | yes, by construction | **execute** | With no world draw yet, the console's EFB held clear colour + 2D draws + pokes — exactly what this EFB holds. Render-to-texture menus, title screens, composed text windows. |

| Key | Type | Default | Meaning |
|---|---|---|---|
| `RemixEfbEmulation` | bool | `True` | Master switch over the whole feature: the EFB store, clears, peeks, pokes, classification and any encode. `False` is the pre-feature behaviour in every particular — peeks return 0, pokes and clears do nothing, every class counter reads 0. |
| `RemixEfbCopy2D` | bool | `True` | Execute the **Composed2D** class. The one default here that changes behaviour, because that class's content is exact by construction. |
| `RemixEfbCopyScene` | bool | `False` | Execute the **Scene** class. `True` is the GFX.ini recovery lever for a game that draws world geometry early and then composes a 2D element by copy later in the same frame — no rebuild needed. |
| `RemixEfbCopyDepth` | bool | `False` | Execute the **Depth** class. |
| `RemixEfbCopyIntensity` | bool | `False` | Execute the **Intensity** class, for a game using an intensity copy as a legitimate 2D mask. |
| `RemixEfbXfbEncode` | bool | `False` | Execute the **Xfb** class. Does **not** wire Dolphin's screenshot/AV-dump pipeline to Remix frames. |
| `RemixEfbUiCompose` | bool | `True` | Composite the frame's 2D layer into the EFB before an executed encode, via a second `UiRasterizer` running at 640×528. Without it an executed copy encodes bare clear colour. |
| `RemixEfbSkipDiscardedTex` | bool | `True` | Skip draws whose stage-0 texture samples a **discarded** copy destination, so the game's screen-space fake is absent and the traced result behind it shows. Without it those draws render as blank rectangles over the scene — zero bytes decode into a valid texture, so nothing else refuses them. Confirmed on SpongeBob: Battle for Bikini Bottom (two 256×256 top-left copies per frame, drawn as a white box over a quarter of the screen). Counter: `efbdisc` in the frame line. |

Reading the result. The frame line carries
`efb copies N (… | xfb A depth B int C scene D 2d E | exec F disc G, H us, I folds) | efb peeks J pokes K`,
and on trace frames each copy also prints

```
Remix EFB copy: class scene action discard | rect [...] dst 0x... fmt 4 depth 0 int 0 half 1 yscale 1.000 clear 1 | world-draws 214 ui-draws 0
```

That line names **every input the decision used**, which is the whole point of
it: a depth copy says `depth 1`, a bloom tap says `int 1 half 1`, a mid-gameplay
colour copy says `world-draws` above zero. If a class looks wrong, the knob above
flips its action without a rebuild. `efb_ui_folds` exceeding `exec` means the
fold bookkeeping has broken and copied UI is being double-blended.

**Still broken, by design.** Nothing rasterizes *world* geometry into this EFB —
that is a full software TEV rasterizer, i.e. the Software backend inside this
one, and it is rejected outright. So anything that samples world pixels back
(Wind Waker's pictograph, heat haze, peek-driven logic that inspects the scene)
is wrong under **both** actions: executing gives flat clear colour, discarding
gives zeroes. Frame-late traced depth/colour through the runtime's own readback
is the upgrade path, not something this does.

### Diagnostics

`RemixLogStats` needs `Logger.ini` to have `Video = True`, `Verbosity = 4` and
`WriteToFile = True`, or the per-frame line goes nowhere. **The log appends
across runs** — delete it before a run you intend to read, or you will analyse an
older run's output.

| Key | Type | Default | Emits |
|---|---|---|---|
| `RemixLogStats` | bool | `True` | One statistics line per frame: draw classification, mesh/instance counts, colour routes, lights, UI, sky. The primary instrument. |
| `RemixTraceColors` | bool | `False` | Per-draw TEV chain, both channels' lighting state, resolved args, raw vertex colour bytes and the albedo texture's mean colour. |
| `RemixDebugColorRoutes` | bool | `False` | Paints every world draw a flat colour naming which colour *route* it took (red vertex+folded, magenta vertex+unfolded, blue tFactor, green none). Counters say how many draws took a route; this says **which pixels** — which is usually the actual question. |
| `RemixTraceProjections` | bool | `False` | Per-frame projection variant table. |
| `RemixTraceModelviews` | bool | `True` | Modelview histogram — which matrix the most distinct meshes share, i.e. the camera candidate. |
| `RemixTraceEfbCopies` | bool | `True` | Every EFB copy's rect, destination, XFB flag and clear bit, plus each UI draw's EFB-space footprint. |
| `RemixUiDumpFrame` | int | `0` | Dump the composited overlay to `Logs/remix-ui-overlay.bmp` on frame *n*, drawn over a checkerboard so transparent and black are distinguishable. |

Two diagnostics that are not Dolphin options but belong in the same toolkit:
`rtx.debugView.debugViewIdx = 23` in `rtx.conf` shows **Diffuse Albedo**, which
separates "no geometry is there" from "geometry that isn't being lit" outright;
and `rtx.logApiDrawCategoryKeys = True` reports which key each API draw is
categorised on — writing to the *runtime's* log, which with `RemixPerGamePaths`
on lives at `Remix\<GameID>\rtx-remix\logs\remix-dxvk.log` (and otherwise at
`rtx-remix\logs\remix-dxvk.log` next to the executable), not `dolphin.log`.

Both of those go in `rtx.conf` — which, with per-game files on, means the game's
own `Remix\<GameID>\rtx.conf`, or the global one if you want them everywhere.

---

## Status

**Working:** geometry, textures, materials and lighting path-traced; camera
recovery from the position-matrix palette; **the game's own skybox, rendered as
unlit geometry pushed behind the world**; GX semantics (vertex colour resolved
through the TEV chain, texgen, blend translation, alpha test, face winding, XF
spot and distant lights); UI, HUD and menus via a software rasterizer composited
as a screen overlay.

**Known broken or missing:**

- ⚠ **No keyboard input reaches the emulator while a game is running.** Not
  hotkeys, not keyboard-bound controller input. The Remix runtime lives in
  Dolphin's process and registers a raw-input keyboard device for its overlay
  window (`RIDEV_INPUTSINK | RIDEV_NOLEGACY`, `rtx_overlay_window.cpp`).
  Raw-input registration is per-process per-usage-page, so it displaces
  DirectInput's own registration: DInput then returns `DI_OK` forever with an
  empty state array, never an error, so Dolphin's re-acquire path never fires.
  Measured against the Vulkan backend on the same binary and the same injected
  keys: Vulkan sees them, Remix sees `keysDown 0` always. **Workaround: use a
  gamepad** — only the keyboard and mouse usage pages are hijacked. The fix
  belongs in dxvk-remix (scope the registration to when the dev menu is open, or
  drop it in favour of the runtime's existing legacy WndProc path), not here.

- Split-screen — one world camera cannot express two views. Each half carries its
  own view matrix, and `RemixViewportFix` folds *placement*, not a second camera:
  it puts the reference player's draws in their half and leaves the other
  player's wrong. Sub-screen viewports that share the frame's view — PiP
  portraits, position ladders, viewmodel-style rects — are handled.
- BC-compressed custom texture packs, and mipmaps.
- Points and lines are not submitted.
- Skinned characters go through the runtime's GPU skinning path
  (`RemixGpuSkinning`, on by default): the object-space mesh is submitted once
  with one bone per vertex and the palette rides each instance, so the hash is
  stable and motion vectors are real. Two caveats remain. Normals are
  transformed by the *position* matrix — the GPU kernel has no separate GX
  normal matrix — which is exact for a rigid palette and drifts under
  non-uniform scale. And the runtime skins each BLAS once per frame, so two
  clones of one mesh in the same frame share the first submission's pose; that
  is a runtime-wide limitation, the same one D3D9 titles have. Turning the knob
  off restores the CPU bake, ghost included.
- **EFB copies are classified, not all executed** (see *EFB emulation* above).
  Copies of *2D-composed* content run against a real CPU-side EFB and produce
  real pixels; scene, depth and intensity copies are **deliberately** discarded,
  so the path tracer's own shadows, reflections and bloom replace the game's
  baked versions instead of fighting them. What stays genuinely broken is
  anything that samples **world** content back out — heat haze, Wind Waker's
  pictograph, peek-driven logic that inspects the scene — because nothing
  rasterizes world geometry into that EFB and nothing will.
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
