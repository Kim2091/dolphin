// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Config/RemixSettings.h"

#include <array>

namespace Config
{
// Remix video backend. RemixDllPath is passed straight to LoadLibrary, so the
// bare name resolves next to Dolphin.exe; RemixSceneScale is pushed to the
// runtime as rtx.sceneScale (centimetres per GC world unit) and RemixLightScale
// multiplies the radiance derived from XF lights.
//
// ⚠ THE FILENAME IS LOAD-BEARING AND MUST NOT BE `d3d9.dll`. Qt's Windows
// platform plugin (QtPlugins\platforms\qwindows.dll) has a real import on
// d3d9.dll, and Windows resolves an implicit import out of the APPLICATION
// directory first - so a Remix runtime named d3d9.dll next to Dolphin.exe is
// loaded and fully self-initialized while Qt is starting up, before any game is
// chosen. Measured: the runtime parsed its config layers 1.8 seconds before the
// video backend ran. Nothing set afterwards can reach it, which silently
// disables per-game files (RemixPerGamePaths) altogether.
//
// Any other name is fine - the backend loads it explicitly and calls it through
// its remixapi_* exports, so nothing depends on it being a d3d9 stand-in. Doing
// this also stops a 242 MB path tracer from being loaded into every Dolphin
// process regardless of which backend is selected.
//
// A runtime still named d3d9.dll is found anyway (RemixApi::Initialize falls
// back to it and says so), so an existing install keeps working - it just cannot
// have per-game files until the file is renamed.
const Info<std::string> GFX_REMIX_DLL_PATH{{System::GFX, "Settings", "RemixDllPath"},
                                           "d3d9-remix.dll"};
const Info<float> GFX_REMIX_SCENE_SCALE{{System::GFX, "Settings", "RemixSceneScale"}, 1.0f};
const Info<float> GFX_REMIX_LIGHT_SCALE{{System::GFX, "Settings", "RemixLightScale"}, 1.0f};
const Info<bool> GFX_REMIX_LOG_STATS{{System::GFX, "Settings", "RemixLogStats"}, true};
// What to do with draws the sky heuristic matches:
//   0 = nothing (submit them as ordinary world geometry)
//   1 = tag REMIXAPI_INSTANCE_CATEGORY_BIT_SKY
//   2 = drop them entirely, leaving clear sky for Remix's own atmosphere
// 2 exists because tagging alone still hands Remix geometry to render as the
// skybox, which can occlude a replacement atmosphere just as the raw draw did.
const Info<int> GFX_REMIX_SKY_MODE{{System::GFX, "Settings", "RemixSkyMode"}, 0};
// Comma-separated stage-0 texture hashes (as logged by the Remix backend, e.g.
// "0x8b1d0f1752d9a3c1,0x…") whose draws are the skybox. Explicit hashes beat a
// depth-state heuristic: the runtime's own texture-grid categories cannot reach
// API-submitted draws, because cameraType is frozen from the instance's API
// category flags before the grid's texture-category lookup ever runs. Passing
// the SKY bit ourselves is the path that does work.
const Info<std::string> GFX_REMIX_SKY_TEXTURES{{System::GFX, "Settings", "RemixSkyTextures"}, ""};
// Remix takes one camera per frame, but GX projection state is per draw and
// Dolphin flushes the batch whenever it changes. Folding each draw's projection
// difference into its instance transform is what keeps mid-frame projection
// switches - and the off-centre raw[1]/raw[3] shear terms, which the
// parameterized camera cannot express at all - from moving geometry on screen.
// Off is the pre-fix behaviour: first perspective projection of the frame wins
// and every other draw is rendered through it.
const Info<bool> GFX_REMIX_PROJECTION_FIX{{System::GFX, "Settings", "RemixProjectionFix"}, true};
// The same problem one step further out. GX maps clip space to the EFB per draw
// through xfmem.viewport - screen.x = (clip.x/clip.w)*wd + xOrig
// (Clipper.cpp:553-554) - so a game that renders a picture-in-picture panel or a
// position ladder gives those draws their own small screen rect. Remix's one
// camera renders the reference draw's rect, so folding the difference between
// the two rects into the instance transform is what puts such a draw in its own
// corner of the screen instead of in the middle of the world. It rides the same
// affine correction as the projection fold and reduces to it exactly when the
// rects match, so a game that never moves its viewport is bit-identical either
// way.
//
// Off is the pre-fix behaviour: the world path reads xfmem.viewport for the
// face-winding sign and nothing else, and every sub-screen draw is rendered
// full-screen. Split screens stay wrong with it either way - a second view
// carries a second view matrix, which one camera cannot express no matter where
// the geometry is folded.
const Info<bool> GFX_REMIX_VIEWPORT_FIX{{System::GFX, "Settings", "RemixViewportFix"}, true};
// Which draw is allowed to define the frame's reference - the projection and
// viewport Remix's single camera renders, and the rect every other draw's
// viewport correction is measured against. Off is first-perspective-draw-wins,
// which breaks any game that renders an off-screen helper pass before its main
// scene: Sonic Unleashed draws 27 helper draws into the top-left 320x240
// quarter of the EFB first, so that quarter latched as "the screen" and the
// real 640x480 scene was folded out of the frustum - only its top-left quarter
// stayed visible, magnified 2x.
//
// On, a draw may only latch the reference if its viewport covers more than half
// of the region the previous frame's XFB copy presented. The XFB rect is the
// ground truth for "the screen" (a game that renders 512x448 in a corner and
// presents exactly that rect - which exists, and is correct today - keeps its
// sub-rect reference), and the previous frame's is used because the copy that
// ends the frame is the only place the rect is knowable. Games whose first
// perspective draw already covers the presented region latch identically with
// this on or off, which is the safety argument for the default. When no
// viewport covered the presented rect at all last frame (split screen, menus),
// the gate stands down and first-draw-wins returns.
const Info<bool> GFX_REMIX_VIEWPORT_REF_XFB{{System::GFX, "Settings", "RemixViewportRefXfb"},
                                            true};
// Log every distinct projection seen per frame, every frame. The per-frame
// summary already reports variants whenever there is more than one (or any
// off-centre term), so this is only needed to watch a projection change live.
const Info<bool> GFX_REMIX_TRACE_PROJECTIONS{
    {System::GFX, "Settings", "RemixTraceProjections"}, false};
// Histogram every draw's modelview by VALUE and report the one shared by the
// most distinct meshes. Pure instrument: nothing reads the result.
//
// It exists to answer one question. GX has no view matrix, so the backend
// ESTIMATES one from inter-frame deltas - but a modelview is V*M, and for any
// object whose model transform is the identity that product IS V. World-authored
// geometry (terrain, rooms, the sea) is very often drawn exactly that way, so V
// is probably sitting in xfmem.posMatrices as a literal value and the only real
// question is which slot. A histogram answers it from every draw in the frame,
// where the estimator votes with the few dozen meshes that happen to persist.
//
// The dominant value alone is not proof - a room full of props drawn in one
// room-local space would also dominate - so the log line carries the two checks
// that separate the cases: whether the value is RIGID (a view matrix carries no
// model scale), and what fraction of objects hold still when it is used as the
// camera, measured against the estimator's own number on the same objects.
const Info<bool> GFX_REMIX_TRACE_MODELVIEWS{
    {System::GFX, "Settings", "RemixTraceModelviews"}, true};
// Take the camera from the histogram's dominant modelview instead of estimating
// it from inter-frame deltas. Needs RemixCameraRecovery on; off restores the
// estimator exactly, so the two are a clean A/B.
//
// The estimator's problem was never its consensus rule, it was its evidence: it
// votes with the few dozen meshes that persist across a frame boundary AND clear
// a vertex floor, and when that electorate goes bad it invents camera motion out
// of nothing - measured on Wind Waker at 0.66-1.25 deg/frame and hundreds of
// units of translation across 90 frames during which the game's own view matrix
// did not change to four decimal places. Every instance carries V^-1, so that
// error swings the entire world around the viewer.
//
// This reads the answer instead. For an object drawn with an identity model
// transform the combined modelview IS the view matrix, and Wind Waker puts it in
// posMatrices slot 0 on every frame measured, rigid, shared by 160-293 meshes
// against a runner-up of 14. A gate (mesh count, 2x dominance, rigidity) decides
// per frame whether to believe it; a miss holds the previous pose.
const Info<bool> GFX_REMIX_CAMERA_FROM_MODELVIEW{
    {System::GFX, "Settings", "RemixCameraFromModelview"}, true};
// How orthographic draws - HUD, menus, every 2D screen - are handled.
//   0 = dropped. What v1 did, and why the path-traced output had no 2D in it.
//   1 = software-rasterized into a screen overlay, composited at present. (default)
//   2 = submitted as world-space geometry on a plane in front of the camera.
//
// Mode 1 is what looks like a normal UI, because it IS one: the pixels are drawn
// by a small software rasterizer (RemixUiRaster) and handed to
// remixapi_DrawScreenOverlay, which the runtime composites after the frame is
// traced and denoised. Nothing about the UI touches the path tracer.
//
// Mode 2 was the first attempt and is kept only because it does something mode 1
// cannot - it puts the UI inside the traced world, where it can light the scene
// and be viewed at an angle. It is NOT passthrough: the runtime treats it as
// geometry, so it is denoised, and being welded to the camera it swims whenever
// the view moves. REMIXAPI_INSTANCE_CATEGORY_BIT_WORLD_UI patches the material
// to emissive-from-albedo (rtx_instance_manager.cpp:1116-1123) so at least it is
// unlit, but that does not make it a HUD.
const Info<int> GFX_REMIX_UI_MODE{{System::GFX, "Settings", "RemixUiMode"}, 1};
// Resolution of the mode-1 overlay, as a fraction of the render window. The
// rasterizer is fill-rate bound and this is the only lever with a linear effect
// on its cost: halving it quarters the pixels.
//
// 1.0 costs 3-9 ms a frame on Wind Waker's busiest 2D screens even threaded,
// which is most of a 60 Hz budget, so this is the knob to reach for on a slow
// machine. The runtime scales the overlay to the output when compositing, so the
// only cost is sharpness - and GC UI is authored for a 640x528 framebuffer, so
// there is not much real detail to lose below 1.0 on a high-resolution window.
const Info<float> GFX_REMIX_UI_OVERLAY_SCALE{{System::GFX, "Settings", "RemixUiOverlayScale"},
                                             1.0f};
// Cap the overlay surface at twice the console's own 640x528, preserving
// aspect, however large the window. GC UI is authored at that resolution, so
// past 2x the extra pixels buy edge sharpness the art does not contain while
// the fill cost keeps growing with the window squared - F-Zero GX's menu is
// ~47 screens of overlapping fill a frame, 84 ms of CPU at a 2560-wide window
// and 15 ms at 960. Off restores surface = window * RemixUiOverlayScale
// exactly as before.
const Info<bool> GFX_REMIX_UI_OVERLAY_CAP{{System::GFX, "Settings", "RemixUiOverlayCap"}, true};
// Skip re-rasterizing a frame whose recorded 2D draws are bit-identical to the
// previous frame's (same draws, order, textures by content hash, surface size,
// filter verdict) and serve the previous composite instead. Menus and pause
// screens hold still for hundreds of frames; this removes their entire raster
// cost. Off is the pre-cache behaviour byte for byte.
const Info<bool> GFX_REMIX_UI_FRAME_CACHE{{System::GFX, "Settings", "RemixUiFrameCache"}, true};
// Non-zero writes the composited UI overlay at that frame index to
// Logs/remix-ui-overlay.bmp, once, over a checkerboard so transparent and black
// are distinguishable. Diagnostic only - it is the only way to see what this
// backend actually produced without trusting the screen.
const Info<int> GFX_REMIX_UI_DUMP_FRAME{{System::GFX, "Settings", "RemixUiDumpFrame"}, 0};
// Mode 2 only. The mapping from a draw's screen space onto the plane is exactly
// affine, so it rides the INSTANCE transform rather than being baked into
// vertices, which keeps one mesh handle per UI element instead of re-hashing it
// every time the camera moves:
//   ndc = (raw0*x + raw1, raw2*y + raw3, raw4*z + raw5)   [w = 1]
// and a point at that ndc, distance d along the camera's forward, is
//   C + F*d + R*(ndc.x * d*tan(fovY/2)*aspect) + U*(ndc.y * d*tan(fovY/2))
// which composes with the draw's own modelview into a single 3x4.
//
// How far in front of the camera that plane sits, in game units. Just past the
// near plane by default: the game guarantees nothing it draws is nearer than
// that, so world geometry cannot poke through the HUD. Raise it only if the UI
// is being occluded; the plane scales with distance so its apparent size does
// not change.
const Info<float> GFX_REMIX_WORLD_UI_DISTANCE{{System::GFX, "Settings", "RemixWorldUiDistance"},
                                              2.0f};
// Mirror the UI vertically. GX ndc y points up (Dolphin's vertex shader negates
// it on the way out, for APIs whose y points down - VertexShaderGen.cpp:876), so
// the default should be right. This exists because an upside-down HUD is a
// one-bit mistake that otherwise costs a rebuild to test.
const Info<bool> GFX_REMIX_WORLD_UI_FLIP_Y{{System::GFX, "Settings", "RemixWorldUiFlipY"}, false};
// GX has no view matrix - posMatrices are combined object-to-view - so by
// default the backend submits an identity camera and lets instances carry the
// modelview, making Remix's world space the same thing as camera space. That
// costs every temporal feature: motion vectors are meaningless and RTXDI /
// ReSTIR / the denoiser all see the whole world move whenever the camera does.
// Enabling this recovers a real camera from inter-frame modelview deltas so
// world space holds still. Off is byte-identical to the identity-view path.
const Info<bool> GFX_REMIX_CAMERA_RECOVERY{{System::GFX, "Settings", "RemixCameraRecovery"},
                                           false};
// Resolve what TEV stage 0 actually rasterizes as its colour - the channel named
// by tevorders, and either the vertex colour or the xfmem.matColor register
// depending on that channel's material source - and hand it to Remix as a
// texture-stage argument so it modulates albedo. Off leaves the runtime at its
// defaults, where the vertex colour reaches the geometry buffer but nothing ever
// reads it and a register tint is lost outright.
const Info<bool> GFX_REMIX_GX_COLOR{{System::GFX, "Settings", "RemixGxColor"}, true};
// Run GX texture coordinate generation instead of passing vertex attribute 0
// through raw: the texgen slot TEV stage 0 actually samples, its source row, and
// the xfmem texture matrix - which is how every scrolling or animated texture on
// the console works, and is frozen without this. Off is the raw passthrough.
const Info<bool> GFX_REMIX_GX_TEXGEN{{System::GFX, "Settings", "RemixGxTexGen"}, true};
// Translate the draw's GX blend state and hand it to the runtime's own legacy
// blend classifier, so fire, glows, light shafts, windows and water stop being
// submitted as opaque geometry that also casts full shadows. Off submits
// everything opaque, which is what the backend did before.
const Info<bool> GFX_REMIX_GX_BLEND{{System::GFX, "Settings", "RemixGxBlend"}, true};
// Translate GX lights the way the console's own renderers read them, instead of
// approximately. Three things change: a spot cone is aimed along -ddir (xfmem's
// ddir points from the scene TOWARD the light, so the pre-fix cone faced
// backwards), the cosatt polynomial's inner edge becomes a real coneSoftness
// instead of a hard 0, and radiance is derived from the distance attenuation the
// way the Remix runtime's own D3D9 legacy-light conversion does it rather than
// being handed the raw 0-1 colour, which is roughly a hundred times too dim.
// Off reproduces the pre-fix behaviour exactly.
const Info<bool> GFX_REMIX_GX_LIGHT_FIX{{System::GFX, "Settings", "RemixGxLightFix"}, true};
// Distance, in GC world units, at which a light with NO distance attenuation
// (GX_DA_OFF leaves distatt = (1,0,0), so the polynomial never falls off) is
// considered to have ended. It stands in for D3D9's Light.Range, which the
// radiance conversion needs and GX simply does not have.
const Info<float> GFX_REMIX_LIGHT_RANGE{{System::GFX, "Settings", "RemixLightRange"}, 5000.0f};
// Let every persisting draw vote on the camera delta, and drop the ones that
// cannot mean anything. The sample map was capped at 256 entries, which on a
// scene submitting well over a thousand instances makes the electorate an
// arbitrary submission-order slice; and a mesh hash submitted more than once in
// a frame (ocean tiles, repeated props) has no unique cross-frame
// correspondence, so the delta built from it pairs two arbitrary instances.
// Off is the 256-entry cap plus keep-first, exactly the pre-fix behaviour.
const Info<bool> GFX_REMIX_VIEW_ELECTORATE_FIX{
    {System::GFX, "Settings", "RemixViewElectorateFix"}, true};
// On a run of frames the estimator could not read, keep the view it already has
// instead of resetting it to the identity. Both are equally correct for
// geometry - the image is invariant to the view and the world origin is
// arbitrary - but a reset re-welds world space onto the CURRENT camera pose,
// which rotates the entire replacement sky in a single frame and, after a
// pitched or rolled cut, leaves its horizon permanently tilted to that pose.
// Off is the reset, which is what the backend did before.
const Info<bool> GFX_REMIX_VIEW_HOLD_ON_MISS{{System::GFX, "Settings", "RemixViewHoldOnMiss"},
                                             true};
// When the two largest hypothesis clusters are within a quarter of each other's
// inlier count, take the one closer to "the camera did not move" instead of the
// merely-bigger one. A genuinely turning camera makes every static draw vote
// together and never gets here; two comparable clusters mean a large rigid
// animated object arguing with the static world, and the camera is not the part
// of a title screen swinging around. Off is pure max-inliers.
//
// Measured on Wind Waker's 3D window, where it is what stops the camera
// inventing motion. Frames 780-960, pure max-inliers: the estimator claims
// 1.20-1.30 deg of rotation and 330-434 units of translation EVERY frame,
// forever, so the recovered basis rotates without end - which is precisely the
// "sky swings while the geometry holds still" symptom. With the tie-break: 0.06
// to 0.13 deg and 57-62 units, and the absolute drift plateaus instead of
// climbing. The classifier agrees independently: the sky signature only resolves
// under this estimate, and when it does it names exactly the frame's first draws
// including both hand-tagged sky textures.
//
// Do NOT read stable W alone here. It is HIGHER with the tie-break off
// (37-41% vs 18-19%) because it measures agreement with whichever cluster won,
// not whether that cluster was the camera - so an estimator riding a large
// coherent moving object scores well on it. That is the trap this default was
// briefly flipped into and back out of.
const Info<bool> GFX_REMIX_VIEW_TIE_BREAK{{System::GFX, "Settings", "RemixViewTieBreak"}, true};
// Identify skyboxes by the one property that defines them: they translate with
// the camera. Under camera recovery a skybox's recovered world transform slides
// with the camera position while its rotation holds still, so the frame-to-frame
// ratio is a pure translation equal to the camera's own position delta - which
// static world geometry (ratio = identity) and camera-welded overlays (rotation
// tracks the camera) both fail. This needs RemixCameraRecovery on, and it can
// only make progress on frames where the camera actually translates.
//   0 = off, manual RemixSkyTextures only - exactly the pre-feature behaviour
//   1 = classify, count and log, but tag nothing (the default)
//   2 = classify and tag matching draws as sky
// Nothing about the retired depth-state heuristic is involved; that is
// RemixSkyMode, and it stays off.
const Info<int> GFX_REMIX_SKY_AUTO_DETECT{{System::GFX, "Settings", "RemixSkyAutoDetect"}, 1};
// Consecutive informative frames a mesh has to satisfy the signature before it
// is classified. Classification is sticky for the session: sky must not flicker,
// and a restart clears the set.
const Info<int> GFX_REMIX_SKY_AUTO_FRAMES{{System::GFX, "Settings", "RemixSkyAutoFrames"}, 30};
// Minimum size, as a fraction of the frame's far plane, for a draw to be
// considered a skybox. A camera-welded view model is small, and during a
// translation-only window this is the only thing separating it from a dome.
//
// The value is empirical and NOT a quarter of the far plane, which was the first
// guess: Wind Waker's seven real sky draws measure 0.09x to 0.16x of a 160000
// far plane, because a GC skybox is a modest dome drawn near the camera rather
// than something scaled out to the clip distance. At 0.25 the gate rejected
// every genuine skybox in the game and the feature classified nothing at all.
// 0.05 keeps a real gate with room to spare under the smallest true positive.
const Info<float> GFX_REMIX_SKY_AUTO_MIN_EXTENT{
    {System::GFX, "Settings", "RemixSkyAutoMinExtent"}, 0.05f};
// Hashes - texture or mesh, same format as RemixSkyTextures - that are never
// treated as sky, for the day the classifier is wrong about something. Highest
// precedence: veto beats the manual list, which beats auto-detection.
const Info<std::string> GFX_REMIX_SKY_VETO_HASHES{
    {System::GFX, "Settings", "RemixSkyVetoHashes"}, ""};
// Auto-classified draws that carry NO texture are tagged IGNORE rather than SKY.
//
// The SKY tag routes a draw to CameraType::Sky, which under rtx.skyMode = 1 is
// meant to drop it. IGNORE removes it from the scene outright. The difference
// matters for the untextured draws specifically: four of Wind Waker's seven
// classified sky meshes have no texture at all, and if such a dome survives as a
// closed shell around the viewpoint it occludes the Numos distant sun no matter
// what the sky path does with it - which reads as a dark scene rather than as a
// sky problem. Untextured is the right discriminator because those draws carry
// no albedo worth keeping, whereas a textured sky dome is exactly what the sky
// path is designed to consume.
//
// False = tag every auto-classified draw SKY, the pre-flag behaviour.
const Info<bool> GFX_REMIX_SKY_AUTO_UNTEXTURED_IGNORE{
    {System::GFX, "Settings", "RemixSkyAutoUntexturedIgnore"}, true};
// Render the game's OWN sky instead of deleting it, by pushing classified sky
// geometry behind the world rather than tagging it away.
//
// Every classified draw logs `ztest 1 zwrite 0`: the console draws its sky
// first and never lets it write depth, which is a hard guarantee it cannot
// occlude anything drawn afterwards. A path tracer has no draw order, so the
// same dome - 15k-25k units out against a 160k far plane in Wind Waker - is
// simply solid geometry parked in front of the island, and it hides it.
//
// Scaling the instance about the CAMERA POSITION is the exact translation of
// that guarantee into geometry. Every vertex keeps its direction from the eye,
// so the image is unchanged angle for angle, while the surface moves beyond all
// world geometry. It also removes the residual parallax that makes a dome drawn
// close to the camera read as fake.
//
// Note this is what makes RemixSkyAutoDetect = 1 no longer purely log-only:
// classification still does not TAG anything, but it does now move geometry.
const Info<bool> GFX_REMIX_SKY_AT_INFINITY{
    {System::GFX, "Settings", "RemixSkyAtInfinity"}, true};
// How far out RemixSkyAtInfinity pushes. Needs to exceed far_plane / dome_extent
// to clear the world - about 10.5 on Wind Waker's smallest classified dome - and
// wants margin without going so far that float precision at the resulting
// coordinates, or a ray tmax, becomes the next problem.
const Info<float> GFX_REMIX_SKY_INFINITY_SCALE{
    {System::GFX, "Settings", "RemixSkyInfinityScale"}, 16.0f};
// Route falloff-free GX Spot lights to Remix's DISTANT light instead of a sphere.
//
// GX has no directional light type. A light whose distance attenuation is
// GX_DA_OFF - distatt = (1,0,0), so the polynomial is the constant 1 - and whose
// angular attenuation is likewise constant has no falloff of any kind, which is
// how a GC title builds a sun: an ordinary light parked far enough away that its
// direction hardly varies over the scene. Wind Waker's is at |dpos| 24873 with
// distatt (1,0,0) and cosatt (1,0,0).
//
// Submitting that as a sphere applies a real 1/r^2 the console never applied, so
// the sun contributes essentially nothing - and because it still counts as "a
// light was drawn", it also suppresses rtx.fallbackLightMode's rescue. The scene
// renders black with a light in it. False reproduces the pre-fix behaviour.
const Info<bool> GFX_REMIX_GX_LIGHT_NO_FALLOFF_DISTANT{
    {System::GFX, "Settings", "RemixGxLightNoFalloffDistant"}, true};
// Give classified sky geometry an UNLIT (emissive) material.
//
// GX draws a skybox with lighting off, and Wind Waker's dome goes further: its
// TEV chain is `K0` alone, a constant with no rasterized-colour input at all, so
// the authored value [80 120 255] IS the final pixel on console. Submitting that
// as diffuse albedo asks a light to reveal it, which shades a surface that was
// never meant to be shaded - it goes dark on the side facing away from the sun
// and can never match the original. An emissive material reproduces "this
// colour regardless of lighting" exactly, and lets the sky light the scene.
//
// Textured sky uses its own albedo as the emissive texture, the same patch the
// runtime applies to WorldUI; untextured sky uses the folded TEV constant.
const Info<bool> GFX_REMIX_SKY_EMISSIVE{{System::GFX, "Settings", "RemixSkyEmissive"}, true};
// Emissive radiance multiplier for the sky. 1.0 reproduces the console's colour
// at face value; higher makes the sky a stronger light source for the scene.
const Info<float> GFX_REMIX_SKY_EMISSIVE_INTENSITY{
    {System::GFX, "Settings", "RemixSkyEmissiveIntensity"}, 1.0f};
// Record every EFB copy the game triggers - rect, destination, XFB-or-not, the
// clear bit - and, on trace frames, print each UI draw's EFB-space footprint
// next to a verdict on whether a non-XFB copy+clear later in the same frame
// swallowed it.
//
// It exists to answer one question with console truth rather than with more
// heuristics. A UI draw is invisible on hardware if the region it drew into is
// copied off-screen and then cleared before the frame reaches the XFB - whatever
// that draw's own blend, alpha and scissor state says about it. This backend
// executes no EFB copies at all (RemixTextureCache), so such a draw survives to
// the overlay and appears on screen. Four alpha/blend/scissor discriminators
// were already tried on Wind Waker's title screen and none of them separated the
// spurious HUD from the real title art; what happens to the EFB pixels
// AFTERWARDS is the remaining console-side difference between the two.
//
// Log-only, and the heavy per-draw lines are gated to ShouldTraceDraws()
// frames, so it defaults on the same way RemixTraceModelviews does.
const Info<bool> GFX_REMIX_TRACE_EFB_COPIES{{System::GFX, "Settings", "RemixTraceEfbCopies"}, true};
// Skip UI overlay draws whose blend factors read the DESTINATION ALPHA.
//
// The overlay is composited over a finished path-traced image at present time.
// There is no EFB alpha for a destination-alpha factor to read, so a draw that
// blends against it is not representable in this compositing model at all - and
// it is not merely an approximation problem, because the value it wants is what
// the 3D pass wrote, not what the overlay accumulated. Emulating it against the
// overlay's own alpha would look right on a UI-only screen and be silently wrong
// on a post-process pass, which is exactly where these draws come from.
//
// Today such a draw matches neither arm of SubmitUiDraw's classifier and falls
// through to Over at full weight, which is the worst available answer: Wind
// Waker's glare/scatter pass is a full-screen untextured quad with
// DstAlpha/One, so it lands as an opaque wash over the entire title screen.
// Skipping it is strictly better, and for a glare pass specifically it is
// correct - Remix does its own bloom and glare.
//
// Narrow on purpose. It does not touch SrcAlpha/InvSrcAlpha, so a fade to black
// survives by construction, and it says nothing about full-screen ortho draws in
// general. Wind Waker's whole title screen contains exactly one such draw per
// frame; a count much above one per frame is the sign it is over-reaching.
//
// False = classify as before, i.e. fall through to Over.
const Info<bool> GFX_REMIX_UI_DROP_DST_ALPHA{{System::GFX, "Settings", "RemixUiDropDstAlpha"}, true};
// Skip UI overlay draws textured from the destination of an EFB copy.
//
// This backend executes no EFB copies (RemixTextureCache::CopyEFB copies
// nothing), so the GC memory an EFB copy targets keeps whatever was there
// before. A cache entry decoded out of that memory is not empty - it decodes
// successfully, from stale bytes - so RemixTexture::HasData() is true and the
// existing skipped_efb_texture guard, which tests exactly that, lets it through.
// The result is a quad painted with garbage: Wind Waker's white-noise speckle,
// whose texture hash is byte-identical across 27 trace frames of moving camera
// because no one is writing the source.
//
// There is no correct content available for such a draw, and inventing some
// would be worse than omitting it, so it is omitted. The counter in the frame
// line is the diagnostic for the real risk here: a game that legitimately
// composes its menu through an EFB copy loses that menu, and the count is what
// makes that recognisable instead of mysterious.
//
// DEFAULT OFF, and measured rather than cautious. On Wind Waker's title screen
// this fires on exactly the draw it was designed for - one per frame, the
// full-screen compose quad whose texture sits at 0x0065ff20, the address the
// copy recorder saw one draw earlier - and removing it changes ZERO pixels of
// the overlay dump. That draw's alpha test is "alpha > 0" and the stale bytes it
// decodes are transparent, so it paints nothing to begin with. The white-noise
// speckle it was blamed for is a different thing entirely: the item-box texture
// 0x745553491ccc0dba at 0x00ea1580, an ordinary heap address that no EFB copy
// has ever targeted, and the white field behind it was the destination-alpha
// wash above.
//
// So the rule is principled but has no demonstrated benefit, while its risk - a
// game that legitimately composes a menu through an EFB copy losing that menu -
// is real. It stays off until some game shows a symptom it actually fixes.
//
// False = draw it, garbage and all.
const Info<bool> GFX_REMIX_UI_DROP_EFB_COPY_TEXTURES{
    {System::GFX, "Settings", "RemixUiDropEfbCopyTextures"}, false};
// Map the UI overlay onto the region the console PRESENTS rather than onto the
// whole EFB.
//
// UI draw coordinates arrive in EFB units and the overlay is the swapchain, so
// something has to say how large the EFB region is that fills the window. The
// backend used EFB_WIDTH x EFB_HEIGHT, 640x528 (VideoCommon.h:15-16) - but that
// is the EFB's maximum size, not the part of it that reaches the screen. What
// reaches the screen is the source rect of the frame's XFB copy, and Wind Waker
// copies 480 rows, not 528. Scaling by 528 therefore squeezed the whole HUD into
// the top 91% of the window and left the bottom ~9% permanently empty.
//
// The rect is taken from the XFB copies the texture cache already reports
// (RemixApi::NoteEfbCopy), unioned over the frame - an interlaced or two-field
// game presents more than one - and promoted at frame end, so a UI draw uses the
// region the PREVIOUS frame presented. That one-frame lag is deliberate: this
// frame's XFB copy is the event that ends the frame, long after its UI has been
// submitted, and a game changing its presented size mid-run is the only case it
// could ever be visible in.
//
// Until a first XFB copy is seen the EFB constants stand, so a game that somehow
// presents without one behaves exactly as before.
//
// False = scale by the EFB constants, the pre-fix mapping.
const Info<bool> GFX_REMIX_UI_SCALE_TO_XFB{{System::GFX, "Settings", "RemixUiScaleToXfb"}, true};
// Take the rasterized colour channel from the TEV stage that actually consumes
// it, rather than from stage 0.
//
// GX names the rasterized channel PER STAGE - bpmem.tevorders[stage>>1]
// .getColorChan(stage&1) (Tev.cpp:489, PixelShaderGen.cpp:257) - and the stage
// that reads RasColor/RasAlpha is routinely not stage 0. Reading stage 0's
// channel therefore answers a different question than the one being asked: on a
// chain whose stage 0 is a plain texture fetch that rasterizes nothing, the old
// code resolved "no tint" no matter how strongly a later stage tinted the draw,
// and on a chain whose stages name different channels it read the wrong one.
//
// The colour half and the alpha half are resolved separately, because they are
// separate LitChannels with their own material sources; when they end up on
// different vertex attributes the colour half wins and the frame line's
// "colour ... split" counter says so.
//
// WORLD DRAWS ONLY - orthographic draws are governed by RemixUiRasChannel below.
//
// False = stage 0's channel for both halves, the pre-fix behaviour.
const Info<bool> GFX_REMIX_GX_RAS_CHANNEL{{System::GFX, "Settings", "RemixGxRasChannel"}, true};
// Take the albedo off a later TEV stage when stage 0's texture coordinate is
// generated from a lit channel - i.e. when stage 0 samples a toon RAMP rather
// than the surface.
//
// False = stage 0 unconditionally, the pre-fix behaviour.
const Info<bool> GFX_REMIX_GX_RAMP_ALBEDO_SKIP{
    {System::GFX, "Settings", "RemixGxRampAlbedoSkip"}, true};
// The same per-stage rasterized-channel rule, applied to orthographic (UI
// overlay) draws.
//
// This is separated from RemixGxRasChannel because it is the one knob known to
// change Wind Waker's title-screen HUD leak, and it needs to be A/B-able on its
// own. With it ON the leaked gameplay HUD - hearts, D-pad, item icons, the R
// counter - is GONE from the overlay, observed by eye on the build at commit
// 495ea26957. The mechanism is direct: the overlay's software rasterizer reads
// its rasterized-alpha input out of the submitted vertex colour, so a draw
// promoted from "no channel, write opaque white" to "channel 0, write the vertex
// colour" gets Wind Waker's actual UI vertex alpha of 0 and resolves away.
//
// It is ON by default because a leaked HUD is the worse of the two failures, but
// this is NOT settled and the caveat is concrete: the same screenshot is also
// missing PRESS START and the Japanese subtitle, and frame 600 of that run
// produced no overlay at all (HasContent false). So it plausibly over-suppresses
// legitimate UI along with the leak. The frame it was first judged on is not
// enough - always confirm against a frame that contains PRESS START, and read
// ui_upload_us in the frame line, not one dump.
//
// What a UI draw's rasterized alpha SHOULD be is still the open question; this
// knob is the best answer measured so far, not the right one derived.
//
// False = stage 0's channel, which reinstates the HUD leak.
const Info<bool> GFX_REMIX_UI_RAS_CHANNEL{{System::GFX, "Settings", "RemixUiRasChannel"}, true};
// Skip world draws whose scissor rectangle is empty.
//
// Every reference implementation clips every draw: the software rasterizer
// builds scissor rects from bpmem.scissorTL/BR and rejects pixels outside them
// (Rasterizer.cpp:117-119, 364-365), and the hardware backends set the scissor
// per draw (BPFunctions.cpp:103-104). The Remix UI path already honours it, but
// the world path read scissor state nowhere - so geometry the game hid by
// scissoring it away was drawn in full, and in a path tracer it also lit and
// shadowed the scene.
//
// Only the empty case is acted on, and deliberately: Remix has no screen-space
// clip, so a draw the scissor merely trims cannot be expressed and is submitted
// whole. Empty is the one case where the console's answer - "no pixels" - is
// exactly representable.
//
// Emptiness is read off ScissorResult::rectangles, not off Best(): Best()
// fabricates an out-of-bounds rectangle when the list is empty
// (BPFunctions.cpp:166-171), so a caller testing the returned rect would never
// see the condition at all.
//
// False = submit them, which is the pre-fix behaviour.
const Info<bool> GFX_REMIX_WORLD_SCISSOR_SKIP{{System::GFX, "Settings", "RemixWorldScissorSkip"},
                                              true};

// Per-draw trace of where a WORLD draw's colour comes from: the TEV stage that
// consumes ras, the XF channel it names, both channels' material/lighting/
// ambient sources, the resolved texture-stage arguments, and the raw vertex
// colour bytes.
//
// Log-only and gated to ShouldTraceDraws() frames, but it emits a line per draw
// for the first few hundred draws of such a frame, which is far heavier than the
// other trace knobs. Off by default; turn it on to answer "why is this
// vertex-coloured surface white", which the frame counters cannot.
const Info<bool> GFX_REMIX_TRACE_COLORS{{System::GFX, "Settings", "RemixTraceColors"}, false};

// Resolve the TEV COLOUR chain and fold it into the submitted vertex colours.
//
// GC titles keep a surface's palette in TEV colour REGISTERS (GXSetTevColor) and
// use the per-vertex rasterized colour as the lerp WEIGHT between two of them.
// Measured on Wind Waker's title scene: 3263 of 3865 vertex-coloured draws are
// exactly lerp(c0, c1, ras) and another 546 are lerp(c0, konst, ras). Submitting
// the raw weight as an albedo tint therefore drops the colour and renders a
// greyscale surface - which is why the ocean and the vertex-coloured skybox came
// out white.
//
// False = submit the raw rasterized colour, which is exactly the pre-fix
// behaviour, so this is a clean A/B. Chains the evaluator cannot resolve fall back
// to it per draw; the frame log counts folded/identity/bailed.
const Info<bool> GFX_REMIX_GX_TEV_COLOR{{System::GFX, "Settings", "RemixGxTevColor"}, true};

// Take the albedo texture from the first ENABLED TEV stage rather than from stage
// 0 alone.
//
// Wind Waker's sea samples nothing on stage 0 (it is a pure register lerp) and
// samples the water texture on stage 1, so the whole surface arrived untextured.
// Only consulted when stage 0 samples nothing, which makes it a strict extension:
// a draw that samples on stage 0 resolves exactly as before. Identity decisions -
// the sky texture list, the sky auto-detector's untextured test - keep reading
// stage 0 either way.
//
// False = stage 0 only, the pre-fix behaviour.
const Info<bool> GFX_REMIX_GX_TEXTURE_STAGE{{System::GFX, "Settings", "RemixGxTextureStage"},
                                            true};

// Diagnostic. Paints every world draw a flat colour naming the colour ROUTE it
// took, with the texture and the register selected out of the material so the
// route colour is the whole albedo:
//   red     - vertex colour, TEV chain folded
//   magenta - vertex colour, fold refused or identity
//   blue    - register tint through tFactor
//   green   - no rasterized colour at all
//
// The frame counters say how many draws took each route; they cannot say which
// PIXELS a route owns, and that is the question when a surface comes out the
// wrong colour and the arithmetic says it should not. Never on by default.
const Info<bool> GFX_REMIX_DEBUG_COLOR_ROUTES{
    {System::GFX, "Settings", "RemixDebugColorRoutes"}, false};

// Maintain a real CPU-side EFB: honour clears, pokes and peeks against it, and
// let a CLASSIFIED subset of EFB copies encode out of it into game RAM.
//
// Before this, the backend had no EFB at all. Peeks returned 0, pokes did
// nothing, ClearRegion was a no-op through the stub pipelines, and
// RemixTextureCache::CopyEFB wrote nothing - so every copy destination received
// the staging buffer's zeroes. Every copy was, in effect, discarded.
//
// That accident is desirable for a large class of copies and is deliberately
// preserved: a GC game's baked shadow maps, mirrored-camera reflections and
// bloom chains are screen-space fakes of things the path tracer does natively
// and better, and discarding them is what lets the traced result show. So the
// copies are classified (see RemixEfbCopy2D and friends below) rather than
// executed wholesale - executing a world-content copy against this EFB would
// paint a flat clear-coloured rectangle, which is visibly WORSE than the
// invisible zeroes it replaced.
//
// The storage, the clear and the encoders are the Software backend's, reused
// unchanged (SWEfbInterface.cpp, EfbCopy.cpp, TextureEncoder.cpp) rather than
// duplicated.
//
// False = the pre-change behaviour in every particular: peeks return 0, pokes
// and clears do nothing, no copy is classified and no copy is encoded.
const Info<bool> GFX_REMIX_EFB_EMULATION{{System::GFX, "Settings", "RemixEfbEmulation"}, true};
// Execute a colour EFB copy taken before any perspective draw reached submission
// this frame.
//
// This is the one class whose content the synthesized EFB holds EXACTLY, by
// construction rather than by luck: with no world draw yet, everything the
// console's EFB contained is clear colour plus orthographic/UI draws plus pokes,
// which is precisely what this EFB holds. Render-to-texture menus, title
// screens and composed text windows are the shapes that fit.
//
// It is therefore the only class that executes by default, and the only knob
// here whose default changes behaviour.
//
// False = discard it, i.e. the pre-change behaviour.
const Info<bool> GFX_REMIX_EFB_COPY_2D{{System::GFX, "Settings", "RemixEfbCopy2D"}, true};
// Execute a colour EFB copy taken AFTER a perspective draw this frame.
//
// The frame-level world-draw count is a heuristic, and a deliberately blunt one:
// the copied rect very likely holds world pixels, and this backend rasterizes no
// world pixels, so executing hands the game a flat clear-coloured image where
// the console had the scene. Discarding leaves today's zeroes and lets the
// traced effect show instead of the baked one.
//
// Its failure mode is the safe one - a game that draws world geometry early and
// then composes a pure-2D element by copy later in the same frame loses that
// element - and this knob is the recovery lever for exactly that case, with no
// rebuild. The per-copy trace line names `world-draws N`, which is the signal
// that decided it.
//
// True = execute, i.e. encode the (mostly clear-coloured) EFB into the copy's
// destination.
const Info<bool> GFX_REMIX_EFB_COPY_SCENE{{System::GFX, "Settings", "RemixEfbCopyScene"}, false};
// Execute a DEPTH EFB copy (source pixel format Z24, BPStructs.cpp:308).
//
// The format signal is exact; the purpose - almost always a shadow map - is
// inferred. Both point the same way. The path tracer casts real shadows, so the
// baked one is redundant; and this EFB's depth plane holds only the clear Z plus
// pokes, so executing would hand the game a UNIFORM depth map, i.e. a
// full-screen wrong shadow test. That is worse than the absence.
//
// True = execute anyway, for a game that uses a depth copy for something else.
const Info<bool> GFX_REMIX_EFB_COPY_DEPTH{{System::GFX, "Settings", "RemixEfbCopyDepth"}, false};
// Execute an INTENSITY (luminance-format, isIntensity) EFB copy.
//
// Luminance extraction is what a bloom or glow chain opens with, and it usually
// rides with half_scale. The runtime does its own bloom, and feeding the game's
// chain a flat clear-luminance only blends a uniform wash back over the screen.
//
// True = execute, which is the lever if a game turns out to use an intensity
// copy for a legitimate 2D mask. The trace line names `int 1` on these.
const Info<bool> GFX_REMIX_EFB_COPY_INTENSITY{{System::GFX, "Settings", "RemixEfbCopyIntensity"},
                                              false};
// Execute the XFB copy - the frame's presentation copy - by running the YUV
// encoder over the synthesized EFB.
//
// Off, and right to be off twice over. Nothing on this backend consumes the XFB
// image: the Remix runtime produces and presents the picture. And even executed,
// the encode would only emit clear colour plus the 2D layer, because no world
// content is ever rasterized into this EFB. Meanwhile it is the one recurring
// per-frame full-width cost in the whole feature, which the fill-rate history of
// this backend's UI rasterizer says not to pay by default.
//
// True = encode it. Dolphin's screenshot and AV-dump pipeline is NOT wired to
// this and will not start producing Remix frames because of it.
const Info<bool> GFX_REMIX_EFB_XFB_ENCODE{{System::GFX, "Settings", "RemixEfbXfbEncode"}, false};
// Composite the frame's 2D layer into the EFB before an EXECUTED copy encodes.
//
// The EFB's colour plane is otherwise only written by clears and pokes, so
// without this an executed Composed2D copy encodes bare clear colour - correct,
// but empty. The fold replays the frame's orthographic draws through a second
// UiRasterizer instance running at the EFB's own 640x528 (their viewport and
// scissor arrive in EFB units already) and blends the result Over the colour
// plane.
//
// Costs nothing on a frame whose copies are all discarded beyond the recording
// itself: the fold is called from the execute arm only.
//
// False = encode the EFB without the 2D layer.
const Info<bool> GFX_REMIX_EFB_UI_COMPOSE{{System::GFX, "Settings", "RemixEfbUiCompose"}, true};
// Skip draws that sample an EFB copy destination whose copy was DISCARDED.
//
// Discarding a copy leaves that memory holding zeroes. Zero bytes decode into a
// perfectly valid texture, so the draw that samples it is not refused anywhere -
// it renders as a blank rectangle sitting over the path-traced scene. SpongeBob:
// Battle for Bikini Bottom is the confirmed case: two 256x256 copies from the
// top-left corner every frame, both classified Scene, drawn as a white box over
// a quarter of the screen.
//
// True = drop those draws, so the game's screen-space fake is ABSENT and the
// traced result behind it shows. That is what the discard decision already
// meant; this only stops the game papering over it.
//
// False = draw them anyway, blank texture and all - the behaviour before this
// existed. Set it if skipping ever removes something a game genuinely needed;
// the frame line's `efbdisc` counter says how many draws are affected.
//
// Ships FALSE. The benefit was never actually measured - the SpongeBob white box
// that motivated it turned out to be fixed by the pre-world drop instead - and a
// default-on drop rule that has not been shown to help is a default-on way to
// lose content. It stays available per game, and its earlier draw-drop in the
// vertex manager is unchanged, so turning it on restores the old behaviour
// exactly.
const Info<bool> GFX_REMIX_EFB_SKIP_DISCARDED_TEX{
    {System::GFX, "Settings", "RemixEfbSkipDiscardedTex"}, false};
// Drop perspective draws that render a helper pass this backend then throws
// away. The signature, learned from the previous frame: a viewport whose rect
// was EFB-copied to a non-XFB destination WITH the clear flag, and the copy was
// discarded. On console those pixels exist only to feed the copied texture -
// the clear erases them and the main pass overdraws the region - but as
// path-traced world geometry nothing erases them, so after the reference fix
// they would composite as a ghost mini-scene in that corner of the view. Never
// fires on a viewport covering most of the presented region (that IS the
// scene, whatever the copy pattern around it), and stands down entirely on
// frames with no majority-coverage viewport (split screen), so a false cull
// requires the game to draw VISIBLE content in a sub-rect it also
// copies-and-clears to a texture nobody executes - the mirror-composite
// pattern, whose composite quad is already blank today for the same discard.
const Info<bool> GFX_REMIX_EFB_DROP_AUX_PASS{{System::GFX, "Settings", "RemixEfbDropAuxPass"},
                                             true};
// Drop untextured 2D draws that arrive before any world geometry in the frame.
//
// Those are EFB clears and scratch-region fills, not UI. The console draws the
// scene over them; this backend composites the 2D layer ON TOP of the traced
// image, so they land over everything instead of under it and paint the screen
// flat. SpongeBob: Battle for Bikini Bottom is the confirmed case - a
// full-screen white quad (the screen going white outdoors) and a 256x256 one
// clipped to the exact rect of the EFB copy it feeds (the white box top-left),
// both carrying vertex colour 0xffffffff.
//
// Untextured is what keeps this from swallowing real 2D: menus, HUD elements
// and text are textured. The pre-world test is what separates a clear from a
// legitimate 2D layer drawn after the scene.
//
// True = drop them. False = composite them as before. If a game's genuine 2D
// background disappears, this is the knob; the frame line's `preworld` counter
// says how many draws are affected.
const Info<bool> GFX_REMIX_UI_DROP_PRE_WORLD_BLANK{
    {System::GFX, "Settings", "RemixUiDropPreWorldBlank"}, true};
// Drop the game's DIRECTIONAL lights, so an atmosphere mod owns the key light.
//
// A GC title's sun is a baked directional light with a fixed colour and
// direction that knows nothing about a physically-modelled sky. Run it alongside
// one and they double up - the scene reads far too bright and the game's flat
// white fights the atmosphere's own sun. Numos is the case this exists for.
//
// Positional lights are kept: lamps, glows and cone lights are local set
// dressing no atmosphere model replaces, and dropping them would darken
// interiors.
//
// True = drop them. Default False, because with no atmosphere mod loaded this
// removes the scene's only key light. Turn it on together with the sky, and
// watch the frame line's `dropped` count to confirm the game really was adding
// one. If the scene goes black rather than sky-lit, check the runtime's
// rtx.fallbackLightMode - dropping every light is what lets NoLightsPresent
// trigger.
const Info<bool> GFX_REMIX_GX_LIGHT_DROP_DISTANT{
    {System::GFX, "Settings", "RemixGxLightDropDistant"}, false};
// The backend's own fallback light: one persistent distant light, drawn only on
// frames where the scene submitted NO lights at all.
//
// It exists because many GC titles bake their lighting into vertex colours and
// enable no XF lights, which would otherwise leave the path tracer nothing to
// integrate. It is NOT the runtime's rtx.fallbackLightMode - that is a separate
// mechanism in rtx.conf, and having both is why "where is this extra distant
// light coming from" is an easy question to get wrong.
//
// False = never draw it, so an atmosphere mod is the only key light. Turn it off
// whenever a sky mod is providing illumination; leave it on for bare Remix.
// RemixGxLightDropDistant already implies this off - dropping the game's suns
// and then inserting our own would cancel out.
const Info<bool> GFX_REMIX_FALLBACK_LIGHT{{System::GFX, "Settings", "RemixFallbackLight"}, true};
// Give every game its own rtx.conf, mods folder, captures folder and runtime
// log, under <Dolphin.exe dir>/Remix/<GameID>/.
//
// Without this every title shares the single rtx.conf next to Dolphin.exe, and
// that file holds mesh and texture hashes: a skybox or ignore tag written while
// playing one game is read back while playing another, where the hash means
// something else entirely or nothing at all. That is not hypothetical - a stale
// skybox tag from one title deleted another title's sky, silently, and cost a
// day to find.
//
// The mechanism is the runtime's own: the paths are handed over in five
// environment variables it reads at load time (DXVK_RTX_CONFIG_FILE,
// DXVK_USER_CONFIG_FILE, DEFAULT_MODS_DIR, DXVK_CAPTURE_PATH, DXVK_LOG_PATH).
//
// The rtx.conf and user.conf next to Dolphin.exe are TEMPLATES. A game's folder
// is seeded from them the first time it is set up and the game then owns its
// copies outright - they are not layered underneath it afterwards, and nothing
// ever writes back to them. So a game's config is genuinely its own: deleting
// something from it deletes it, rather than being re-supplied from below on the
// next boot. The trade is deliberate - a later improvement to a template does
// not reach games that already exist.
//
// DXVK_USER_CONFIG_FILE is a fork addition and is the load-bearing one: every
// dev-menu edit targets the USER layer, so without it a texture tagged in one
// game is read back in every other game, which is the failure this exists to
// stop. A stock runtime ignores it.
//
// False restores the single shared set of files exactly.
const Info<bool> GFX_REMIX_PER_GAME_PATHS{{System::GFX, "Settings", "RemixPerGamePaths"}, true};
// Where the per-game folders live. Empty means `Remix` next to Dolphin.exe,
// falling back to the Dolphin user directory when the executable's own directory
// cannot be written to.
//
// It is a setting rather than a constant for two reasons. A development build
// runs out of a build output directory, and cleaning that directory would delete
// mod projects. And the RTX Remix Toolkit binds a project to one of these
// folders with a SYMLINK PAIR, so once a project exists the folder cannot be
// moved or renamed without breaking it - which makes "choose the location before
// you start" worth offering, and makes changing it afterwards something to do
// deliberately.
const Info<std::string> GFX_REMIX_PER_GAME_ROOT{{System::GFX, "Settings", "RemixPerGameRoot"}, ""};
// Whether the per-game folder supplies the runtime's mods directory too.
//
// The runtime takes exactly ONE mods directory, so this is a straight choice
// rather than a preference: on, the game reads <root>/<GameID>/rtx-remix/mods
// and the shared rtx-remix/mods next to Dolphin.exe is not searched at all; off,
// every game shares that one folder and per-game mods are not possible. Per-game
// is the useful default because a GameCube mod is authored against one title's
// meshes and textures, and the Toolkit's project wizard expects a folder per
// project.
//
// Turn it off if you have mods in the shared folder that you want every game to
// see. The rest of the separation - config, captures, log - is unaffected either
// way.
const Info<bool> GFX_REMIX_PER_GAME_MODS{{System::GFX, "Settings", "RemixPerGameMods"}, true};
// Relaunch Dolphin when emulation ends, so every game gets a fresh process.
// The Remix runtime initializes a pile of process-global state for its first
// game - crash reporting, raw-input registration, the overlay, config layers,
// per-game folders - and resetting all of it in place has been tried twice
// (unload: intermittent crashes from threads still inside the module;
// resident re-resolve: each round surfaced another subsystem that assumed a
// fresh process). A new process is the one configuration that is correct by
// construction, and it is what single-game users have implicitly always run.
// A game queued behind the stop (double-clicking another title) is carried to
// the new instance on its command line, so switching games this way becomes
// exactly "quit and reopen with the next game" without the manual steps.
const Info<bool> GFX_REMIX_RESTART_ON_STOP{{System::GFX, "Settings", "RemixRestartOnStop"}, true};
// Submit matrix-palette (skinned) draws as stable object-space meshes with
// per-vertex bone indices and a per-draw bone palette, so the runtime skins them
// on the GPU, instead of transforming every vertex on the CPU.
//
// The CPU bake is what makes characters ghost. A baked draw's vertex bytes are
// in VIEW space, so they change whenever the character animates OR the camera
// moves; the mesh hash covers those bytes, so every frame mints a brand new mesh
// handle and a brand new instance. Remix has nothing to match against the
// previous frame - no BLAS history, no motion vectors - so the denoiser and
// every temporal feature see a character that teleports into existence each
// frame, which reads on screen as a duplicate trail dragging behind anything
// that moves.
//
// Skinning on the GPU instead makes the submitted mesh the object-space one the
// game authored: identical bytes every frame, one stable hash, one reused
// handle, real per-vertex motion vectors. Only the bone palette changes, and the
// runtime already re-skins exactly when it does.
//
// Off restores the CPU bake exactly, byte for byte, so the two are a clean A/B -
// which is the tool to reach for if a character ever renders exploded or
// collapsed, since a bone/vertex convention mistake presents that way and
// nothing else does.
const Info<bool> GFX_REMIX_GPU_SKINNING{{System::GFX, "Settings", "RemixGpuSkinning"}, true};
// Give geometry the game regenerates every frame - CPU-skinned characters and
// their whole class - one stable Remix mesh handle, updated in place, instead
// of a brand new mesh per frame.
//
// A regenerated draw's vertex bytes change every frame, so its full mesh hash
// changes every frame, so the old path minted a new handle, a new BLAS and a
// new instance per frame: no temporal identity, zero motion vectors, and the
// same duplicate-trail ghost the GPU-skinning knob fixes for palette draws -
// but for geometry the GAME skins on ITS cpu, where no palette ever reaches
// this backend. This knob keys identity on what a re-pose does NOT change -
// indices, UVs, vertex colours, material - and when that topology is seen
// re-posing across frames, the existing handle's vertices are rewritten
// through the runtime's UpdateMeshBatched, which refits the BLAS and yields
// real per-vertex motion vectors.
//
// Off restores the old behaviour exactly, byte for byte. It also degrades to
// off automatically (with one warning) when the deployed runtime predates
// UpdateMeshBatched.
const Info<bool> GFX_REMIX_DYNAMIC_MESH_IDENTITY{
    {System::GFX, "Settings", "RemixDynamicMeshIdentity"}, true};

// Some games split one rendered frame across two presents: a full world pass,
// then a present carrying only the 2D layer. Skylanders alternates ~900 world
// instances with 34 HUD quads (drawn through the world path, with a
// perspective projection and a screen-pixel modelview, so no ortho rule can
// catch them) and zero lights. Handing that second present to the path tracer
// as a scene strobes the world at half rate and resets the denoiser's history
// every frame - the world never accumulates and only the sky reads. This knob
// drops such minor frames whole: no camera update, no instances, no lights, no
// Present, so the runtime keeps showing the last full frame and consecutive
// full frames become adjacent for the temporal stack (which is also what lets
// the camera estimator match deltas again - the alternation starved it to 2
// matched samples). A frame is minor only relative to the recent peak, so all-
// 2D stretches (menus, loads) never trip it: there the small count IS the
// scene. Validated on Skylanders only, so it ships off; enable per game.
const Info<bool> GFX_REMIX_SKIP_MINOR_FRAMES{
    {System::GFX, "Settings", "RemixSkipMinorFrames"}, false};

// Let the Remix dev menu's texture categories decide what happens to a 2D draw.
//
// The runtime's texture grid already offers "UI Texture", "Ignore" and "World
// Space UI" for every texture it knows about, but tagging one had no effect on
// this backend: 2D draws never become runtime draw calls at all - they are
// rasterized here and handed over as finished pixels - so the runtime never
// gets the chance to apply its own categories to them. This knob closes that
// loop from our side: the tag sets are polled back from the runtime once a
// frame and each 2D draw is routed by the tag on its own texture (or, for an
// untextured draw, on its mesh identity). UI = composite it and skip every drop
// heuristic; Ignore = discard it; World Space UI = send it through the
// world-space plane path instead.
//
// It also makes the tagging possible in the first place: while the dev menu is
// open every 2D draw is temporarily routed world-side, so it becomes a real
// clickable object in the runtime's picker - including untextured white boxes,
// which have no texture to find in the grid.
//
// On by default because it is inert until something is actually tagged. Off is
// the pre-feature behaviour exactly. Tags take about two frames to take effect
// (they are polled, not pushed); the frame line's `ui-tags` group is what says
// whether a tag is being seen.
const Info<bool> GFX_REMIX_UI_TAG_ROUTING{
    {System::GFX, "Settings", "RemixUiTagRouting"}, true};

// Extend the "UI Texture" tag to draws the game submits with a PERSPECTIVE
// projection.
//
// Not every HUD is orthographic. A GameCube game is free to park its HUD quads a
// short distance in front of the camera and draw them through the ordinary 3D
// frustum, and Resident Evil 4 does exactly that: tagging its health ring or
// ammo counter "UI Texture" moved nothing, because the routing above only ever
// looked at ortho draws. Measured on RE4: seven UI tags matching zero draws.
// With this on, a perspective draw whose texture (or mesh identity, when it has
// no texture) carries a UI tag is diverted into the same screen overlay the 2D
// layer uses, transformed through the game's own perspective matrix instead of
// the ortho one.
//
// Deliberately a knob of its own rather than a widening of RemixUiTagRouting:
// the A/B that matters is "this behaviour off, 2D tag routing intact", which a
// shared knob cannot express. It is subordinate - the divert requires BOTH, so
// RemixUiTagRouting remains the master switch over everything tag-driven.
//
// Only the UI tag acts here. Ignore and World Space UI on a perspective draw are
// already applied by the runtime itself (it sees a real API draw and its own
// category hook handles those two); acting on them from this side as well would
// double-apply. RemixUiStrict stays ortho-only for the obvious reason - applied
// to untagged perspective draws it would delete the world.
//
// While the dev menu is open the divert is skipped, so a tagged element goes
// back to being a clickable world object and the tag can be removed again.
//
// On by default because, like the routing knob itself, it is inert until
// something is tagged. Counters: `persp`, `wref` and `pskin` in the frame line's
// `ui-tags` group.
const Info<bool> GFX_REMIX_UI_TAG_PERSPECTIVE{
    {System::GFX, "Settings", "RemixUiTagPerspective"}, true};

// Drop every 2D draw that arrived before any world geometry this frame, textured
// or not.
//
// The wider sibling of RemixUiDropPreWorldBlank, which only catches UNTEXTURED
// pre-world draws. Some games wash the framebuffer with a textured quad instead
// - same intent, same wrongness here (the console draws the scene over it; we
// composite 2D on top, so it covers everything), but the untextured test cannot
// see it. This one ignores the texture and judges purely on ordering.
//
// Riskier for exactly that reason: a game whose legitimate 2D background is
// drawn before the world loses that background. Off by default, enable per game,
// and watch the frame line's `preworld` counter under `ui-heur`. A UI-Texture
// tag rescues any specific draw this eats.
//
// The framebuffer-copy compose path is deliberately NOT filtered by this: a copy
// executed mid-frame genuinely did contain the pre-world wash on console, and
// reproducing that is the whole point of the compose.
const Info<bool> GFX_REMIX_UI_DROP_PRE_WORLD{
    {System::GFX, "Settings", "RemixUiDropPreWorld"}, false};

// Drop opaque 2D draws that cover essentially the whole presented image on a
// frame that already has world geometry.
//
// A full-screen opaque quad composited over the traced scene hides the scene
// completely, so if one shows up after the world has been drawn it is almost
// always a fake the console would have drawn UNDER everything, or a fade this
// backend cannot express. "Essentially the whole image" is 95% of the presented
// region's area, measured on what the draw actually paints rather than on its
// scissor.
//
// Off by default: a game with a genuine full-screen 2D background on a frame
// that also draws world geometry is unusual but not impossible, and it would
// vanish. The frame line's `fullscr` counter says whether this is firing, and a
// UI-Texture tag rescues any specific victim.
const Info<bool> GFX_REMIX_UI_DROP_FULL_SCREEN_OPAQUE{
    {System::GFX, "Settings", "RemixUiDropFullScreenOpaque"}, false};

// Show only 2D draws that were explicitly tagged "UI Texture", and drop the rest.
//
// The opposite policy to the heuristics: instead of guessing which 2D draws are
// junk, assume all of them are and let the user name the keepers in the dev
// menu. Useful on a game whose 2D layer is mostly washes and fakes with a few
// elements worth keeping, and as the A/B lever for finding out which elements
// those are - it is live, so it can be flipped while the game runs.
//
// Only untagged draws are affected. Explicit Ignore and World Space UI tags are
// still honoured, and heuristics never get a say (there is nothing left for them
// to judge). Requires RemixUiTagRouting.
const Info<bool> GFX_REMIX_UI_STRICT{{System::GFX, "Settings", "RemixUiStrict"}, false};

// Honour each 2D draw's real depth state in the overlay rasterizer.
//
// The overlay was a pure painter's algorithm: later draws paint over earlier
// ones. Every 2D HUD measured so far draws with the depth test off, so order
// was all they needed - but a HUD the game draws through the 3D frustum is
// world geometry, and world geometry is free to rely on the depth buffer
// instead. Resident Evil 4 submits its HUD background AFTER its text with the
// text parked nearer the camera: the console's z test puts the text in front,
// and submission order alone puts the background on top of it.
//
// With this on, a draw whose z test is enabled interpolates the console's own
// screen-space z (the software renderer's exact viewport mapping) and tests
// and writes a depth plane with the draw's own compare function, after the
// alpha test, exactly as late z works on hardware. Draws with the test off -
// which is every ordinary 2D HUD - never touch the plane and behave exactly
// as before, so False here is the pre-fix painter's algorithm, byte for byte.
const Info<bool> GFX_REMIX_UI_DEPTH{{System::GFX, "Settings", "RemixUiDepth"}, true};

// Show the game's whole 2D layer as world geometry instead of compositing it.
//
// This is the click-to-tag view, and it used to be automatic: opening the
// runtime's dev menu routed every 2D draw world-side so the picker could reach
// it, and closing the menu put everything back. In practice that coupling was
// wrong - the menu is opened for plenty of reasons that are not tagging
// (flipping options, reading light statistics), and every one of them yanked
// the HUD into the world and back. Now it is this toggle, flipped by hand:
// on = every 2D draw is a real, clickable world object (including untextured
// white boxes, which have no grid thumbnail and can be reached no other way);
// off = normal compositing. Live, so it can be flipped without a restart.
const Info<bool> GFX_REMIX_UI_WORLD_VIEW{{System::GFX, "Settings", "RemixUiWorldView"}, false};

// The GUI's view of everything above. Rows are in README section order; the
// tooltips are the comments above rewritten for someone who has never read this
// file. See RemixSettingMeta in the header for what each column means.
namespace
{
using Group = RemixSettingMeta::Group;
using Liveness = RemixSettingMeta::Liveness;
using Maturity = RemixSettingMeta::Maturity;

// Combo entries. The stored value is the entry's INDEX, so entry n has to be the
// description of value n.
constexpr const char* const SKY_AUTO_DETECT_CHOICES[] = {
    "0: Off",
    "1: Classify",
    "2: Classify and tag",
};
constexpr const char* const SKY_MODE_CHOICES[] = {
    "0: Off (submit as world)",
    "1: Tag as sky",
    "2: Drop draws",
};
constexpr const char* const UI_MODE_CHOICES[] = {
    "0: Drop 2D layer",
    "1: Screen overlay",
    "2: World-space plane (experimental)",
};

// One builder per control type, so a row says what it is rather than counting
// commas. Defaults cover the common case: read once at backend init, and not an
// experiment.
constexpr RemixSettingMeta Toggle(const Info<bool>* setting, const char* label,
                                  const char* tooltip, Group group,
                                  Liveness liveness = Liveness::RequiresRestart,
                                  Maturity maturity = Maturity::Stable)
{
  return {setting, label, tooltip, group, liveness, maturity, 0.0f, 0.0f, 0.0f, {}};
}

constexpr RemixSettingMeta Whole(const Info<int>* setting, const char* label, const char* tooltip,
                                 Group group, float min, float max,
                                 Liveness liveness = Liveness::RequiresRestart,
                                 Maturity maturity = Maturity::Stable)
{
  return {setting, label, tooltip, group, liveness, maturity, min, max, 1.0f, {}};
}

constexpr RemixSettingMeta Choice(const Info<int>* setting, const char* label, const char* tooltip,
                                  Group group, std::span<const char* const> choices,
                                  Liveness liveness = Liveness::RequiresRestart,
                                  Maturity maturity = Maturity::Stable)
{
  return {setting,  label, tooltip, group, liveness, maturity, 0.0f,
          static_cast<float>(choices.size() - 1), 1.0f, choices};
}

constexpr RemixSettingMeta Real(const Info<float>* setting, const char* label, const char* tooltip,
                                Group group, float min, float max, float step,
                                Liveness liveness = Liveness::RequiresRestart,
                                Maturity maturity = Maturity::Stable)
{
  return {setting, label, tooltip, group, liveness, maturity, min, max, step, {}};
}

constexpr RemixSettingMeta Text(const Info<std::string>* setting, const char* label,
                                const char* tooltip, Group group,
                                Liveness liveness = Liveness::RequiresRestart,
                                Maturity maturity = Maturity::Stable)
{
  return {setting, label, tooltip, group, liveness, maturity, 0.0f, 0.0f, 0.0f, {}};
}

constexpr auto REMIX_SETTINGS_META = std::to_array<RemixSettingMeta>({
    // Runtime and scale.
    Text(&GFX_REMIX_DLL_PATH, "Runtime DLL",
         "Filename or path of the Remix runtime DLL to load. It is passed straight to LoadLibrary, "
         "so a bare name resolves next to Dolphin.exe. Point it elsewhere to try a different Remix "
         "build without moving files around.<br><br>Do not call the file 'd3d9.dll': Qt's Windows "
         "plugin imports that name, so Windows would load the runtime while Dolphin is still "
         "starting up - before a game is picked, which makes per-game Remix files impossible. A "
         "runtime still named d3d9.dll is loaded anyway, with a warning.",
         Group::RuntimeScale),
    Real(&GFX_REMIX_SCENE_SCALE, "World scale (cm per game unit)",
         "Centimetres per GameCube world unit, handed to the runtime as rtx.sceneScale. It drives "
         "every distance the path tracer cares about: light falloff, volumetric fog, displacement. "
         "Titles use wildly different world units, so this is the first thing to adjust when fog "
         "and light ranges look wrong for the size of the scene.",
         Group::RuntimeScale, 0.01f, 100.0f, 0.01f),
    Real(&GFX_REMIX_LIGHT_SCALE, "Light brightness",
         "Multiplies the brightness of every light translated from the game. The lever for 'the "
         "whole scene is too dim' or 'too bright' once the lights themselves are being translated "
         "correctly.",
         Group::RuntimeScale, 0.0f, 10.0f, 0.1f),
    Real(&GFX_REMIX_LIGHT_RANGE, "Falloff-free light range",
         "Distance, in game units, at which a light that never falls off is treated as ending. "
         "GameCube lights can be configured with no distance falloff at all, and the brightness "
         "conversion borrowed from Direct3D needs a range; this stands in for it.",
         Group::RuntimeScale, 1.0f, 100000.0f, 100.0f),

    // Files and per-game folders.
    Toggle(&GFX_REMIX_PER_GAME_PATHS, "Separate Remix files per game",
           "Give each game its own settings, mods, captures and runtime log, under "
           "Remix\\<GameID>\\ next to Dolphin.exe. Otherwise every game shares one rtx.conf - and "
           "that file holds mesh and texture hashes, so a skybox or ignore tag saved while playing "
           "one game is read back while playing another, where it means something else.<br><br>The "
           "rtx.conf and user.conf next to Dolphin.exe act as templates: a new game's folder is "
           "copied from them once, and after that the game is completely independent. They are "
           "never edited by Dolphin or by the Remix menu, so they stay a clean starting point.",
           Group::Files),
    Text(&GFX_REMIX_PER_GAME_ROOT, "Per-game folder location",
         "Where those per-game folders live. Empty means a Remix folder next to Dolphin.exe, or "
         "the Dolphin user folder if that cannot be written to. Worth setting deliberately: the "
         "RTX Remix Toolkit links a mod project to one of these folders, and moving or renaming it "
         "afterwards breaks that link.",
         Group::Files),
    Toggle(&GFX_REMIX_PER_GAME_MODS, "Per-game mods folder",
           "Load mods from the game's own folder rather than from the shared rtx-remix\\mods next "
           "to Dolphin.exe. The runtime accepts only one mods folder, so this is a choice between "
           "the two, not an addition: with this on the shared folder is not searched at all. Turn "
           "it off if you have mods there that every game should see.",
           Group::Files),
    Toggle(&GFX_REMIX_RESTART_ON_STOP, "Restart Dolphin after each game",
           "Relaunch Dolphin when emulation ends, so the next game starts in a fresh process. The "
           "Remix runtime sets up crash reporting, input hooks, the overlay and per-game folders "
           "once per process, for the first game it sees; running a second game in the same "
           "process has produced wrong config folders, a dead overlay, and crashes. Restarting is "
           "the one arrangement that is always correct. Double-clicking another game while one "
           "runs still works: the new game rides along to the restarted Dolphin.",
           Group::Files, Liveness::Live),

    // Camera recovery.
    Toggle(&GFX_REMIX_CAMERA_RECOVERY, "Camera recovery",
           "Work out where the camera is instead of pinning it at the origin. GameCube hardware "
           "has no separate view matrix, so without this the world swings around a fixed viewer "
           "and every temporal feature - motion vectors, denoising, light sampling reuse - sees "
           "the entire world move whenever you turn. Off is identical to the old fixed-camera "
           "behaviour. In practice this needs to be on.",
           Group::CameraRecovery),
    Toggle(&GFX_REMIX_CAMERA_FROM_MODELVIEW, "Take the camera from the dominant matrix",
           "Take the camera from the transform slot that the most objects share, instead of "
           "guessing it from how objects move between frames. The guess can invent camera motion "
           "out of a parked camera, which swings the whole world around the viewer. Needs "
           "RemixCameraRecovery; off restores the estimator exactly, so the two are a clean A/B.",
           Group::CameraRecovery),
    Toggle(&GFX_REMIX_VIEW_ELECTORATE_FIX, "Vote on every camera sample",
           "Let every object that survives from one frame to the next vote on how the camera "
           "moved, and ignore objects drawn more than once per frame (ocean tiles, repeated "
           "props), which cannot be matched up between frames. Off restores the old behaviour of "
           "sampling only the first 256 draws.",
           Group::CameraRecovery),
    Toggle(&GFX_REMIX_VIEW_HOLD_ON_MISS, "Hold the camera when recovery fails",
           "When the camera cannot be worked out for a few frames, keep the last known one rather "
           "than resetting to the origin. A reset re-anchors the world onto the current view, "
           "which rotates the entire sky in a single frame and can leave the horizon permanently "
           "tilted after a pitched cut.",
           Group::CameraRecovery),
    Toggle(&GFX_REMIX_VIEW_TIE_BREAK, "Break camera consensus ties",
           "When two camera motions look equally plausible, prefer the calmer one. Two comparable "
           "candidates usually means a large moving object arguing with the static world, and the "
           "camera is not the part that is moving.",
           Group::CameraRecovery),

    // Sky.
    Choice(&GFX_REMIX_SKY_AUTO_DETECT, "Skybox detection",
           "Find the game's skybox automatically by its giveaway property: it slides along with "
           "the camera while its rotation stays put. 'Off' uses only the manual RemixSkyTextures "
           "list. 'Classify' finds the sky and applies the sky options below without tagging it. "
           "'Classify and tag' also marks it for the runtime - which on this backend DELETES it, "
           "so leave that alone unless you want the game's sky gone.",
           Group::Sky, SKY_AUTO_DETECT_CHOICES),
    Toggle(&GFX_REMIX_SKY_AT_INFINITY, "Push skyboxes out to infinity",
           "Push the detected sky far behind the world so it cannot block anything. The console "
           "draws the sky first and never lets it write depth; a path tracer has no draw order, so "
           "the same dome is otherwise solid geometry parked in front of the scenery. Scaling it "
           "about the camera keeps the picture identical angle for angle.",
           Group::Sky),
    Real(&GFX_REMIX_SKY_INFINITY_SCALE, "Skybox distance multiplier",
         "How far RemixSkyAtInfinity pushes the sky out, as a multiplier. It has to beat the ratio "
         "of view distance to dome size to clear the world (about 10.5 in Wind Waker). Raise it if "
         "the sky still hides scenery; too high and floating-point precision becomes the next "
         "problem.",
         Group::Sky, 1.0f, 64.0f, 0.5f),
    Toggle(&GFX_REMIX_SKY_EMISSIVE, "Emissive skyboxes",
           "Make the detected sky glow with its own colour instead of waiting to be lit. The "
           "console draws a skybox with lighting off, so its authored colour IS the final pixel; "
           "treating it as an ordinary surface makes it go dark on the side facing away from the "
           "sun. This also lets the sky light the scene.",
           Group::Sky),
    Real(&GFX_REMIX_SKY_EMISSIVE_INTENSITY, "Skybox brightness",
         "How brightly the sky glows. 1.0 reproduces the console's colour at face value; higher "
         "makes the sky a stronger light source for the rest of the scene.",
         Group::Sky, 0.0f, 10.0f, 0.1f),
    Whole(&GFX_REMIX_SKY_AUTO_FRAMES, "Frames before classifying a skybox",
          "How many frames in a row a surface has to look like a skybox before it is accepted as "
          "one. Higher is slower to react but less likely to misfire. The decision sticks for the "
          "rest of the session, because a flickering sky would be worse than a late one.",
          Group::Sky, 1.0f, 600.0f),
    Real(&GFX_REMIX_SKY_AUTO_MIN_EXTENT, "Minimum skybox size",
         "Smallest a surface may be, as a fraction of the view distance, and still count as a "
         "skybox. It exists to reject camera-mounted view models. A GameCube skybox is a modest "
         "dome near the camera - 0.09 to 0.16 of the view distance in Wind Waker - and NOT "
         "geometry scaled out to the far plane, so at 0.25 this gate alone rejects every real "
         "skybox.",
         Group::Sky, 0.0f, 1.0f, 0.01f),
    Text(&GFX_REMIX_SKY_TEXTURES, "Manual skybox hashes",
         "Comma-separated texture hashes, as printed in dolphin.log (for example "
         "0x8b1d0f1752d9a3c1), whose draws are the skybox. Use this when auto-detection cannot see "
         "it. Explicit hashes beat the runtime's own texture tagging, which never reaches draws "
         "submitted through the Remix API.",
         Group::Sky),
    Text(&GFX_REMIX_SKY_VETO_HASHES, "Never-a-skybox hashes",
         "Comma-separated hashes - texture or mesh - that must never be treated as sky, for when "
         "the detector is wrong about something. Highest precedence: a veto beats the manual list, "
         "which beats auto-detection.",
         Group::Sky),
    Toggle(&GFX_REMIX_SKY_AUTO_UNTEXTURED_IGNORE, "Remove untextured sky domes",
           "Only consulted when RemixSkyAutoDetect is set to tag. Untextured sky is removed from "
           "the scene outright rather than marked as sky. Four of Wind Waker's seven sky meshes "
           "have no texture at all, and such a dome left in place is a closed shell around the "
           "viewer that blocks a sky mod's sun - which reads as a dark scene rather than as a sky "
           "problem.",
           Group::Sky),
    Choice(&GFX_REMIX_SKY_MODE, "Manual skybox handling",
           "Superseded legacy sky heuristic based on depth-buffer state. Best left off: in Wind "
           "Waker it matched 41 to 88 draws a frame and never the actual dome, which is "
           "depth-tested. Use RemixSkyAutoDetect instead.",
           Group::Sky, SKY_MODE_CHOICES),

    // GX semantics.
    Toggle(&GFX_REMIX_GX_COLOR, "Material and vertex colour",
           "Read each draw's material and ambient colour registers, so surfaces the game tints "
           "keep that tint. Off drops register tints outright, and the vertex colour reaches the "
           "renderer with nothing reading it.",
           Group::GxSemantics),
    Toggle(&GFX_REMIX_GX_TEV_COLOR, "Resolve colour through the TEV chain",
           "Evaluate the game's colour-combining chain and fold the result into the vertex "
           "colours. GameCube titles keep a surface's palette in combiner REGISTERS and use the "
           "vertex colour only as the blend weight between two of them, so without this a "
           "register-coloured surface comes out greyscale or white. This is what fixed Wind "
           "Waker's white ocean and sky.",
           Group::GxSemantics),
    Toggle(&GFX_REMIX_GX_TEXTURE_STAGE, "Sample from any texture stage",
           "Take the surface texture from whichever combiner stage actually samples one, for draws "
           "whose first stage samples nothing. Wind Waker's sea samples its water texture on stage "
           "1 and nothing on stage 0, so the whole surface used to arrive untextured. A draw that "
           "samples on stage 0 behaves exactly as before.",
           Group::GxSemantics),
    Toggle(&GFX_REMIX_GX_RAMP_ALBEDO_SKIP, "Skip toon-ramp albedo",
           "Take the albedo from a later combiner stage when stage 0's coordinate is generated "
           "from a lit channel. A Color0/Color1 texgen means stage 0 samples a shading ramp, not "
           "the surface, and only one texture reaches Remix - so handing it the ramp paints the "
           "model in the ramp. Shading is the path tracer's job.",
           Group::GxSemantics),
    Toggle(&GFX_REMIX_GX_RAS_CHANNEL, "Per-stage colour channel (world)",
           "Take the vertex colour channel from the combiner stage that actually uses it rather "
           "than from stage 0. The console names that channel per stage, and the stage that reads "
           "it is routinely not the first one. World geometry only; the 2D equivalent is "
           "RemixUiRasChannel.",
           Group::GxSemantics),
    Toggle(&GFX_REMIX_GX_TEXGEN, "Texture coordinate generation",
           "Run the game's texture-coordinate generation instead of passing raw vertex coordinates "
           "through. This is how every scrolling and animated texture on the console works, and "
           "they are frozen without it.",
           Group::GxSemantics),
    Toggle(&GFX_REMIX_GX_BLEND, "Blend mode translation",
           "Translate the game's blend settings so the runtime can classify transparency. Without "
           "it, fire, glows, light shafts, windows and water are all submitted as opaque geometry "
           "that also casts full shadows.",
           Group::GxSemantics),
    Toggle(&GFX_REMIX_GX_LIGHT_FIX, "GameCube light translation",
           "Translate the game's lights the way the console's own renderer reads them: the "
           "spotlight cone pointed the right way (the raw data points at the light, not away from "
           "it), a real soft cone edge, and brightness derived the way the Remix runtime does it "
           "rather than from the raw 0-1 colour, which is roughly a hundred times too dim.",
           Group::GxSemantics),
    Toggle(&GFX_REMIX_GX_LIGHT_NO_FALLOFF_DISTANT, "Treat falloff-free lights as suns",
           "Treat a light with neither distance nor angle falloff as a distant sun rather than a "
           "light bulb. GameCube hardware has no directional light type, so a sun is an ordinary "
           "light parked very far away with falloff switched off. Sending that as a point light "
           "applies an inverse-square law the console never applied, so it contributes nothing "
           "while still counting as 'a light exists'.",
           Group::GxSemantics),
    Toggle(&GFX_REMIX_GX_LIGHT_DROP_DISTANT, "Drop the game's directional lights",
           "Throw away the game's own sun so a sky or atmosphere mod owns the key light. Two suns "
           "double up: the scene reads far too bright and the game's flat white fights the mod's "
           "sun. Local lights - lamps, glows, cone lights - are kept. Off by default, because with "
           "no sky mod loaded this removes the scene's only key light.",
           Group::GxSemantics),
    Toggle(&GFX_REMIX_FALLBACK_LIGHT, "Fallback light for unlit scenes",
           "Add one distant light on frames where the game submitted no lights at all. Many "
           "GameCube titles bake their lighting into vertex colours and enable no real lights, "
           "which would leave the path tracer nothing to integrate. Turn it off whenever a sky mod "
           "is providing the light. Note this is the backend's own light, not the runtime's "
           "rtx.fallbackLightMode - there are three separate things that can put a distant light "
           "in a scene.",
           Group::GxSemantics),
    Toggle(&GFX_REMIX_WORLD_SCISSOR_SKIP, "Skip fully scissored draws",
           "Skip world geometry the game has clipped away to nothing. Only the completely-empty "
           "case is acted on, deliberately: a path tracer has no screen-space clipping, so a draw "
           "the clip merely trims is submitted whole.",
           Group::GxSemantics),
    Toggle(&GFX_REMIX_GPU_SKINNING, "GPU-skinned characters",
           "Let the renderer pose characters, instead of moving every vertex on the CPU first. "
           "Moving them here rebuilds the character from scratch every frame, so the renderer "
           "cannot tell it is the same character as last frame and leaves a ghostly duplicate "
           "trailing behind anything that moves. Sending the unposed model plus the bones fixes "
           "that, and it is also what makes characters replaceable by a mod. Off restores the old "
           "behaviour exactly - turn it off if a character ever comes out mangled.",
           Group::GxSemantics, Liveness::RequiresRestart, Maturity::Experimental),
    Toggle(&GFX_REMIX_DYNAMIC_MESH_IDENTITY, "Stable identity for regenerated meshes",
           "Recognise geometry the game rebuilds every frame - characters the game animates on "
           "its own CPU - and update one mesh in place instead of creating a brand new one each "
           "frame. Without this the renderer cannot tell it is the same object as last frame, so "
           "a ghostly duplicate trails behind it and captures fill up with thousands of one-frame "
           "meshes. Off restores the old behaviour exactly - turn it off if two identically-built "
           "objects ever appear to swap or flicker for a frame.",
           Group::GxSemantics, Liveness::RequiresRestart, Maturity::Experimental),

    // Projection and viewport.
    Toggle(&GFX_REMIX_PROJECTION_FIX, "Per-draw projection correction",
           "Correct for the game changing its projection partway through a frame. Remix takes one "
           "camera per frame, but the console sets projection per draw, so each draw's difference "
           "from the frame's reference is folded into its position. A dropped off-centre term "
           "looks almost exactly like a small camera rotation.",
           Group::ProjectionViewport),
    Toggle(&GFX_REMIX_VIEWPORT_FIX, "Per-draw viewport correction",
           "The same correction for the screen rectangle a draw targets. Without it a "
           "picture-in-picture panel or an F-Zero GX position-ladder portrait is rendered through "
           "the full-screen rectangle and lands in the middle of the world. Inert while "
           "RemixProjectionFix is off, and exactly equivalent to it when every draw uses the same "
           "rectangle.",
           Group::ProjectionViewport),
    Toggle(&GFX_REMIX_VIEWPORT_REF_XFB, "Latch the camera from the presented viewport",
           "Only let a draw define the frame's camera and screen rectangle if its viewport covers "
           "most of the region the console actually presents. Some games render a small helper "
           "pass into a corner of the framebuffer before the real scene - Sonic Unleashed does - "
           "and without this the helper's quarter-rectangle becomes 'the screen', which shows only "
           "the top-left quarter of the game magnified 2x. Games whose first draw is the real "
           "scene behave identically with this on or off.",
           Group::ProjectionViewport, Liveness::Live),

    // UI overlay.
    Choice(&GFX_REMIX_UI_MODE, "2D and HUD handling",
           "What to do with the game's 2D layer: HUD, menus, every flat screen. 'Drop' removes it "
           "entirely. 'Screen overlay' draws it in software and composites it over the finished "
           "image, which is what looks like a normal UI. 'World-space plane' puts it inside the "
           "traced world, where it is genuinely lit and denoised - a look experiment that swims "
           "whenever the camera moves.",
           Group::UiOverlay, UI_MODE_CHOICES, Liveness::Live),
    Toggle(&GFX_REMIX_SKIP_MINOR_FRAMES, "Drop 2D-only frames",
           "Some games present twice per rendered frame: the full world, then a frame holding "
           "only the 2D layer. Tracing that second frame as a scene makes the world strobe at "
           "half rate and the denoiser never settles - Skylanders shows only sky without this. "
           "Dropping the minor frame shows each full frame twice as long instead. Menus are "
           "unaffected: a frame only counts as minor while recent frames held a real scene.",
           Group::UiOverlay, Liveness::Live, Maturity::Experimental),
    Real(&GFX_REMIX_UI_OVERLAY_SCALE, "Overlay resolution",
         "Fraction of the window the 2D overlay is drawn at. The main UI performance lever: the "
         "overlay is rasterized on the CPU and is fill-rate bound, so 0.5 quarters its cost. At "
         "1.0 it costs 3 to 9 ms a frame on Wind Waker's busiest 2D screens. GameCube UI is "
         "authored for a 640x528 framebuffer, so there is little real detail to lose on a large "
         "window. Applies immediately, and the overlay now follows window resizes too.",
         Group::UiOverlay, 0.1f, 1.0f, 0.05f, Liveness::Live),
    Toggle(&GFX_REMIX_UI_OVERLAY_CAP, "Cap the overlay at 2x GameCube resolution",
           "Never rasterize the 2D overlay larger than 1280x1056 - twice the console's own "
           "framebuffer - however large the window. The UI art holds no detail past that, but the "
           "CPU cost keeps growing with the window: F-Zero GX's menu costs 84 ms a frame at a "
           "2560-wide window and 15 ms under the cap, which is the difference between 10 and 60 "
           "FPS. Turn off only if 2D edges look too soft on a very large display.",
           Group::UiOverlay, Liveness::Live),
    Toggle(&GFX_REMIX_UI_FRAME_CACHE, "Reuse unchanged 2D frames",
           "When a frame's 2D draws are exactly the ones the previous frame drew, show the "
           "previous overlay instead of rasterizing it again. Menus and pause screens hold still "
           "for hundreds of frames, so this removes most of their CPU cost. Any change in any "
           "draw re-rasterizes, so animation is unaffected; if UI ever freezes while the game "
           "visibly animates, turn this off and report it. Watch the 'cached' flag in the log.",
           Group::UiOverlay, Liveness::Live),
    Toggle(&GFX_REMIX_UI_SCALE_TO_XFB, "Scale the overlay to the presented image",
           "Stretch the overlay over the region the console actually presents rather than over the "
           "whole internal framebuffer. Wind Waker presents 480 rows rather than 528, so the old "
           "mapping squeezed the entire HUD into the top 91% of the window and left the bottom "
           "empty.",
           Group::UiOverlay),
    Toggle(&GFX_REMIX_UI_RAS_CHANNEL, "Per-stage colour channel (2D)",
           "The per-stage vertex-colour rule described under RemixGxRasChannel, applied to 2D "
           "draws. This is what removes Wind Waker's leaked title-screen HUD. Not settled: the "
           "same change may also suppress legitimate UI, so check a frame that contains PRESS "
           "START before trusting it.",
           Group::UiOverlay),
    Toggle(&GFX_REMIX_UI_DROP_DST_ALPHA, "Drop destination-alpha blends",
           "Skip 2D draws that blend against what is already on screen. The overlay is composited "
           "over an already-finished image, so there is nothing for such a draw to read; letting "
           "it fall through to ordinary blending paints an opaque wash over the whole screen, "
           "which is Wind Waker's title-screen glare pass. Narrow on purpose - ordinary fades are "
           "untouched.",
           Group::UiOverlay),
    Toggle(&GFX_REMIX_UI_DROP_EFB_COPY_TEXTURES, "Drop draws reading copied framebuffers",
           "Skip 2D draws textured from memory the game filled with a framebuffer copy. This "
           "backend does not carry out most of those copies, so that memory holds stale bytes that "
           "still decode into a valid-looking texture. Measured a no-op on Wind Waker, hence off; "
           "the risk is that a game legitimately composing its menu this way loses the menu.",
           Group::UiOverlay),
    Toggle(&GFX_REMIX_UI_DROP_PRE_WORLD_BLANK, "Drop framebuffer-clear rectangles",
           "Drop screen-clear rectangles that arrive before any world geometry in the frame. Those "
           "are clears, not UI: the console draws the scene over them, but this backend composites "
           "2D ON TOP of the traced image, so they land over everything and paint the screen flat. "
           "This is the SpongeBob white-box fix. Two shapes count as a clear - untextured, and "
           "opaque-with-depth-write carrying a placeholder texture of 16x16 or smaller (Mario "
           "Kart: Double Dash's in-race clear is a full-screen quad on a 4x4 texture, which the "
           "untextured test alone let through). Real 2D artwork is neither, so a genuine "
           "background is unaffected.",
           Group::UiOverlay),
    Toggle(&GFX_REMIX_UI_TAG_ROUTING, "Route 2D draws by Remix texture tags",
           "Let the Remix dev menu decide what happens to each 2D draw. Tag a texture 'UI Texture' "
           "and it is always composited; 'Ignore' and it disappears; 'World Space UI' and it is "
           "drawn inside the traced world. While the dev menu is open every 2D draw is temporarily "
           "sent world-side so it can be clicked and tagged - including untextured boxes, which "
           "have no thumbnail to find. Inert until something is tagged, and tags take about two "
           "frames to apply.",
           Group::UiOverlay, Liveness::Live),
    Toggle(&GFX_REMIX_UI_TAG_PERSPECTIVE, "Route 3D-drawn HUD by Remix texture tags",
           "Extend the 'UI Texture' tag to elements the game draws in 3D. Not every HUD is flat 2D "
           "- Resident Evil 4 parks its health ring and ammo counter just in front of the camera "
           "and draws them through the ordinary 3D view, so tagging them did nothing. With this on, "
           "a tagged 3D element is composited into the same screen overlay the 2D layer uses. Only "
           "the 'UI Texture' tag acts here; 'Ignore' and 'World Space UI' on 3D draws are already "
           "handled by Remix itself. Needs 'Route 2D draws by Remix texture tags'. Watch the "
           "'persp' counter in the log.",
           Group::UiOverlay, Liveness::Live, Maturity::Experimental),
    Toggle(&GFX_REMIX_UI_DROP_PRE_WORLD, "Drop all 2D drawn before the world",
           "Drop every 2D draw that arrives before any world geometry this frame, textured or not. "
           "The wider version of 'Drop framebuffer-clear rectangles', for games that wash the "
           "screen with a textured quad. Riskier: a game whose real 2D background is drawn before "
           "the world loses that background. Off by default - enable per game and watch the "
           "'preworld' counter in the log.",
           Group::UiOverlay, Liveness::Live, Maturity::Experimental),
    Toggle(&GFX_REMIX_UI_DROP_FULL_SCREEN_OPAQUE, "Drop full-screen opaque 2D over the scene",
           "Drop opaque 2D draws covering essentially the whole image on a frame that already has "
           "world geometry. Such a draw hides the traced scene completely, so it is almost always "
           "a screen-space fake the console would have drawn underneath. Off by default: a genuine "
           "full-screen 2D background on a frame with world geometry would vanish. Watch the "
           "'fullscr' counter in the log.",
           Group::UiOverlay, Liveness::Live, Maturity::Experimental),
    Toggle(&GFX_REMIX_UI_STRICT, "Show only tagged 2D draws",
           "Assume the game's 2D layer is junk and keep only what has been tagged 'UI Texture' in "
           "the Remix dev menu. The opposite policy to the drop heuristics, for a game whose 2D is "
           "mostly washes and fakes with a few elements worth keeping. Can be flipped while the "
           "game runs, which is how those elements get found. Needs 'Route 2D draws by Remix "
           "texture tags'.",
           Group::UiOverlay, Liveness::Live, Maturity::Experimental),
    Toggle(&GFX_REMIX_UI_DEPTH, "Depth-test 2D draws that ask for it",
           "Give the 2D overlay a depth buffer for draws whose z test is enabled. A HUD drawn "
           "through the 3D frustum can rely on depth instead of draw order - Resident Evil 4 "
           "submits its HUD background after its text and lets the z test sort them, so without "
           "this the background paints over the text. Ordinary 2D draws have the test off and are "
           "untouched. Off restores the pure painter's algorithm.",
           Group::UiOverlay, Liveness::Live),
    Toggle(&GFX_REMIX_UI_WORLD_VIEW, "Show the 2D layer in the world (tagging view)",
           "Turn the game's whole 2D layer into clickable world geometry so elements can be "
           "click-tagged in the Remix dev menu - the only way to reach an untextured white box, "
           "which has no thumbnail in the texture grid. Flip it on to tag, off to play; it can be "
           "changed while the game runs. This used to happen automatically whenever the dev menu "
           "was open, which moved the HUD around every time the menu was opened for any other "
           "reason.",
           Group::UiOverlay, Liveness::Live),
    Real(&GFX_REMIX_WORLD_UI_DISTANCE, "World-space HUD distance",
         "World-space UI mode only. How far in front of the camera the UI plane sits, in game "
         "units. Just past the near plane by default, so world geometry cannot poke through the "
         "HUD. The plane scales with distance, so its apparent size does not change.",
         Group::UiOverlay, 0.1f, 20.0f, 0.1f, Liveness::RequiresRestart, Maturity::Experimental),
    Toggle(&GFX_REMIX_WORLD_UI_FLIP_Y, "Flip the world-space HUD vertically",
           "World-space UI mode only. Flip the UI plane vertically. The default should be right; "
           "this exists so that an upside-down HUD does not cost a rebuild to test.",
           Group::UiOverlay, Liveness::RequiresRestart, Maturity::Experimental),

    // EFB emulation.
    Toggle(&GFX_REMIX_EFB_EMULATION, "Framebuffer copy handling",
           "Keep a real CPU-side copy of the console's framebuffer, so screen clears, CPU reads "
           "and CPU writes all work and a classified subset of the game's framebuffer copies can "
           "be carried out. Off is the pre-feature behaviour in every particular: reads return "
           "zero, writes and clears do nothing, every copy counter reads zero.",
           Group::Efb),
    Toggle(&GFX_REMIX_EFB_COPY_2D, "Execute 2D-only copies",
           "Carry out framebuffer copies taken before any 3D geometry was drawn this frame. This "
           "is the one class whose contents this backend holds exactly, by construction - "
           "render-to-texture menus, title screens, composed text windows. The only copy class on "
           "by default, and the only default here that changes behaviour.",
           Group::Efb),
    Toggle(&GFX_REMIX_EFB_COPY_SCENE, "Execute copies containing 3D",
           "Carry out framebuffer copies taken after 3D geometry was drawn this frame. Off by "
           "default because the copied region very likely holds world pixels, which this backend "
           "never rasterizes, so carrying it out hands the game a flat clear-coloured rectangle "
           "where the console had the scene. Turn it on for a game that draws the world early and "
           "then composes a genuine 2D element by copy later in the same frame.",
           Group::Efb, Liveness::RequiresRestart, Maturity::Experimental),
    Toggle(&GFX_REMIX_EFB_COPY_DEPTH, "Execute depth copies",
           "Carry out depth-buffer copies, which are almost always shadow maps. Off because the "
           "path tracer casts real shadows, and because this backend's depth plane holds only the "
           "clear value - so carrying one out would hand the game a uniform depth map, i.e. a "
           "full-screen wrong shadow test, which is worse than its absence.",
           Group::Efb, Liveness::RequiresRestart, Maturity::Experimental),
    Toggle(&GFX_REMIX_EFB_COPY_INTENSITY, "Execute intensity copies",
           "Carry out brightness-extraction copies, which is what a bloom or glow chain opens "
           "with. Off because the runtime does its own bloom, and feeding the game's chain a flat "
           "clear-brightness only blends a uniform wash back over the screen. Turn it on if a game "
           "uses one as a legitimate 2D mask.",
           Group::Efb, Liveness::RequiresRestart, Maturity::Experimental),
    Toggle(&GFX_REMIX_EFB_XFB_ENCODE, "Execute presentation copies",
           "Carry out the frame's presentation copy. Off: the Remix runtime produces and presents "
           "the picture, nothing here consumes this copy, and it is the only recurring "
           "full-width per-frame cost in the whole feature. Turning it on does NOT wire Dolphin's "
           "screenshot or video-dump pipeline to Remix frames.",
           Group::Efb, Liveness::RequiresRestart, Maturity::Experimental),
    Toggle(&GFX_REMIX_EFB_UI_COMPOSE, "Include the 2D layer in copies",
           "Draw the frame's 2D layer into the framebuffer before a copy is carried out. Without "
           "it, a carried-out copy records bare clear colour - correct, but empty. Costs nothing "
           "on a frame whose copies are all discarded, because it runs only on the execute path.",
           Group::Efb),
    Toggle(&GFX_REMIX_EFB_SKIP_DISCARDED_TEX, "Skip draws reading discarded copies",
           "Skip draws that read from a framebuffer copy this backend discarded. A discarded copy "
           "leaves zeroes behind, and zero bytes decode into a perfectly valid blank texture, so "
           "nothing else refuses the draw and it renders as a blank rectangle over the traced "
           "scene. Confirmed on SpongeBob: Battle for Bikini Bottom, where it was a white box over "
           "a quarter of the screen.",
           Group::Efb),
    Toggle(&GFX_REMIX_EFB_DROP_AUX_PASS, "Drop helper passes feeding discarded copies",
           "Skip 3D draws that only exist to feed a framebuffer copy this backend throws away - a "
           "sub-rectangle that was copied out and cleared without being executed last frame. On "
           "console the clear erases those pixels; here nothing would, so they linger as a ghost "
           "mini-scene in a corner of the view. Never fires on the viewport carrying the real "
           "scene. Watch the 'aux' counter in the log.",
           Group::Efb, Liveness::Live),

    // Diagnostics.
    Toggle(&GFX_REMIX_LOG_STATS, "Per-frame statistics in the log",
           "Print one statistics line per frame to dolphin.log: draw classification, mesh and "
           "instance counts, colour routes, lights, UI and sky. The primary instrument for "
           "everything else here. It needs Logger.ini to have Video = True, Verbosity = 4 and "
           "WriteToFile = True, or the line goes nowhere. The log APPENDS across runs, so delete "
           "it before a run you intend to read.",
           Group::Diagnostics, Liveness::Live, Maturity::Diagnostic),
    Toggle(&GFX_REMIX_TRACE_COLORS, "Trace colour resolution",
           "Print, per draw, where a world surface's colour comes from: the combiner chain, both "
           "channels' lighting state, the resolved arguments and the raw vertex colour bytes. "
           "Heavy - hundreds of lines on each traced frame. Turn it on to answer 'why is this "
           "vertex-coloured surface white', which the frame counters cannot.",
           Group::Diagnostics, Liveness::Live, Maturity::Diagnostic),
    Toggle(&GFX_REMIX_DEBUG_COLOR_ROUTES, "Paint draws by colour route",
           "Paint every world surface a flat colour naming which colour path it took: red for "
           "vertex colour with the combiner chain folded in, magenta for vertex colour without, "
           "blue for a register tint, green for no colour at all. The counters say how many draws "
           "took each route; this says which PIXELS, which is usually the actual question.",
           Group::Diagnostics, Liveness::RequiresRestart, Maturity::Diagnostic),
    Toggle(&GFX_REMIX_TRACE_PROJECTIONS, "Trace projections",
           "Print every distinct projection seen in a frame. The per-frame summary already reports "
           "these whenever there is more than one, so this is only needed to watch a projection "
           "change live.",
           Group::Diagnostics, Liveness::Live, Maturity::Diagnostic),
    Toggle(&GFX_REMIX_TRACE_MODELVIEWS, "Trace modelview matrices",
           "Print the per-frame histogram of object transforms - which matrix the most distinct "
           "meshes share, i.e. the camera candidate. This is also where RemixCameraFromModelview "
           "reads the camera from, so it is forced on while that option is enabled.",
           Group::Diagnostics, Liveness::Live, Maturity::Diagnostic),
    Toggle(&GFX_REMIX_TRACE_EFB_COPIES, "Trace framebuffer copies",
           "Print every framebuffer copy's rectangle, destination and classification, plus each 2D "
           "draw's footprint. That line names every input the decision used, which is what makes a "
           "misclassified copy diagnosable instead of mysterious.",
           Group::Diagnostics, Liveness::Live, Maturity::Diagnostic),
    Whole(&GFX_REMIX_UI_DUMP_FRAME, "Dump the overlay on frame",
          "Non-zero writes the composited 2D overlay at that frame number to "
          "Logs/remix-ui-overlay.bmp, once, drawn over a checkerboard so transparent and black can "
          "be told apart. The only way to see what the overlay actually produced without trusting "
          "the screen.",
          Group::Diagnostics, 0.0f, 999999.0f, Liveness::Live, Maturity::Diagnostic),
});

static_assert(REMIX_SETTINGS_META.size() == kRemixSettingCount,
              "Every Remix Info<> declaration in RemixSettings.h needs a metadata row here, or the "
              "settings tab would silently omit it. Add the row (or fix kRemixSettingCount).");
}  // namespace

std::span<const RemixSettingMeta> GetRemixSettingsMetadata()
{
  return REMIX_SETTINGS_META;
}

}  // namespace Config
