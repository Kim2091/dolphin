# Dolphin + RTX Remix — portable test build

**{{VERSION}} — {{DATE}}**

GameCube/Wii emulation with **real path tracing**, via a Dolphin video backend
that submits geometry to the NVIDIA RTX Remix runtime.

This is a **work-in-progress test build**, not a finished product. Please read
§2 and §5 before judging it.

---

## 1. Requirements

- Windows 10/11, 64-bit
- **An NVIDIA RTX GPU** — this is path tracing, it will not run on non-RTX
  hardware
- A recent NVIDIA driver
- **A gamepad** — see §2, keyboard input does not currently work in-game
- Your own dumped games. **None are included.**

---

## 2. Read this first: the keyboard does not work while a game runs

**While a game is running, no keyboard input reaches the emulator.** Not
hotkeys (F9 screenshot, F10, Tab), not a keyboard-bound controller.

This is not a Dolphin bug. The Remix runtime registers a raw-input keyboard
device for its own overlay window, and Windows grants that per-process — which
displaces the registration Dolphin's input system relies on. Dolphin then reads
an empty key state forever and never learns anything went wrong.

**Use a gamepad.** Only the keyboard and mouse are affected; controllers work
normally. The Remix dev menu (**Alt+X**) still works, because that is the
runtime's own window reading its own input.

One consequence worth knowing: binding a key in Dolphin's hotkey dialog *looks*
like it works, because with no game running there is no overlay and input is
healthy at that moment. It stops working the instant a game starts.

A fix belongs in the Remix runtime and has not been made yet.

### Also new in v0.0.4: the runtime is `d3d9-remix.dll`, not `d3d9.dll`

If you are updating over an older copy, **delete the old `d3d9.dll`.** Leaving it
there does real harm rather than none.

Qt's Windows plugin has a built-in reference to the name `d3d9.dll`, and Windows
satisfies that from the program's own folder first. So a Remix runtime called
`d3d9.dll` gets loaded and fully started up *while Dolphin's window is still
opening* — about 1.8 seconds before the graphics backend runs, and before you
have picked a game. Everything it decides at that moment is fixed for the whole
session, which makes the per-game files in §4 impossible.

Renaming it fixes that outright, and has a second benefit: Dolphin no longer
loads a 242 MB path tracer into memory when you are using Vulkan or D3D12.

Nothing needs configuring — the build already looks for the new name. An old
`d3d9.dll` left lying around is still found, with a warning in the log.

---

## 3. Running it

1. Unzip anywhere. Nothing is installed.
2. Put your `.iso` / `.rvz` / `.gcm` files in `Games\` (or anywhere you like).
3. Launch:

   ```
   Dolphin.exe --config Dolphin.Core.GFXBackend=Remix -e "Games\YourGame.rvz"
   ```

   Or just run `Dolphin.exe` and pick **Remix** under Graphics → Backend.

`portable.txt` keeps all settings, saves and logs inside `User\` in this folder,
so nothing touches an existing Dolphin install. Delete that file if you would
rather use your system-wide Dolphin configuration.

**Good games to try first** — the ones this build has actually been looked at in:
Wind Waker and SpongeBob: Battle for Bikini Bottom. Super Monkey Ball, F-Zero GX,
Luigi's Mansion and Mario Kart: Double Dash all run and reach 3D quickly.

Menus and HUD now render, so you can navigate normally — with a gamepad, per §2.
Wind Waker and Super Monkey Ball also self-advance into a 3D attract mode after
about 15 and 60 seconds respectively, if you just want to see it draw something.

---

## 4. Settings

Already configured in `User\Config\GFX.ini`. Three matter:

| Key | Value | Why |
|---|---|---|
| `CPUCull` | `False` | Dolphin's CPU culling drops draws before the backend sees them. |
| `RemixCameraRecovery` | `True` | Defaults off upstream. Without it the camera sits at the origin and the world swings around it instead of the camera moving through it. |
| `RemixSkyAutoDetect` | `2` | Detect *and tag* the skybox. At `1` it only logs. |

> Backend options **cannot** be passed on the command line. Only
> `--config Dolphin.Core.GFXBackend=Remix` works there; `--config GFX.Settings.X=Y`
> is silently ignored. Edit `User\Config\GFX.ini` instead.

Settings can also be set **per game**, which is the better place for anything you
only want in one title. Create `User\GameSettings\<GameID>.ini` (e.g. `GZLE01.ini`
for Wind Waker) containing:

```ini
[Video_Settings]
RemixGxLightDropDistant = True
```

### Each game gets its own Remix folder (new in v0.0.4)

Everything the Remix runtime keeps for a game — its settings, its mods, its
captures, its log — now lives in one folder per game:

```
Remix\
    GZLE01\                 the game ID; Wind Waker in this case
        rtx.conf            this game's Remix settings
        user.conf           what the Remix dev menu (Alt+X) saves
        rtx-remix\
            mods\           point the RTX Remix Toolkit here
            captures\
            logs\           the runtime's own log lives here now
```

**Why it exists.** `rtx.conf` stores *hashes* — the fingerprints of individual
textures and meshes. Tag a skybox in one game and that fingerprint means nothing
in another game, or worse, accidentally matches something. When every game shared
one file, a leftover tag from one title deleted another title's sky, silently.
That took a day to find. Now it cannot happen.

**The two `.conf` files next to `Dolphin.exe` are templates.** A new game's folder
is copied from them the first time it is set up, and after that the game is
completely independent — those templates are never read or written again. Put
whatever you want every *new* game to start with in them, and leave them alone
otherwise.

Two things follow from that:

- Editing a template does **not** change games that already exist. To start a
  game over, delete its `rtx.conf` and `user.conf` and launch it again.
- Don't leave texture hashes in a template, or every new game inherits tags from
  a game they have nothing to do with. Dolphin counts them and says so in
  `dolphin.log` when it copies.

There is an **Open Remix Folder** button in Graphics → Remix, and in a game's
Properties, because `GZLE01` is not a name anyone recognises. It creates the
folder if it does not exist yet.

⚠ **Once the RTX Remix Toolkit is pointed at one of these folders, do not rename
or move it.** The Toolkit links your mod project to it with shortcuts that a
rename breaks. (For the same reason this needs to be on an NTFS drive — a USB
stick formatted exFAT cannot host mod projects.)

*Not to be confused with the per-game `.ini` above: that one holds Dolphin's own
backend options, this one holds the Remix runtime's.*

### If the scene looks too blue, or too dark

The sky is now a real emissive surface filling the upper hemisphere, so it lights
the scene — which the original console never did. The balance between it and the
game's own sun is the look control:

- `RemixSkyEmissiveIntensity` (default `1.0`) — sky brightness
- `RemixLightScale` (default `1.0`) — the game's lights

Auto-exposure normalises the total, so it is the **ratio** between the two that
changes the look, not either number alone.

If you are running an atmosphere mod that supplies its own sun, set
`RemixGxLightDropDistant = True` to drop the game's directional lights so the
mod owns the key light. Positional lights (lamps, glows) are kept.

### If it runs slowly

The main lever is **`RemixUiOverlayScale`** in `GFX.ini` (default `1.0`). The
UI/HUD is rasterized on the CPU and is fill-rate bound, so `0.5` quarters its
cost. GameCube UI is authored for a 640×528 framebuffer, so there is little real
detail to lose on a large window.

Everything else is standard RTX Remix — the runtime's own settings menu
(**Alt+X**) has the path tracer's quality and DLSS controls.

### Reporting a problem

`User\Logs\dolphin.log` carries a statistics line for every frame, which is the
most useful thing you can attach. Note it **appends across runs** — delete it
before the run you want to report, so the file contains only that run.

---

## 5. What works, and what does not

Being straight about this, because it is a test build.

**Working**

- Geometry, textures, materials and lighting, path-traced.
- **Per-game Remix files** — each game's settings, mods, captures and log are
  kept apart, so tagging something in one game can no longer affect another
  (§4).
- Camera recovery — the real game camera is recovered from the position-matrix
  palette, so the world stays put and the camera moves through it.
- **The game's own skybox renders**, pushed out around the camera so it no longer
  occludes the world, and unlit the way the console draws it.
- **GameCube colour resolved properly.** GC titles keep their colours in TEV
  registers and use the per-vertex value only as a blend weight between two of
  them, so surfaces that looked plain white — Wind Waker's ocean and sky most
  visibly — now come out the right colour.
- Automatic skybox detection, including untextured domes that no texture list
  could ever reach.
- GX semantics: vertex colour, texgen, blend translation, alpha test (cutouts —
  foliage, fences, billboards), face winding, XF spot and distant lights, textures
  bound to any TEV stage, and the rasterized colour channel taken from the stage
  that actually reads it.
- UI, HUD and menus, via a software rasterizer composited as a screen overlay,
  scaled to the region the console actually presents.
- Sub-screen viewports fold into the instance transform, so a draw the game
  renders through a smaller viewport is no longer placed mid-screen.

**Known broken or missing**

- **Keyboard input, while a game runs.** See §2.
- **Copies of the 3D scene are not reproduced.** Anything a game renders to
  texture from the world and reads back is wrong or absent — heat haze, Wind
  Waker's pictographs, real reflections, picture-in-picture panels. In F-Zero GX
  this is why the position-ladder rings and trackside screens are empty and the
  racer portraits appear in the world instead of in their rings. This is
  deliberate rather than unfinished: for shadow maps, mirrored-camera reflections
  and bloom taps the path tracer already does the real thing, and reproducing the
  game's screen-space fake would put it on top. What is *not* deliberate is the
  scene-content case above, and it remains the biggest gap.
- **Split-screen** is broken, and structurally so: each view carries its own view
  matrix, and Remix takes one world camera per frame. Viewport folding places
  geometry; it cannot supply a second camera.
- **Skinned characters can ghost.** Matrix-palette draws are CPU-transformed and
  re-hash every frame, so they carry no motion vectors.
- **BC-compressed custom texture packs** and mipmaps are dropped.
- **Points and lines** are not submitted.

**Fixed since v0.0.2**

- **Vertex-coloured surfaces are no longer white** — see "GameCube colour" above.
  A texture bound to a later TEV stage was also being dropped entirely, which is
  why Wind Waker's ocean had no water texture either.
- **The sky renders instead of being deleted.** Previously the only way to stop
  the skybox dome blocking everything was to remove it; now it is scaled out
  around the camera and made emissive, which is what the console does.
- **Suns work.** Games build a sun as a spot light parked ~25,000 units away with
  its falloff switched off. Treating that as an ordinary point light applied a
  distance falloff the console never did, so the sun contributed almost nothing
  and scenes rendered dark *with a light in them*. Falloff-free spot lights now
  become distant lights.
- **The white box / white screen in Battle for Bikini Bottom.** The game clears
  the screen by drawing plain white rectangles, and this backend composites 2D
  over the traced image — so a draw meant to sit *underneath* the scene landed
  over it. Both the corner box and the full-screen wash are gone.
- **EFB copies are now classified** rather than universally discarded. Pure-2D
  composed copies execute; scene, depth, intensity and framebuffer copies stay
  discarded on purpose (see above). This is groundwork — on the games tested so
  far it changes nothing visible yet.

Each of those is behind a setting whose `False` position restores the old
behaviour exactly — useful if one of them turns out to be wrong in your game.
See `GFX-settings-reference.ini`.

**Verified where?** The sky, colour and light fixes were confirmed by eye on Wind
Waker; the white-box fix on Battle for Bikini Bottom. Other titles have not been
looked at since these landed. If something regressed in your game, the bisect
knobs above are the fastest way to say which change did it.

---

## 6. Building it yourself

Source: <https://github.com/Kim2091/dolphin>, branch `remix-backend`.
Full build instructions and the complete option reference are in
`Source/Core/VideoBackends/Remix/README.md` in that repository.

Short version — needs **Visual Studio 2026** (v145 toolset) and **CMake 4.x**
(older CMake does not know the pinned `Visual Studio 18 2026` generator):

```
git clone https://github.com/Kim2091/dolphin.git
cd dolphin
git checkout remix-backend
git submodule update --init --recursive
cmake --workflow --preset visualstudio-release-x64
```

Output lands flat in `build\release\x64\Binaries\`. Then copy the Remix runtime
files from this zip in alongside `Dolphin.exe` — that is `d3d9-remix.dll` and
the NVIDIA/USD DLLs plus `usd\`, everything here that is not Dolphin's own
output. **Keep the runtime named `d3d9-remix.dll`** — see §2 for why the name
matters.

**Do not substitute a stock RTX Remix release.** This backend targets a fork with
its own ABI line; a stock runtime fails at load with `INCOMPATIBLE_VERSION`.

---

## 7. Licensing

Dolphin is GPL-2.0-or-later — see `COPYING` and `Licenses\`. The RTX Remix
runtime is NVIDIA's, under its own terms, redistributed unmodified. No game
content is included.
