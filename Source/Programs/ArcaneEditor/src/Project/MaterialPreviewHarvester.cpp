#include "Project/MaterialPreviewHarvester.hpp"
#include "Documents/MaterialSpherePreview.hpp"   // the sphere scene, shared with the document's live preview (T3-D6)
#include "Settings/DocumentSettings.hpp"          // editor.preview.*: the checker, the one preview light (S6-35)
#include "Settings/EditorThumbnailSettings.hpp"   // editor.thumbnail.*: size, clock, retries, mesh camera (S6-39)

#include <Arcane/Assets/Assets.hpp>          // LoadDisplayPixels / WriteThumbnailPngRgba
#include <Arcane/Assets/ImageIo.hpp>         // PixelData
#include <Arcane/Base/Log.hpp>
#include <Arcane/Config/Settings.hpp>
#include <Arcane/Material/GlobalParams.hpp>
#include <Arcane/Material/MaterialAsset.hpp>
#include <Arcane/Material/MaterialInstance.hpp>
#include <Arcane/Material/MaterialSource.hpp>
#include <Arcane/Material/MaterialTemplate.hpp>
#include <Arcane/Mesh/MeshAsset.hpp>          // F2c Plan 2 Task 9: LoadMeshAsset/ResolveMeshData -- the mesh-ASSET branch
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Mesh/MeshBuilder.hpp>
#include <Arcane/Render/MeshMaterialCache.hpp>
#include <Arcane/Render/Nri/BindlessTable.hpp>
#include <Arcane/Render/Nri/NriGraphContext.hpp>
#include <Arcane/Render/PostChainCache.hpp>   // PostChainDesc
#include <Arcane/Render/ShaderCompiler.hpp>
#include <Arcane/Render/ShaderConventions.hpp>
#include <Arcane/Render/ShaderSourceProvider.hpp>
#include <Arcane/Scene/SceneCamera.hpp>       // PerspectiveProjection

#include <Project/MeshImportWave.hpp>          // F2c Plan 2 Task 9: FrameMeshBounds

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cctype>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Arcane::Editor
{
    namespace
    {
        // editor.thumbnail.* (S6-39) replaced the four thumbnail literals that
        // lived here. thumb.size is the rendered target, the on-disk PNG's
        // size and the loader's maxSize at once -- three numbers that must
        // agree, so one setting. thumb.time is a FIXED clock for every
        // thumbnail: a time-driven material (a pulse, a scroll) would
        // otherwise harvest a different picture every time, which makes the
        // persisted PNG and the live render disagree for no reason anybody can
        // act on. The checker cell derives from the size (ThumbnailCheckerCell).

        // ===== THE HARVESTER'S SLOT IN THE PROCESS-WIDE STAGE-KEY SCHEME ====
        // 0x40/0x80, disjoint from ShaderEditorDocument's 0x1/0x2, the sprite
        // cache's 0x4/0x8 and the post cache's 0x10/0x20 for the SAME asset.
        // This is not tidiness: ShaderCompiler::Submit COALESCES by key and a
        // newer Submit CANCELS the older pending one, so a thumbnail compile
        // sharing a key with the scene's would silently strand the scene's
        // cache on a result that never arrives. `pass` mixes in for the same
        // reason PostStageKey's does -- a chain's passes are separate jobs.
        std::uint64_t ThumbStageKey(const Arcane::Guid& id, bool vertex, std::size_t pass = 0)
        {
            return (id.hi ^ (id.lo * 1099511628211ull)) ^
                   (vertex ? 0x40u : 0x80u) ^
                   (static_cast<std::uint64_t>(pass) << 32);
        }

        // ===== F2c PLAN 2 TASK 9: WHAT SUBJECT ONE WORK ITEM IS ============
        // A material .arcmat, or (new) a mesh .arcmesh asset. NOT a second queue --
        // see MaterialPreviewHarvester.hpp's file header for why one shared LIFO
        // queue is load-bearing (a burst of one subject must not starve the other).
        enum class Subject : std::uint8_t { Material, Mesh };

        // The queue/pending/inFlight/ready key, widened from a bare material Guid.
        // Asset Guids are unique across the WHOLE project registry regardless of
        // kind (a material and a mesh asset can never share one), so `entries`,
        // `failed` and `thumbToMaterial` below stay Guid-keyed unchanged -- widening
        // THOSE would only add a lookup ambiguity ThumbTextureId/PixelsForThumb's
        // Guid-only signatures have no way to resolve. What genuinely needs the
        // Subject tag is dispatch: the shared queue must know, per item, whether to
        // resolve a material (through the compile path) or a mesh (through
        // ResolveMeshData) once it is popped.
        struct WorkKey
        {
            Arcane::Guid id;
            Subject subject = Subject::Material;

            // Deliberately NOT explicit: every pre-existing material call site
            // (Fail(id, ...), Push(id), queue/inFlight erase-by-Guid) keeps
            // compiling unchanged, implicitly widening to {id, Subject::Material} --
            // which is what makes the material path's behaviour provably untouched
            // by this widening rather than a second hand-copy of it.
            WorkKey() = default;
            WorkKey(const Arcane::Guid& g, Subject s = Subject::Material) : id(g), subject(s) {}

            bool operator==(const WorkKey&) const noexcept = default;
        };

        struct WorkKeyHash
        {
            std::size_t operator()(const WorkKey& k) const noexcept
            {
                std::size_t h = std::hash<Arcane::Guid>{}(k.id);
                return h ^ (static_cast<std::size_t>(k.subject) + 0x9e3779b97f4a7c15ULL +
                            (h << 6) + (h >> 2));
            }
        };

        // Layer the SAVED values onto a fresh instance -- template <- base's
        // params <- ... <- the leaf's params. Byte-for-byte the shape
        // SpriteMaterialCache::Impl::Bind and PostChainCache::Impl::Bind both
        // use; kept here rather than shared because neither exports it.
        std::shared_ptr<Arcane::MaterialInstance> LayerParams(
            const std::shared_ptr<Arcane::MaterialTemplate>& templ,
            const std::vector<Arcane::MaterialAssetData>& parentChain,
            const Arcane::MaterialAssetData& leaf)
        {
            auto inst = std::make_shared<Arcane::MaterialInstance>(
                std::shared_ptr<const Arcane::MaterialTemplate>(templ));
            for (auto it = parentChain.rbegin(); it != parentChain.rend(); ++it)
            {
                Arcane::ApplyMaterialParams(*it, *inst);
                inst = std::make_shared<Arcane::MaterialInstance>(
                    std::shared_ptr<const Arcane::MaterialInstance>(inst));
            }
            Arcane::ApplyMaterialParams(leaf, *inst);
            return inst;
        }
    }

    struct MaterialPreviewHarvester::Impl
    {
        // ---- what one material's compile is waiting on ------------------
        struct PassJob
        {
            std::uint64_t vsJob = 0, psJob = 0;
            std::vector<std::uint8_t> vsBytes, psBytes;
        };
        struct Pending
        {
            Arcane::Guid id;
            Arcane::MaterialSurface surface = Arcane::MaterialSurface::Sprite;
            std::shared_ptr<Arcane::MaterialTemplate> templ;
            Arcane::MaterialAssetData data;
            std::vector<Arcane::MaterialAssetData> parentChain;
            std::vector<PassJob> jobs;                        // 1 for sprite, N for a chain
            std::vector<std::vector<std::uint32_t>> passInputs;   // fullscreen only
            std::uint32_t chainInputSlots = 1;                    // fullscreen only
        };

        // ---- what one work item renders as, once it is ready to harvest --
        struct Ready
        {
            Arcane::Guid id;
            Subject subject = Subject::Material;
            Arcane::MaterialSurface surface = Arcane::MaterialSurface::Sprite;  // Material only
            std::uint16_t spriteMaterial = Arcane::Batcher2D::kInvalidMaterialId;  // Material/Sprite
            Arcane::PostChainDesc post;                                  // Material/Fullscreen
            glm::vec4 meshColor{1.0f};                     // Material/Mesh (the mesh-kind sphere)
            std::uint32_t meshSlot = 0xFFFFFFFFu;          // Material/Mesh (the mesh-kind sphere)

            // ---- Subject::Mesh (F2c Plan 2 Task 9): a mesh ASSET's own geometry ----
            // Resolved synchronously in StartOne (ResolveMeshData compiles nothing, so
            // there is no async Pending stage for this subject -- same shape as the
            // Material/Mesh sphere branch above, which also skips straight to Ready).
            Arcane::MeshData               meshGeometry;    // local space, unit-rule scaled
            Arcane::MeshBounds             meshBounds;      // ResolveMeshData's answer -- FrameMeshBounds' input
            std::vector<Arcane::MeshInstance> meshInstances;   // one per section, materials pre-resolved
            std::vector<Arcane::Guid>         meshInstanceMaterials;   // the material each section's slot came from (nil = none), so a re-render can re-Request it
        };

        // ---- one material's finished thumbnail --------------------------
        struct Entry
        {
            Arcane::Guid      thumbId;          // synthetic chrome-cache key
            Arcane::PixelData pixels;           // 64px RGBA, the supply's answer
            std::uint64_t     texture = 0;      // ImTextureID, 0 until resolved
        };

        Services services;

        // The vehicle + its recorder. Created LAZILY, ONCE, on the first
        // actual harvest -- never at construction (no device then) and never
        // per harvest.
        std::unique_ptr<Arcane::NriGraphContext> ctx;
        std::unique_ptr<Arcane::Batcher2D>       batch;
        // Mesh thumbnails: one sphere for the whole session, and a
        // params-only material cache. MeshMaterialCache touches NO compiler
        // (its own header says so), so a second instance of it cannot
        // collide with the scene resolver's the way a second
        // SpriteMaterialCache/PostChainCache would.
        Arcane::MeshData sphere;
        std::unique_ptr<Arcane::MeshMaterialCache> meshMats;

        // F2c Plan 2 Task 9: the ONE mesh-asset harvest in flight -- its REAL asset
        // guid and the geometry StartOneMesh resolved for it, served to the vehicle
        // by the SetMeshSupply lambda EnsureVehicle installs. Populated by Harvest()
        // immediately before RenderFrameOffscreen and released immediately after;
        // there is only ever one harvest per Pump, so nothing else reads or writes
        // them in between.
        //
        // THE REAL GUID, NOT A SYNTHETIC ONE, and that is final-review C1's fix. This
        // used to be a session-fixed 'MESH' guid the way `sphere` uses 'SPHR'. That
        // was harmless while MeshNode re-uploaded from the supply every record, but
        // since Plan 2 Task 4 residency is cached BY GUID and the supply is consulted
        // only on a MISS -- so harvest #2 hit harvest #1's buffers and drew mesh A's
        // geometry with mesh B's per-section index ranges (an out-of-bounds index
        // read whenever B had more indices), then wrote the picture to
        // Saved/Thumbnails/<B>.png. Every .arcmesh row in a project with two or more
        // imported meshes showed the same picture. Keying on the asset's own guid is
        // what makes each harvest's residency ITS OWN; the invalidate bracket in
        // Harvest is what keeps it from outliving the stash it is served from.
        Arcane::Guid     meshHarvestId;
        Arcane::MeshData meshHarvestGeometry;

        // Drop the in-flight harvest's geometry AND the vehicle residency that was
        // uploaded from it. Both halves, always together: the stash is freed so a
        // large imported mesh's CPU buffer does not linger for the session, and the
        // residency goes with it because a resident entry whose supply can no longer
        // answer is a guid the cache would serve stale (or, after an eviction, refuse
        // and memoize). The pre-fix code freed the stash alone, which is exactly that
        // hazard.
        void ReleaseMeshHarvest()
        {
            if (ctx && meshHarvestId.IsValid())
                ctx->InvalidateMeshGeometry(meshHarvestId);
            meshHarvestId = Arcane::Guid{};
            meshHarvestGeometry = {};
        }

        // THE QUEUE IS A STACK, and it is SHARED across both subjects (F2c Plan 2
        // Task 9): Task 10's Browse draw pushes every VISIBLE un-thumbed material or
        // mesh each frame, so the newest push is the one on screen right now -- LIFO
        // is what makes the frame's single harvest land on something the user is
        // looking at, REGARDLESS of which subject it is -- a second, subject-specific
        // queue would let a burst of one starve the other.
        std::deque<WorkKey> queue;
        std::unordered_map<WorkKey, Pending, WorkKeyHash> pending;
        std::vector<Ready> ready;                    // also LIFO (back = newest)
        // `entries`/`failed`/`thumbToMaterial` stay Guid-keyed, deliberately NOT
        // widened to WorkKey: ThumbTextureId/PixelsForThumb take a bare Guid with no
        // Subject to disambiguate, and a material Guid can never collide with a mesh
        // asset Guid (both are minted by the SAME project-wide registry), so keying
        // these three on the asset id alone is exact, not an approximation.
        std::unordered_set<Arcane::Guid> failed;     // memoized refusals
        std::unordered_map<Arcane::Guid, Entry> entries;
        std::unordered_map<Arcane::Guid, Arcane::Guid> thumbToMaterial;

        // `inFlight` IS THE ONE MEMBERSHIP TEST, and it spans all three
        // stages -- `queue`, `pending` and `ready` -- for BOTH subjects at once.
        // Keeping one set rather than probing three containers is what makes
        // Request()/RequestMesh()'s per-frame, per-visible-row call a single hash
        // lookup, and what makes "did this work item get dropped somewhere" a
        // single invariant instead of three. Every terminal outcome (a harvest, a
        // refusal) erases from it; nothing else does, except Invalidate/
        // InvalidateMesh, which re-arms the item.
        std::unordered_set<WorkKey, WorkKeyHash> inFlight;

        [[nodiscard]] Arcane::NriGraphContext* Chrome() const
        {
            return services.chromeGraph ? services.chromeGraph() : nullptr;
        }

        [[nodiscard]] std::filesystem::path ThumbPath(const Arcane::Guid& id) const
        {
            if (!services.thumbnailDir)
                return {};
            const std::filesystem::path dir = services.thumbnailDir();
            if (dir.empty())
                return {};
            // Keyed by the picture-changing settings: "<guid>.png" at the
            // defaults, another name otherwise (ThumbnailCacheSuffix).
            return dir / (id.ToString() + ThumbnailCacheSuffix(thumb) + ".png");
        }

        void Push(const WorkKey& key)
        {
            if (inFlight.insert(key).second)
                queue.push_front(key);   // LIFO
        }

        // What one Harvest call did. THREE outcomes, not two, because a
        // dropped VEHICLE and a refused MATERIAL must not be memoized the
        // same way: on a dropped vehicle the material is fine and the FRAME
        // was not, so it goes back on the queue to be re-resolved against the
        // replacement rather than being written off.
        enum class HarvestOutcome : std::uint8_t { Done, Refused, Retry };

        bool EnsureVehicle(Arcane::NriGraphContext& chrome);
        void ResetVehicle();                      // destroy it + re-queue what it scoped
        // The device table released bindless slot `slot` (a texture replaced or
        // evicted, a material save, a project switch): this harvester is a holder of
        // slot indices (meshMats' memo and the queued Ready records), so it resets
        // the memo (the next Request re-resolves) and re-queues any Ready that
        // captured the index -- never draws with a released index.
        void OnAlbedoSlotReleased(std::uint32_t slot);
        void DropVehicle();                       // ResetVehicle, as a FAILURE (spends budget)
        void GiveUp(std::string_view why);        // no previews at all, this session
        void StartOne(const WorkKey& key, double now);
        void StartOneMesh(const Arcane::Guid& mesh);   // F2c Plan 2 Task 9: the mesh-ASSET branch
        [[nodiscard]] static bool AllStagesLanded(const Pending& p);
        [[nodiscard]] bool Publish(Pending& p);   // pending -> ready; false = refused
        HarvestOutcome Harvest(Ready& r);         // ONE device idle
        std::uint64_t EnsureTexture(const Arcane::Guid& material, Entry& e) const;
        void ReleaseThumbTexture(Entry& e);       // the chrome-cache release sequence
        // BY VALUE, deliberately: every caller passes a key that lives INSIDE
        // the `pending` node this function erases.
        void Fail(WorkKey key, std::string_view why);

        // How many times the vehicle has been dropped this session. The budget
        // this feeds is what keeps DropVehicle's re-queue from becoming an
        // unbounded rebuild-render-fail loop -- see DropVehicle.
        int vehicleDrops = 0;
        // editor.thumbnail.*, LATCHED at construction: one session never mixes
        // two thumbnail sizes, and a change applies on the next start.
        EditorThumbnailSettings thumb;
        // Latched by GiveUp: this session renders no previews at all. Checked
        // at every entry point that would otherwise start work again, so a
        // write-off is genuinely terminal rather than something the next
        // Request() quietly revives.
        bool givenUp = false;
        // One-shot, for PrimeFromDisk's extension sniff -- a boot that hands this
        // class a guid resolving to neither .arcmesh nor .arcmat says it once and
        // then stops talking.
        bool warnedPrimeExtension = false;
    };

    // =====================================================================
    // Lifecycle
    // =====================================================================
    MaterialPreviewHarvester::MaterialPreviewHarvester(Services services)
        : m_impl(new Impl)
    {
        m_impl->services = std::move(services);
        m_impl->thumb = Arcane::Settings<EditorThumbnailSettings>();
    }

    MaterialPreviewHarvester::~MaterialPreviewHarvester()
    {
        // The vehicle BORROWS the chrome context's device, so it must not
        // outlive it. EditorApp::ShutdownGraphPath calls Shutdown() while
        // that device is provably alive; this is the backstop for a run that
        // never got that far (a failed boot), where nothing was ever created
        // in the first place.
        // The slot observer is removed before the vehicle dies (ResetVehicle).
        if (m_impl->ctx)
            m_impl->ResetVehicle();
        delete m_impl;
    }

    // ===== DESTROYING THE VEHICLE INVALIDATES `ready`, NOT JUST `ctx` =====
    // THE BUG THIS EXISTS TO PREVENT, because it is a SILENT one: a Ready
    // carries either `spriteMaterial` -- a slot index into the Batcher2D being
    // destroyed here -- or `meshSlot`, an index into the destroyed context's
    // bindless material table. Neither means anything to the replacement
    // EnsureVehicle builds on the next Pump, and NEITHER FAILS LOUDLY:
    // Batcher2D::QuadMaterial falls back to the plain sprite pipeline for an
    // unknown id with no warning at all. So a surviving Ready would harvest
    // SUCCESSFULLY and write an untextured quad -- or, once that slot has been
    // re-registered, ANOTHER MATERIAL'S PIXELS -- to <guid>.png, which
    // PrimeFromDisk then trusts on every later boot. Wrong pixels, persisted,
    // forever.
    //
    // They are RE-QUEUED rather than dropped: the material is fine, whatever
    // took the vehicle out was not, and re-resolving costs one JSON load plus
    // (for a sprite or a chain) a cache-hot recompile. `pending` is
    // deliberately left alone -- Publish registers into whatever `batch` is
    // current at the moment it runs, so an in-flight compile lands correctly
    // on the replacement vehicle.
    //
    // NO BUDGET IS SPENT HERE. This is the neutral "the vehicle is gone" half,
    // which teardown needs as much as a failed frame does; DropVehicle below
    // is the FAILURE flavour that also pays the budget.
    void MaterialPreviewHarvester::Impl::OnAlbedoSlotReleased(std::uint32_t slot)
    {
        if (meshMats)
            meshMats->ResetAlbedoSlot(slot);
        for (std::size_t i = 0; i < ready.size();)
        {
            const Ready& r = ready[i];
            bool stale = r.meshSlot == slot;
            for (const Arcane::MeshInstance& inst : r.meshInstances)
                stale = stale || inst.materialSlot == slot;
            if (!stale)
            {
                ++i;
                continue;
            }
            // Re-queue (the material is fine, the index it captured is not).
            const WorkKey key{ r.id, r.subject };
            inFlight.erase(key);
            ready.erase(ready.begin() + static_cast<std::ptrdiff_t>(i));
            Push(key);
        }
    }

    void MaterialPreviewHarvester::Impl::ResetVehicle()
    {
        // Stop listening BEFORE the context dies: its teardown releases the slots it
        // minted, which would call back into half-reset state.
        if (ctx)
            if (Arcane::BindlessTable* table = ctx->Device().BindlessIfCreated())
                table->RemoveObserver(this);
        ctx.reset();
        batch.reset();
        meshMats.reset();
        sphere = {};
        // The stash goes with the vehicle it was serving -- the replacement vehicle
        // has an empty cache, and the re-queued harvest re-resolves its own geometry.
        // No InvalidateMeshGeometry here (and none needed): `ctx` is already gone,
        // and its cache went with it.
        meshHarvestId = Arcane::Guid{};
        meshHarvestGeometry = {};
        for (const Ready& r : ready)
        {
            const WorkKey key{ r.id, r.subject };
            inFlight.erase(key);   // ...so Push's dedupe cannot swallow it
            Push(key);
        }
        ready.clear();
    }

    // THE FAILURE FLAVOUR, and the budget is what makes re-queueing safe at
    // all. Without it a vehicle that fails to render every frame would
    // rebuild, fail, re-queue and rebuild again forever, spending a frame's
    // worth of validation errors per frame for the rest of the session --
    // which is what dropping the vehicle ALONE never prevented, since the next
    // Pump rebuilds it immediately. After thumb.maxRetries the whole pipeline
    // is written off exactly the way a failed CreateOffscreen writes it off.
    void MaterialPreviewHarvester::Impl::DropVehicle()
    {
        ResetVehicle();
        if (++vehicleDrops >= thumb.maxRetries)
            GiveUp("the material-preview vehicle failed " +
                   std::to_string(vehicleDrops) + " times");
    }

    // ===== NO PREVIEWS AT ALL, FOR THE REST OF THE SESSION =====
    // The ONE write-off path, reached from the two places previews can stop
    // being possible: CreateOffscreen refusing outright, and the retry budget
    // above running out. Everything still in the pipeline is memoized as
    // failed so nothing re-attempts it, and `givenUp` is what makes that
    // terminal rather than something the next Request() quietly revives --
    // without it, a panel drawing rows would push work back in every frame and
    // the loop this budget exists to bound would simply resume.
    void MaterialPreviewHarvester::Impl::GiveUp(std::string_view why)
    {
        if (!givenUp)
            ARC_WARN("[thumbs] {} -- materials and meshes keep their kind icon for "
                     "the rest of this session", why);
        givenUp = true;
        for (const WorkKey& k : inFlight)
            failed.insert(k.id);
        for (const Ready& r : ready)
            failed.insert(r.id);
        queue.clear();
        pending.clear();
        ready.clear();
        inFlight.clear();
    }

    // ===== RELEASING ONE THUMBNAIL'S CHROME-CACHE TEXTURE =====
    // The exact sequence a re-harvest owes (NriGraphContext.hpp item (2) under
    // TWO CONTEXTS, TWO LANES), factored out because there are TWO callers
    // that owe it: the re-harvest in Harvest, and Clear() on a project switch.
    //
    // NriTextureCache::Invalidate destroys the texture, and NRI does not
    // ref-count -- so a later Resolve may land its replacement on the address
    // this one just vacated. ImGuiNri caches per texture by RAW POINTER, so a
    // bit-identical id would report a cache HIT on a descriptor set naming the
    // DESTROYED texture. The evict therefore runs BEFORE the destroy, and
    // unconditionally rather than "if the pointer changed" -- an unchanged
    // pointer is precisely the case a recycled address fakes.
    //
    // COST, stated because it is not free: InvalidateUserTextureNow idles the
    // whole device unconditionally (ImGuiNri.cpp's own note). Both call sites
    // are rare -- a material edit, a project switch -- and the `e.texture == 0`
    // guard means an entry nothing ever uploaded costs nothing at all.
    void MaterialPreviewHarvester::Impl::ReleaseThumbTexture(Entry& e)
    {
        if (e.texture == 0)
        {
            // Created and queued on the chrome cache but not uploaded yet (the texture
            // id is only handed out once it is ready): the queued copy holds the OLD
            // pixels, so drop it now or it would upload them over the new ones.
            if (e.thumbId.IsValid())
                if (Arcane::NriGraphContext* chrome = Chrome())
                    if (Arcane::NriTextureCache* cache = chrome->Textures())
                        if (cache->UploadQueued(e.thumbId, Arcane::NriTextureCache::ColorSpace::Display))
                            chrome->InvalidateContentTexture(e.thumbId);
            return;
        }
        if (Arcane::NriGraphContext* chrome = Chrome())
        {
            if (Arcane::RenderOverlayNode* hud = chrome->ImGuiHud())
                hud->InvalidateUserTextureNow(
                    reinterpret_cast<nri::Texture*>(static_cast<std::intptr_t>(e.texture)));
            chrome->InvalidateContentTexture(e.thumbId);
        }
        e.texture = 0;
    }

    void MaterialPreviewHarvester::Shutdown()
    {
        Impl& im = *m_impl;
        // Order matters only in one direction: the batcher is device-less and
        // the vehicle owns real NRI objects, so the vehicle goes while the
        // device it borrowed is still alive. Through ResetVehicle so the
        // vehicle-scoped `ready` ids cannot survive this either -- latent
        // today (nothing pumps after Shutdown) but the identical hazard, and a
        // second copy of that reasoning is how the two drift apart. NOT
        // DropVehicle: an orderly teardown is not a failure and must not spend
        // the retry budget (three project switches would otherwise write the
        // pipeline off on a perfectly healthy session).
        im.ResetVehicle();
        // The thumbnails themselves are CPU bytes and survive -- but their
        // nri::Texture* ids named textures the CHROME cache owns, and that
        // cache dies with the chrome context. Zeroed, NOT released: at this
        // point the chrome context is about to be destroyed, and
        // ~NriGraphContext's own ordering covers a texture and the ImGuiNri
        // that cached it when both belong to the SAME context (see
        // EditorApp's m_graphLogoTexture, which states the distinction).
        // Paying N device idles on the way out would buy nothing.
        for (auto& [id, e] : im.entries)
            e.texture = 0;
    }

    void MaterialPreviewHarvester::Clear()
    {
        Impl& im = *m_impl;
        // ===== RELEASE FIRST, WHILE THE CHROME CONTEXT IS STILL ALIVE =====
        // A project switch KEEPS the chrome context and its NriTextureCache by
        // design (EditorApp::TeardownGraphForSwitch), so every synthetic
        // thumbnail guid stays Resident in it -- and the incoming project
        // mints fresh Guid::Generate() ids, so nothing can ever address the
        // old ones again. Erasing the maps without this loop leaks one 64x64
        // texture plus one pointer-keyed ImGuiNri entry per thumbnailed
        // material, per switch, for the life of the process.
        //
        // BEFORE Shutdown(), which zeroes e.texture (its own comment says why
        // that is right at exit) and would otherwise make every release below
        // a silent no-op.
        for (auto& [id, e] : im.entries)
            im.ReleaseThumbTexture(e);

        // A Guid means something else in a different project, and the
        // batcher's registered material slots are the OLD project's, so the
        // vehicle goes with everything else. The next Pump rebuilds it.
        Shutdown();
        im.queue.clear();
        im.inFlight.clear();
        im.pending.clear();
        im.ready.clear();
        im.failed.clear();
        im.entries.clear();
        im.thumbToMaterial.clear();
        // A fresh project gets a fresh budget AND a fresh write-off: the
        // outgoing project's failures say nothing about this one's, and a
        // switch rebuilds the render bridge anyway.
        im.vehicleDrops = 0;
        im.givenUp = false;
    }

    // =====================================================================
    // The queue
    // =====================================================================
    void MaterialPreviewHarvester::Request(const Arcane::Guid& material)
    {
        Impl& im = *m_impl;
        if (!material.IsValid() || im.givenUp)
            return;
        // The steady-state cost of the Browse draw's per-frame push for every
        // visible row: three hash lookups and out.
        if (im.entries.contains(material) || im.failed.contains(material) ||
            im.inFlight.contains(material))
            return;
        im.Push(material);
    }

    void MaterialPreviewHarvester::Invalidate(const Arcane::Guid& material)
    {
        Impl& im = *m_impl;
        if (!material.IsValid())
            return;
        // The EXISTING thumbnail stays served until the new one lands
        // (last-good -- the same rule SpriteMaterialCache::Invalidate keeps),
        // so `entries` is deliberately NOT erased here. What IS dropped is
        // every memo that would stop a re-harvest: the refusal memo, any
        // half-finished compile, and the on-disk PNG's claim (the harvest
        // overwrites it, but a crash between here and then must not leave a
        // stale PNG that PrimeFromDisk would trust on the next boot -- the
        // .arcmat mtime comparison would catch a disk edit, but not a cook or
        // a parent-chain change, which touch this material's file not at all).
        im.failed.erase(material);
        im.pending.erase(material);
        // id AND subject, matching InvalidateMesh below exactly. Erasing by id alone
        // is exact today (one flat guid map project-wide, so a material guid cannot
        // also be a mesh guid) but the asymmetry invited a reader to assume one of
        // the two was wrong.
        im.ready.erase(std::remove_if(im.ready.begin(), im.ready.end(),
                                      [&](const Impl::Ready& r)
                                      { return r.id == material && r.subject == Subject::Material; }),
                       im.ready.end());
        im.queue.erase(std::remove(im.queue.begin(), im.queue.end(), material), im.queue.end());
        // AFTER the three erases above, never before: `inFlight` is what Push
        // dedupes against, so a material already somewhere in the pipeline
        // with now-STALE compile jobs or stale bytes would otherwise be
        // silently dropped rather than restarted.
        im.inFlight.erase(material);
        if (const std::filesystem::path png = im.ThumbPath(material); !png.empty())
        {
            std::error_code ec;
            std::filesystem::remove(png, ec);
        }
        // The PNG goes either way -- it IS stale, and a session that has given
        // up on previews should still not leave a wrong picture on disk for
        // the NEXT session (which starts with a fresh vehicle) to trust.
        // Re-queueing is what a written-off session skips.
        if (!im.givenUp)
            im.Push(material);
    }

    // F2c Plan 2 Task 9: mirrors Request() exactly, EXCEPT it tags the pushed work
    // item Subject::Mesh so StartOne dispatches it through ResolveMeshData instead
    // of the compile path. `entries`/`failed` are checked bare-Guid (shared with the
    // material half, see WorkKey's own comment on why that is exact, not sloppy);
    // `inFlight` is checked with the Mesh tag, since that IS the container where the
    // two subjects genuinely need telling apart.
    void MaterialPreviewHarvester::RequestMesh(const Arcane::Guid& mesh)
    {
        Impl& im = *m_impl;
        if (!mesh.IsValid() || im.givenUp)
            return;
        if (im.entries.contains(mesh) || im.failed.contains(mesh) ||
            im.inFlight.contains(WorkKey{ mesh, Subject::Mesh }))
            return;
        im.Push(WorkKey{ mesh, Subject::Mesh });
    }

    // Mirrors Invalidate() exactly, tagged Mesh throughout -- see that function's
    // own comment for the full last-good/re-queue reasoning, which applies here
    // unchanged.
    void MaterialPreviewHarvester::InvalidateMesh(const Arcane::Guid& mesh)
    {
        Impl& im = *m_impl;
        if (!mesh.IsValid())
            return;
        const WorkKey key{ mesh, Subject::Mesh };
        im.failed.erase(mesh);
        im.pending.erase(key);
        im.ready.erase(std::remove_if(im.ready.begin(), im.ready.end(),
                                      [&](const Impl::Ready& r)
                                      { return r.id == mesh && r.subject == Subject::Mesh; }),
                       im.ready.end());
        im.queue.erase(std::remove(im.queue.begin(), im.queue.end(), key), im.queue.end());
        im.inFlight.erase(key);
        // ...AND THE VEHICLE'S RESIDENCY FOR THIS GUID (final-review C1). Since the
        // harvest keys residency on the asset's own guid, a re-harvest after an edit
        // or a cook would otherwise HIT the buffers uploaded from the PRE-edit
        // geometry. Harvest's own pre-render invalidate already covers the ordinary
        // path; this one covers the window before the re-harvest runs, and costs
        // nothing when the guid is not resident.
        if (im.ctx && mesh.IsValid())
            im.ctx->InvalidateMeshGeometry(mesh);
        if (im.meshHarvestId == mesh)
            im.ReleaseMeshHarvest();
        if (const std::filesystem::path png = im.ThumbPath(mesh); !png.empty())
        {
            std::error_code ec;
            std::filesystem::remove(png, ec);
        }
        if (!im.givenUp)
            im.Push(key);
    }

    std::size_t MaterialPreviewHarvester::PendingCount() const
    {
        const Impl& im = *m_impl;
        return im.queue.size() + im.pending.size() + im.ready.size();
    }

    // =====================================================================
    // Persistence -- the load half
    // =====================================================================
    void MaterialPreviewHarvester::PrimeFromDisk(const std::vector<Arcane::Guid>& materials)
    {
        Impl& im = *m_impl;
        if (!im.services.resolveAsset)
            return;

        std::size_t loaded = 0, staleOrMissing = 0;
        for (const Arcane::Guid& id : materials)
        {
            if (!id.IsValid())
                continue;
            // Resolved ONCE per guid, up front -- both the staleness check
            // below and (F2c Plan 2 Task 10) the stale/missing dispatch at
            // the bottom of this loop need it, and it is one cheap
            // registry lookup either way.
            const auto src = im.services.resolveAsset(id);
            const std::filesystem::path png = im.ThumbPath(id);
            bool usable = !png.empty();
            if (usable)
            {
                std::error_code ec;
                const auto pngTime = std::filesystem::last_write_time(png, ec);
                if (ec)
                {
                    usable = false;   // no PNG at all
                }
                else if (src)
                {
                    std::error_code srcEc;
                    const auto srcTime = std::filesystem::last_write_time(*src, srcEc);
                    // A source (.arcmat OR .arcmesh) NEWER than its PNG means
                    // the thumbnail is of an asset that no longer exists in
                    // that shape -- a slot reassignment moves an .arcmesh's
                    // OWN mtime exactly like an edit moves a material's, so
                    // this same comparison covers both without asking which
                    // kind `id` is. An unreadable source is not a reason to
                    // throw a good PNG away.
                    if (!srcEc && srcTime > pngTime)
                        usable = false;
                }
            }

            if (usable)
            {
                Impl::Entry e;
                // maxSize thumb.size -- the loader's own cap, matching what
                // the harvest wrote, so a hand-dropped oversized PNG still lands
                // as a 64px thumbnail rather than a surprise upload.
                if (Arcane::LoadDisplayPixels(png, im.thumb.size, e.pixels) && e.pixels.Valid())
                {
                    e.thumbId = Arcane::Guid::Generate();
                    im.thumbToMaterial.emplace(e.thumbId, id);
                    im.entries.emplace(id, std::move(e));
                    ++loaded;
                    continue;   // NOT queued: this is the whole point
                }
                usable = false;   // decode failed -- fall through and re-harvest
            }
            ++staleOrMissing;
            // F2c Plan 2 Task 10: `materials` now carries the project's mesh
            // guids too (EditorApp's one call site widens its filter), so a
            // stale/missing entry has to dispatch through the RIGHT subject
            // rather than always widening to Subject::Material the way a bare
            // Guid implicitly would (WorkKey's own comment). The resolved
            // source's extension is the answer -- Subject is this .cpp's own
            // private dispatch tag, so asking the caller to carry it alongside
            // each Guid would just duplicate what the extension already says.
            //
            // AN UNRESOLVABLE OR UNRECOGNISED PATH FALLS BACK TO Material, and now
            // SAYS SO ONCE. The fallback itself is right -- Material is the only
            // subject that can be attempted blind, and the attempt costs one
            // Fail("not in the asset registry") -- but doing it in silence turned a
            // widened-filter mistake at the one call site into a thumbnail that
            // simply never appears. Latched, the house idiom: one line names the
            // extension, the rest of the boot is quiet.
            bool isMesh = false;
            std::string ext;
            if (src)
            {
                ext = src->extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                isMesh = (ext == ".arcmesh");
            }
            if (!isMesh && ext != ".arcmat" && !im.warnedPrimeExtension)
            {
                im.warnedPrimeExtension = true;
                ARC_WARN("[thumbs] PrimeFromDisk: {} resolves to {} -- neither .arcmesh nor "
                         ".arcmat, so it is dispatched as a MATERIAL and will most likely be "
                         "refused. Further occurrences are silent.",
                         id.ToString(), src ? src->string() : std::string("nothing"));
            }
            im.Push(isMesh ? WorkKey{ id, Subject::Mesh } : WorkKey{ id });
        }
        ARC_INFO("[thumbs] previews: {} loaded from disk, {} queued for harvest",
                 loaded, staleOrMissing);
    }

    // =====================================================================
    // The compile half
    // =====================================================================
    void MaterialPreviewHarvester::Impl::Fail(WorkKey key, std::string_view why)
    {
        ARC_WARN("[thumbs] no preview for {} {} -- {}",
                 key.subject == Subject::Mesh ? "mesh" : "material", key.id.ToString(), why);
        failed.insert(key.id);
        pending.erase(key);
        inFlight.erase(key);
    }

    void MaterialPreviewHarvester::Impl::StartOne(const WorkKey& key, double now)
    {
        // Subject::Mesh has its own branch, entirely separate from the compile
        // pipeline below (a mesh asset compiles nothing -- this class's file header
        // states that as the reason it needs no new stage-key slot).
        if (key.subject == Subject::Mesh)
            return StartOneMesh(key.id);

        const Arcane::Guid& id = key.id;
        if (!services.resolveAsset)
            return Fail(id, "no asset resolver");
        const auto path = services.resolveAsset(id);
        if (!path)
            return Fail(id, "not in the asset registry");
        auto data = Arcane::LoadMaterialAsset(*path);
        if (!data)
            return Fail(id, "asset failed to load");

        // The parent chain, through the SHARED cycle-guarded walker every
        // material cache in the tree uses.
        std::vector<Arcane::MaterialAssetData> chain;
        if (const auto why =
                Arcane::LoadMaterialParentChain(services.resolveAsset, id, data->parent, chain))
            return Fail(id, *why);

        // The BASE's kind, not the leaf's -- an instance carries no kind of
        // its own (the same rule all three engine caches state).
        const Arcane::MaterialAssetData& base = chain.empty() ? *data : chain.back();
        const Arcane::MaterialSurface surface = Arcane::MaterialSurfaceForKind(base.kind);

        // ---- MESH: nothing to compile ----------------------------------
        // A "mesh"-kind .arcmat has no snippet and no template at all
        // (MeshMaterialCache.hpp's own opening), so it resolves
        // SYNCHRONOUSLY into a colour + an optional albedo slot and goes
        // straight to the render queue.
        if (surface == Arcane::MaterialSurface::Mesh)
        {
            // Pump guarantees the vehicle exists before it calls this (the
            // cache is created alongside it), so this is a defensive read
            // rather than a state the queue can sit in.
            if (!meshMats)
                return Fail(id, "the preview vehicle is not available");
            meshMats->Request(id);
            const auto& table = meshMats->Table();
            const auto it = table.find(id);
            if (it == table.end())
                return Fail(id, "mesh material did not resolve");
            Ready r;
            r.id = id;
            r.surface = surface;
            r.meshColor = it->second.baseColor;
            r.meshSlot = it->second.materialSlot;
            ready.push_back(std::move(r));
            return;
        }

        if (!services.compiler || !services.compiler->IsAvailable() || !services.sources)
            return Fail(id, "no shader compiler (dxcompiler.dll) -- material previews need one");

        const auto templateText = services.sources->Get(Arcane::MaterialTemplateFile(surface));
        if (!templateText)
            return Fail(id, "engine material template not found");

        Pending p;
        p.id = id;
        p.surface = surface;

        if (surface == Arcane::MaterialSurface::Sprite)
        {
            Arcane::MaterialBuildResult build = Arcane::BuildMaterialShaderSource(
                *templateText, chain.empty() ? data->snippet : chain.back().snippet,
                data->name, Arcane::MaterialSurface::Sprite);
            if (!build.errors.empty())
                return Fail(id, "material source errors: " + build.errors.front());

            p.templ = std::make_shared<Arcane::MaterialTemplate>(std::move(build.templ));
            p.jobs.resize(1);
            Arcane::ShaderCompileRequest req;
            req.debugName = data->name + ".thumb.sprite.hlsl";
            req.sourceUtf8 = build.hlsl;
            req.entry = Arcane::kPsEntry;
            req.profile = Arcane::kPsProfile;
            req.coalesceKey = ThumbStageKey(id, /*vertex=*/false);
            p.jobs[0].psJob = services.compiler->Submit(req, now);
            req.entry = Arcane::kVsEntry;
            req.profile = Arcane::kVsProfile;
            req.coalesceKey = ThumbStageKey(id, /*vertex=*/true);
            p.jobs[0].vsJob = services.compiler->Submit(std::move(req), now);
        }
        else   // Fullscreen -- the material's OWN pass chain, exactly as the
        {      // shader editor previews it (kSceneInput = the checkerboard).
            std::vector<Arcane::MaterialChainPassDesc> descs;
            descs.reserve(1 + base.passes.size());
            descs.push_back({ base.snippet, base.baseInputs });
            for (const Arcane::MaterialPass& pass : base.passes)
                descs.push_back({ pass.snippet, pass.inputs });

            Arcane::MaterialChainBuildResult build = Arcane::BuildMaterialChainSource(
                *templateText, descs, data->name, base.vertexSnippet, /*externalInput=*/true);
            if (!build.Ok())
            {
                for (const auto& pe : build.passErrors)
                    if (!pe.empty())
                        return Fail(id, "material source errors: " + pe.front());
                return Fail(id, "material source errors: " + build.errors.front());
            }

            p.templ = std::make_shared<Arcane::MaterialTemplate>(std::move(build.templ));
            p.passInputs = std::move(build.passInputs);
            p.chainInputSlots = build.chainInputSlots;
            p.jobs.resize(build.hlsl.size());
            for (std::size_t i = 0; i < build.hlsl.size(); ++i)
            {
                Arcane::ShaderCompileRequest req;
                req.debugName = data->name + ".thumb.post.p" + std::to_string(i) + ".hlsl";
                req.sourceUtf8 = build.hlsl[i];
                req.entry = Arcane::kPsEntry;
                req.profile = Arcane::kPsProfile;
                req.coalesceKey = ThumbStageKey(id, /*vertex=*/false, i);
                p.jobs[i].psJob = services.compiler->Submit(req, now);
                req.entry = Arcane::kVsEntry;
                req.profile = Arcane::kVsProfile;
                req.coalesceKey = ThumbStageKey(id, /*vertex=*/true, i);
                p.jobs[i].vsJob = services.compiler->Submit(std::move(req), now);
            }
        }

        p.data = std::move(*data);
        p.parentChain = std::move(chain);
        pending.emplace(id, std::move(p));
    }

    // ===== F2c Plan 2 Task 9: THE MESH-ASSET BRANCH =========================
    // Resolves SYNCHRONOUSLY, like the mesh-kind MATERIAL branch just above (a mesh
    // asset compiles nothing either) -- straight from `queue` to `ready`, with no
    // `pending` stage at all. PER SECTION, the same material chain
    // GpuSceneSync resolves for a scene's MeshRenderer
    // (Render/GpuSceneSync.hpp, ResolveRowMaterial): slots[section.slotIndex]'s material -> the
    // resolved baseColor/materialSlot, or white when the slot is empty/unresolved.
    // There is no materialOverride here -- a thumbnail has no entity, only the
    // asset's own default slots.
    void MaterialPreviewHarvester::Impl::StartOneMesh(const Arcane::Guid& id)
    {
        const WorkKey key{ id, Subject::Mesh };
        if (!services.resolveAsset)
            return Fail(key, "no asset resolver");
        const auto path = services.resolveAsset(id);
        if (!path)
            return Fail(key, "not in the asset registry");
        const auto data = Arcane::LoadMeshAsset(*path);
        if (!data)
            return Fail(key, "asset failed to load");

        // THE ONE entry point a host resolves a .arcmesh through (Mesh/MeshAsset.hpp's
        // own comment on ResolveMeshData): a generated source resolves synchronously
        // regardless of the seams below; an Imported source consults them and may
        // answer PendingCook -- s7.1's quiet-not-a-failure outcome, treated here the
        // same way a broken/missing artifact is (Fail, memoized), since this class has
        // no per-frame poll of its own the way MeshCache::Request's per-frame sweep
        // does -- Task 10's invalidation-on-cook-completion is what re-arms it.
        const Arcane::MeshResolveResult resolved =
            Arcane::ResolveMeshData(*data, services.meshArtifactFor, services.cookPending);
        if (resolved.state == Arcane::MeshResolveState::PendingCook)
            return Fail(key, "the mesh's cook has not landed yet");
        if (resolved.state == Arcane::MeshResolveState::Failed)
            return Fail(key, resolved.reason.empty() ? "mesh failed to build" : resolved.reason);
        if (!resolved.mesh || resolved.mesh->sections.empty())
            return Fail(key, "mesh has no geometry");

        // Pump guarantees the vehicle exists before it calls this (meshMats is
        // created alongside it) -- the same defensive read the mesh-kind MATERIAL
        // branch above takes, for the identical reason.
        if (!meshMats)
            return Fail(key, "the preview vehicle is not available");

        Ready r;
        r.id          = id;
        r.subject     = Subject::Mesh;
        r.meshBounds  = resolved.bounds;
        r.meshGeometry = std::move(*resolved.mesh);
        r.meshInstances.reserve(r.meshGeometry.sections.size());
        for (const Arcane::MeshSection& section : r.meshGeometry.sections)
        {
            Arcane::Guid slotMat{};
            if (section.slotIndex < data->slots.size())
                slotMat = data->slots[section.slotIndex].material;

            glm::vec4 baseColor(1.0f);
            std::uint32_t materialSlot = 0xFFFFFFFFu;
            if (slotMat.IsValid())
            {
                meshMats->Request(slotMat);
                const auto& table = meshMats->Table();
                if (const auto it = table.find(slotMat); it != table.end())
                {
                    baseColor = it->second.baseColor;
                    materialSlot = it->second.materialSlot;
                }
            }

            Arcane::MeshInstance inst;
            inst.baseColor    = baseColor;
            inst.materialSlot = materialSlot;
            inst.indexOffset  = section.indexOffset;
            inst.indexCount   = section.indexCount;
            r.meshInstances.push_back(inst);
            r.meshInstanceMaterials.push_back(slotMat);
        }
        ready.push_back(std::move(r));
    }

    void MaterialPreviewHarvester::OfferCompileResult(const Arcane::ShaderCompileResult& result)
    {
        // NEVER reports a consume: this is a passive observer BESIDE the
        // scene caches on the process's one drain site, not a competitor for
        // their results (its own jobs carry its own coalesce keys, so a
        // result can only ever match one of the two).
        Impl& im = *m_impl;
        for (auto it = im.pending.begin(); it != im.pending.end(); ++it)
        {
            Impl::Pending& p = it->second;
            for (Impl::PassJob& job : p.jobs)
            {
                const bool isVs = result.jobId == job.vsJob && job.vsJob != 0;
                const bool isPs = result.jobId == job.psJob && job.psJob != 0;
                if (!isVs && !isPs)
                    continue;

                const auto& target = im.services.backend == Arcane::GraphicsBackend::Vulkan
                                         ? result.spirv : result.dxil;
                if (!target.succeeded)
                {
                    // No last-good subtlety here, unlike the scene caches: a
                    // thumbnail that cannot compile simply has no picture, and
                    // whatever was already harvested stays in `entries`.
                    im.Fail(p.id, "shader compile failed");
                    return;   // `it` is dangling now -- Fail erased it
                }
                (isVs ? job.vsBytes : job.psBytes) = target.bytecode;
                if (!Impl::AllStagesLanded(p))
                    return;   // the other stages are still in flight
                // Publish must NOT erase the pending node itself (this scope
                // still holds `it` and `p` into it), so it reports a refusal
                // rather than calling Fail -- and the erase order below is
                // then unambiguous: read everything out of `p`, THEN drop it.
                const bool ok = im.Publish(p);
                const Arcane::Guid id = p.id;
                im.pending.erase(it);
                if (!ok)
                    im.Fail(id, "the preview batcher refused the material");
                return;
            }
        }
    }

    bool MaterialPreviewHarvester::Impl::AllStagesLanded(const Pending& p)
    {
        for (const PassJob& j : p.jobs)
            if (j.vsBytes.empty() || j.psBytes.empty())
                return false;
        return true;
    }

    bool MaterialPreviewHarvester::Impl::Publish(Pending& p)
    {
        Ready r;
        r.id = p.id;
        r.surface = p.surface;
        auto inst = LayerParams(p.templ, p.parentChain, p.data);

        if (p.surface == Arcane::MaterialSurface::Sprite)
        {
            if (!batch)
                return false;   // vehicle went away mid-flight
            Arcane::Material2DDesc desc;
            desc.templ = p.templ;
            desc.instance = inst;
            desc.vsBytes = std::make_shared<const std::vector<std::uint8_t>>(
                std::move(p.jobs[0].vsBytes));
            desc.psBytes = std::make_shared<const std::vector<std::uint8_t>>(
                std::move(p.jobs[0].psBytes));
            // Registered into THIS object's own batcher, never the editor's
            // scene batcher: two owners of one recorder is how two frames'
            // content merges into one (ShaderEditorDocument's preview states
            // the same rule for the same reason). Slots are never reclaimed,
            // and are bounded by "materials thumbed in one session".
            r.spriteMaterial = batch->RegisterMaterial(std::move(desc));
            if (r.spriteMaterial == Arcane::Batcher2D::kInvalidMaterialId)
                return false;
        }
        else
        {
            std::vector<Arcane::PostChainPassDesc> passes;
            passes.reserve(p.jobs.size());
            for (std::size_t i = 0; i < p.jobs.size(); ++i)
            {
                Arcane::PostChainPassDesc bytes;
                bytes.vsBytes = std::make_shared<const std::vector<std::uint8_t>>(
                    std::move(p.jobs[i].vsBytes));
                bytes.psBytes = std::make_shared<const std::vector<std::uint8_t>>(
                    std::move(p.jobs[i].psBytes));
                bytes.inputs = p.passInputs[i];
                passes.push_back(std::move(bytes));
            }
            r.post.templ = p.templ;
            r.post.instance = inst;
            r.post.chainInputSlots = p.chainInputSlots;
            r.post.passes = std::move(passes);
        }
        ready.push_back(std::move(r));
        return true;
    }

    // =====================================================================
    // The render half
    // =====================================================================
    bool MaterialPreviewHarvester::Impl::EnsureVehicle(Arcane::NriGraphContext& chrome)
    {
        if (ctx)
            return true;
        if (givenUp || !services.hostConfig)
            return false;

        // No NodeSet argument: CreateOffscreen's own `const NodeSet& nodes = {}`
        // default is what we want -- hostHud/pickOutline/gameUi all false, so
        // the vehicle is batch + post + tonemap + mesh and nothing else. A
        // thumbnail has no host chrome, no game HUD and nothing to pick.
        // (This comment used to name a `NodeSet{}` argument the call has never
        // passed; the effective node set is the same either way.)
        ctx = Arcane::NriGraphContext::CreateOffscreen(*services.hostConfig, chrome.Device(),
                                                       thumb.size, thumb.size);
        if (!ctx)
        {
            // Degraded, not fatal, and it degrades to what a missing device
            // already degrades to: kind icons instead of pictures. The refusal
            // is already logged + latched inside CreateOffscreen. Every queued
            // material is dropped so this cannot re-attempt every frame.
            // Through the ONE write-off path, so this cannot be re-attempted
            // (or re-logged) on every single frame. The refusal itself is
            // already logged + latched inside CreateOffscreen.
            GiveUp("the material-preview context could not be created");
            return false;
        }

        // The two injected seams, the same pair EditorApp::CreateGraphVehicles
        // and ShaderEditorDocument::EnsureGraphPreviewContext install: a
        // material's texture params are Guids and THIS device has to resolve
        // them itself.
        if (services.resolveAsset)
            ctx->SetAssetResolver(services.resolveAsset);
        if (services.pixelSupply)
            ctx->SetPixelSupply(services.pixelSupply);
        ctx->SetMeshSupply(
            [this](const Arcane::Guid& id) -> Arcane::NriMeshBufferCache::SupplyResult
            {
                if (id == kMaterialPreviewSphereId && !sphere.vertices.empty())
                    return { &sphere, Arcane::MeshResolveState::Ready };
                // F2c Plan 2 Task 9: the mesh-ASSET harvest's own geometry, resolved by
                // StartOneMesh and stashed on `this` (Harvest, immediately before
                // RenderFrameOffscreen) under the ASSET'S OWN GUID -- final-review C1.
                // Exactly one asset is ever in flight, so this answers for exactly one
                // guid at a time, and Harvest's invalidate bracket is what keeps the
                // cache from holding a guid this lambda can no longer answer for.
                if (id == meshHarvestId && !meshHarvestGeometry.vertices.empty())
                    return { &meshHarvestGeometry, Arcane::MeshResolveState::Ready };
                return { nullptr, Arcane::MeshResolveState::Failed };
            });

        batch = Arcane::Batcher2D::Create();
        if (!batch)
        {
            ARC_WARN("[thumbs] the material-preview batcher could not be created");
            ctx.reset();
            return false;
        }

        // The mesh half: one sphere for the session (the mocks' sphere), and
        // the params-only material cache that colours it.
        sphere = BuildMaterialPreviewSphere();
        Arcane::MeshMaterialCache::Services ms;
        ms.resolveAsset = services.resolveAsset;
        ms.resolveAlbedoSlot = [this](const Arcane::Guid& g) -> std::uint32_t
        {
            // The DEVICE's shared bindless table (one index space for every context
            // on the device): the slot is valid until the table releases it, at
            // which point OnAlbedoSlotReleased resets this cache's memo.
            return ctx ? ctx->ResolveMeshAlbedoSlot(g) : 0xFFFFFFFFu;
        };
        meshMats = std::make_unique<Arcane::MeshMaterialCache>(std::move(ms));
        // Every holder of a device slot index is a released-slot observer: the
        // device's mapping memo is shared by every context, so a slot this cache
        // resolved can be released by the viewport's cook or material save.
        if (Arcane::BindlessTable* table = ctx->Device().Bindless())
            table->SetObserver(this, [this](std::uint32_t slot) { OnAlbedoSlotReleased(slot); });
        return true;
    }

    MaterialPreviewHarvester::Impl::HarvestOutcome
    MaterialPreviewHarvester::Impl::Harvest(Ready& r)
    {
        // Pump has already made both of these true (it creates the vehicle
        // before it dequeues anything); re-read rather than assume. Retry, not
        // Refused: a vanished vehicle says nothing about this material.
        Arcane::NriGraphContext* chrome = Chrome();
        if (!chrome || !ctx || !batch)
            return HarvestOutcome::Retry;

        Arcane::GlobalParams globals;
        globals.time = thumb.time;
        globals.deltaTime = 0.0f;
        globals.viewportWidth  = static_cast<float>(thumb.size);
        globals.viewportHeight = static_cast<float>(thumb.size);

        // ===== THE PICTURE, PER SURFACE ==================================
        // The backdrop is the shader editor preview's checkerboard on every
        // surface -- it is what makes an alpha-cutout sprite material read as
        // a shape rather than as a hole, and on the FULLSCREEN arm it is
        // additionally what kSceneInput samples (one texture, same picture --
        // ShaderEditorDocument::RenderGraphPreview's own reasoning).
        //
        //   Sprite      -> the compiled material on a centred quad.
        //   Fullscreen  -> the material's whole pass chain over the canvas.
        //   Mesh        -> a lit UV sphere in the material's resolved
        //                  baseColor. THE REAL MESH PATH, not the brief's
        //                  sanctioned "same quad" fallback: MeshBuilder::
        //                  BuildUvSphere supplies the geometry, MeshNode is
        //                  built EAGERLY on every vehicle (it is not behind a
        //                  NodeSet flag, and a context whose MeshNode failed
        //                  to create does not exist at all), and
        //                  SceneCamera::PerspectiveProjection supplies the
        //                  lens -- so "trivially reachable" was true, and the
        //                  mocks show spheres.
        Arcane::Batcher2D& b = *batch;

        // The picture's 2D half, as a lambda because a frame whose uploads were still
        // in flight is RENDERED AGAIN (below): RenderFrameOffscreen drains the batcher,
        // so every pass re-fills it.
        const auto fillBatch = [&]()
        {
        b.Begin(thumb.size, thumb.size);
        b.SetGlobals(globals);   // AFTER Begin -- matching every other call site

        const float extent = static_cast<float>(thumb.size);
        const ShaderEditorSettings& preview = Arcane::Settings<ShaderEditorSettings>();   // editor.shader.previewChecker*
        const float cell = ThumbnailCheckerCell(thumb);
        const glm::vec4 light(preview.previewCheckerLight.r, preview.previewCheckerLight.g,
                              preview.previewCheckerLight.b, preview.previewCheckerLight.a);
        for (int y = 0; y * cell < extent; ++y)
            for (int x = 0; x * cell < extent; ++x)
                if ((x + y) & 1)
                    b.Rect(glm::vec2(x * cell, y * cell),
                           glm::vec2(cell, cell), light);

        if (r.subject == Subject::Material && r.surface == Arcane::MaterialSurface::Sprite &&
            r.spriteMaterial != Arcane::Batcher2D::kInvalidMaterialId)
        {
            const float s = preview.previewCheckerSpriteScale * extent;
            b.QuadMaterial(r.spriteMaterial,
                           glm::vec2((extent - s) * 0.5f, (extent - s) * 0.5f),
                           glm::vec2(s, s),
                           glm::vec2(0.0f), glm::vec2(1.0f), glm::vec4(1.0f));
        }
        };   // fillBatch
        fillBatch();

        Arcane::NriGraphContext::FrameDesc vp;
        vp.batch   = &b;
        vp.globals = &globals;
        vp.capture = true;   // the readback node -- the whole point of the frame

        std::vector<Arcane::MeshInstance> instances;
        Arcane::MeshSceneDesc meshScene;
        if (r.subject == Subject::Material && r.surface == Arcane::MaterialSurface::Fullscreen &&
            !r.post.passes.empty())
        {
            vp.post = &r.post;
        }
        else if (r.subject == Subject::Material && r.surface == Arcane::MaterialSurface::Mesh &&
                 !sphere.vertices.empty())
        {
            // The shared sphere scene (Documents/MaterialSpherePreview.hpp): the
            // document's live preview draws the same picture from live params.
            instances.push_back(MaterialPreviewSphereInstance(r.meshColor, r.meshSlot));
            meshScene = MaterialPreviewSphereScene(instances);
            vp.mesh = &meshScene;
        }
        // ===== F2c Plan 2 Task 9: A MESH ASSET, FRAMED BY ITS OWN AABB =====
        // Unlike the mesh-kind MATERIAL branch above (a fixed sphere, a fixed
        // camera), the geometry and the framing both vary per asset -- FrameMeshBounds
        // (MeshImportWave.hpp) is the pure half of that, taking only the resolved
        // bounds and a FOV. Same 35-degree FOV the sphere camera uses, so a material
        // thumbnail and a mesh thumbnail read as one family at 64px.
        else if (r.subject == Subject::Mesh && !r.meshGeometry.vertices.empty() &&
                 !r.meshInstances.empty())
        {
            // Stashed on `this` for SetMeshSupply's lambda (installed once, in
            // EnsureVehicle) to find when RenderFrameOffscreen below resolves this
            // asset's guid -- there is only one harvest per Pump, so nothing else
            // reads this between the assignment and the render call.
            meshHarvestId       = r.id;
            meshHarvestGeometry = std::move(r.meshGeometry);
            // AND DROP ANY RESIDENCY THIS GUID ALREADY HAS, before the render rather
            // than after (final-review C1). The vehicle outlives every harvest, so a
            // re-harvest of the same asset -- which is precisely what InvalidateMesh
            // queues after an edit or a cook -- would otherwise HIT the buffers built
            // from the PREVIOUS version of that asset and redraw the old shape with
            // the new section ranges.
            ctx->InvalidateMeshGeometry(meshHarvestId);

            const MeshThumbCamera cam = FrameMeshBounds(r.meshBounds, thumb.meshFovDegrees, thumb.framingMargin);

            instances = r.meshInstances;   // per-section baseColor/materialSlot/range,
                                            // resolved by StartOneMesh; mesh/model are
                                            // the same for every section of one asset.
            for (Arcane::MeshInstance& inst : instances)
            {
                inst.mesh  = meshHarvestId;     // the ASSET's own guid -- must match
                                                 // SetMeshSupply above (final-review C1)
                inst.model = glm::mat4(1.0f);   // unit geometry -- MeshAsset.hpp's UNIT RULE
            }

            meshScene.instances = instances;
            meshScene.view = glm::lookAtRH(cam.eye, cam.target, glm::vec3(0.0f, 1.0f, 0.0f));
            meshScene.projection =
                Arcane::PerspectiveProjection(thumb.meshFovDegrees, 1.0f, cam.nearZ, cam.farZ);
            // The SAME light as the mesh-kind material's sphere -- the one preview
            // light, editor.preview.light.* (R1) -- so every mesh thumbnail,
            // material or asset, has one lighting feel.
            const EditorPreviewLightSettings& l = Arcane::Settings<EditorPreviewLightSettings>();
            meshScene.lightDirection = glm::vec3(l.direction.x, l.direction.y, l.direction.z);   // TOWARD the light
            meshScene.lightColor = glm::vec3(l.color.r, l.color.g, l.color.b);
            meshScene.ambient = glm::vec3(l.ambient);
            vp.mesh = &meshScene;
        }

        // ===== ALBEDO SLOTS FOLLOW THE UPLOADS (Plan C Task 5, review I2) =====
        // StartOne/StartOneMesh asked the material cache ONCE. On a first use an albedo
        // texture is only QUEUED by that ask (ResolveMeshAlbedoSlot answers the invalid
        // slot), so the slot baked into `instances` would stay invalid and the flat-colour
        // picture would be published as a success. Before every re-render the materials
        // are Requested again (a re-ask re-resolves an invalid slot) and the slots
        // rewritten into `instances`. Reports whether any slot CHANGED (the picture last rendered
        // used the old one, so it must not be published) and whether an albedo is still on its way.
        struct SlotState { bool changed = false; bool outstanding = false; };
        const auto refreshAlbedoSlots = [&]() -> SlotState
        {
            SlotState state;
            const auto onItsWay = [&](const Arcane::ResolvedMeshMaterial& m)
            {
                if (m.albedo.IsValid() && m.materialSlot == 0xFFFFFFFFu
                    && (ctx->HasPendingUploads()
                        || (ctx->Textures() && ctx->Textures()->UploadQueued(m.albedo))))
                    state.outstanding = true;
            };
            if (!meshMats)
                return state;
            if (r.subject == Subject::Material && r.surface == Arcane::MaterialSurface::Mesh && !instances.empty())
            {
                meshMats->Request(r.id);
                const auto& table = meshMats->Table();
                if (const auto it = table.find(r.id); it != table.end())
                {
                    state.changed = state.changed || r.meshSlot != it->second.materialSlot
                                 || r.meshColor != it->second.baseColor;
                    r.meshColor = it->second.baseColor;
                    r.meshSlot  = it->second.materialSlot;
                    instances[0] = MaterialPreviewSphereInstance(r.meshColor, r.meshSlot);
                    onItsWay(it->second);
                }
            }
            else if (r.subject == Subject::Mesh && r.meshInstanceMaterials.size() == instances.size())
            {
                for (std::size_t i = 0; i < instances.size(); ++i)
                {
                    const Arcane::Guid& mat = r.meshInstanceMaterials[i];
                    if (!mat.IsValid())
                        continue;
                    meshMats->Request(mat);
                    const auto& table = meshMats->Table();
                    if (const auto it = table.find(mat); it != table.end())
                    {
                        state.changed = state.changed || instances[i].materialSlot != it->second.materialSlot
                                     || instances[i].baseColor != it->second.baseColor;
                        instances[i].baseColor    = it->second.baseColor;
                        instances[i].materialSlot = it->second.materialSlot;
                        onItsWay(it->second);
                    }
                }
            }
            return state;
        };

        // REFRESH BEFORE THE FIRST RENDER too: a slot that became valid between StartOne and this harvest
        // (another harvest's frames uploaded the albedo) must not be rendered flat. The first refresh's
        // own `changed` is moot -- nothing was rendered with the old slot yet.
        (void)refreshAlbedoSlots();

        Arcane::NriGraphContext::FrameOutcome outcome = ctx->RenderFrameOffscreen(vp);

        // ===== DEFERRED UPLOADS (Plan C Task 5): ONE FRAME IS NOT THE PICTURE ========
        // A mesh, a texture or a declared material image that this frame resolved for
        // the first time is only QUEUED by it: the next frame uploads, the one after
        // draws. So while the vehicle reports uploads in flight the same picture is
        // rendered again -- bounded by the retry budget this harvester already owns
        // (editor.thumbnail.maxRetries, default 3 = the upload frame, the first
        // drawing frame, and one spare). The stash SetMeshSupply serves from stays
        // alive across the loop; it is released only after the last frame.
        // NOTE (review, fix round 3): no path where a slot flips with NOTHING pending has been constructed -- a texture
        // becomes ready only through a committed upload, which leaves HasPendingUploads() true for one more frame, and
        // the refresh before the first render already absorbs a flip that happened before this harvest. `changed` is
        // therefore a DEFENSIVE condition (it keeps the never-publish-a-stale-slot invariant if that ever stops holding).
        // Render again while uploads are in flight, an albedo is still on its way, OR a slot changed since the
        // picture last rendered (that picture used the old slot and is stale).
        SlotState slots = refreshAlbedoSlots();
        for (int again = 0;
             outcome == Arcane::NriGraphContext::FrameOutcome::Presented
             && (ctx->HasPendingUploads() || slots.changed || slots.outstanding)
             && again < thumb.maxRetries;
             ++again)
        {
            fillBatch();
            outcome = ctx->RenderFrameOffscreen(vp);
            slots = refreshAlbedoSlots();   // what the NEXT frame must use; changed == the one just rendered is stale
        }

        if (outcome == Arcane::NriGraphContext::FrameOutcome::Presented
            && (ctx->HasPendingUploads() || slots.changed))
        {
            // Gave up: the uploads never settled inside the budget. Behave as today's
            // failure path does for a picture that cannot be made -- the item is
            // Refused (memoized, kind icon), not published half-drawn.
            ARC_WARN("[thumbs] the preview for {} {} still had uploads in flight after {} extra frame(s) "
                     "-- giving up on it", r.subject == Subject::Mesh ? "mesh" : "material",
                     r.id.ToString(), thumb.maxRetries);
            ReleaseMeshHarvest();
            return HarvestOutcome::Refused;
        }

        if (outcome != Arcane::NriGraphContext::FrameOutcome::Presented)
        {
            // Skipped is impossible at a fixed 64px; Failed has already
            // latched + logged. The vehicle is dropped so the replacement is
            // built from scratch rather than re-recording through whatever
            // left the graph in a bad state -- and DropVehicle is what makes
            // that safe, by re-queueing every `ready` entry whose slot indices
            // the destroyed vehicle scoped (read its comment: a stale one
            // harvests SUCCESSFULLY into the wrong pixels). It also owns the
            // retry budget, which is what actually bounds a vehicle that fails
            // every time -- dropping alone never did, since the next Pump
            // rebuilds it immediately.
            ARC_ERROR("[thumbs] the preview frame for {} {} failed "
                      "-- dropping the preview vehicle",
                      r.subject == Subject::Mesh ? "mesh" : "material", r.id.ToString());
            ReleaseMeshHarvest();   // before DropVehicle, while `ctx` is still there
            DropVehicle();
            return HarvestOutcome::Retry;
        }
        // The transient stash SetMeshSupply's lambda served this render from, AND the
        // residency uploaded out of it -- both released here rather than left for the
        // next harvest, so a large imported mesh costs neither a lingering CPU buffer
        // nor a slice of the vehicle's mesh budget for the rest of the session. A
        // no-op when `r.subject` was Material.
        ReleaseMeshHarvest();

        std::uint32_t w = 0, h = 0;
        std::vector<unsigned char> rgba;
        // TIGHT RGBA8, already swizzled out of the output's BGRA. A false is
        // NOT a run failure (ReadCapture's own contract) -- just no picture.
        // Refused, not Retry: the frame itself was fine, so retrying it would
        // idle the device again for the same answer.
        if (!ctx->ReadCapture(w, h, rgba) || w == 0 || h == 0)
            return HarvestOutcome::Refused;

        // ===== THE CACHE SWAP, AND WHAT A RE-HARVEST OWES ================
        Entry& e = entries[r.id];
        if (!e.thumbId.IsValid())
        {
            // A SYNTHETIC guid, the toolbar mark's own idiom: this image is
            // not a project asset, and the chrome cache's whole vocabulary is
            // Guids. Generated so it cannot collide with a real asset id.
            e.thumbId = Arcane::Guid::Generate();
            thumbToMaterial.emplace(e.thumbId, r.id);
        }
        // RE-HARVEST: release the old texture (and the chrome ImGuiNri entry
        // keyed on its raw pointer) BEFORE the new bytes go in -- the full
        // reasoning is on ReleaseThumbTexture. NOTHING RENDERS BETWEEN ITS TWO
        // CALLS: they run inside one Pump, at phase 13, strictly after this
        // frame's render and strictly before the next one's. A no-op on a
        // first harvest.
        ReleaseThumbTexture(e);

        e.pixels.width = w;
        e.pixels.height = h;
        e.pixels.rgba = rgba;   // copied: the PNG writer below consumes the other one
        EnsureTexture(r.id, e);

        // ===== PERSIST (the UE lesson) ==================================
        // maxWidth 0: these bytes are ALREADY the thumbnail size, and the
        // writer's cap is a downscale, not a target.
        //
        // A FAILURE IS NOT FATAL BUT IT IS NOT SILENT EITHER: the thumbnail is
        // live in the cache regardless, so the session is fine -- but a
        // read-only Saved/, a full disk or a bad path would otherwise turn the
        // whole persistence path off with nothing to read, and every later
        // boot would silently pay the full harvest again. (The writer logs its
        // own WARN too; this one names the material, which is what makes the
        // pattern visible when it is one material rather than all of them.)
        if (const std::filesystem::path png = ThumbPath(r.id); !png.empty())
            if (!Arcane::WriteThumbnailPngRgba(png, w, h, std::move(rgba), 0))
                ARC_WARN("[thumbs] could not persist the preview for {} {} to '{}' "
                         "-- it will be re-harvested on the next boot",
                         r.subject == Subject::Mesh ? "mesh" : "material",
                         r.id.ToString(), png.generic_string());

        return HarvestOutcome::Done;
    }

    std::uint64_t MaterialPreviewHarvester::Impl::EnsureTexture(const Arcane::Guid& material,
                                                                Entry& e) const
    {
        if (e.texture != 0)
            return e.texture;
        if (!e.pixels.Valid() || !e.thumbId.IsValid())
            return 0;
        Arcane::NriGraphContext* chrome = Chrome();
        Arcane::NriTextureCache* cache = chrome ? chrome->Textures() : nullptr;
        if (!cache)
            return 0;   // no device yet (a PrimeFromDisk during boot) -- retried
        // ColorSpace::Display is load-bearing, exactly as it is for the
        // toolbar mark: ImGui draws AFTER the tonemap, and an sRGB view under
        // it decodes a second time and reads dark. These bytes are already
        // display-referred -- they came out of the tonemap.
        nri::Texture* t = cache->Resolve(e.thumbId, Arcane::NriTextureCache::ColorSpace::Display);
        if (!t && cache->UploadQueued(e.thumbId, Arcane::NriTextureCache::ColorSpace::Display))
            return 0;   // created and queued: the chrome context's next frames upload it; asked again every draw
        if (!t)
        {
            ARC_WARN("[thumbs] the chrome texture cache refused the preview for material {}",
                     material.ToString());
            return 0;
        }
        e.texture = static_cast<std::uint64_t>(reinterpret_cast<std::intptr_t>(t));
        return e.texture;
    }

    // =====================================================================
    // The pump
    // =====================================================================
    void MaterialPreviewHarvester::Pump(double now)
    {
        Impl& im = *m_impl;

        // ===== THE EARLY-OUT, and it is the whole performance contract ===
        // Nothing rendered and nothing to start means NOTHING is touched: no
        // device, no fence, no allocation, no cache lookup beyond these two
        // emptiness tests. `pending` deliberately does NOT keep this alive --
        // a compile in flight is the compiler's business and lands through
        // OfferCompileResult, not through a poll here.
        if (im.ready.empty() && im.queue.empty())
            return;

        // ===== THE VEHICLE, LAZILY, ONCE ================================
        // Not "per harvest" and not "at construction": the first material
        // that actually wants a picture is what pays for the offscreen
        // context, the batcher, the sphere and the mesh-material cache. A
        // null chrome (boot has not reached CreateGraphVehicles, or a run
        // that never built a vehicle) leaves EVERYTHING queued rather than
        // failing it -- nothing about a missing device says the material is
        // un-previewable.
        Arcane::NriGraphContext* chrome = im.Chrome();
        if (!chrome || !im.EnsureVehicle(*chrome))
            return;

        // ===== ONE HARVEST PER PUMP =====================================
        // ReadCapture idles the whole device, so this is capped at one --
        // the same "rare event pays for the DeviceWaitIdle" reasoning
        // EditorApp::CaptureGraphViewportPng states for the cover screenshot.
        // LIFO: the newest ready entry is the one whose Request came from the
        // most recent visible draw.
        if (!im.ready.empty())
        {
            Impl::Ready r = std::move(im.ready.back());
            im.ready.pop_back();
            const WorkKey key{ r.id, r.subject };
            im.inFlight.erase(key);
            switch (im.Harvest(r))
            {
                case Impl::HarvestOutcome::Done:
                    ARC_INFO("[thumbs] harvested a 64px preview for {} {} ({} left)",
                             r.subject == Subject::Mesh ? "mesh" : "material",
                             r.id.ToString(), PendingCount());
                    break;
                case Impl::HarvestOutcome::Refused:
                    // Harvest already logged whatever went wrong. Memoize it so
                    // a broken work item cannot idle the device once a frame
                    // forever.
                    im.failed.insert(r.id);
                    break;
                case Impl::HarvestOutcome::Retry:
                    // The VEHICLE failed, not the work item. DropVehicle has
                    // already re-queued every OTHER `ready` entry and taken a
                    // bite out of the retry budget; this one is the entry it
                    // could not see, because Pump popped it before the call.
                    // Unless that budget just ran out, in which case the
                    // pipeline is written off and reviving one entry would
                    // restart exactly the loop the budget bounds.
                    if (im.givenUp)
                        im.failed.insert(r.id);
                    else
                        im.Push(key);
                    break;
            }
            return;   // one device idle per frame, no matter what else is due
        }

        // ===== OTHERWISE START ONE (pure CPU: a JSON load + a DXC submit, OR --
        // for a Subject::Mesh item -- a JSON load + ResolveMeshData) ==========
        // It stays in `inFlight` across the move from `queue` to `pending` /
        // `ready`, so Request()/RequestMesh() cannot re-push it and nothing is
        // ever lost between the two containers.
        const WorkKey key = im.queue.front();
        im.queue.pop_front();
        im.StartOne(key, now);
    }

    // =====================================================================
    // Readers
    // =====================================================================
    std::uint64_t MaterialPreviewHarvester::ThumbTextureId(const Arcane::Guid& material) const
    {
        Impl& im = *m_impl;   // the pimpl loophole: `const` here is the
                              // CALLER's contract (a query), not a promise
                              // that the lazy upload below cannot happen.
        const auto it = im.entries.find(material);
        if (it == im.entries.end())
            return 0;
        return im.EnsureTexture(material, it->second);
    }

    const Arcane::PixelData* MaterialPreviewHarvester::PixelsForThumb(
        const Arcane::Guid& id) const
    {
        const Impl& im = *m_impl;
        const auto m = im.thumbToMaterial.find(id);
        if (m == im.thumbToMaterial.end())
            return nullptr;
        const auto e = im.entries.find(m->second);
        if (e == im.entries.end() || !e->second.pixels.Valid())
            return nullptr;
        return &e->second.pixels;
    }
}
