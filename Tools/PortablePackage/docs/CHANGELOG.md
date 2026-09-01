# Changelog — Dolphin + RTX Remix portable test build

## v0.0.7 — 2026-08-31

Twenty-two changes since v0.0.5, most of them fixing surfaces that arrived in
the wrong colour. If a game looked flat, white or solid-black in v0.0.5, try it
again.

### Colour and materials

Four separate bugs all had the same cause: the backend took a draw's surface
texture from TEV stage 0, and plenty of games do not keep it there.

- **Characters are no longer near-white.** A stage can have a texture enabled
  and never reference it. The albedo now comes from the first stage that
  actually samples *and* uses one (`RemixGxUnusedStageAlbedoSkip`). Measured on
  Super Mario Galaxy, where Mario wore a 64×64 white mask while his real
  128×256 texture sat unused.
- **World geometry is no longer flat solid colour.** Games that put an
  environment map on stage 0 had their reflection used as the surface texture.
  Measured on Skyward Sword.
- **Cel-shaded characters are no longer painted in their gradient.** Same bug
  with a toon ramp on stage 0 — Wind Waker's characters rendered flat yellow.
- **Surfaces lit through a Color0/Color1 texgen are no longer flat.** The
  coordinate is generated from a *lit* colour channel; substituting the raw
  vertex colour collapsed every vertex onto one ramp texel.
- **Foliage, fences and hair are cut out instead of solid rectangles.** A
  depth-writing SrcAlpha/InvSrcAlpha blend is coverage, not translucency, and
  now resolves as an alpha test. Where the alpha mask lives on a different
  texture than the colour, the two are composed into a derived texture
  (`RemixGxAlphaMask`). Measured on Resident Evil 4.
- **Per-pixel stencil effects work.** The EFB alpha-mask idiom — prime alpha in
  a colourless pass, then blend against it — survives translation now.

### HUD and UI

- **Fixed the screen going solid black with only the HUD visible**
  (Skyward Sword). Additive overlay draws now contribute coverage matching the
  light they add, instead of replacing the traced image.
- **Fixed the overlay painting solid white over the world** in Mario Kart:
  Double Dash, whose in-race clear is a full-screen quad carrying a 4×4 texture
  and so passed a test that only rejected untextured draws.
- **Menus no longer crawl at high resolutions.** F-Zero GX's menu cost 84 ms of
  CPU fill per frame at 2560 wide. The overlay surface is re-derived every
  frame, so window resizes and `RemixUiOverlayScale` now apply immediately, and
  `RemixUiOverlayCap` bounds the cost.
- **Fixed a use-after-free** where the UI rasterizer could read texels the
  texture cache had already evicted.

### Camera

- **Fixed the view showing only the top-left quarter, magnified** in games that
  render an off-screen helper pass first (Sonic Unleashed). The reference
  viewport is now latched from the presented region.
- **Mirrored cameras keep their reflection** instead of being silently flipped
  back (`RemixGxPreserveHandedness`, default off). Star Wars: The Force
  Unleashed hands us a mirrored basis every frame.
- **Aspect ratio is correct**, taken from Remix's swapchain rather than a
  placeholder backbuffer.
- A classified skybox no longer poisons the camera's translation consensus at
  `RemixSkyAutoDetect` 1.

### Per-game behaviour

- **Dolphin now restarts itself between games on the Remix backend.** The
  runtime initializes its crash handling, input hooks, overlay and per-game
  folders once per process; a fresh process is the only reliable way to give
  the next game its own. Expect a brief relaunch when you stop a game.
- Per-game config, mods and log folders are now set unconditionally, fixing
  every game after the first inheriting the previous game's folders.

### Dev menu

- The click-to-tag world view can follow the dev menu automatically. Turning
  the preference off returns to whatever you last chose by hand.

### Diagnostics

- `RemixLogStats` now reports what the backend holds on the GPU: live textures
  and bytes uploaded, how many were derived alpha-mask or masked-albedo
  textures, live materials, live meshes with vertex/index bytes, and running
  made/reaped totals. Textures and materials are never reaped, so a total that
  keeps climbing after a scene settles is a leak by definition.
- Fixed a statistics counter underflowing to four billion.

### Documentation

- Every setting this branch adds now has a row in the backend README, and each
  tooltip names the game its rule was verified against.

### Known issues

- Super Monkey Ball's title/menu 2D layer still has minor layering quirks.
- See README §4 for the standing limitations of the backend.

Requires the bundled runtime. Do not substitute a stock RTX Remix release.

## v0.0.5 — 2026-08-05

### Characters and animation
- **Animated characters no longer leave ghost trails.** Two fixes work together:
  characters the console skins on the GPU (matrix-palette draws) are now skinned
  by the Remix runtime with real motion vectors, and characters the game animates
  on the CPU keep a stable mesh identity from frame to frame instead of counting
  as a brand-new mesh every frame.
- Because their mesh identity is now stable, animated characters can be **tagged
  in the dev menu and replaced with USD assets** for the first time.
- **Taking a capture with skinned characters in the scene no longer crashes.**

### HUD and UI
- **The dev menu's texture tags now control how UI is drawn.** Tag a texture
  *UI Texture* to pin it to the screen overlay, *Ignore* to hide it, or
  *World Space UI* to keep it in the 3D world. This works for normal 2D UI **and**
  for HUDs that games draw as 3D geometry parked in front of the camera
  (Resident Evil 4 style). Tags win over every built-in heuristic.
- New **click-to-tag world view** (`RemixUiWorldView`, live toggle): flip it on to
  click and tag any UI element — including untextured boxes that have no thumbnail
  in the grid — then flip it off to play. It no longer triggers automatically
  every time the dev menu opens.
- The 2D overlay now honours each draw's **real depth mode** (`RemixUiDepth`,
  on by default), so HUDs that rely on the console's z-buffer instead of draw
  order layer correctly.
- **Additive HUD effects no longer erase what's underneath them** — glows drawn
  with SrcAlpha/One blending (for example RE4's ammo-counter glow) now add light
  instead of punching a hole in the digits below.
- **Fixed the world strobing black** in games that present UI-only frames between
  world frames (`RemixSkipMinorFrames`, per-game opt-in — fixes Skylanders:
  Spyro's Adventure).

### Materials and tagging
- New **"Make Emissive"** texture category with its own intensity slider
  (multiplies with the global emissive intensity).
- **"Ignore Alpha Channel" now works** for GameCube/Wii games (it was silently
  doing nothing on the Remix-API path).

### Mesh replacements
- Replacements anchored to a game mesh now **despawn and respawn with their
  anchor** instead of lingering in the world.
- An anchor kept visible with *includeOriginal* now **keeps its real material**
  instead of rendering solid white while the enhancement is active.

### Known issues
- Super Monkey Ball's title/menu 2D layer still has minor layering quirks;
  under investigation. The game itself is fully playable.
- See README §4 for the standing limitations of the backend.

Requires the bundled runtime: this build calls a new runtime API
(`GetTextureHashList`) that only exists in the d3d9-remix.dll shipped here.

## v0.0.4 — 2026-08-04
- Each game gets its own Remix config, mods, captures and logs under
  `Remix\<GAMEID>\` next to the emulator.
- The runtime was renamed `d3d9.dll` → `d3d9-remix.dll` (load-bearing, see
  README §2).

## v0.0.3 and earlier — 2026-08-03
- Predate this changelog.
