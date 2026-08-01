// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Config/GraphicsSettings.h"

#include <string>

#include "VideoCommon/VideoConfig.h"

namespace Config
{
// Configuration Information

// Graphics.Hardware

const Info<bool> GFX_VSYNC{{System::GFX, "Hardware", "VSync"}, false};
const Info<int> GFX_ADAPTER{{System::GFX, "Hardware", "Adapter"}, 0};

// Graphics.Settings

const Info<bool> GFX_WIDESCREEN_HACK{{System::GFX, "Settings", "wideScreenHack"}, false};
const Info<AspectMode> GFX_ASPECT_RATIO{{System::GFX, "Settings", "AspectRatio"}, AspectMode::Auto};
const Info<int> GFX_CUSTOM_ASPECT_RATIO_WIDTH{{System::GFX, "Settings", "CustomAspectRatioWidth"},
                                              1};
const Info<int> GFX_CUSTOM_ASPECT_RATIO_HEIGHT{{System::GFX, "Settings", "CustomAspectRatioHeight"},
                                               1};
const Info<AspectMode> GFX_SUGGESTED_ASPECT_RATIO{{System::GFX, "Settings", "SuggestedAspectRatio"},
                                                  AspectMode::Auto};
const Info<u32> GFX_WIDESCREEN_HEURISTIC_TRANSITION_THRESHOLD{
    {System::GFX, "Settings", "WidescreenHeuristicTransitionThreshold"}, 3};
const Info<float> GFX_WIDESCREEN_HEURISTIC_ASPECT_RATIO_SLOP{
    {System::GFX, "Settings", "WidescreenHeuristicAspectRatioSlop"}, 0.11f};
const Info<float> GFX_WIDESCREEN_HEURISTIC_STANDARD_RATIO{
    {System::GFX, "Settings", "WidescreenHeuristicStandardRatio"}, 1.f};
const Info<float> GFX_WIDESCREEN_HEURISTIC_WIDESCREEN_RATIO{
    {System::GFX, "Settings", "WidescreenHeuristicWidescreenRatio"}, (16 / 9.f) / (4 / 3.f)};
const Info<bool> GFX_CROP_TO_ASPECT_RATIO{{System::GFX, "Settings", "Crop"}, false};
const Info<bool> GFX_CROP_CUSTOM{{System::GFX, "Settings", "CropCustom"}, false};
const Info<int> GFX_CROP_CUSTOM_LEFT{{System::GFX, "Settings", "CropCustomLeft"}, 0};
const Info<int> GFX_CROP_CUSTOM_TOP{{System::GFX, "Settings", "CropCustomTop"}, 0};
const Info<int> GFX_CROP_CUSTOM_RIGHT{{System::GFX, "Settings", "CropCustomRight"}, 0};
const Info<int> GFX_CROP_CUSTOM_BOTTOM{{System::GFX, "Settings", "CropCustomBottom"}, 0};
const Info<int> GFX_SAFE_TEXTURE_CACHE_COLOR_SAMPLES{
    {System::GFX, "Settings", "SafeTextureCacheColorSamples"}, 128};
const Info<bool> GFX_SHOW_FPS{{System::GFX, "Settings", "ShowFPS"}, false};
const Info<bool> GFX_SHOW_FTIMES{{System::GFX, "Settings", "ShowFTimes"}, false};
const Info<bool> GFX_SHOW_VPS{{System::GFX, "Settings", "ShowVPS"}, false};
const Info<bool> GFX_SHOW_VTIMES{{System::GFX, "Settings", "ShowVTimes"}, false};
const Info<bool> GFX_SHOW_GRAPHS{{System::GFX, "Settings", "ShowGraphs"}, false};
const Info<bool> GFX_SHOW_SPEED{{System::GFX, "Settings", "ShowSpeed"}, false};
const Info<bool> GFX_SHOW_SPEED_COLORS{{System::GFX, "Settings", "ShowSpeedColors"}, true};
const Info<bool> GFX_MOVABLE_PERFORMANCE_METRICS{
    {System::GFX, "Settings", "MovablePerformanceMetrics"}, false};
const Info<int> GFX_PERF_SAMP_WINDOW{{System::GFX, "Settings", "PerfSampWindowMS"}, 1000};
const Info<bool> GFX_SHOW_NETPLAY_PING{{System::GFX, "Settings", "ShowNetPlayPing"}, false};
const Info<bool> GFX_SHOW_NETPLAY_MESSAGES{{System::GFX, "Settings", "ShowNetPlayMessages"}, false};
const Info<bool> GFX_LOG_RENDER_TIME_TO_FILE{{System::GFX, "Settings", "LogRenderTimeToFile"},
                                             false};
const Info<bool> GFX_OVERLAY_STATS{{System::GFX, "Settings", "OverlayStats"}, false};
const Info<bool> GFX_OVERLAY_PROJ_STATS{{System::GFX, "Settings", "OverlayProjStats"}, false};
const Info<bool> GFX_OVERLAY_SCISSOR_STATS{{System::GFX, "Settings", "OverlayScissorStats"}, false};
const Info<bool> GFX_SHOW_INTERNAL_RESOLUTION{{System::GFX, "Settings", "ShowInternalResolution"},
                                              false};
const Info<bool> GFX_DUMP_TEXTURES{{System::GFX, "Settings", "DumpTextures"}, false};
const Info<bool> GFX_DUMP_MIP_TEXTURES{{System::GFX, "Settings", "DumpMipTextures"}, true};
const Info<bool> GFX_DUMP_BASE_TEXTURES{{System::GFX, "Settings", "DumpBaseTextures"}, true};
const Info<int> GFX_TEXTURE_PNG_COMPRESSION_LEVEL{
    {System::GFX, "Settings", "TexturePNGCompressionLevel"}, 6};
const Info<bool> GFX_HIRES_TEXTURES{{System::GFX, "Settings", "HiresTextures"}, false};
const Info<bool> GFX_CACHE_HIRES_TEXTURES{{System::GFX, "Settings", "CacheHiresTextures"}, false};
const Info<bool> GFX_DUMP_EFB_TARGET{{System::GFX, "Settings", "DumpEFBTarget"}, false};
const Info<bool> GFX_DUMP_XFB_TARGET{{System::GFX, "Settings", "DumpXFBTarget"}, false};
const Info<bool> GFX_DUMP_FRAMES_AS_IMAGES{{System::GFX, "Settings", "DumpFramesAsImages"}, false};
const Info<bool> GFX_USE_LOSSLESS{{System::GFX, "Settings", "UseLossless"}, false};
const Info<std::string> GFX_DUMP_FORMAT{{System::GFX, "Settings", "DumpFormat"}, "avi"};
const Info<std::string> GFX_DUMP_CODEC{{System::GFX, "Settings", "DumpCodec"}, ""};
const Info<std::string> GFX_DUMP_PIXEL_FORMAT{{System::GFX, "Settings", "DumpPixelFormat"}, ""};
const Info<std::string> GFX_DUMP_ENCODER{{System::GFX, "Settings", "DumpEncoder"}, ""};
const Info<std::string> GFX_DUMP_PATH{{System::GFX, "Settings", "DumpPath"}, ""};
const Info<int> GFX_BITRATE_KBPS{{System::GFX, "Settings", "BitrateKbps"}, 25000};
const Info<FrameDumpResolutionType> GFX_FRAME_DUMPS_RESOLUTION_TYPE{
    {System::GFX, "Settings", "FrameDumpsResolutionType"},
    FrameDumpResolutionType::XFBAspectRatioCorrectedResolution};
const Info<int> GFX_PNG_COMPRESSION_LEVEL{{System::GFX, "Settings", "PNGCompressionLevel"}, 6};
const Info<bool> GFX_ENABLE_GPU_TEXTURE_DECODING{
    {System::GFX, "Settings", "EnableGPUTextureDecoding"}, false};
const Info<bool> GFX_ENABLE_PIXEL_LIGHTING{{System::GFX, "Settings", "EnablePixelLighting"}, false};
const Info<bool> GFX_FAST_DEPTH_CALC{{System::GFX, "Settings", "FastDepthCalc"}, true};
const Info<u32> GFX_MSAA{{System::GFX, "Settings", "MSAA"}, 1};
const Info<bool> GFX_SSAA{{System::GFX, "Settings", "SSAA"}, false};
const Info<int> GFX_EFB_SCALE{{System::GFX, "Settings", "InternalResolution"}, 1};
const Info<int> GFX_MAX_EFB_SCALE{{System::GFX, "Settings", "MaxInternalResolution"}, 12};
const Info<bool> GFX_TEXFMT_OVERLAY_ENABLE{{System::GFX, "Settings", "TexFmtOverlayEnable"}, false};
const Info<bool> GFX_TEXFMT_OVERLAY_CENTER{{System::GFX, "Settings", "TexFmtOverlayCenter"}, false};
const Info<bool> GFX_ENABLE_WIREFRAME{{System::GFX, "Settings", "WireFrame"}, false};
const Info<bool> GFX_DISABLE_FOG{{System::GFX, "Settings", "DisableFog"}, false};
const Info<bool> GFX_BORDERLESS_FULLSCREEN{{System::GFX, "Settings", "BorderlessFullscreen"},
                                           false};
const Info<bool> GFX_ENABLE_VALIDATION_LAYER{{System::GFX, "Settings", "EnableValidationLayer"},
                                             false};

const Info<bool> GFX_BACKEND_MULTITHREADING{{System::GFX, "Settings", "BackendMultithreading"},
                                            true};
const Info<int> GFX_COMMAND_BUFFER_EXECUTE_INTERVAL{
    {System::GFX, "Settings", "CommandBufferExecuteInterval"}, 100};

const Info<bool> GFX_SHADER_CACHE{{System::GFX, "Settings", "ShaderCache"}, true};
const Info<bool> GFX_WAIT_FOR_SHADERS_BEFORE_STARTING{
    {System::GFX, "Settings", "WaitForShadersBeforeStarting"}, false};
const Info<ShaderCompilationMode> GFX_SHADER_COMPILATION_MODE{
    {System::GFX, "Settings", "ShaderCompilationMode"}, ShaderCompilationMode::Synchronous};
const Info<int> GFX_SHADER_COMPILER_THREADS{{System::GFX, "Settings", "ShaderCompilerThreads"}, 1};
const Info<int> GFX_SHADER_PRECOMPILER_THREADS{
    {System::GFX, "Settings", "ShaderPrecompilerThreads"}, -1};
const Info<bool> GFX_SAVE_TEXTURE_CACHE_TO_STATE{
    {System::GFX, "Settings", "SaveTextureCacheToState"}, true};
const Info<bool> GFX_PREFER_VS_FOR_LINE_POINT_EXPANSION{
    {System::GFX, "Settings", "PreferVSForLinePointExpansion"}, false};
const Info<bool> GFX_CPU_CULL{{System::GFX, "Settings", "CPUCull"}, false};

const Info<TriState> GFX_MTL_MANUALLY_UPLOAD_BUFFERS{
    {System::GFX, "Settings", "ManuallyUploadBuffers"}, TriState::Auto};
const Info<TriState> GFX_MTL_USE_PRESENT_DRAWABLE{
    {System::GFX, "Settings", "MTLUsePresentDrawable"}, TriState::Auto};

const Info<bool> GFX_SW_DUMP_OBJECTS{{System::GFX, "Settings", "SWDumpObjects"}, false};
const Info<bool> GFX_SW_DUMP_TEV_STAGES{{System::GFX, "Settings", "SWDumpTevStages"}, false};
const Info<bool> GFX_SW_DUMP_TEV_TEX_FETCHES{{System::GFX, "Settings", "SWDumpTevTexFetches"},
                                             false};

const Info<bool> GFX_PREFER_GLES{{System::GFX, "Settings", "PreferGLES"}, false};

const Info<bool> GFX_MODS_ENABLE{{System::GFX, "Settings", "EnableMods"}, false};

// Remix video backend. RemixDllPath is passed straight to LoadLibrary, so the
// bare name resolves next to Dolphin.exe; RemixSceneScale is pushed to the
// runtime as rtx.sceneScale (centimetres per GC world unit) and RemixLightScale
// multiplies the radiance derived from XF lights.
const Info<std::string> GFX_REMIX_DLL_PATH{{System::GFX, "Settings", "RemixDllPath"}, "d3d9.dll"};
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

const Info<std::string> GFX_DRIVER_LIB_NAME{{System::GFX, "Settings", "DriverLibName"}, ""};

const Info<VertexLoaderType> GFX_VERTEX_LOADER_TYPE{{System::GFX, "Settings", "VertexLoaderType"},
                                                    VertexLoaderType::Native};

// Graphics.Enhancements

const Info<TextureFilteringMode> GFX_ENHANCE_FORCE_TEXTURE_FILTERING{
    {System::GFX, "Enhancements", "ForceTextureFiltering"}, TextureFilteringMode::Default};
const Info<AnisotropicFilteringMode> GFX_ENHANCE_MAX_ANISOTROPY{
    {System::GFX, "Enhancements", "MaxAnisotropy"}, AnisotropicFilteringMode::Default};
const Info<OutputResamplingMode> GFX_ENHANCE_OUTPUT_RESAMPLING{
    {System::GFX, "Enhancements", "OutputResampling"}, OutputResamplingMode::Default};
const Info<std::string> GFX_ENHANCE_POST_SHADER{
    {System::GFX, "Enhancements", "PostProcessingShader"}, ""};
const Info<bool> GFX_ENHANCE_FORCE_TRUE_COLOR{{System::GFX, "Enhancements", "ForceTrueColor"},
                                              true};
const Info<bool> GFX_ENHANCE_DISABLE_COPY_FILTER{{System::GFX, "Enhancements", "DisableCopyFilter"},
                                                 true};
const Info<bool> GFX_ENHANCE_ARBITRARY_MIPMAP_DETECTION{
    {System::GFX, "Enhancements", "ArbitraryMipmapDetection"}, false};
const Info<float> GFX_ENHANCE_ARBITRARY_MIPMAP_DETECTION_THRESHOLD{
    {System::GFX, "Enhancements", "ArbitraryMipmapDetectionThreshold"}, 14.0f};
const Info<bool> GFX_ENHANCE_HDR_OUTPUT{{System::GFX, "Enhancements", "HDROutput"}, false};

// Color.Correction

const Info<bool> GFX_CC_CORRECT_COLOR_SPACE{{System::GFX, "ColorCorrection", "CorrectColorSpace"},
                                            false};
const Info<ColorCorrectionRegion> GFX_CC_GAME_COLOR_SPACE{
    {System::GFX, "ColorCorrection", "GameColorSpace"}, ColorCorrectionRegion::SMPTE_NTSCM};
const Info<bool> GFX_CC_CORRECT_GAMMA{{System::GFX, "ColorCorrection", "CorrectGamma"}, false};
const Info<float> GFX_CC_GAME_GAMMA{{System::GFX, "ColorCorrection", "GameGamma"}, 2.35f};
const Info<bool> GFX_CC_SDR_DISPLAY_GAMMA_SRGB{
    {System::GFX, "ColorCorrection", "SDRDisplayGammaSRGB"}, true};
const Info<float> GFX_CC_SDR_DISPLAY_CUSTOM_GAMMA{
    {System::GFX, "ColorCorrection", "SDRDisplayCustomGamma"}, 2.2f};
const Info<float> GFX_CC_HDR_PAPER_WHITE_NITS{{System::GFX, "ColorCorrection", "HDRPaperWhiteNits"},
                                              203.f};

// Graphics.Stereoscopy

const Info<StereoMode> GFX_STEREO_MODE{{System::GFX, "Stereoscopy", "StereoMode"}, StereoMode::Off};
const Info<bool> GFX_STEREO_PER_EYE_RESOLUTION_FULL{
    {System::GFX, "Stereoscopy", "StereoPerEyeResolutionFull"}, false};
const Info<float> GFX_STEREO_DEPTH{{System::GFX, "Stereoscopy", "StereoDepth"}, 20};
const Info<float> GFX_STEREO_CONVERGENCE_PERCENTAGE{
    {System::GFX, "Stereoscopy", "StereoConvergencePercentage"}, 100};
const Info<bool> GFX_STEREO_SWAP_EYES{{System::GFX, "Stereoscopy", "StereoSwapEyes"}, false};
const Info<float> GFX_STEREO_CONVERGENCE{{System::GFX, "Stereoscopy", "StereoConvergence"}, 20};
const Info<bool> GFX_STEREO_EFB_MONO_DEPTH{{System::GFX, "Stereoscopy", "StereoEFBMonoDepth"},
                                           false};
const Info<float> GFX_STEREO_DEPTH_PERCENTAGE{{System::GFX, "Stereoscopy", "StereoDepthPercentage"},
                                              100};

// Graphics.Hacks

const Info<bool> GFX_HACK_EFB_ACCESS_ENABLE{{System::GFX, "Hacks", "EFBAccessEnable"}, false};
const Info<bool> GFX_HACK_EFB_DEFER_INVALIDATION{
    {System::GFX, "Hacks", "EFBAccessDeferInvalidation"}, false};
const Info<int> GFX_HACK_EFB_ACCESS_TILE_SIZE{{System::GFX, "Hacks", "EFBAccessTileSize"}, 64};
const Info<bool> GFX_HACK_BBOX_ENABLE{{System::GFX, "Hacks", "BBoxEnable"}, false};
const Info<bool> GFX_HACK_FORCE_PROGRESSIVE{{System::GFX, "Hacks", "ForceProgressive"}, true};
const Info<bool> GFX_HACK_SKIP_EFB_COPY_TO_RAM{{System::GFX, "Hacks", "EFBToTextureEnable"}, true};
const Info<bool> GFX_HACK_SKIP_XFB_COPY_TO_RAM{{System::GFX, "Hacks", "XFBToTextureEnable"}, true};
const Info<bool> GFX_HACK_DISABLE_COPY_TO_VRAM{{System::GFX, "Hacks", "DisableCopyToVRAM"}, false};
const Info<bool> GFX_HACK_DEFER_EFB_COPIES{{System::GFX, "Hacks", "DeferEFBCopies"}, true};
const Info<bool> GFX_HACK_IMMEDIATE_XFB{{System::GFX, "Hacks", "ImmediateXFBEnable"}, false};
const Info<bool> GFX_HACK_CAP_IMMEDIATE_XFB{{System::GFX, "Hacks", "CapImmediateXFB"}, false};
const Info<bool> GFX_HACK_SKIP_DUPLICATE_XFBS{{System::GFX, "Hacks", "SkipDuplicateXFBs"}, true};
const Info<bool> GFX_HACK_EARLY_XFB_OUTPUT{{System::GFX, "Hacks", "EarlyXFBOutput"}, true};
const Info<bool> GFX_HACK_COPY_EFB_SCALED{{System::GFX, "Hacks", "EFBScaledCopy"}, true};
const Info<bool> GFX_HACK_EFB_EMULATE_FORMAT_CHANGES{
    {System::GFX, "Hacks", "EFBEmulateFormatChanges"}, false};
const Info<bool> GFX_HACK_VERTEX_ROUNDING{{System::GFX, "Hacks", "VertexRounding"}, false};
const Info<bool> GFX_HACK_VI_SKIP{{System::GFX, "Hacks", "VISkip"}, false};
const Info<u32> GFX_HACK_MISSING_COLOR_VALUE{{System::GFX, "Hacks", "MissingColorValue"},
                                             0xFFFFFFFF};
const Info<bool> GFX_HACK_FAST_TEXTURE_SAMPLING{{System::GFX, "Hacks", "FastTextureSampling"},
                                                true};
#ifdef __APPLE__
const Info<bool> GFX_HACK_NO_MIPMAPPING{{System::GFX, "Hacks", "NoMipmapping"}, false};
#endif

// Graphics.GameSpecific

const Info<bool> GFX_PERF_QUERIES_ENABLE{{System::GFX, "GameSpecific", "PerfQueriesEnable"}, false};

}  // namespace Config
