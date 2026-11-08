#pragma once

// MaterialPreviewHarvester (asset-manager redesign, Plan 1 Task 8; widened F2c Plan 2
// Task 9): LIVE 64px thumbnails of real .arcmat MATERIALS and, from F2c, real .arcmesh
// MESH ASSETS, for the Assets panel -- real shaded pixels of the actual asset, not a
// kind icon. The class name stays (renaming it would touch every call site for no
// behavioural gain); its charter is what widened.
//
// ===== THE SHAPE, IN ONE PARAGRAPH ==================================
// One LAZY 64x64 offscreen NriGraphContext (the same vehicle
// ShaderEditorDocument::EnsureGraphPreviewContext builds for its own preview,
// at 64 instead of 512), one device-less Batcher2D, and ONE SHARED LIFO queue
// of {Guid, Subject} work items -- a material or a mesh asset, never two
// separate queues (a burst of one subject must not starve the other). Pump()
// takes AT MOST ONE item per call, renders the shader editor's
// quad-on-checkerboard (or a lit sphere, for a mesh-kind material), or --
// for a mesh ASSET -- the mesh's own geometry framed by its AABB
// (MeshImportWave.hpp's FrameMeshBounds), captures it with ReadCapture, hands
// the tight RGBA to the CHROME context's NriTextureCache under a synthetic
// per-asset Guid, and writes the same bytes to
// <project>/Saved/Thumbnails/<guid>.png. The panel then draws
// ThumbTextureId(id) like any other ImGui image.
//
// A MESH thumbnail compiles NOTHING (a mesh-kind .arcmat has no snippet, and
// the mesh path was already the harvester's one compile-free branch), so it
// needs no new stage-key slot in the process-wide scheme documented below --
// not needing to extend that table is the evidence this belongs in the same
// class rather than a second harvester competing for the same device idle.
//
// ===== WHY ONE PER PUMP, AND WHY THE PUMP MUST EARLY-OUT ============
// ReadCapture idles the whole device (NriGraphContext::ReadCapture's own
// contract; EditorApp::CaptureGraphViewportPng states the same rationale for
// the cover screenshot: "BOTH CALL SITES ARE RARE EVENTS ... which is what
// pays for ReadCapture's DeviceWaitIdle"). A harvester that idled once per
// material per frame would stall the editor for as long as the backlog lasts,
// so: one harvest per frame, and Pump() returns before touching ANY device
// object when the queue is empty. Once the queue drains there is no per-frame
// cost at all -- not a resolve, not a fence wait, not an allocation.
//
// ===== WHY IT IS PERSISTED (the UE lesson) ==========================
// UE stores a rendered FObjectThumbnail in the package header and re-renders
// only on save (EThumbnailRenderFrequency::OnAssetSave) precisely because
// re-rendering every thumbnail at every editor start does not scale. Same
// here: a successful harvest writes <project>/Saved/Thumbnails/<guid>.png, and
// PrimeFromDisk() loads those PNGs straight into the pixel supply at project
// open, queueing a harvest ONLY for a material with no PNG or whose .arcmat is
// NEWER than its PNG. An unchanged project therefore boots with ZERO device
// idles for thumbnails.
//
// ===== THE COMPILE PATH, AND THE ONE RULE IT MUST NOT BREAK =========
// A real material thumbnail needs the material's COMPILED bytes, and this
// class resolves + submits them itself rather than owning a second
// SpriteMaterialCache/PostChainCache. That is not duplication for its own
// sake, it is the only safe shape: ShaderCompiler::Submit COALESCES by
// coalesceKey ("a newer Submit replaces a pending older one, and stale
// in-flight results are dropped at Drain"), so a second instance of either
// cache would submit the SAME key for the same material as the scene's
// instance -- silently cancelling the scene's in-flight compile and stranding
// its pending entry forever. So this class takes its own slot in the
// process-wide stage-key scheme:
//
//     ShaderEditorDocument  0x1 / 0x2      (open document)
//     SpriteMaterialCache   0x4 / 0x8      (scene sprite materials)
//     PostChainCache        0x10 / 0x20    (scene post chain)
//     THIS CLASS            0x40 / 0x80    (material thumbnails)
//
// Results reach it through the process's ONE drain site
// (SceneRenderResolver::Services::consumeFirst, EditorApp::Init) via
// OfferCompileResult, which NEVER reports a result as consumed -- it is a
// passive observer beside the caches, not a competitor for their results.
//
// ===== WHAT EACH SURFACE ACTUALLY RENDERS ===========================
// See Pump()'s implementation comment; the short version is sprite -> the
// material on a quad over the checkerboard, fullscreen -> the material's own
// pass chain over that same checkerboard as kSceneInput (exactly what the
// shader editor's preview does), mesh -> a lit UV sphere in the material's
// resolved baseColor (the mocks' spheres; MeshBuilder::BuildUvSphere and the
// vehicle's always-built MeshNode made the real mesh path trivially reachable,
// so the brief's "same quad" fallback was not needed).

#include <Arcane/Guid.hpp>
#include <Arcane/Host/HostConfig.hpp>
#include <Arcane/Render/GraphicsBackend.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <vector>

namespace Arcane
{
    class NriGraphContext;
    class ShaderCompiler;
    class ShaderSourceProvider;
    struct PixelData;
    struct ShaderCompileResult;
    struct LoadedClientMesh;   // F2c Plan 2 Task 9: Services::meshArtifactFor's payload
}

namespace Arcane::Editor
{
    class MaterialPreviewHarvester
    {
    public:
        using ResolveAssetFn =
            std::function<std::optional<std::filesystem::path>(const Arcane::Guid&)>;
        using PixelSupplyFn = std::function<const Arcane::PixelData*(const Arcane::Guid&)>;
        // F2c Plan 2 Task 9: the SAME two seams MeshCache::Services forwards from the
        // Assets facade verbatim (Render/MeshCache.hpp's own comment on why no wrapper
        // is needed) -- Guid -> the cooked mesh artifact, and Guid -> "is a cook
        // plausibly still pending". Only an Imported .arcmesh's resolve ever consults
        // these; a generated primitive needs neither, so leaving both unset (as
        // EditorApp does until Task 10 wires the Browser panel's calls) is a safe,
        // fully guarded no-op -- it degrades a mesh-asset request to "no cooked
        // geometry yet" rather than crashing.
        using MeshArtifactSupplyFn = std::function<const Arcane::LoadedClientMesh*(const Arcane::Guid&)>;
        using CookPendingFn        = std::function<bool(const Arcane::Guid&)>;

        struct Services
        {
            // THE CHROME VEHICLE, LOOKED UP LIVE. This object is constructed
            // during Init (before CreateGraphVehicles has made a device at
            // all) and outlives any one project, so every device-adjacent
            // thing it needs -- the shared NriDevice and the NriTextureCache the
            // thumbnails are uploaded into (whose drop path takes the ImGuiNri
            // entry with it on a re-harvest) -- is reached through
            // this callback at CALL time. Same `[this]`-capture idiom
            // EditorApp::Init already uses for resolveAssetThumb. Null (or a null return) is a hard no-op.
            std::function<Arcane::NriGraphContext*()> chromeGraph;

            // Read for backend/vsync-free knobs by CreateOffscreen, exactly as
            // ShaderEditorDocument's preview reads it. Required.
            const Arcane::HostConfig* hostConfig = nullptr;

            // The app-shared compile service and template source provider. A
            // null or unavailable compiler disables sprite/fullscreen thumbs
            // (mesh thumbs still work -- they compile nothing).
            Arcane::ShaderCompiler*       compiler = nullptr;
            Arcane::ShaderSourceProvider* sources  = nullptr;
            Arcane::GraphicsBackend       backend{};

            // Guid -> file, through the LIVE project (re-read per call, so it
            // survives a project switch).
            ResolveAssetFn resolveAsset;
            // Guid -> decoded RGBA8, i.e. Assets::PixelsFor -- what the
            // preview vehicle's own NriTextureCache resolves a material's
            // declared texture params through.
            PixelSupplyFn pixelSupply;
            // <project>/Saved/Thumbnails, resolved live (empty = no
            // persistence this session; harvests still work, they just are
            // not written or re-read).
            std::function<std::filesystem::path()> thumbnailDir;

            // F2c Plan 2 Task 9: the mesh-ASSET branch's own resolve seams -- see
            // MeshArtifactSupplyFn/CookPendingFn's own comments above.
            MeshArtifactSupplyFn meshArtifactFor;
            CookPendingFn        cookPending;
        };

        explicit MaterialPreviewHarvester(Services services);
        ~MaterialPreviewHarvester();
        MaterialPreviewHarvester(const MaterialPreviewHarvester&)            = delete;
        MaterialPreviewHarvester& operator=(const MaterialPreviewHarvester&) = delete;

        // ASK FOR A THUMBNAIL, cheaply and idempotently -- the call the Browse
        // draw makes for every VISIBLE un-thumbed material, every frame (Task
        // 10). A no-op once the material has a thumbnail, is already queued,
        // is in flight, or has failed, so it costs one hash lookup in the
        // steady state. Pushes to the FRONT of the queue (LIFO), so the one
        // harvest this frame pays for is always something on screen: a FIFO
        // queue would instead spend it on whatever was scrolled past first.
        void Request(const Arcane::Guid& material);

        // THE FORCED FORM: this material's pixels are now WRONG (it was saved,
        // its file changed on disk, one of its textures finished cooking, or an
        // ancestor it derives from changed). Clears the failed/known memo,
        // drops the on-disk PNG's claim, and queues a re-harvest. The EXISTING
        // thumbnail keeps being served until the new one lands (last-good,
        // the same discipline SpriteMaterialCache::Invalidate states).
        void Invalidate(const Arcane::Guid& material);

        // MESH ASSET counterparts of Request/Invalidate above (F2c Plan 2 Task 9,
        // spec s8/R5) -- mirror them exactly, idempotent, LIFO push-front, clearing
        // the failed memo, keeping the last-good thumbnail served until the new one
        // lands. `mesh` shares this class's ONE queue/inFlight/failed/entries space
        // with material Guids (asset Guids are unique across the whole registry
        // regardless of kind, so the two can never collide) -- what actually
        // distinguishes a mesh work item from a material one is the Subject tag
        // carried alongside its Guid in the shared queue, not a second queue.
        void RequestMesh(const Arcane::Guid& mesh);
        void InvalidateMesh(const Arcane::Guid& mesh);

        // PROJECT OPEN: load every already-harvested PNG straight into the
        // pixel supply, and queue a harvest ONLY for an asset with no PNG or
        // whose SOURCE (.arcmat or, F2c Plan 2 Task 10, .arcmesh) mtime is
        // newer than its PNG's. `materials` -- the parameter keeps its
        // original name, now carrying mesh guids too, exactly as EditorApp's
        // one call site widens it -- needs no Subject alongside each Guid:
        // the "usable" PNG-load half of this function doesn't care what kind
        // produced the picture, and the stale/missing half determines the
        // right dispatch itself, off the resolved source path's own
        // extension (Subject is this .cpp's private dispatch tag; a second
        // public parameter here would duplicate what the extension already
        // says). Pure CPU -- it is called from a boot stage, strictly before
        // any device exists.
        void PrimeFromDisk(const std::vector<Arcane::Guid>& materials);

        // Offered every drained compile result by the process's ONE drain
        // site. NEVER consumes: see the stage-key block at the top of this
        // file.
        void OfferCompileResult(const Arcane::ShaderCompileResult& result);

        // ONE harvest per call, at most, and a no-op that touches nothing when
        // the queue is empty. `now` is the compile service's clock.
        void Pump(double now);

        // The ImGui texture id for this material's thumbnail, or 0 when there
        // is none yet. Lazily uploads pixels that PrimeFromDisk loaded before
        // a device existed -- which is why the pimpl is mutated from a const
        // method (the caller is a `resolveAssetThumb`-style const query and
        // should not have to know that).
        [[nodiscard]] std::uint64_t ThumbTextureId(const Arcane::Guid& material) const;

        // The chrome NriTextureCache's PixelSupplyFn answer for a SYNTHETIC
        // thumbnail guid, or null when `id` is not one of ours. EditorApp's
        // chrome pixel-supply lambda consults this first, exactly as it
        // consults m_graphLogoId first.
        [[nodiscard]] const Arcane::PixelData* PixelsForThumb(const Arcane::Guid& id) const;

        // How many materials are still waiting (queued + in flight). Boot-log
        // evidence, and the Browse draw's "still working" hint later.
        [[nodiscard]] std::size_t PendingCount() const;

        // PROJECT SWITCH: forget every thumbnail, every queue entry and the
        // preview vehicle itself (a Guid means something else in a different
        // project, and the batcher holds the old project's registered
        // materials). Safe to call with the chrome context alive, which is the
        // only state a switch leaves.
        void Clear();

        // THE CHROME CONTEXT IS ABOUT TO DIE. Destroys the preview vehicle
        // (which BORROWS the chrome context's device) while that device is
        // still alive -- the borrower-dies-first half of the rule
        // EditorApp::ShutdownGraphPath states for the viewport context.
        // Idempotent; the thumbnail bytes survive (they are CPU-side).
        void Shutdown();

    private:
        struct Impl;
        Impl* m_impl = nullptr;
    };
}
