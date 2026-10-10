#pragma once

// NriGraphContext -- the render half, on one device and one window.
//
// Everything needed to put a frame on screen, in one object: the HOST's
// window (borrowed, see below), the wrapper-path native device + its NRI
// wrap, an NriSwapChain, the upload ring, the pipeline cache, and ONE
// RenderGraph that is Reset/declared/compiled/executed every frame. The
// frame's nodes hang off BuildFrame() below: batch2d -> [post chain] ->
// tonemap -> [pick/outline] -> [imgui] -> [capture] -> present.
//
// -------------------------------------------------------------------------
// DESK COMMANDS (GPU/windowed runs are desk-only on the dev box):
//
//   ArcaneRuntime --project ..\..\..\ReferenceProject --backend dx12 \
//                 --frames 120 --no-vsync --screenshot nri-dx12.png
//   ArcaneRuntime --project ..\..\..\ReferenceProject --backend vulkan \
//                 --frames 120 --no-vsync --screenshot nri-vulkan.png
//
// Both boot the REAL engine (project, plugin, scene, material compiles).
// THIS DEVICE IS THE FIRST AND ONLY GRAPHICS DEVICE IN THE PROCESS, and a
// desk user sees two consequences of that:
//   * dx12 Debug genuinely gets the D3D12 CPU debug layer. EnableDebugLayer
//     is a before-any-device call and nothing declines it here, so D3D12
//     validation messages reach D3D12DebugLayerCallback and therefore the
//     RenderErrorCount latch.
//   * Vulkan's two-VkDevice dispatcher hazard is impossible by construction:
//     there is one VkDevice, so the Vulkan-Hpp default dispatcher binds the
//     only one there is.
//
// Exit codes follow the host's contract: 0 clean, 1 the graph run failed or
// the device was lost, 2 RenderErrorCount GREW during the run (a validation
// error fired -- Debug turns the D3D12 debug layer and VK sync validation ON
// for exactly this). The lower number wins.
// -------------------------------------------------------------------------
//
// THE BORROWED WINDOW (read before changing where the graph renders).
// This object does NOT own a window. Create() takes the host's -- the same
// Window GpuContext::Create built and the same one the host's ImGuiLayer,
// InputDevices and event pump use.
//
// WHY BORROWED RATHER THAN OWNED: DXGI allows only ONE flip-model swap chain
// per HWND at a time (IDXGIFactory2::CreateSwapChainForHwnd's own remark),
// and this swapchain (NRI SwapChainD3D12 -> DXGI_SWAP_EFFECT_FLIP_DISCARD) is
// flip-model. A SECOND presentation surface in this process would therefore
// need a second WINDOW -- and a second window is exactly what borrowing this
// one avoids.
//
// WHAT THE CALLER OWES: the borrowed window must OUTLIVE this object. The
// runtime host guarantees that by declaring m_graphContext AFTER m_gpu, so it
// is destroyed first (RuntimeApp.hpp's teardown contract). Nothing in this
// class destroys, resizes or re-titles the window -- it only reads its native
// handle at swapchain create, and Resize() is driven by the host's pump.
//
// THE HUD'S DISPLAY SIZE NEEDS NO LOCKSTEP: ImGui_ImplSDL3_NewFrame reads the
// window the platform backend was initialised with, and that is the same
// window this swapchain binds. One surface, one extent. It defaults to
// 1280x720.
//
// -------------------------------------------------------------------------
// OFFSCREEN MODE -- the SECOND way to run one of these.
//
// CreateOffscreen() builds a vehicle with NO WINDOW and NO SWAPCHAIN that
// renders the same frame into a persistent, sampled texture it owns, and
// presents nothing. That is the editor's capability: a viewport panel is an
// ImGui::Image over a texture, not a surface, so the frame has to end in a
// SHADER_RESOURCE the chrome pass can sample rather than in a PRESENT.
//
// WHAT THE TWO MODES SHARE IS EVERYTHING EXCEPT THE FINAL TARGET -- same
// Create(), same RenderFrame(), same Resize(), same barriers. DeclareGraphFrame builds
// one frame shape and the tonemap writes either the acquired backbuffer or the
// imported output (RgFrameShape::offscreenOutput). A node census over the two
// is identical, which is what the [nri] cases pin.
//
// The four differences, in full:
//   * the device is SHARED, not owned (see below);
//   * there is no NriSwapChain, so Execute() runs with
//     RgExecuteDesc::swapChain = nullptr -- no acquire, no present -- and the
//     graph's own submission fence is the frame's only completion signal;
//   * PACING is therefore ours: RenderFrameOffscreen waits THIS object's own
//     timeline fence FramesInFlight() deep, signalled by a trailing
//     signal-only submit, exactly the shape NriSwapChain::Present uses to
//     stamp its pacing fence. Without it nothing would bound frames in flight
//     and the per-frame-slot command buffers, upload-ring arena and node
//     descriptor sets would all be rewritten under a GPU still reading them;
//   * the output texture is BGRA8_UNORM and persistent, recreated only by
//     ResizeOffscreen().
//
// ONE DEVICE PER PROCESS, TWO CONTEXTS. The editor holds the host-window
// context (chrome -> present) and an offscreen context (the viewport), over
// ONE NriDevice: the host-window one creates and owns it, the offscreen one
// BORROWS it. That is not a convenience -- Vulkan-Hpp's default dispatcher
// binds one VkDevice per process and DXGI allows one flip-model swapchain per
// HWND, so a second device is exactly the topology this design excludes.
//
// ===== TWO CONTEXTS, TWO LANES -- WHAT A SECOND CONTEXT OWES ==============
// This is a RULE, not a warning list, and it is the rule a THIRD context
// would have to obey too.
//
// ---- (1) ONE GRAVEYARD LANE PER CONTEXT ------------------------------
// EVERY NRI object a context owns is buried in THAT CONTEXT'S OWN Graveyard
// (NriGraphContext::Graves()), never in the shared NriDevice::Graves(), and is
// keyed to THAT context's graph's submission fence. RenderGraph receives the
// lane through RgExecuteDesc::graves and latches it beside the device at
// Execute's ENTRY; nodes reach it from an exec fn through
// RenderGraphNodeContext::graph->Graves(); ImGuiNri takes it as a parameter on
// every disposal it makes.
//
// WHY IT HAD TO BE PER-CONTEXT. A fence value only means something inside ONE
// submission timeline. Two contexts have two RenderGraphs and therefore two
// INDEPENDENT timelines, so with one shared graveyard:
//   * Graveyard::Bury's nondecreasing-fenceValue assert fires in Debug the
//     moment the two counters interleave out of order; and, worse,
//   * each RenderGraph::Execute reaps with ITS fence's completed value, which
//     runs the OTHER graph's thunks before that graph's submission has
//     retired -- a use-after-free no assert catches.
// Both are now impossible by construction: a lane is reaped only by the one
// graph that buries into it, with the one fence that describes those burials.
//
// THE FOUR WINDOWS THAT USED TO OUTLIVE "while both render", and how each is
// closed -- keep this list, it is what a future edit must not reopen:
//   (a) CONTEXT TEARDOWN. ~RenderGraph buries the command buffers, the
//       allocators and its own fence at its m_submitValue
//       (ReleaseGpuResourcesInternal(all=true)) -- and a MEMBER destructor runs
//       AFTER this class's destructor BODY, i.e. after its drain. CLOSED by
//       ~NriGraphContext destroying m_graph EXPLICITLY, in the body, right
//       before the drain: the tail lands in this context's lane and that lane
//       is then emptied. Nothing is left pending anywhere (~Graveyard asserts
//       exactly that in Debug, on the way out).
//   (b1) EXECUTE WAS NEVER ENTERED -- a failed InitOffscreen, or a context
//       created and dropped without a frame. Every burial would key at fence 0.
//       CLOSED by the lane, which starts EMPTY: 0 is nondecreasing against
//       nothing. ONE mechanism covers this, not a special case beside it.
//   (b2) EXECUTE WAS ENTERED AND NEVER SUCCEEDED -- the first frame reached
//       RenderGraph::Execute and failed inside it (a first-frame device loss on
//       the viewport is the realistic shape). CLOSED by the lane, and it is the
//       window that REQUIRED it: m_device is latched unconditionally at
//       Execute's ENTRY while m_submitValue only advances after a successful
//       QueueSubmit, so such a graph buries at 0 with a latched device. The
//       sharpest edge was never at teardown at all --
//       EnsureExecutionResources's all-or-nothing cleanup buries the partially
//       created command buffers and allocators at m_submitValue == 0
//       SYNCHRONOUSLY, inside the failing Execute, before any destructor runs.
//       The lane latches at that same entry, so it is already in place.
//   (c) ANY FUTURE OWNER burying on its own clock. Still the live hazard, and
//       now with a stated rule: NriDevice::Graves() is DRAIN-ONLY (nothing in
//       the tree reaps it; ~NriDevice drains it once, behind a DeviceWaitIdle).
//       An owner that needs fence-paced reclamation must own a lane, exactly
//       like this class does. See NriDevice::Graves().
//
// A LANE IS PRIVATE TO ITS CONTEXT. That is the part that does not go away:
// nothing may bury one context's objects into another's lane, and nothing may
// split one context's objects ACROSS two lanes -- ordering is the contract (a
// view must be destroyed before the texture it views), and no ordering exists
// between two graveyards.
//
// ---- (2) INVALIDATE ImGuiNri *BEFORE* EVERY ResizeOffscreen ----------
// ResizeOffscreen destroys the output texture and creates a replacement, and
// NRI does not ref-count -- so it may hand the replacement the address the
// destroyed one just vacated. ImGuiNri caches per texture by RAW POINTER
// (EnsureEntry matches `entry.texture == texture`), so a bit-identical
// ImTextureID would report a cache HIT on an nri::Descriptor + descriptor set
// that still name the DESTROYED texture.
//
// THE HOOK EXISTS NOW: ImGuiNri::InvalidateUserTexture{,Now} (and their node
// forwards on ImGuiNriNode) evict the pointer-keyed entry, dispose its view
// and RETIRE its descriptor set for age-gated recycling -- the same discipline
// DestroyTexture applies to an ImTextureData-owned entry. RE-READING
// OffscreenTextureId() IS STILL NOT A SUBSTITUTE: the id may legitimately come
// back identical, and the staleness lives in the cache, not in the id.
//
// THE CALLER CONTRACT, which is Task 8's to honour: CALL
// InvalidateUserTextureNow WITH THE OLD POINTER, ON THE CHROME CONTEXT'S NODE,
// **BEFORE** ResizeOffscreen -- whenever the size actually changed, and then
// unconditionally (never gated on the pointer having changed).
//
// ...AND ONCE MORE BEFORE THIS CONTEXT IS DESTROYED. Same rule, other end of
// the object's life, and easy to miss because it LOOKS like something member
// ordering should handle. It is not: the offscreen context BORROWS the chrome
// context's device, so it must be destroyed FIRST -- while the view over its
// output, which the CHROME backend owns, must be destroyed first too. Opposite
// orders; no declaration order satisfies both, which is why the owner's last
// living moment is the only place this can close. EditorApp::Shutdown is that
// site; a project switch is the other one --
// EditorApp::TeardownGraphForSwitch, which destroys and rebuilds the
// OFFSCREEN context and deliberately KEEPS the host-window one, so the backend
// holding the view provably outlives the texture's owner and this call is the
// only thing that can order the two. Unconditional and idempotent there too:
// a miss is routine, a null is an early-out.
//
// AND NOTHING MAY RENDER BETWEEN THE TWO CALLS. The invalidate leaves NO entry
// for that pointer, so a chrome frame recorded before the resize would take
// EnsureEntry's create path and build a fresh view + set over a texture that is
// about to be destroyed -- the same inversion again, plus a live sample of a
// dead resource. The two calls are one operation; see ResizeOffscreen.
//
// *BEFORE*, AND THAT IS THIS RULE'S OWN INSTANCE OF ITEM (1). The one
// descriptor here inherently spans BOTH contexts: the chrome backend owns the
// view, the viewport context owns the texture, and they die through different
// lanes. Nothing orders two graveyards -- so invalidating AFTER the resize
// (which destroys its output synchronously, draining its own lane inside the
// call) would leave the chrome lane to run DestroyDescriptor one to two frames
// after DestroyTexture, every resize. `Now` sidesteps the lanes entirely: it
// destroys the view inside the call behind its own DeviceWaitIdle, so run
// first it provably dies while the texture is still alive. The deferred
// variant remains correct for a texture that dies in the SAME lane after it.
//
// The hook cannot be folded INTO ResizeOffscreen -- this class owns no
// ImGuiNri for the CHROME context, which is the backend that actually caches
// the viewport's texture (a different context entirely from the one being
// resized). ResizeOffscreen carries the exact three-line sequence and the desk
// watch list for it.
// =====================================================================
//
// Adapted, where marked, from .example/NRISamples (MIT -- see that tree's
// LICENSE.txt): the readback shape (Source/Readback.cpp's COPY_SOURCE ->
// CmdReadbackTextureToBuffer -> map, and its BGRA-vs-RGBA channel check).
//
// Include order: NRI headers first, ALWAYS (see NriCommon.hpp) --
// Extensions/NRIDeviceCreation.h (via NriDevice.hpp) declares
// nri::Message::ERROR and <windows.h> (via spdlog) #defines ERROR.
#include <NRI.h>

#include <Arcane/Base/Api.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Host/HostConfig.hpp>          // HostConfig
#include <Arcane/Material/GlobalParams.hpp>    // GlobalParams (held by value)
#include <Arcane/Platform/Window.hpp>
#include <Arcane/Render/Nri/NriDevice.hpp>
#include <Arcane/Render/Nri/GpuScene.hpp>           // GpuScene -- the GPU scene's device half (F3 plan 1 T6); after NriDevice.hpp (ERROR clash, see NriCommon.hpp)
#include <Arcane/Render/Nri/NriPipelineCache.hpp>
#include <Arcane/Render/Nri/NriSwapChain.hpp>
#include <Arcane/Render/Nri/NriMeshBufferCache.hpp>
#include <Arcane/Render/Nri/NriTextureCache.hpp>
#include <Arcane/Render/Nri/NriUploadRing.hpp>
#include <Arcane/Render/Nri/RenderGraph.hpp>
#include <Arcane/Render/RenderDeviceSettings.hpp>   // CanvasFormat, DepthFormat (render.canvasFormat / render.depthFormat)
#include <Arcane/Scene/ViewTransform.hpp>       // FrameDesc::pickView (held by value)
// The node types are held BY VALUE-OWNING unique_ptr below, and this class is
// dllexported -- so every TU that sees this header must see complete node
// types (MSVC instantiates the exported class's implicit members). Including
// them here rather than forward-declaring is also the honest dependency: the
// vehicle OWNS its nodes. The include is acyclic -- the node headers only
// forward-declare NriGraphContext.
#include <Arcane/Render/Nri/nodes/Batch2DNode.hpp>
#include <Arcane/Render/Nri/nodes/FullscreenNodes.hpp>
#include <Arcane/Render/Nri/nodes/ImGuiNriNode.hpp>
#include <Arcane/Render/Nri/nodes/MeshNode.hpp>
#include <Arcane/Render/Nri/nodes/MeshCullNode.hpp>
#include <Arcane/Render/Nri/nodes/GridNode.hpp>
#include <Arcane/Render/Nri/nodes/PickOutlineNodes.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

struct ImDrawData;

namespace Arcane
{
    class Batcher2D;
    struct PostChainDesc;

    // THE LINEAR CANVAS FORMAT every node on this path renders into: the batch
    // node's canvas, and every post-chain pass's target. Colours are LINEAR
    // and may exceed 1.0, and the tonemap node is what turns them
    // display-referred. render.canvasFormat picks it (RGBA16F by default;
    // R11G11B10F halves the bandwidth). It lives HERE, next to the frame's
    // shape, because two nodes have to agree on it: a post target that did
    // not match the canvas would be a pipeline built for one format bound to
    // an attachment of another.
    //
    // THE FRAME'S DEPTH FORMAT (render.depthFormat; D32_SFLOAT by default,
    // D24_UNORM_S8_UINT on request). Two places have to agree on it --
    // whoever CREATES a depth transient and whoever keys a PSO on it
    // (NriPipelineCache::GraphicsKey::depthFormat) -- and a pipeline built
    // for one format bound to an attachment of another is undefined on both
    // backends.
    //
    // BOTH ARE LATCHED ONCE PER PROCESS (settings arc S6-19; Restart), on the
    // first call, the FramesInFlight() idiom: every graph context, every node
    // and every pipeline key in the process -- including a node built with no
    // context -- reads the SAME format, so a value published mid-session can
    // never split an attachment from the PSOs keyed for it. A host builds no
    // graph before HostBoot::ApplyEarlyConfigRungs has published the rungs;
    // that function warns (CheckGraphFormatLatch, RenderDeviceSettings.hpp)
    // when an earlier read latched a format other than the published one.
    [[nodiscard]] constexpr nri::Format ToNriFormat(CanvasFormat format) noexcept
    {
        return format == CanvasFormat::R11g11b10f ? nri::Format::R11_G11_B10_UFLOAT : nri::Format::RGBA16_SFLOAT;
    }

    [[nodiscard]] constexpr nri::Format ToNriFormat(DepthFormat format) noexcept
    {
        return format == DepthFormat::D24s8 ? nri::Format::D24_UNORM_S8_UINT : nri::Format::D32_SFLOAT;
    }

    [[nodiscard]] ARC_API nri::Format GraphCanvasFormat() noexcept;
    [[nodiscard]] ARC_API nri::Format GraphDepthFormat() noexcept;

    // THE OFFSCREEN OUTPUT FORMAT. Display-referred: BGRA8_UNORM is exactly
    // the swapchain's own format, and the tonemap already gamma-2.2 encodes,
    // so a plain UNORM target matches what a real backbuffer shows AND lets
    // ImGuiNri sample the raw nri::Texture* this class hands out
    // (OffscreenTextureId) with no extra sRGB conversion -- a `_SRGB` format
    // would double-apply gamma on that sample. It is stated as
    // a CONCRETE format rather than resolved the way NriSwapChain::Format() is,
    // and that is the difference between an output we create and a surface the
    // driver hands us: nothing here is free to pick a channel order.
    inline constexpr nri::Format kGraphOffscreenFormat = nri::Format::BGRA8_UNORM;

    class ARC_API NriGraphContext
    {
    public:
        // Which of the two ways this vehicle was created -- see OFFSCREEN MODE
        // in the file header. Not a runtime switch: it is fixed at Create and
        // decides which of the RenderFrame/Resize pairs is legal to call.
        enum class Mode : std::uint8_t { HostWindow, Offscreen };

        // WHICH OPTIONAL NODES THIS VEHICLE BUILDS.
        //
        // The three core nodes (batch, post, tonemap) are built on every run;
        // the three below are not, and the gate is not laziness. Each costs
        // real GPU objects a context that will never DECLARE them should not
        // hold: the pick pair costs a HOST_READBACK buffer, a descriptor pool,
        // a pipeline layout and a constant arena; each ImGui backend costs a
        // sampler, a descriptor pool and a font-atlas cache.
        //
        // IT DOES NOT DECIDE THE FRAME'S SHAPE -- FrameDesc does. This decides
        // only whether the frame CAN ask, which is what keeps "the flag off
        // leaves the previous task's frame byte for byte" true: a context that
        // builds the nodes but is never asked for them declares the identical
        // graph.
        //
        // ===== AND IT IS WHAT MAKES "ONE ImGuiNri PER ImGui CONTEXT" TRUE =====
        // That is an INVARIANT, not a tidiness claim: ImGuiNri::Release walks
        // its adopted context's
        // platform texture list and DISOWNS every RefCount==1 ImTextureData
        // (invalid TexID, and a Destroyed request ImGui bounces to WantCreate
        // while the CPU pixels live). Correct for THE backend of that context;
        // destructive for a SECOND one over the same context, which leaves
        // ImGui asking the OWNING backend to re-create an atlas it still holds
        // a live cache entry for.
        //
        // Building the host HUD UNCONDITIONALLY is exactly how a second one
        // appeared: the editor's OFFSCREEN viewport context got a host-HUD
        // backend it can never draw through, and -- being created while the
        // EDITOR context was current -- it adopted the editor context alongside
        // the CHROME context's backend. Two backends, one context, and the
        // viewport context destructs first. Inert at process exit; live at a
        // project switch, which destroys the viewport context and keeps the
        // chrome one. Hence the gate below.
        struct NodeSet
        {
            // The HOST CHROME's ImGuiNriNode -- what FrameDesc::imgui draws
            // through. Set by Create(), because a host-window context presents
            // chrome by definition; left FALSE for an offscreen target, which
            // by construction has no host chrome on it. An offscreen context
            // that genuinely wants to draw chrome INTO its output says so here.
            bool hostHud = false;
            // PickNode + OutlineNode. Also implied by --pick-probe, which arms
            // a fixed probe pixel on top of it; a host that sets this gets the
            // nodes with NO probe pixel of its own until it names one per frame
            // (FrameDesc::pickPixel).
            bool pickOutline = false;
            // The GAME/plugin HUD's ImGuiNriNode, drawn from FrameDesc::gameUi
            // between the tonemap and the pick/outline chain. A SEPARATE node
            // from the host HUD's because the two draw datas come from two
            // different ImGui CONTEXTS -- see ImGuiNodeSlot, and the invariant
            // block above.
            bool gameUi = false;
        };

        // What one RenderFrame() call renders. FOUR fields change the graph's
        // SHAPE -- `capture`, `pickOutline`, `imgui` and `gameUi` -- plus the
        // two optional scene passes gated on their pointers (`mesh`, `grid`),
        // and every other one is content the already-declared nodes read. Anything a
        // vehicle was not built with (NodeSet) is ignored rather than declared.
        struct FrameDesc
        {
            // ===== THERE IS NO `stage` ORDINAL. SLICE THE FRAME BY NULLING ===
            // Each optional stage is switched off by passing the field that
            // carries it as null, per that field's own documented meaning:
            // `post = nullptr` is "no chain", `gameUi = nullptr` is "no game
            // HUD", `imgui = nullptr` is "no HUD".
            //
            //   batch slice  ->  post, gameUi, imgui all null
            //   post slice   ->  gameUi, imgui null
            //   full         ->  pass everything
            //
            // AND FLAGS ARE STRICTLY MORE EXPRESSIVE than an ordinal would
            // be. An ordinal imposes a TOTAL ORDER on passes, so it cannot say
            // "post chain on, game HUD off, chrome on" -- a combination this
            // shape gets for free. That matters as the pass count grows: a
            // renderer's passes are a DAG, not a chain, and the moment shadows,
            // GI, SSAO, SSR and TAA exist, "truncate at stage N" stops being a
            // meaningful question. It is also the shape the industry settled
            // on -- Unreal drives per-pass FEngineShowFlags bits and layers
            // named ViewModes ON TOP of them, and Frostbite's frame graph culls
            // a pass whose output nothing consumes. Neither uses an ordinal.
            //
            // If named presets are ever wanted ("give me the batch slice"),
            // that is a helper ABOVE this struct that sets these fields -- the
            // ViewMode layer -- never a field inside it.

            // Add the readback node to THIS frame's graph, so the presented
            // backbuffer can be read back afterwards through ReadCapture().
            // Set on the last frame of a --screenshot run.
            bool capture = false;

            // This frame's 2D content, ALREADY SUBMITTED into the batcher by
            // the frame driver (ClientRuntime::SetRenderContext + SubmitRender) and
            // not yet drained -- this path drains it. Borrowed for
            // the duration of the RenderFrame call and never stored; null
            // renders a cleared canvas.
            Batcher2D* batch = nullptr;

            // ---- the mesh pass (Task 7; opaque + masked + ordered ----------
            //      transparent since F3 plan 2, MeshNode.hpp's head comment)
            // THIS FRAME'S MESH SCENE -- geometry, per-instance transforms,
            // the camera and the one directional light. Null (the default) is
            // how a caller asks for the frame WITHOUT the mesh pass, exactly
            // as `post = nullptr` asks for it without the chain; an EMPTY
            // instance span means the same thing.
            //
            // BORROWED for the duration of the RenderFrame call and never
            // stored -- the same rule `pickables` carries, and for the same
            // reason: MeshNode copies the vertex/index streams into the upload
            // ring at RECORD time, which is inside this call.
            //
            // TASK 7 ADDED THIS FIELD EARLY, because the [gpu][pixel] case that
            // proves the mesh pass draws anything at all reaches the vehicle
            // only through RenderFrameOffscreen(FrameDesc). Task 9 is what
            // teaches the two HOSTS to fill it in from a real scene.
            const MeshSceneDesc* mesh = nullptr;

            // ---- the 3D reference grid (F4 plan 1 T10, spec s5.2) --------
            // THIS FRAME'S GRID -- the editor camera and the plane. Null (the
            // default) is "no grid", the same slice-by-nulling rule as `mesh`.
            // Declared AFTER the mesh pass and depth-tested against its depth
            // transient (never writing it); with no mesh pass the grid draws
            // unoccluded. Editor chrome: the editor arms it only in Edit mode
            // with the Perspective view and Show grid on; the runtime never
            // does. BORROWED for the duration of the RenderFrame call.
            const GridSceneDesc* grid = nullptr;

            // This frame's scene POST CHAIN as bytecode + layout + values --
            // SceneRenderResolver::PostDesc(). Borrowed for the duration of the RenderFrame
            // call (the resolver is not Refreshed inside one) and never
            // stored; null means "no chain", i.e. canvas -> tonemap.
            const PostChainDesc* post = nullptr;

            // The engine-global material constants for THIS frame
            // (SceneRenderResolver::Globals()). Copied, not borrowed -- the
            // post chain's b1 is written at RECORD time, long after this
            // pointer's referent is out of scope. Null leaves them zeroed.
            const GlobalParams* globals = nullptr;

            // ---- the pick + outline chain (Task 11) ---------------------
            // THIS FRAME'S PICKABLE SILHOUETTES, in WORLD space, from
            // CollectPickables (Render/PickEmit.hpp) -- the k-th entry IS
            // hit-proxy id k+1. Collected by the FRAME DRIVER, which is the
            // thing that owns a registry and a camera; this class must not
            // grow either (it is a RENDER vehicle, the same reasoning
            // AssetResolveFn carries). Borrowed for the duration of the
            // RenderFrame call and never stored.
            //
            // EMPTY IS LEGAL and is not "no pick": the chain is still declared
            // when `pickOutline` is set, and a scene with nothing pickable
            // simply produces an all-zero id target -- which is exactly what
            // a probe over empty space must read back.
            std::span<const PickDrawable> pickables;

            // THE VIEW THE ID PASS PROJECTS THROUGH (F4 spec s7.1). The drawables are
            // WORLD-space now, so the pass needs the same ViewTransform the scene
            // render just used -- the editor's camera in Edit, the scene camera in
            // Play. Copied, not borrowed (136 bytes; read at declaration time by
            // PrepareDrawables). A default-constructed view (zero viewport)
            // rasterises nothing, which is what a frame that armed the chain
            // without a view must read back: background.
            ViewTransform pickView{};

            // The hit-proxy ids the outline traces as ONE silhouette (their
            // union). Task 11's driver scripted this to the scene's first
            // pickable entity; PHASE 3 REPLACED THIS ONE LINE AT THE HOST with
            // the editor's real selection (EditorApp::RenderSceneToViewport
            // maps m_selection through PickPassId over the same drawables it
            // hands `pickables`) and nothing here changed -- which was the
            // claim. An EMPTY span is the correct arming for a frame that must
            // show no outline (Play mode, where the editor's phase 12 does not
            // run at all): the seed then finds no selected silhouette and the
            // composite discards every pixel.
            std::span<const std::uint32_t> selectedIds;

            // Add the pick node, the readback and the JFA outline chain to
            // this frame's graph. Off on every ordinary run, which is what
            // keeps an ordinary frame's shape free of the whole chain.
            bool pickOutline = false;

            // ---- the pick chain's PER-FRAME coordinates (Task 9) ---------
            // Unset means "the --pick-probe pixel the config armed"
            // (ProbeX/ProbeY), which is what keeps the Phase-2 probe path
            // reading exactly as it did: a host that names neither gets the
            // old behaviour, including the deliberate double duty where the
            // probe pixel is ALSO the hover cursor.
            //
            // A host with a real cursor needs them apart. The editor hovers
            // continuously (cyan follows the mouse every frame) but probes only
            // when a click has to be resolved, and pointing the readback at the
            // cursor every frame would answer a question nobody asked.
            //
            // Canvas pixels, y down -- the space the id pass's ViewTransform
            // (pickView) lands its drawables in. (-1, -1) is the
            // "no hover" convention the outline seed already understands.
            std::optional<glm::ivec2> pickPixel;    // the readback's texel
            std::optional<glm::ivec2> hoverPixel;   // the outline seed's cursor

            // Rides with THIS frame's readback copy and comes back beside the
            // id FramesInFlight() frames later (PickNode::
            // LastProbeTicket). The vehicle ascribes no meaning to any value:
            // it is the HOST's label for "which request does this answer",
            // which a host that probes a different pixel every frame cannot do
            // without one. 0 is a perfectly ordinary label and the default.
            std::uint64_t pickTicket = 0;

            // ---- the GAME's own HUD (Task 9) ----------------------------
            // A SECOND ImDrawData, from a SECOND ImGui context, drawn between
            // the tonemap and the pick/outline chain -- the editor's phase 11
            // (the game HUD) then phase 12 (the selection outline) order,
            // expressed against this recorder. Same borrowing rules as `imgui`
            // below (record-time consumption, valid until that context's next
            // NewFrame). Null means "no game HUD this frame", which is what
            // Edit mode passes.
            //
            // NOT the same field as `imgui` -- `imgui` is HOST CHROME, this
            // is CONTENT inside the rendered image -- but it is gated the same
            // way: DeclareGraphFrame declares the node only when this pointer
            // is non-null (see RgFrameShape::gameUi). Ignored unless the
            // vehicle was created with NodeSet::gameUi.
            ImDrawData* gameUi = nullptr;

            // ---- the HUD ------------------------------------------------
            // THIS FRAME'S ImGui DRAW DATA, already built by the frame driver
            // (ImGui::Render() -> ImGui::GetDrawData()) before RenderFrame was
            // called. Null means "no HUD this frame" -- how a caller asks for
            // the scene without host chrome, and what leaves the rest of the
            // frame's shape untouched.
            //
            // BORROWED, and unlike `pickables`/`selectedIds` it is NOT fully
            // consumed at declaration time: the vertex/index copy happens at
            // RECORD time, inside the node's exec fn (the ring's BeginFrame
            // runs after the frame is declared, so an earlier allocation would
            // land in the previous frame's slot). ImGui's own buffers stay
            // valid until the next ImGui::NewFrame, which is a whole frame
            // away -- so this pointer is live for the whole RenderFrame call
            // and nothing retains it past one.
            ImDrawData* imgui = nullptr;
        };

        // What one frame did. Distinguishes the two false-y outcomes the frame
        // driver must treat differently: a SKIPPED frame is routine (a
        // zero-sized surface, a minimized window, an OUT_OF_DATE acquire) and
        // must not advance the frame counter or end a --frames N run, while a
        // FAILED frame has already bumped RenderErrorCount and must stop the
        // run.
        //
        // OFFSCREEN MODE reads `Presented` as "rendered and submitted" -- there
        // is nothing to present to. Its Skipped cases are a zero-extent target
        // and a context whose last ResizeOffscreen could not create one (both
        // routine for a collapsed editor panel); Failed still means the latch
        // grew.
        enum class FrameOutcome : std::uint8_t { Presented, Skipped, Failed };

        // Guid -> asset file, the SAME resolution the engine's caches use
        // (Project::ResolveAsset behind Runtime::CurrentProject; see
        // SceneRenderResolver's constructor, which builds the identical
        // lambda). Injected rather than derived here because this class owns no
        // Runtime and must not grow one -- it is a RENDER vehicle.
        using AssetResolveFn = std::function<std::optional<std::filesystem::path>(const Guid&)>;

        // THE SAME SEAM, EXTENDED TO PIXELS. Guid -> decoded RGBA8, i.e.
        // `Assets::PixelsFor` -- the engine's retained, DEVICE-FREE decode
        // cache. Injected for exactly the reason
        // AssetResolveFn is: this class owns no Runtime and no Assets facade
        // and must not grow either. It feeds NriTextureCache, which is what
        // turns a drained span's texture Guid into something t0 can bind.
        //
        // TASK 7: this stays installed on every vehicle (including a content
        // one) and is what a Display-space lookup still uses -- see
        // ArtifactSupplyFn below and NriTextureCache::ColorSpace's own
        // routing-rule comment for which supply answers which key.
        using PixelSupplyFn = NriTextureCache::PixelSupplyFn;

        // THE SAME SEAM AGAIN, EXTENDED TO COMPILED ARTIFACTS (Task 7). Guid ->
        // a compiled texture artifact (BC7/RGBA8 mips), i.e. `Assets::ArtifactFor`.
        // A content vehicle installs THIS instead of PixelSupplyFn for its
        // Srgb-space (scene) content; the chrome vehicle keeps PixelSupplyFn
        // alone and never installs this at all.
        using ArtifactSupplyFn = NriTextureCache::ArtifactSupplyFn;

        // Builds native device + NRI wrap + swapchain (over `window`) + ring +
        // cache + graph, in that order, honouring `config.backend` and
        // `config.vsync`. Null on any failure (already logged + latched).
        //
        // `window` is BORROWED and must outlive the returned object -- see THE
        // BORROWED WINDOW above. It must already exist and carry a native
        // handle, and (on Vulkan) have been created with SDL_WINDOW_VULKAN;
        // GpuContext::CreateForGraph does both.
        //
        // Debug turns validation ON -- the D3D12 debug layer through
        // ID3D12InfoQueue1, VK core + SYNCHRONIZATION validation, and NRI's own
        // validation layer -- all three of which end at RenderErrorCount. That
        // is the dev-loop gate for the whole phase (NRI validation alone
        // cannot catch barrier bugs). Since Task 6 this device is the FIRST in
        // the process, so the D3D12 debug-layer request is no longer declined.
        static std::unique_ptr<NriGraphContext> Create(const HostConfig& config, Window& window);

        // THE OFFSCREEN FLAVOR -- see OFFSCREEN MODE in the file header for
        // the full contract.
        //
        // ============ RUNNING THIS ALONGSIDE A LIVE HOST-WINDOW CONTEXT is
        // SUPPORTED, and it costs the caller exactly one obligation, because
        // the other one is structural:
        //   1. THE GRAVEYARD is no longer shared. This context owns its own
        //      lane (Graves()) and reaps it with its own fence, so the two
        //      contexts' independent fence timelines never meet. Nothing to do
        //      -- it is a member, created and drained by this class.
        //   2. ImGuiNri MUST BE TOLD when ResizeOffscreen replaces the output:
        //      call ImGuiNri::InvalidateUserTextureNow (through the CHROME
        //      context's ImGuiHud() node) with the OLD pointer, **BEFORE** the
        //      resize. The hook exists; the CALL is the caller's, and this
        //      class cannot make it -- the backend that caches the viewport
        //      texture belongs to the other context. ResizeOffscreen carries
        //      the exact sequence and why the order is load-bearing.
        // Both are stated in full -- mechanism, windows, closure -- under TWO
        // CONTEXTS, TWO LANES in the file header. A LONE offscreen context (no
        // second context, no ImGui sampling) needs neither.
        // ============
        //
        // No window, no swapchain: builds the ring, the cache, the graph, the
        // nodes and ONE persistent kGraphOffscreenFormat output texture at
        // `width` x `height`, then renders through RenderFrameOffscreen().
        // Null on any failure (already logged + latched), including a zero
        // extent -- unlike a minimised window, a zero-sized offscreen target is
        // a caller bug rather than a routine state, because nothing outside
        // this call chose the size.
        //
        // `shared` is BORROWED and must OUTLIVE this object. It is deliberately
        // the whole device rather than a backend enum: one process holds one
        // graphics device, and the host-window context is what creates
        // and owns it. `config` is read for the same knobs Create() reads
        // EXCEPT the ones a surface owns -- vsync is meaningless with nothing to
        // present to, so it is not armed here.
        //
        // IT DOES NOT ARM THE CRASH CHAIN. NriDiagnostics::Arm/Disarm install
        // and clear ONE process-wide slot with no per-owner identity, so an
        // offscreen context that armed would be harmless but one that DISARMED
        // would tear down the chain the host-window context owns. The device
        // this borrows is already covered by whoever created it; see
        // ~NriGraphContext.
        // `nodes` names the OPTIONAL nodes this context may declare -- see
        // NodeSet. Defaulted to none.
        static std::unique_ptr<NriGraphContext> CreateOffscreen(const HostConfig& config,
                                                                NriDevice& shared,
                                                                std::uint32_t width,
                                                                std::uint32_t height,
                                                                const NodeSet& nodes);
        // The "defaulted to none" overload. NOT `const NodeSet& nodes = {}`:
        // NodeSet is a nested class with default member initializers, and a
        // default argument naming it is evaluated before NriGraphContext is
        // complete -- GCC rejects that ("could not convert '{}'", the
        // long-standing NSDMI-in-nested-class rule); MSVC accepts it. The
        // inline body is a complete-class context, so this spelling is portable.
        static std::unique_ptr<NriGraphContext> CreateOffscreen(const HostConfig& config,
                                                                NriDevice& shared,
                                                                std::uint32_t width,
                                                                std::uint32_t height)
        {
            return CreateOffscreen(config, shared, width, height, NodeSet{});
        }

        ~NriGraphContext();

        NriGraphContext(const NriGraphContext&)            = delete;
        NriGraphContext& operator=(const NriGraphContext&) = delete;

        [[nodiscard]] Mode Kind()        const noexcept { return m_mode; }
        [[nodiscard]] bool IsOffscreen() const noexcept { return m_mode == Mode::Offscreen; }

        // NO Win() ACCESSOR, deliberately: both hosts reach the window
        // through their own GpuContext::Win(), which is the object that
        // actually owns it. A null-dereferencing accessor (an offscreen
        // context borrows no window) with no caller is a trap, not an
        // affordance. m_borrowedWindow stays: Init/Resize use it.

        // The extent the frame is being rendered at, whichever mode this is --
        // the swapchain's, or the offscreen output's. The mode-agnostic reading
        // of Swap().Width()/Height(), which an offscreen context cannot answer.
        [[nodiscard]] std::uint32_t SurfaceWidth()  const noexcept;
        [[nodiscard]] std::uint32_t SurfaceHeight() const noexcept;

        // Destroy + recreate the swapchain at the new size. MUST be called
        // strictly BETWEEN RenderFrame() calls -- never from inside one --
        // which is exactly what a frame driver that pumps events at the top of
        // its loop does. (The graph's acquire/present pair lives entirely
        // inside one Execute(), so that is the whole of the CALLER's
        // obligation; RgExecuteDesc::swapChain.)
        //
        // Not a bare forward to NriSwapChain::Resize: it first idles, releases
        // the graph's GPU resources and DRAINS the graveyard, so that every
        // descriptor naming a current backbuffer is destroyed BEFORE those
        // backbuffers are. RenderGraph turns imported views over per Execute,
        // but it does so by BURYING them -- and a burial reaped after the
        // swapchain is gone destroys a view over a freed image, which is a
        // Vulkan validation error and therefore a nonzero exit on a vehicle
        // whose contract is "the latch did not grow". See the .cpp.
        //
        // HOST-WINDOW MODE ONLY -- ResizeOffscreen() is the offscreen twin.
        void Resize(std::uint32_t width, std::uint32_t height);

        // One frame: Reset the graph, declare this frame's nodes, Compile,
        // Execute (which acquires, records, submits and presents).
        //
        // HOST-WINDOW MODE ONLY -- the offscreen twin is below.
        FrameOutcome RenderFrame(const FrameDesc& frame);

        // ------------------------------------------------------------------
        // THE OFFSCREEN HALF. Everything below is
        // OFFSCREEN MODE ONLY and refuses (loudly, latched) on a host-window
        // context, exactly as RenderFrame/Resize refuse on an offscreen one --
        // a vehicle that quietly did the wrong thing for its mode would be a
        // wrong picture, not an error.
        // ------------------------------------------------------------------

        // One frame into the persistent output: pacing wait, Reset, declare
        // (the tonemap's final target is the IMPORTED output rather than an
        // acquired backbuffer), Compile, Execute with no swapchain, then stamp
        // the pacing fence. Presents nothing.
        //
        // On return -- once this frame's submission retires, which the next
        // pacing wait is what guarantees -- OffscreenOutput() is in
        // SHADER_RESOURCE and is the frame's finished, display-referred image.
        // FrameOutcome means what it does on the present path: Skipped is
        // routine (a zero extent), Failed has already bumped the latch.
        FrameOutcome RenderFrameOffscreen(const FrameDesc& frame);

        // The persistent output texture -- kGraphOffscreenFormat, owned here,
        // stable until ResizeOffscreen(). Null on a host-window context.
        //
        // BORROWED BY THE CALLER, and ResizeOffscreen DESTROYS IT: a caller
        // holding this pointer across a resize holds a destroyed texture. NRI
        // does not ref-count, so the replacement may land on the freed
        // address -- a pointer comparison cannot tell you it happened, which is
        // the same hazard RenderGraph's per-execute imported views exist to
        // dodge.
        //
        // RE-READING IT AFTER A RESIZE IS NECESSARY BUT NOT SUFFICIENT, and
        // that distinction is the whole of item (2) under TWO CONTEXTS, TWO
        // LANES in the file header: any cache DOWNSTREAM keyed on this pointer
        // -- ImGuiNri's is -- must be invalidated EXPLICITLY, because the new
        // pointer may compare EQUAL to the old one and the staleness lives in
        // that cache rather than in this value. The closure is
        // ImGuiNri::InvalidateUserTextureNow, called with this pointer BEFORE
        // every ResizeOffscreen -- so read it first, both because this accessor
        // cannot hand the old value back afterwards and because the disposal
        // has to happen while the texture is still alive.
        [[nodiscard]] nri::Texture* OffscreenOutput() noexcept { return m_offscreen; }

        // OffscreenOutput() as an ImGui texture id, ready for ImGui::Image().
        //
        // The return type is `std::uint64_t`, which IS `ImTextureID` (imgui.h:
        // `typedef ImU64 ImTextureID`, and ImU64 is `unsigned long long`) --
        // spelled as the underlying type for the same reason ImGuiNri.hpp and
        // OffscreenCanvas::TextureId() do: no NRI header in this directory
        // pulls in <imgui.h>, and one that did would drag it into every
        // consumer of this header for a typedef.
        //
        // THE ImGuiNri CONVENTION (that header's §7.3): an ImTextureID is the
        // backend's RAW TEXTURE HANDLE cast through intptr_t -- here the
        // nri::Texture* itself, not a descriptor. 0 on a host-window context,
        // which is ImTextureID_Invalid.
        //
        // HANDING THIS TO ImGui::Image CARRIES ONE CALLER OBLIGATION, because
        // the id IS the raw pointer: an id that is UNCHANGED across a
        // ResizeOffscreen is exactly the case that would make ImGuiNri's
        // pointer-keyed user-texture entry serve a descriptor over the
        // destroyed texture. CALL ImGuiNri::InvalidateUserTextureNow WITH THE
        // OLD POINTER **BEFORE** EVERY ResizeOffscreen -- that is the closure,
        // it is the caller's because the caching backend belongs to the CHROME
        // context rather than to this one, and the BEFORE is load-bearing (the
        // resize destroys the texture synchronously; a view disposed afterwards
        // outlives it). See ResizeOffscreen() and OffscreenOutput().
        [[nodiscard]] std::uint64_t OffscreenTextureId() const noexcept;

        // Destroy + recreate the output at the new size. MUST be called
        // strictly BETWEEN RenderFrameOffscreen() calls, the same contract
        // Resize() carries and for the same reason.
        //
        // Not a bare destroy/create pair, for the reason Resize() is not a bare
        // forward to NriSwapChain::Resize: it first idles, releases the graph's
        // GPU resources and DRAINS the graveyard, so that every descriptor
        // naming the current output -- the graph's own imported attachment view
        // above all -- is destroyed BEFORE the texture it views is. The node
        // view invalidation (PoolEpoch discipline) is byte-for-byte the
        // present path's. A no-op on an unchanged size, and on a zero extent it
        // logs and leaves the current output alone.
        //
        // ===== WHAT THE CALLER OWES, AND IT IS OWED *BEFORE* =====
        // This destroys the output texture SYNCHRONOUSLY, inside the call --
        // step 4 drains this context's lane, so the old output is already gone
        // by the time it returns. Any ImGuiNri that has drawn that texture --
        // i.e. the CHROME context's, the one whose ImGui::Image names
        // OffscreenTextureId() -- still holds a descriptor + descriptor set
        // keyed on the OLD raw pointer, and NRI may hand the replacement that
        // very address. So:
        //
        //     if (w != viewport->SurfaceWidth() || h != viewport->SurfaceHeight())
        //     {
        //         nri::Texture* old = viewport->OffscreenOutput();
        //         chrome->ImGuiHud()->InvalidateUserTextureNow(old);   // BEFORE
        //         viewport->ResizeOffscreen(w, h);
        //     }
        //
        // ===== TWO THINGS ABOUT THE SHAPE OF THAT SNIPPET =====
        //
        // (i) THE SIZE GUARD IS THE CALLER'S, and it is not decoration. THIS
        // function no-ops on an unchanged size -- but that guard is INSIDE it,
        // i.e. after the invalidate has already run. InvalidateUserTextureNow
        // idles the device unconditionally, so a panel that called these three
        // lines every frame would stall the whole device every frame, forever,
        // on a size that never changed. The invalidate must sit UNDER the same
        // "did the size actually change" test the resize does. (`SurfaceWidth`/
        // `SurfaceHeight` report the current output's extent, which is exactly
        // what this function compares against.)
        //
        // (ii) NOTHING MAY RENDER BETWEEN THEM. The invalidate leaves NO cache
        // entry for `old`, so a chrome frame recorded between the two calls
        // takes EnsureEntry's CREATE path and builds a brand-new view +
        // descriptor set over a texture that is about to be destroyed -- the
        // very inversion this ordering exists to prevent, plus a live sample of
        // a dead resource once the resize lands. Treat the pair as ONE
        // operation: same handler, no frame boundary, no early-out between them.
        //
        // THE ORDER IS THE POINT, and it is why the `Now` variant exists.
        // Invalidating AFTERWARDS -- with the deferred hook -- would bury the
        // chrome backend's view over an ALREADY-DESTROYED texture into the
        // CHROME lane, where it waits one to two frames for that context's
        // fence: DestroyDescriptor after DestroyTexture, on every resize, for
        // the one descriptor that inherently spans both contexts. No lane
        // ordering can fix that, because the view and the texture die through
        // DIFFERENT graveyards and nothing orders two graveyards (file header,
        // TWO CONTEXTS, TWO LANES). `Now` destroys the view inside the call
        // behind its own DeviceWaitIdle, so run BEFORE the resize it dies while
        // `old` is still alive -- the invariant restored rather than an
        // exception documented.
        //
        // Unconditionally, not "if the pointer changed" -- an unchanged pointer
        // is precisely the case a recycled address fakes. Skipping it is a
        // stale-SRV sample or a GPU fault on every viewport-panel drag. It is
        // not folded in here because this class owns no ImGuiNri for the other
        // context; see item (2) under TWO CONTEXTS, TWO LANES in the file
        // header.
        //
        // ===== DESK WATCH (Task 8 / checkpoint D3b) =====
        // THE VIEWPORT DRAG-STORM is the motion that exercises all of this at
        // once -- resize the panel continuously while the chrome context keeps
        // presenting -- and it is the only way to see most of it: every part is
        // either device-less-invisible (a real sampler reading real texels) or
        // once-per-resize. Watch for, in order of what each would prove:
        //   * VK validation on a destroyed VkImageView/VkImage -- the ordering
        //     above went wrong, or the invalidate was skipped entirely;
        //   * the panel sampling the PREVIOUS size's contents or garbage -- the
        //     cache served a stale entry (ABA), i.e. the invalidate ran but on
        //     the wrong pointer or the wrong context's backend;
        //   * RenderErrorCount growing across the storm at all, which is this
        //     vehicle's whole exit-code contract;
        //   * ImGuiNri::LiveTextureCount climbing with the drag -- entries
        //     accumulating means eviction is missing its match, and the
        //     descriptor-pool chain (ImGuiNri::PoolCount) growing with it is
        //     the leak made visible;
        //   * FRAME TIME / HITCHING, which is the one cost this ordering NEWLY
        //     introduces and the storm is the worst case for it: a size change
        //     per frame means InvalidateUserTextureNow's DeviceWaitIdle plus
        //     ResizeOffscreen's own, i.e. TWO full device idles per frame for
        //     the duration of the drag. Expect the panel to be visibly less
        //     smooth than the host-window drag-storm; what would be a FINDING is
        //     a stall that persists after the drag ends (the size guard in (i)
        //     above is missing) or one on a drag that never changes the size.
        // NOTE WHAT THE DEVICE-LESS GATE CANNOT SEE HERE: NONE's DeviceWaitIdle is
        // a bare SUCCESS with no effect, so no [nri] case can observe that the
        // idle was issued at all -- an edit that silently dropped it would stay
        // green. VK synchronization validation at D3b is what would catch it,
        // which is another reason this storm is a required desk item rather
        // than a nice-to-have.
        // The HOST-WINDOW drag-storm (D1/D2's) stays exactly as it was; this is
        // the offscreen twin of it and does not replace it.
        void ResizeOffscreen(std::uint32_t width, std::uint32_t height);

        // ===== THE GPU-PROGRESS HEARTBEAT, FOR AN OFFSCREEN-ONLY PROCESS ====
        // OFF by default, and OFFSCREEN MODE ONLY (a host-window context is
        // the publisher unconditionally -- see RenderFrame).
        //
        // Diagnostics::GpuHeartbeat holds ONE process-wide counter and its
        // stall rule fires on "the value stopped changing", so TWO publishers
        // with unrelated fence timelines would alternate values and read as
        // progress forever -- disarming the rule on exactly the case it exists
        // to catch. That is why RenderFrameOffscreen does not publish on its
        // own: in the EDITOR topology the presenting chrome context is the
        // process's publisher, and its pacing fence only advances when the
        // queue retires -- this context's work included, since they share the
        // queue.
        //
        // A HOST THAT RENDERS AND NEVER PRESENTS BREAKS THAT ASSUMPTION, and
        // it is a host this engine now has (ArcaneRuntime --headless). With
        // no presenting context there is no publisher at all, so Diagnostics'
        // `g_gpuBeatSeen` never arms and the GPU-stall watchdog stays disabled
        // for the whole run -- a GPU hang produces NO capture. That is not a
        // false positive; it is no coverage.
        //
        // THE CALLER ASSERTS THE TOPOLOGY, because only the caller knows it:
        // "this offscreen context is the ONLY graph context in this process."
        // A host that also holds a presenting context must NOT call this --
        // that is the two-publisher case above. Refused (logged, no effect) on
        // a host-window context, which already publishes.
        //
        // What gets published is the offscreen pacing fence's COMPLETED value,
        // the exact analogue of the present path's
        // NriSwapChain::CompletedFrameValue(): it advances only when the GPU
        // retires the trailing signal submit, whereas m_frameIndex is what the
        // CPU has submitted and advances happily while the GPU is wedged --
        // which is precisely the state this heartbeat exists to make visible.
        void SetGpuHeartbeatPublisher(bool enable) noexcept;

        // Maps the buffer the last capture frame read the backbuffer into and
        // hands back TIGHT RGBA8 -- swizzled from BGRA when that is what the
        // swapchain resolved to, so the bytes are display-referred RGBA
        // whatever NRI picked (NriSwapChain::Format() is resolved, never
        // pinned). That normalization is what makes a capture's bytes
        // comparable at all, whichever channel order the swapchain resolved
        // to.
        //
        // Idles the device first (the copy has to have landed). False, already
        // logged, if no capture frame was recorded or the map failed. Callers
        // MUST NOT treat a false here as a run failure -- a capture that could
        // not be read is not a bad frame.
        [[nodiscard]] bool ReadCapture(std::uint32_t& width, std::uint32_t& height,
                                       std::vector<unsigned char>& rgba);

        // Accessors for the node authors of Tasks 8-12. All references stay
        // valid for this object's lifetime.
        [[nodiscard]] NriDevice&        Device()    noexcept { return *m_device; }

        // THIS CONTEXT'S GRAVEYARD LANE -- where every NRI object this context
        // owns is buried, keyed to Graph()'s own submission fence value
        // (Graph().DebugSubmitCount()).
        //
        // USE THIS, NEVER Device().Graves(), for anything this context owns.
        // The device's graveyard is SHARED between every context on that device
        // and has no fence timeline of its own; two contexts' fence values mean
        // nothing to each other, so a burial that lands there is either an
        // assert (Graveyard::Bury's nondecreasing rule) or a reap against a
        // foreign fence. See TWO CONTEXTS, TWO LANES in the file header.
        [[nodiscard]] Graveyard&        Graves()    noexcept { return m_graves; }

        // HOST-WINDOW MODE ONLY: an offscreen context holds no swapchain and
        // this would dereference null. SurfaceWidth()/SurfaceHeight() above are
        // the mode-agnostic readings of its two most-asked questions.
        [[nodiscard]] NriSwapChain&     Swap()      noexcept { return *m_swap; }
        [[nodiscard]] NriUploadRing&    Ring()      noexcept { return m_ring; }
        [[nodiscard]] NriPipelineCache& Pipelines() noexcept { return m_pipelines; }
        [[nodiscard]] RenderGraph&      Graph()     noexcept { return *m_graph; }

        // The node objects, reached from the free AddXxxNode() declarators (and
        // therefore from their exec fns). Null only if Create() failed, which
        // never returns a vehicle.
        [[nodiscard]] Batch2DNode*   Batch2D()   noexcept { return m_batch2D.get(); }
        [[nodiscard]] TonemapNode*   Tonemap()   noexcept { return m_tonemap.get(); }
        [[nodiscard]] PostChainNode* PostChain() noexcept { return m_post.get(); }

        // The mesh pass (Task 7; F3 plan 2 widened it to opaque + masked +
        // ordered transparent). Built EAGERLY like the three above rather than
        // behind a NodeSet flag: mesh geometry is part of what this renderer
        // IS, not optional host chrome, and a frame that carries no mesh scene
        // declares no node either way -- so the cost of having it is one small
        // descriptor pool and a two-region constant arena, not a pass.
        [[nodiscard]] MeshNode*      Mesh()      noexcept { return m_mesh.get(); }
        [[nodiscard]] MeshCullNode*  MeshCull()  noexcept { return m_meshCull.get(); }

        // THE GPU SCENE's device half (F3 plan 1 T6): the persistent instance
        // buffer, the per-slot indirect-args + visible-index buffers and the
        // scratch rows, written by GpuSceneSyncNode (declared by AddMeshNode,
        // ahead of the mesh node) and read by the mesh pass. Built EAGERLY
        // beside Mesh(), for the same reason. Null only if Create() failed.
        [[nodiscard]] GpuScene*      Scene()     noexcept { return m_scene.get(); }

        // THE FENCE VALUE THIS FRAME's SUBMIT WILL SIGNAL -- i.e. the value a
        // resource still referenced by the command list being recorded (or
        // declared) THIS frame must be buried against. RenderGraph::Execute
        // signals m_submitValue + 1 per submit and increments afterwards
        // (RenderGraphExec.cpp), and DebugSubmitCount() returns m_submitValue,
        // so mid-frame the pending submit's value is DebugSubmitCount() + 1.
        //
        // NOT A VALUE TO Bury() AT BEFORE Execute RETURNS: Graveyard::Bury
        // requires nondecreasing values, and Execute itself buries at
        // DebugSubmitCount() while it runs. Stamp the retirement with this and
        // bury it right after the successful Execute (GpuScene::Reserve ->
        // FlushGraves is the shape), when the two are equal.
        [[nodiscard]] std::uint64_t CurrentFence() const noexcept
        {
            return m_graph ? m_graph->DebugSubmitCount() + 1 : 0;
        }

        // The 3D reference grid (F4 plan 1 T10). Built EAGERLY beside Mesh()
        // for the same reason: a frame that carries no grid desc declares no
        // node, so the cost of having it is one tiny descriptor pool and a
        // two-region constant arena.
        [[nodiscard]] GridNode*      Grid()      noexcept { return m_grid.get(); }

        // The pick + outline pair (Task 11). NULL on every run that neither
        // passed --pick-probe nor asked for them through NodeSet::pickOutline:
        // an ordinary --nri-graph run creates no readback buffer, no descriptor
        // pool and no extra pipeline layout -- and the frame that never
        // declares them cannot be affected by their absence.
        [[nodiscard]] PickNode*    Pick()    noexcept { return m_pick.get(); }
        [[nodiscard]] OutlineNode* Outline() noexcept { return m_outline.get(); }

        // The HOST CHROME HUD's node (Task 12). Non-null on every HOST-WINDOW
        // context (Create asks for it) and null on an offscreen one unless it
        // asked -- see NodeSet::hostHud and the invariant block above it.
        // Given the node, the ONE thing that decides whether the frame draws it
        // is whether the driver handed this frame draw data. Named ImGuiHud()
        // rather than ImGui() because `ImGui` is a namespace this header's
        // consumers use.
        [[nodiscard]] ImGuiNriNode* ImGuiHud() noexcept { return m_imguiHud.get(); }

        // The GAME HUD's node (Task 9) -- a SECOND backend over a SECOND ImGui
        // context, built only when NodeSet::gameUi asked for it and null
        // otherwise. Whoever creates a context with it owes exactly one call:
        // ImGuiNriNode::AdoptImGuiContext, with the context whose draw data it
        // will be handed (see ImGuiNri::AdoptContext for why this class cannot
        // make that call itself -- it never sees an ImGui context).
        [[nodiscard]] ImGuiNriNode* ImGuiGame() noexcept { return m_imguiGame.get(); }

        // FrameDesc::gameUi for the frame currently being declared AND
        // recorded, with exactly CurrentImGuiDrawData()'s lifetime rules.
        [[nodiscard]] ImDrawData* CurrentGameUiDrawData() const noexcept { return m_currentGameUi; }

        // FrameDesc::imgui for the frame currently being declared AND
        // recorded. Unlike CurrentPickables()/CurrentSelectedIds(), which are
        // cleared the moment the declarations are done, this stays readable
        // for the whole RenderFrame call because the node's exec fn is what
        // copies the geometry -- see FrameDesc::imgui. NOT cleared after the
        // call returns (unlike its two siblings above): it holds whatever the
        // last RenderFrame published -- possibly null, if that frame supplied
        // no HUD -- until the next RenderFrame re-publishes it.
        [[nodiscard]] ImDrawData* CurrentImGuiDrawData() const noexcept { return m_currentImGui; }

        // FrameDesc::pickables / ::selectedIds for the frame currently being
        // declared -- read by the pick and outline declarators the same way
        // CurrentBatch() is. Empty outside a RenderFrame() call.
        [[nodiscard]] std::span<const PickDrawable> CurrentPickables() const noexcept
        { return m_currentPickables; }
        [[nodiscard]] std::span<const std::uint32_t> CurrentSelectedIds() const noexcept
        { return m_currentSelectedIds; }
        // FrameDesc::pickView, by VALUE like the globals -- the view the pick
        // declarator hands PickNode::PrepareDrawables beside CurrentPickables().
        [[nodiscard]] const ViewTransform& CurrentPickView() const noexcept
        { return m_currentPickView; }

        // The --pick-probe pixel, in canvas px (y down) -- the CONFIG's, fixed
        // at Create. On a probe run it doubles as the outline shader's HOVER
        // cursor, deliberately: a desk eyeball then shows amber on the scripted
        // selection and cyan on whatever the probe is over, so one run proves
        // both halves of the composite.
        //
        // The DECLARATORS read the four accessors below instead, which are this
        // pair unless the frame named its own (FrameDesc::pickPixel /
        // ::hoverPixel). Kept separate rather than overwritten so the
        // out-of-range check and the boot log still describe the FLAG.
        [[nodiscard]] std::int32_t ProbeX() const noexcept { return m_probeX; }
        [[nodiscard]] std::int32_t ProbeY() const noexcept { return m_probeY; }

        // THIS FRAME's readback texel and outline hover cursor -- see
        // FrameDesc::pickPixel/::hoverPixel. Valid for the whole of one
        // RenderFrame/RenderFrameOffscreen call; between calls they hold
        // whatever the last frame published.
        [[nodiscard]] std::int32_t PickPixelX() const noexcept { return m_currentPick.x; }
        [[nodiscard]] std::int32_t PickPixelY() const noexcept { return m_currentPick.y; }
        [[nodiscard]] std::int32_t HoverX()     const noexcept { return m_currentHover.x; }
        [[nodiscard]] std::int32_t HoverY()     const noexcept { return m_currentHover.y; }
        // FrameDesc::pickTicket for this frame -- opaque, see that field.
        [[nodiscard]] std::uint64_t CurrentPickTicket() const noexcept { return m_currentPickTicket; }

        // The entity id read back at the probe pixel, or nullopt when no
        // readback has landed yet (the first FramesInFlight() frames of
        // a probe run) or the probe pixel was outside the surface. 0 is a
        // legitimate value and means BACKGROUND -- the caller decides that is a
        // miss, not this.
        [[nodiscard]] std::optional<std::uint32_t> ProbeId() const noexcept;

        // ProbeId() with the LABEL the copy carried (FrameDesc::pickTicket), so
        // a host that probes a different pixel every frame can tell which of
        // its requests the value answers. Same nullopt cases as ProbeId().
        //
        // IT DOES NOT SELF-CLEAR: the pair stays readable until the next drain
        // replaces it, so a host reading every frame sees the same answer over
        // and over. Acting on it exactly once is the HOST's job -- this class
        // knows when a value ARRIVED, only the host knows when it was CONSUMED
        // (see PickNode::LastProbeTicket).
        struct PickReadback
        {
            std::uint32_t id     = 0;   // 0 == background
            std::uint64_t ticket = 0;
        };
        [[nodiscard]] std::optional<PickReadback> ProbeResult() const noexcept;

        // The batcher the frame CURRENTLY being declared should drain, i.e.
        // FrameDesc::batch. Valid only inside a RenderFrame() call; null
        // outside one and on a frame the driver submitted nothing for.
        [[nodiscard]] Batcher2D* CurrentBatch() noexcept { return m_currentBatch; }

        // FrameDesc::globals for the frame currently being declared, COPIED so
        // it stays readable for the whole RenderFrame call (the post chain's
        // b1 is written at record time). All-zero on a frame the driver
        // supplied none for -- which is what a material reads when nobody
        // sets them at all.
        [[nodiscard]] const GlobalParams& CurrentGlobals() const noexcept { return m_currentGlobals; }

        // The raw bytecode of the offline shader artifact `name` -- the SAME
        // `<name>.bin` every other shader loader reads, from the same directory
        // (ShaderPaths::ResolveFlavorDir, so ARCANE_SHADER_DIR moves all of
        // them together). Loaded once and cached; the returned bytes live
        // as long as this vehicle, which is what NriPipelineCache's fill
        // contract needs (CreateGraphicsPipeline dereferences the blob after the
        // fill callback returns). Empty span when the artifact is missing or
        // unreadable -- a failed lookup is NOT cached (only a successful load
        // is), so ARC_ERROR logs on EVERY call that misses, not just the
        // first.
        [[nodiscard]] std::span<const std::uint8_t> ShaderBytecode(const char* name);

        // Called by the capture node's exec fn once the readback copy is
        // actually recorded -- ReadCapture refuses without it rather than
        // mapping whatever was already in host-readback memory. Public because
        // the frame's declarations live in a free function (DeclareGraphFrame),
        // not in a member.
        void NoteCaptureRecorded() noexcept { m_captureRecorded = true; }

        // The capture staging buffer's layout, read by that node's exec fn for
        // the same reason.
        [[nodiscard]] std::uint64_t CaptureRowPitch() const noexcept { return m_captureRowPitch; }
        [[nodiscard]] std::uint64_t CaptureSlicePitch() const noexcept { return m_captureSlicePitch; }
        [[nodiscard]] std::uint32_t CaptureWidth() const noexcept { return m_captureWidth; }
        [[nodiscard]] std::uint32_t CaptureHeight() const noexcept { return m_captureHeight; }

        // Frames this vehicle has actually PRESENTED. Skipped frames do not
        // count -- it advances in lockstep with the swapchain's own frame
        // counter, which is what makes `frameIndex % FramesInFlight()`
        // a safe command-buffer slot.
        //
        // OFFSCREEN MODE counts frames RENDERED (nothing is presented), and the
        // slot property is preserved by the same construction: it advances only
        // on a frame that submitted, and RenderFrameOffscreen's own pacing
        // fence -- signalled once per such frame, waited on at the same depth --
        // is what makes reusing the slot safe. The name is kept because the
        // counter's MEANING for its one consumer (the slot clock) is unchanged.
        [[nodiscard]] std::uint64_t PresentedFrames() const noexcept { return m_frameIndex; }

        // THE slot the frame currently being declared/recorded owns -- the
        // exact value RenderFrame passes to ring.BeginFrame and
        // RgExecuteDesc::frameSlot, exposed so a node that keeps its OWN
        // per-frame-slot storage (Batch2DNode's constant-buffer arena) indexes
        // it with the same number the ring does instead of re-deriving it.
        // Stable for the whole of one RenderFrame call.
        [[nodiscard]] std::uint32_t FrameSlot() const noexcept
        {
            return (std::uint32_t)(m_frameIndex % FramesInFlight());
        }

        // Installed once by the frame driver, right after Create(). Without it
        // every asset Guid resolves to nothing -- a material's declared
        // textures then fall back to the white texel, loudly and once
        // (Batch2DNode). Copied, not borrowed.
        void SetAssetResolver(AssetResolveFn resolver) { m_resolveAsset = std::move(resolver); }
        [[nodiscard]] std::optional<std::filesystem::path> ResolveAsset(const Guid& id) const
        {
            return (m_resolveAsset && id.IsValid()) ? m_resolveAsset(id) : std::nullopt;
        }

        // Installed once by the frame driver beside SetAssetResolver. Without
        // it every image misses (loudly, once) and every texture slot on this
        // path binds its node's white texel.
        void SetPixelSupply(PixelSupplyFn supply)
        {
            if (m_textures)
                m_textures->SetPixelSupply(std::move(supply));
        }

        // Installed once by the frame driver, alongside (not instead of)
        // SetPixelSupply -- a content vehicle wants both: this one for its
        // Srgb-space scene content (Task 7), PixelSupplyFn still there for
        // whatever Display-space chrome that same vehicle might ever need.
        // Without it every Srgb-space Resolve falls back to PixelSupplyFn,
        // exactly as before this task existed.
        void SetArtifactSupply(ArtifactSupplyFn supply)
        {
            if (m_textures)
                m_textures->SetArtifactSupply(std::move(supply));
        }

        // Installed once by the frame driver, right after Create(). Without it every
        // mesh misses (loudly, once) and every mesh instance draws nothing.
        using MeshSupplyFn = NriMeshBufferCache::MeshSupplyFn;
        void SetMeshSupply(MeshSupplyFn supply)
        {
            if (m_meshBuffers)
                m_meshBuffers->SetMeshSupply(std::move(supply));
        }

        // The render-side half of a mesh cook's completion (s7.3) -- forwards to
        // NriMeshBufferCache::Invalidate with THIS context's own graveyard/fence, the
        // pair InvalidateContentTexture already uses. No-op without a cache.
        void InvalidateMeshGeometry(const Guid& id)
        {
            if (m_meshBuffers)
                m_meshBuffers->Invalidate(id, m_graves,
                                          m_graph ? m_graph->DebugSubmitCount() : 0);
        }

        [[nodiscard]] NriMeshBufferCache* MeshBuffers() noexcept { return m_meshBuffers.get(); }

        // F2b Task 12: forwards to NriTextureCache::SetCookPendingOracle --
        // see that method's own doc comment for the full contract. Installed
        // only on a vehicle that also owns a live cook queue (the editor's
        // viewport graph); a vehicle that never calls this keeps the
        // pre-Task-12 behaviour exactly (every artifact-supply miss is
        // PendingCook).
        using CookPendingOracle = NriTextureCache::CookPendingOracle;
        void SetCookPendingOracle(CookPendingOracle oracle)
        {
            if (m_textures)
                m_textures->SetCookPendingOracle(std::move(oracle));
        }

        // F2b Task 12: the render-side half of a cook-completion callback's
        // invalidation, for ONE guid -- forwards to NriTextureCache::
        // Invalidate using THIS context's own graveyard/fence (the same pair
        // Resize/teardown already use for InvalidateSource(s) above), so a
        // stale Resident/PendingCook/Refused entry is dropped safely and the
        // NEXT Resolve() re-attempts from scratch. No-op if this context has
        // no texture cache (Create() failed).
        void InvalidateContentTexture(const Guid& id)
        {
            if (m_textures)
                m_textures->Invalidate(id, m_graves, m_graph ? m_graph->DebugSubmitCount() : 0);
        }

        // THE SHARED image residency cache -- one per vehicle, consumed by
        // BOTH the batch node (a span's own t0 texture and a registered
        // material's declared params) and the post chain (its declared
        // params), so an image named by two of them is uploaded ONCE. Null
        // only if Create() failed, which never returns a vehicle.
        [[nodiscard]] NriTextureCache* Textures() noexcept { return m_textures.get(); }

        // F2b Task 11: resolves a mesh material's albedo Guid into a slot in
        // THIS context's MeshNode bindless material table -- the device seam
        // SceneRenderResolver's `resolveMeshAlbedoSlot` callback (Host/
        // SceneRenderResolver.hpp) is built over, since SceneRenderResolver
        // itself holds no device (its own "WHAT IT DOES NOT OWN" comment).
        //
        // Textures()->Resolve on first sight (uploading the artifact through
        // the shared cache exactly like a sprite or a declared sprite-
        // material param does), then mints a SEPARATE nri::Descriptor VIEW
        // over the resolved texture -- deliberately NOT Textures()->View(id)
        // -- before handing that fresh view to Mesh()->AddMaterial. Two
        // independent views, one shared texture: AddMaterial's own
        // OWNERSHIP contract (MeshNode.hpp) takes the view it is handed and
        // destroys it at BindlessTable::Release, and NriTextureCache
        // already owns and destroys ITS OWN view of the same asset for
        // every other consumer (a sprite, a declared material param) -- one
        // shared view with two destroyers is a double-destroy, not a
        // sharing optimization. Memoized by TEXTURE Guid (not material
        // Guid) in m_meshAlbedoSlots so N mesh materials sharing one albedo
        // still cost one Add, one slot and one extra view -- "the resolver
        // owns the texture->slot map" the plan calls for.
        //
        // kInvalidSlot's numeric value (0xFFFFFFFF -- BindlessTable::
        // kInvalidSlot; restated as a literal so this HEADER need not pull
        // BindlessTable.hpp's <NRI.h> in for one constant, matching how this
        // file already keeps kInvalidSlot's OWN value out of SceneResources.
        // hpp for the identical reason) for: a nil Guid, a context with no
        // texture cache or no bindless-capable MeshNode (bindless tier 0 --
        // MeshNode::Create refused), an image the shared cache cannot make
        // resident, or a table already at its kBindlessCapacity ceiling --
        // see NriTextureCache::Resolve's and BindlessTable::Add's own doc
        // comments for why each degrades silently (one warn, memoized) rather
        // than asserting.
        //
        // SLOT LIFETIME: a memoized slot is NEVER released individually --
        // BindlessTable is Add-only (BindlessTable.hpp's SLOT POLICY: no
        // per-slot free) and NriTextureCache itself never evicts a single
        // texture either (only a whole-cache Release()), so "released when
        // the texture leaves residency" reduces, on THIS shape, to "released
        // when this whole context is": m_meshAlbedoSlots is a plain member
        // with no burial of its own, and it goes stale exactly when
        // m_textures/m_mesh do, at Release()/teardown. A run that streams
        // through many more than kBindlessCapacity distinct albedos over its
        // lifetime exhausts the table -- AddMaterial's own one-shot WARN says
        // so, and the rest simply draw the flat baseColor path (kInvalidSlot)
        // from then on, never a crash.
        //
        // CLOSED by Task 12 (was: "KNOWN GAP, left for Task 12"): a Guid
        // resolved while its artifact was still PendingCook (NriTextureCache's
        // checkerboard placeholder) gets memoized against the PLACEHOLDER's
        // descriptor -- promotion to the real artifact on a later poll does
        // NOT re-Add or update the cached slot on its own, since a slot is
        // only ever taken on the FIRST successful resolve of a given Guid.
        // InvalidateMeshAlbedoSlot (below) is the fix: the editor's cook-
        // completion callback calls it for every guid a cook pass reports as
        // freshly cooked, dropping the stale placeholder-bound memo so the
        // NEXT ResolveMeshAlbedoSlot call re-resolves against the (by then
        // Resident) real artifact and takes a fresh slot.
        //
        // CALL AT DECLARATION TIME ONLY -- forwards straight into
        // NriTextureCache::Resolve/View, which carries that rule itself
        // (NriTextureCache.hpp's NO BARRIERS section). SceneRenderResolver::
        // Refresh -- reached through the `resolveMeshAlbedoSlot` seam every
        // production host wires in -- is the PRODUCTION caller and already
        // runs well before that window, per ITS OWN "call before
        // Batcher2D::Begin" contract; a test calling this directly (outside
        // any RenderFrame) owes itself the same rule.
        [[nodiscard]] std::uint32_t ResolveMeshAlbedoSlot(const Guid& id);

        // F2b Task 12: closes the "KNOWN GAP, left for Task 12" paragraph
        // above. Drops the memoized slot for `id` (a plain map erase -- the
        // slot itself is never freed in BindlessTable, per the SLOT LIFETIME
        // paragraph above; this just forgets that THIS guid already has
        // one), so the NEXT ResolveMeshAlbedoSlot(id) re-resolves from
        // scratch: if the texture is Resident by then (this method is meant
        // to be called from the SAME cook-completion callback that already
        // called InvalidateContentTexture(id) for the render cache), that
        // re-resolve mints a fresh view over the REAL artifact and Adds a
        // NEW slot -- the old slot, taken while the texture was still
        // PendingCook and bound to the checkerboard's descriptor, is simply
        // abandoned (an accepted, documented cost: one extra bindless slot
        // per guid that was ever resolved before its first successful
        // cook, bounded by kBindlessCapacity same as every other slot use).
        // A no-op for a guid that was never memoized here.
        // FINAL-REVIEW NOTE (2026-09-04): the abandoned slot's descriptor is the
        // SHARED checkerboard placeholder's own view, which NriTextureCache::Invalidate
        // deliberately never touches, so the slot stays a valid view over a live texture
        // for as long as this context does; if this whole context later tears down
        // (Release()), the placeholder itself gets buried and the abandoned slot ends up
        // pointing at a descriptor scheduled for destruction while BindlessTable was
        // never told to release that specific slot -- a real gap against the reap
        // ordering this class otherwise holds to (every live slot released before its
        // descriptor dies), but only FORMALLY: the table and every MeshInstance that
        // could ever reference that slot index die in the SAME teardown, so nothing ever
        // samples it between the two events.
        void InvalidateMeshAlbedoSlot(const Guid& id) { m_meshAlbedoSlots.erase(id); }

    private:
        NriGraphContext();   // latches FramesInFlight() and the upload ring's size before any per-frame resource exists

        bool Init(const HostConfig& config, Window& window);
        bool InitOffscreen(const HostConfig& config, NriDevice& shared,
                           std::uint32_t width, std::uint32_t height, const NodeSet& nodes);
        // The half both Init flavors share, in the order the teardown contract
        // below reverses: ring, pipeline cache, texture cache, graph, shader
        // dir, nodes. Everything above it (device, swapchain / output texture)
        // is the flavor's own.
        bool InitCommon(const HostConfig& config, const NodeSet& nodes);

        // Creates the persistent offscreen output at `width` x `height` and
        // publishes it as m_offscreen. False (already logged + latched) on
        // failure. Does NOT destroy a previous one -- ResizeOffscreen sequences
        // that, because the destroy has to follow a drain.
        bool CreateOffscreenTarget(std::uint32_t width, std::uint32_t height);

        // This frame's probe/hover pixels, its pick ticket and its game-UI draw
        // data, published where the declarators and exec fns read them. Called
        // from BOTH render paths, from one function, because the two publishing
        // different defaults is exactly the drift the per-frame probe must not
        // introduce.
        void PublishFrameCoordinates(const FrameDesc& frame);

        // Declares this frame's nodes into m_graph (already Reset()). The one
        // place Tasks 8-12 add to. Mode-agnostic: the only thing the mode
        // changes is RgFrameShape::offscreenOutput.
        void BuildFrame(const FrameDesc& frame);

        // Creates the HOST_READBACK staging buffer for the current surface
        // extent (the swapchain's, or the offscreen output's), or reuses the
        // existing one when the extent is unchanged. False (already logged) on
        // failure -- the frame still renders, it just cannot be captured.
        bool EnsureCaptureBuffer();

        // --- TEARDOWN CONTRACT: declaration order == reverse destruction ---
        // m_graph dies first -- EXPLICITLY, from ~NriGraphContext's body rather
        // than as a member (it buries its pool/views/command slots into THIS
        // CONTEXT'S LANE at its own last submitted fence value, and the body is
        // the only place that still drains that lane afterwards) -- then the
        // cache and the ring, then the swapchain, then m_graves itself, then
        // the NRI device, then the native device it wrapped. Do NOT reorder:
        // this is contract item 15 (the NRI device is destroyed BEFORE the
        // native one) plus the graveyard's "everything buried must outlive
        // nothing".
        //
        // THE WINDOW IS NO LONGER PART OF THIS CONTRACT because this object no
        // longer owns one (Task 6). It is the host's, and the swapchain that
        // names its HWND/surface must still die before it does -- which the
        // host guarantees by OUTER ordering instead: RuntimeApp declares
        // m_graphContext after m_gpu, so this whole object (swapchain
        // included) is destroyed while that window is still alive. A pointer
        // rather than a reference so the default ctor + Init() shape survives.
        // OFFSCREEN MODE holds neither of the first three: no window, no native
        // device (the shared one's owner has it) and no swapchain. m_device
        // points at m_ownedDevice in host-window mode and at the caller's
        // NriDevice in offscreen mode -- which is exactly why the owning handle
        // and the reading pointer are two members rather than one. The teardown
        // order below is unchanged by the split: m_ownedDevice sits where
        // m_device used to, so a host-window context is destroyed member for
        // member as before, and an offscreen context simply has nothing there.
        Mode                               m_mode = Mode::HostWindow;
        Window*                            m_borrowedWindow = nullptr;
        std::unique_ptr<NativeDeviceOwner> m_native;
        std::unique_ptr<NriDevice>         m_ownedDevice;
        NriDevice*                         m_device = nullptr;
        // ---- THIS CONTEXT'S GRAVEYARD LANE --------------------------------
        // Everything this context, its graph, its nodes and its caches destroy
        // goes in HERE, keyed to m_graph's own submission fence -- see Graves()
        // and the TWO CONTEXTS, TWO LANES block in the file header.
        //
        // ITS POSITION IS THE CONTRACT. Declared ABOVE every object that buries
        // into it, so it is destroyed AFTER all of them; and BELOW m_device /
        // m_ownedDevice, so the device whose function table its thunks call is
        // still alive when Release's ~Graveyard drains a straggler. Nothing
        // should ever reach that drain -- ~NriGraphContext empties this lane
        // itself, after destroying m_graph explicitly -- and in Debug
        // ~Graveyard asserts exactly that.
        Graveyard                          m_graves;
        std::unique_ptr<NriSwapChain>      m_swap;
        NriUploadRing                      m_ring;
        // render.uploadRingBytesPerFrame, latched by the constructor (Restart):
        // the size m_ring is Init()'d with and every log line reports.
        std::uint64_t                      m_uploadRingBytes = 0;
        NriPipelineCache                   m_pipelines;
        // BEFORE the graph and the nodes in declaration order, so it is
        // destroyed AFTER them: a node holds descriptor SETS naming this
        // cache's views, and its Release must run before the views do. It
        // holds nothing of the graph's (its textures are persistent and are
        // not pool tenants), so it is deliberately NOT released on Resize.
        std::unique_ptr<NriTextureCache>   m_textures;
        // Immediately after m_textures: a node's recorded command buffer names
        // this cache's vertex/index buffers, so it must be destroyed AFTER the
        // nodes (declaration order). Persistent, not a pool tenant --
        // deliberately NOT released on Resize/ResizeOffscreen; re-uploading
        // every mesh on a viewport drag would be the ring cliff through a
        // different door.
        std::unique_ptr<NriMeshBufferCache> m_meshBuffers;
        // The GPU scene's device half (F3 plan 1 T6), beside the mesh cache
        // for the same reason: recorded command buffers name its buffers, so
        // it is destroyed AFTER the nodes (declaration order). Persistent.
        std::unique_ptr<GpuScene>           m_scene;
        std::unique_ptr<RenderGraph>       m_graph;
        // The nodes, after everything they borrow (device, cache) and after the
        // graph whose transient pool the tonemap's source view names. Their
        // objects are RELEASED explicitly in ~NriGraphContext, before the drain
        // -- these destructors are the safety net, not the path.
        std::unique_ptr<Batch2DNode>       m_batch2D;
        std::unique_ptr<MeshCullNode>      m_meshCull;
        std::unique_ptr<MeshNode>          m_mesh;
        std::unique_ptr<GridNode>          m_grid;
        std::unique_ptr<PostChainNode>     m_post;
        std::unique_ptr<TonemapNode>       m_tonemap;
        // Built only under --pick-probe or NodeSet::pickOutline (see
        // Pick()/Outline()). The outline node holds views over the graph's
        // transient pool, so it is released in ~NriGraphContext beside the
        // other two and BEFORE the graph releases that pool.
        std::unique_ptr<PickNode>          m_pick;
        std::unique_ptr<OutlineNode>       m_outline;
        // The two HUD nodes, built only under NodeSet::hostHud / ::gameUi.
        // Neither caches a view over the graph's transient pool (see
        // ImGuiNriNode.hpp), but both are released in ~NriGraphContext beside
        // the others anyway -- one teardown order for every node is cheaper to
        // keep correct than a per-node exception.
        //
        // AT MOST ONE OF THESE MAY ADOPT ANY GIVEN ImGui CONTEXT -- NodeSet's
        // invariant block says why, and the gating is what enforces it.
        std::unique_ptr<ImGuiNriNode>      m_imguiHud;
        std::unique_ptr<ImGuiNriNode>      m_imguiGame;

        // Raw offline shader artifacts, keyed by artifact stem. unordered_map
        // and not vector: the node authors hold SPANS into these vectors for
        // their whole lifetime, and a node-based container never relocates a
        // value (NriPipelineCache's fill contract, rule 2).
        std::filesystem::path m_shaderDir;
        std::unordered_map<std::string, std::vector<std::uint8_t>> m_shaderBins;

        // F2b Task 11's texture->slot memo -- see ResolveMeshAlbedoSlot's own
        // doc comment for the full contract (what populates it, and why
        // there is no per-entry release). Keyed by the ALBEDO TEXTURE Guid,
        // never by a .arcmat Guid -- deliberately a DIFFERENT key space than
        // MeshMaterialCache's own table (Render/MeshMaterialCache.hpp), which
        // is keyed by material Guid and lives one layer up, device-free.
        std::unordered_map<Guid, std::uint32_t> m_meshAlbedoSlots;

        // The capture staging buffer, owned here (the graph only IMPORTS it --
        // a transient graph buffer is DEVICE-local and could never be mapped).
        // Buried in ~NriGraphContext at the graph's last submitted fence value.
        nri::Buffer*  m_capture          = nullptr;
        std::uint64_t m_captureRowPitch  = 0;
        std::uint64_t m_captureSlicePitch = 0;
        std::uint32_t m_captureWidth     = 0;
        std::uint32_t m_captureHeight    = 0;
        // Set when a capture node was actually RECORDED into a submitted
        // frame -- ReadCapture refuses without it rather than mapping and
        // writing whatever was already in host-readback memory.
        bool          m_captureRecorded  = false;

        // FrameDesc::batch for the frame currently being declared -- see
        // CurrentBatch(). Cleared at the end of every RenderFrame so a stale
        // batcher can never be drained by a later frame.
        Batcher2D* m_currentBatch = nullptr;

        // FrameDesc::globals, by VALUE -- see CurrentGlobals().
        GlobalParams m_currentGlobals{};

        // FrameDesc::pickables / ::selectedIds, published for exactly the span
        // of one frame's declaration and cleared afterwards -- the same
        // treatment m_currentBatch gets, and for the same reason: nothing may
        // reach a stale span from a later frame.
        //
        // BOTH ARE FULLY CONSUMED AT DECLARATION TIME, which is what makes a
        // borrowed span the right shape here: PickNode::PrepareDrawables turns
        // the drawables into vertices and OutlineNode::PrepareSelection COPIES
        // the ids, both into node-owned storage, before this function returns.
        // Reading either of these from an exec fn would read an empty span.
        std::span<const PickDrawable>  m_currentPickables;
        std::span<const std::uint32_t> m_currentSelectedIds;
        // FrameDesc::pickView, copied beside the two spans (a value, so there
        // is nothing to clear -- a later frame overwrites it).
        ViewTransform                  m_currentPickView{};

        // FrameDesc::imgui, published for the whole of one RenderFrame call
        // (declaration AND execution) and re-published -- possibly as null --
        // at the top of the next one, so a later frame can never reach a
        // previous frame's draw data. See CurrentImGuiDrawData().
        ImDrawData* m_currentImGui = nullptr;
        // FrameDesc::gameUi, with m_currentImGui's lifetime exactly.
        ImDrawData* m_currentGameUi = nullptr;

        // --pick-probe: armed at Create from the config, so an ordinary run
        // never builds the nodes at all.
        bool         m_pickArmed      = false;
        std::int32_t m_probeX         = 0;
        std::int32_t m_probeY         = 0;
        // THIS FRAME's readback texel / hover cursor / request label, defaulted
        // from the two above at the top of every RenderFrame so a driver that
        // names neither reads exactly the --pick-probe behaviour.
        glm::ivec2    m_currentPick{0, 0};
        glm::ivec2    m_currentHover{0, 0};
        std::uint64_t m_currentPickTicket = 0;
        // Latched when the probe pixel falls outside the current surface: the
        // node would still copy a CLAMPED texel (the frame's shape must not
        // depend on a coordinate), but reporting that id as the answer would be
        // a confident lie about a pixel nobody asked for. Said once.
        bool         m_probeOutOfRange = false;

        // See SetAssetResolver. Empty until the frame driver installs one.
        AssetResolveFn m_resolveAsset;

        // ---- offscreen mode -----------------------------------------------
        // The persistent output, and the pacing timeline that bounds frames in
        // flight without a swapchain to do it. Both null/absent in host-window
        // mode; both buried/destroyed in ~NriGraphContext, before the drain,
        // like every other NRI object here.
        //
        // m_offscreenFence is signalled with m_frameIndex + 1 by a trailing
        // signal-only QueueSubmit and waited on FramesInFlight() deep
        // at the top of the next frame -- the same 1-based values, the same
        // depth and the same polling wait NriSwapChain's pacing fence uses.
        // "This offscreen context is its process's ONLY graph context, so it
        // owes the GPU-progress heartbeat nobody else is publishing." The
        // CALLER asserts it (SetGpuHeartbeatPublisher); false by default, so
        // the editor's two-context topology is unchanged by construction.
        bool          m_publishGpuBeat  = false;
        nri::Texture* m_offscreen       = nullptr;
        nri::Fence*   m_offscreenFence  = nullptr;
        std::uint32_t m_offscreenWidth  = 0;
        std::uint32_t m_offscreenHeight = 0;

        // Whether THIS context armed the process-wide crash chain, and
        // therefore whether its destructor may disarm it. Only a host-window
        // context ever arms (CreateOffscreen's comment says why), and only an
        // armer may disarm: NriDiagnostics keeps ONE slot with no owner
        // identity, so an unconditional Disarm() from a second context would
        // silently unplug the first one's chain.
        bool          m_armedDiagnostics = false;

        nri::Format   m_format     = nri::Format::UNKNOWN;
        std::uint64_t m_frameIndex = 0;   // PRESENTED frames; the command-slot clock
        bool          m_vsync      = true;
    };

    // =====================================================================
    // THE FRAME'S SHAPE, as a free function both the vehicle and the device-less
    // [nri] tests DRIVE.
    //
    // NriGraphContext::BuildFrame is a two-line wrapper over
    // DeclareGraphFrame, and RenderGraphTest.cpp's frame-shape case calls the
    // same function with a null context. That is deliberate and it is the
    // point: the previous shape test TRANSCRIBED BuildFrame's declarations
    // into the test, so a change to the frame could not make it red. It can
    // now -- change what the frame declares and the compiled barrier chain the
    // test asserts changes with it.
    //
    // A null `context` means "declarations only": every AddNode call, every
    // resource, every Read/Write and every attachment is identical, and only
    // the exec fns become no-ops (they have no device to record against).
    // =====================================================================
    struct RgFrameShape
    {
        bool          capture      = false;
        // The canvas transient's extent -- the swapchain's, so the tonemap
        // samples 1:1.
        std::uint32_t canvasWidth  = 0;
        std::uint32_t canvasHeight = 0;
        // The HOST_READBACK staging buffer the capture node copies into. It is
        // IMPORTED rather than created as a transient because the graph
        // realizes transients in MemoryLocation::DEVICE, which can never be
        // mapped. May be null when `capture` is false (and device-lessly even when
        // it is true -- ImportBuffer stores the pointer without dereferencing).
        nri::Buffer*  captureBuffer = nullptr;
        std::uint64_t captureBytes  = 0;

        // OFFSCREEN MODE. The persistent, sampled texture the tonemap renders
        // into INSTEAD of a swapchain backbuffer; null means "present mode",
        // i.e. the tonemap calls ImportSwapChainTexture instead.
        //
        // IT IS THE ONLY DIFFERENCE BETWEEN THE TWO MODES' FRAMES. Everything
        // downstream of the tonemap -- the outline composite, the HUD, the
        // capture -- writes RgFrameHandles::backbuffer, which is whichever of
        // the two this chose, so the node census, the reads/writes and every
        // derived barrier except the final one are identical. That is pinned by
        // an [nri] case, so a later node that branches on this field will make
        // it red.
        //
        // Held as a bare nri::Texture* and never dereferenced at declaration
        // time (ImportTexture only records it), which is what lets the device-less
        // [nri] cases drive the real offscreen shape with a stand-in pointer --
        // the same licence `captureBuffer` above takes.
        nri::Texture* offscreenOutput = nullptr;

        // The scene post chain to insert between the canvas and the tonemap,
        // or null for none. Read for its per-pass WIRING and slot count here;
        // its bytecode and values are PostChainNode::PrepareChain's business.
        // Honoured only when `stage` includes the chain (Batch drops it).
        //
        // A device-less drive can point this at a PostChainDesc carrying nothing
        // but `passes[i].inputs` -- the declarations depend on the wiring and
        // on nothing else, which is what lets the [nri] frame-shape cases
        // exercise the real chain shape with no device.
        const PostChainDesc* post = nullptr;

        // Declare the pick + JFA outline chain after the tonemap (Task 11).
        // The ONLY thing about that chain the declarations depend on is this
        // flag and the canvas extent -- the drawables, the selection and the
        // probe pixel are all RECORD-time data that travels through
        // NriGraphContext -- which is exactly what lets the device-less [nri]
        // cases drive the real shape with a null context.
        //
        // Independent of how the scene is sliced, deliberately: nulling
        // `post` or the HUD pointers slices the SCENE render, and the outline
        // is not part of that -- it is asked for by its own flag or not at
        // all.
        bool pickOutline = false;

        // Declare the HUD node after the tonemap. Unlike `pickOutline` this
        // follows the CALLER'S POINTER -- host chrome is drawn only when
        // FrameDesc::imgui is non-null -- and the declarations depend on this
        // flag and on nothing else: the
        // ImDrawData itself is RECORD-time data that travels through
        // NriGraphContext, which is what lets the device-less [nri] cases drive
        // the real shape with a null context.
        bool imgui = false;

        // Declare the GAME HUD's node between the tonemap and the pick/outline
        // chain (Task 9) -- the editor's phase 11 then phase 12 order against
        // this recorder.
        //
        // GATED LIKE `imgui`, and for the same reason: the game HUD is OVERLAY
        // content. Both HUDs sit on top of the scene and mask exactly the
        // pixels a frame-to-frame comparison needs, so a caller that asked for
        // the scene without host chrome does not want the plugin's either. See
        // DeclareGraphFrame's gate for the full account.
        bool gameUi = false;

        // Task 4 (Phase 4): declare a D32_SFLOAT depth transient, canvas-sized,
        // for the frame's mesh pass. `RgUsage::DepthWrite` and
        // `RenderGraph::SetDepthAttachment` both already existed (Phase 2) with
        // no production consumer; this flag gives DeclareGraphFrame a reason to
        // create the resource. It does NOT wire up a consumer -- nothing in
        // DeclareGraphFrame Writes or attaches it yet, because the first real
        // consumer (MeshNode) is Task 7's. See RgFrameHandles::depth and
        // DeclareGraphFrame's own comment at the depth block for the full
        // account of what "declared but unconsumed" costs (nothing: Compile()
        // gives an untouched transient no lifetime and no pool slot).
        //
        // SINCE TASK 7 THIS IS THE "DEPTH WITHOUT A MESH PASS" CASE ONLY. A
        // frame that carries `mesh` below gets its depth target from MeshNode,
        // which creates, writes and attaches it in one node -- and then this
        // placeholder is not declared at all. The two are mutually exclusive by
        // construction (DeclareGraphFrame's own `else`), so no graph ever
        // carries two CreateTexture("depth") calls.
        bool depth = false;

        // Task 7 (Phase 4): THE MESH PASS's scene (opaque, masked and ordered
        // transparent since F3 plan 2), or null for none.
        // Read for MeshSceneDesc::Empty() here (an empty scene -- no ad-hoc
        // instances and no registry scene with draws -- declares nothing,
        // exactly as a null one does) and for nothing else -- the geometry, the
        // per-instance transforms and tints, the camera and the one directional
        // light are all consumed by MeshNode at Record time; the registry
        // scene's rows by GpuSceneSyncNode's (F3 plan 1 T6).
        //
        // A device-less drive can point this at a MeshSceneDesc carrying one
        // instance with a null `mesh`: the DECLARATIONS depend on the scene
        // being non-empty and on nothing else, which is what lets the [nri]
        // frame-shape cases exercise the real mesh pass with no device.
        //
        // BORROWED for the duration of the RenderFrame call -- see
        // MeshSceneDesc::instances.
        const MeshSceneDesc* mesh = nullptr;

        // F4 plan 1 Task 10 (spec s5.2): THE 3D REFERENCE GRID's desc, or null
        // for none. Read for its PRESENCE here and for nothing else: the node
        // is declared after the mesh block, reading the mesh pass's depth
        // transient as its depth attachment when `mesh` above declared one
        // (and with no depth attachment otherwise). The camera, the plane and
        // the colours are consumed by GridNode at Record time. A device-less
        // drive can point this at a default GridSceneDesc.
        const GridSceneDesc* grid = nullptr;
    };

    struct RgFrameHandles
    {
        // THE FRAME'S FINAL TARGET -- the acquired swapchain backbuffer, or
        // (when RgFrameShape::offscreenOutput was set) the imported offscreen
        // output. Named for the common case rather than renamed at Task 7:
        // every node downstream of the tonemap writes THIS handle whichever it
        // is, and a second name would have suggested a second code path where
        // there is none.
        RgTexture backbuffer{};
        RgTexture canvas{};
        RgBuffer  capture{};
        // The LAST post-chain pass's target -- what the tonemap sampled. An
        // invalid handle (and a zero count) when the frame carried no chain,
        // in which case the tonemap sampled `canvas`.
        RgTexture     post{};
        std::uint32_t postPassCount = 0;

        // The pick + outline chain's handles (Task 11). All invalid, and
        // jfaStepCount 0, on a frame that did not declare it.
        RgTexture     pickIds{};        // the R32_UINT entity-id transient, at PickSupersample() x
        RgTexture     pickDepth{};      // the id pass's OWN D32 transient (F4 spec s7.1), same extent
        RgBuffer      pickReadback{};   // the imported HOST_READBACK staging buffer
        RgTexture     outlineField{};   // the LAST JFA target -- what the composite sampled
        // Thickness-derived, so it is the SAME on every surface size (D3c) --
        // OutlineJfaStepCount(OutlineMaxThicknessPx()), clamped to kMaxJfaSteps.
        std::uint32_t jfaStepCount = 0;

        // THE FRAME'S DEPTH TARGET, or an invalid handle on a frame that asked
        // for neither a mesh scene nor `depth`. WHICH OF THE TWO IT CAME FROM
        // is visible here and matters to a reader:
        //
        //   * a MESH SCENE (Task 7) -- MeshNode created, wrote and attached it,
        //     so it carries a real lifetime and its own pool slot;
        //   * RgFrameShape::depth ALONE (Task 4) -- the placeholder node
        //     created the resource and nothing consumes it, so it carries no
        //     lifetime and no pool slot and RealizePool never allocates it.
        //
        // The two are mutually exclusive (DeclareGraphFrame's own if/else).
        RgTexture depth{};
    };

    ARC_API RgFrameHandles DeclareGraphFrame(RenderGraph& graph, const RgFrameShape& shape,
                                                 NriGraphContext* context);
}
