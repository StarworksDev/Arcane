// THE HARVESTER'S PICTURES, on a real adapter. Two cases, one charter --
// "MaterialPreviewHarvester renders the right pixels for the right guid":
//
//   1. [gpu][thumbs]          -- the CONTROL/SUBJECT cross-contamination check
//                                (final-review C1's net), on scratch fixtures.
//   2. [gpu][thumbs][golden]  -- the THUMBNAIL GOLDEN SET (F2c debts arc, Task
//                                E): five real ReferenceProject subjects
//                                through ONE harvester, ImageCompared at budget
//                                0 against committed references, with a
//                                re-bless mode. Its own header block, far
//                                below, carries the decision record and the
//                                bless procedure.
//
// They live side by side deliberately (the decision record's first decision):
// "does harvest #2 draw harvest #1's geometry" and "does harvest #1 look right"
// are two halves of one question over one class, and splitting them across
// files is how they drift.
//
// ---------------------------------------------------------------------------
// Mesh-ASSET thumbnail harvesting, on a real adapter -- final-review C1's net.
//
// WHY THIS IS A [gpu] CASE AND NOT A DEVICE-LESS ONE: the bug lives entirely in
// the render half. MaterialPreviewHarvester::Harvest stashes the resolved
// geometry, names a guid on the MeshInstances, and hands both to a real
// NriGraphContext whose NriMeshBufferCache caches BY GUID and consults the
// supply only on a MISS. There is no seam between "which guid the instances
// name" and "which buffers get bound" short of the device, so the only honest
// instrument is the picture.
//
// THE ASSERTION IS A CONTROL, NOT "the two pictures differ". Two different
// meshes produce different pictures even WITH the bug, because FrameMeshBounds
// re-frames the camera per asset -- so "differs" proves nothing. Instead the
// cube is harvested twice: once on a vehicle that has seen nothing else, and
// once as the SECOND harvest on a vehicle that already holds another mesh's
// residency. Those two pictures must be identical. Under the pre-fix code
// (every mesh harvested under one session-fixed 'MESH' guid) the second harvest
// HIT the sphere's resident buffers and drew the first 36 of the sphere's
// indices, so the two cube pictures differed.
//
// The two fixtures are chosen to share an AABB on purpose -- BuildCube(1.0f) and
// BuildUvSphere(0.5f, ...) both span [-0.5, 0.5]^3 (MeshAsset.cpp's unit rule) --
// so FrameMeshBounds hands both harvests the SAME camera and the picture is a
// function of the geometry alone.

// Include order: NRI headers first, ALWAYS -- see NriCommon.hpp.
#include <NRI.h>
#include <Extensions/NRIHelper.h>

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Host/HostConfig.hpp>
#include <Arcane/Host/OffscreenVehicle.hpp>
#include <Arcane/Render/Nri/NriGraphContext.hpp>

#include <Project/MaterialPreviewHarvester.hpp>

#undef ERROR

#include <Arcane/Guid.hpp>
#include <Arcane/Mesh/MeshAsset.hpp>
#include <Arcane/Render/RenderErrorLatch.hpp>

// The golden set below (F2c debts arc, Task E) drives the harvester against the
// REAL ReferenceProject instead of scratch fixtures, so it needs the project,
// the assets facade, the compile service -- and the comparator.
#include <Arcane/Assets/Assets.hpp>
#include <Arcane/Assets/ImageCompare.hpp>
#include <Arcane/Assets/ImageIo.hpp>
#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Host/ReferenceImages.hpp>
#include <Arcane/Project/AssetId.hpp>
#include <Arcane/Project/Project.hpp>
#include <Arcane/Render/ShaderCompiler.hpp>
#include <Arcane/Render/ShaderSourceProvider.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "Helpers/GpuCapability.hpp"
#include "Helpers/ReferenceProjectDir.hpp"
#include "Helpers/TestTypeContext.hpp"

namespace fs = std::filesystem;

namespace
{
    using Arcane::Guid;
    using Arcane::Editor::MaterialPreviewHarvester;

    fs::path FreshDir(const char* leaf)
    {
        fs::path d = fs::temp_directory_path() / "arcane_mesh_thumb_harvest" / leaf;
        std::error_code ec;
        fs::remove_all(d, ec);
        fs::create_directories(d);
        return d;
    }

    Guid WriteMesh(const fs::path& file, Arcane::MeshSource source, const char* name)
    {
        Arcane::MeshAssetData d;
        d.id     = Guid::Generate();
        d.name   = name;
        d.source = source;
        REQUIRE(Arcane::SaveMeshAsset(file, d));
        return d.id;
    }

    std::vector<unsigned char> ReadAll(const fs::path& p)
    {
        std::vector<unsigned char> bytes;
        std::error_code ec;
        const auto size = fs::file_size(p, ec);
        if (ec)
            return bytes;
        bytes.resize(static_cast<std::size_t>(size));
        std::FILE* f = std::fopen(p.string().c_str(), "rb");
        if (!f)
            return {};
        const std::size_t read = std::fread(bytes.data(), 1, bytes.size(), f);
        std::fclose(f);
        bytes.resize(read);
        return bytes;
    }

    // Drive the harvester until its queue drains. Two Pumps is the steady cost of
    // one mesh (start, then harvest); the bound is generous so a Retry cannot hang
    // the suite.
    void Drain(MaterialPreviewHarvester& h, double& clock)
    {
        for (int i = 0; i < 12 && h.PendingCount() != 0; ++i)
        {
            h.Pump(clock);
            clock += 1.0 / 60.0;
        }
        CHECK(h.PendingCount() == 0u);
    }
}

TEST_CASE("pixel: two meshes harvested through ONE preview vehicle each render their "
          "OWN geometry", "[gpu][thumbs]")
{
    ARC_REQUIRE_BACKEND(Arcane::Test::kNativeBackend);

    Arcane::HostConfig cfg;
    cfg.backend  = Arcane::Test::kNativeBackend;
    cfg.headless = true;
    auto chrome = Arcane::OffscreenVehicle::Create(cfg, 256, 128);
    REQUIRE(chrome != nullptr);

    const fs::path assets = FreshDir("assets");
    const fs::path thumbsControl = FreshDir("thumbs_control");
    const fs::path thumbsShared  = FreshDir("thumbs_shared");

    const fs::path cubeFile   = assets / "cube.arcmesh";
    const fs::path sphereFile = assets / "sphere.arcmesh";
    const Guid cubeId   = WriteMesh(cubeFile,   Arcane::MeshSource::Cube,     "probe-cube");
    const Guid sphereId = WriteMesh(sphereFile, Arcane::MeshSource::UvSphere, "probe-sphere");

    const auto makeServices = [&](const fs::path& thumbs)
    {
        MaterialPreviewHarvester::Services s;
        s.chromeGraph = [&]() { return &chrome->Graph(); };
        s.hostConfig  = &cfg;
        s.backend     = Arcane::Test::kNativeBackend;
        // No compiler and no source provider: a mesh asset compiles nothing, which
        // is the whole reason Subject::Mesh has no `pending` stage.
        s.resolveAsset = [&](const Guid& g) -> std::optional<fs::path>
        {
            if (g == cubeId)   return cubeFile;
            if (g == sphereId) return sphereFile;
            return std::nullopt;
        };
        s.thumbnailDir = [thumbs]() { return thumbs; };
        return s;
    };

    const std::uint64_t errorsBefore = Arcane::RenderErrorCount();
    double clock = 0.0;

    // ---- THE CONTROL: the cube, alone, on a vehicle that has seen nothing ----
    std::vector<unsigned char> cubeAlone;
    {
        MaterialPreviewHarvester h(makeServices(thumbsControl));
        chrome->Graph().SetPixelSupply(
            [&](const Guid& g) { return h.PixelsForThumb(g); });
        h.RequestMesh(cubeId);
        Drain(h, clock);
        h.Shutdown();
        // Before `h` dies: the chrome context outlives it and still holds a lambda
        // capturing it.
        chrome->Graph().SetPixelSupply(nullptr);
        cubeAlone = ReadAll(thumbsControl / (cubeId.ToString() + ".png"));
        REQUIRE(!cubeAlone.empty());
    }

    // ---- THE SUBJECT: sphere FIRST, then the cube, through ONE harvester ----
    std::vector<unsigned char> sphereShared, cubeShared;
    {
        MaterialPreviewHarvester h(makeServices(thumbsShared));
        chrome->Graph().SetPixelSupply(
            [&](const Guid& g) { return h.PixelsForThumb(g); });

        h.RequestMesh(sphereId);
        Drain(h, clock);
        sphereShared = ReadAll(thumbsShared / (sphereId.ToString() + ".png"));
        REQUIRE(!sphereShared.empty());

        h.RequestMesh(cubeId);
        Drain(h, clock);
        h.Shutdown();
        chrome->Graph().SetPixelSupply(nullptr);
        cubeShared = ReadAll(thumbsShared / (cubeId.ToString() + ".png"));
        REQUIRE(!cubeShared.empty());
    }

    // THE ASSERTION (final-review C1): the second harvest through a shared vehicle
    // draws the CUBE, pixel for pixel the same picture the cube gets alone.
    CHECK(cubeShared == cubeAlone);
    // ...and the two assets really are different pictures, so the check above is
    // pinning "the right geometry" rather than "everything looks the same".
    CHECK(sphereShared != cubeAlone);

    // No OOB index read, no refused upload, no validation error on either harvest.
    CHECK(Arcane::RenderErrorCount() == errorsBefore);

    std::error_code ec;
    fs::remove_all(fs::temp_directory_path() / "arcane_mesh_thumb_harvest", ec);
}

// =============================================================================
// THE THUMBNAIL GOLDEN SET (F2c debts arc, Task E)
//
// Decision record: docs/research/2026-09-15-thumbnail-golden-lane-research.md
// (its section 2 is this case's spec, decision by decision). Evidence:
// ...-thumbnail-golden-internal-research.md (this repo's machinery) and
// .superpowers/sdd/2026-09-15-f2c-debts-arc/spike-64px-imagecompare.md (the
// comparator, measured at 64x64 before a single reference was committed).
//
// WHAT IS PINNED, AND AT WHICH SURFACE. The bytes compared here are the ones
// MaterialPreviewHarvester itself writes to `thumbnailDir` -- its OWN 64x64
// offscreen capture, never a crop of a panel screenshot and never a host
// window. That invariant is decision 5 and it is deliberate: the editor shell
// (editor-ui.png) is a shared, layout-volatile slot that gets re-blessed every
// time a panel moves, and a per-asset rendering property hidden inside such a
// capture can be laundered into the new reference by any layout re-bless. The
// harvester needs no host boot to answer "does this asset still render right"
// -- an OffscreenVehicle plus hand-filled Services is the whole vehicle -- so
// this is an in-process render-path case, exactly like GoldenImageTest.cpp's
// lit cube, and exactly NOT a golden-gate.ps1 lane.
//
// ONE HARVESTER, FIVE SUBJECTS, ONE CASE. This is not tidiness. The C1 bug
// class was "the second harvest through a shared vehicle draws the FIRST's
// geometry" (residency cached by guid under a session-fixed synthetic id), and
// a set that stood each subject up in its own harvester could never catch it.
// All five go through one instance, one queue, one preview vehicle -- mesh
// after material after mesh -- so cross-subject contamination surfaces as a
// picture that does not match its own reference.
//
// NAME-KEYED, NOT CONTENT-HASH-KEYED (decision 6). No hashing anywhere here:
// at budget 0 a refactor that leaves pixels unchanged already passes and a
// changed asset already fails loudly, whereas hash-keyed reference names would
// turn "the asset changed" from a red compare into a silent missing-reference
// refusal. The content-hash idea belongs to the harvester's runtime CACHE
// (PrimeFromDisk's mtime-only staleness check), which is its own follow-on.
//
// D3D12 ONLY, one committed set, for GoldenImageTest.cpp's stated reason:
// whether the two backends agree pixel-for-pixel is the reference HIERARCHY's
// question, answered at the host level where a real divergence earns a real
// backend override. The resolution rule below still honours a
// thumbs/<backend>/<name>.png override if one is ever blessed -- it is the
// hierarchy that is reused, not the second backend that is claimed.
//
// DEBUG AND RELEASE SHARE THIS SAME SET, one reference per subject, no
// per-configuration override in the resolution rule -- an assumption the
// final F2c-debts-arc review (I1) named and then measured: a from-scratch
// Release build and a foreground Release run of this case both came back
// clean (seed 18161288, 54/54 assertions), so the assumption held on this
// pipeline, but it is unverified on any other machine or driver.
//
// ===== BUDGET 0, AND THE ONE FAILURE SHAPE THAT IS EXPECTED ==========
// CompareImages at the library default: no ImageCompareOptions, no
// maxDiffPixels, no maxDiffPixelRatio. The spike measured this at 64x64 on
// these very fixtures: identical -> match; a section recolour or a 1px content
// shift -> a clean fail that the antialiasing cascade never swallows; a
// single +-8-on-one-channel pixel -> absorbed below the dE94 JND. Sound for
// everything this set exists to catch.
//
// It is NOT sound against a UNIFORM BRIGHTNESS LIFT, and that is stated here
// rather than papered over with a budget. A +1 lift is absorbed; a +4 lift --
// visually indistinguishable -- fails catastrophically (73% of golden_prop's
// pixels), because a flat-shaded region has zero 3x3 variance BEFORE and AFTER
// the lift, and the comparator short-circuits zero-variance windows to "cannot
// be antialiasing, therefore a real difference" without ever reaching SSIM. A
// 64px render of a shaded prop or sphere is mostly flat-shaded region, so that
// short-circuit dominates. CONSEQUENCE, SAID PLAINLY: a tonemap/exposure arc
// (the north star's T5/T6) is EXPECTED to fail this whole set, and the right
// response is to LOOK at the diffs and RE-BLESS -- not to raise a budget, which
// would also silently swallow a real scattered regression of the same size.
//
// ===== RE-BLESSING, THE WHOLE PROCEDURE =============================
// The renderer is about to move every lit pixel here (T1's GGX cutover, then
// T5/T6/T3-lite), so this set ships with the mechanism its template never had:
// GoldenImageTest.cpp's lit cube has been re-blessed exactly zero times since
// 2026-08-26 and has no way to be.
//
//   1. ARCANE_THUMBS_BLESS=1 ./ArcaneTests.exe "[thumbs][golden]"
//        -- writes each harvested PNG over its reference in the SOURCE tree
//           (<repo>/ReferenceProject/Verify/References/thumbs/...), at the
//           level the reference resolved from, logging one line per file.
//           Nothing is compared on a bless run.
//   2. Rebuild Arcane.slnx so ArcaneTests' postbuild restages Verify/ beside
//      the exe (the compare reads the STAGED copy, not the source one).
//   3. ./ArcaneTests.exe "[thumbs][golden]"   -- no env var. Must be green.
//   4. git diff --stat, then OPEN THE PNGs. A reference nobody looked at is
//      not a reference.
//   5. Commit.
//
// SCOPE IS THE CATCH2 FILTER, deliberately -- one case name re-blesses that
// case's subjects, "[thumbs][golden]" re-blesses the set, and there is no bulk
// overwrite outside the filter. The switch is an environment variable rather
// than a CLI flag only because Catch2 owns the command line here; the semantics
// (opt-in, explicit, writes at the resolved level, never flattens a backend
// override) are the host's own --bless rule.
// =============================================================================
namespace
{
    // The subjects, and what each one is FOR. Guids are the committed content's
    // own (ReferenceProject/Content/...), quoted literally so a renamed or
    // re-idded asset fails as "not in the asset registry" here rather than
    // silently selecting something else.
    struct ThumbSubject
    {
        const char* reference;   // thumbs/<reference>.png
        const char* guid;
        bool        mesh;        // RequestMesh vs Request -- the harvester's Subject tag
        const char* what;
    };

    // WHY THESE FIVE: one per harvester dispatch that actually renders a
    // picture, plus a second mesh so the mesh path has the two fixtures the C1
    // class needs. MaterialPreviewHarvester::Impl::StartOne dispatches exactly
    // four ways -- Subject::Mesh, and Subject::Material over the Mesh /
    // Sprite / Fullscreen surfaces -- and all four are covered below. There is
    // no fifth surface being skipped.
    constexpr ThumbSubject kThumbSubjects[] = {
        { "mesh-golden_prop", "49304328-394e-450d-9816-2b8a2ad98191", true,
          "the IMPORTED multi-section prop: three sections over two slots, Metal (blue) "
          "and Paint (red), resolved through the arccook artifact -- the only subject "
          "that needs meshArtifactFor/cookPending at all" },
        { "mesh-reference_cube", "7e5a0011-0011-4011-8011-000000000011", true,
          "the GENERATED primitive: no cook, no artifact, the other half of the "
          "imported-vs-primitive split in ResolveMeshData" },
        { "material-reference_mesh", "7e5a0010-0010-4010-8010-000000000010", false,
          "MaterialSurface::Mesh -- the lit sphere in the material's resolved baseColor, "
          "the branch that compiles nothing and resolves synchronously" },
        { "material-pulse_sprite", "7e5a0001-0001-4001-8001-000000000001", false,
          "MaterialSurface::Sprite -- a real DXC compile of the stitched sprite template, "
          "on a quad over the checkerboard, with a declared texture param bound through "
          "pixelSupply" },
        { "material-reference_post", "7e5a0002-0002-4002-8002-000000000002", false,
          "MaterialSurface::Fullscreen -- the material's own TWO-pass chain with the "
          "checkerboard as kSceneInput, the widest compile path the harvester has" },
    };

    // THE BACKEND DIRECTORY NAME, not Arcane::ToString(backend). "dx12"/"vulkan"
    // is the CLI's spelling and therefore the one the reference hierarchy's
    // directories are named with (RuntimeApp.cpp's CompareBackendName says why
    // the two spellings must not be conflated). The native backend's: "dx12"
    // on Windows, where the set was blessed.
    constexpr const char* kThumbBackendDir = Arcane::Test::kNativeBackendCli;

    // ReferenceImages.hpp's resolution rule, applied under a thumbs/
    // subdirectory through ResolveReferenceIn -- the same probe order (adapter
    // set, backend override, shared), the same "nothing resolved -> a first
    // bless creates the SHARED image (or the adapter set's own)" rule, and
    // BlessReference as the only writer. `adapterSet` is the vehicle device's
    // Arcane::ReferenceAdapterSet: "" on hardware, e.g. "vulkan-lavapipe" on
    // Mesa's CPU rasteriser, whose bless never touches the hardware images.
    Arcane::ReferenceResolution ResolveThumbReference(const fs::path& projectRoot,
                                                      const std::string& name,
                                                      const std::string& adapterSet)
    {
        return Arcane::ResolveReferenceIn(projectRoot / "Verify" / "References" / "thumbs",
                                          name, kThumbBackendDir, adapterSet);
    }

    bool ThumbBlessRequested()
    {
        // Read ONCE per case, at the top, exactly as the decision record
        // specifies -- not per subject, so a set can never end up half-blessed
        // because something changed the environment mid-run.
        const char* v = std::getenv("ARCANE_THUMBS_BLESS");
        return v != nullptr && std::string(v) == "1";
    }
}

TEST_CASE("golden: the harvester's own 64px renders of five ReferenceProject subjects "
          "match their committed references at a zero budget (native backend)",
          "[gpu][thumbs][golden]")
{
    ARC_REQUIRE_BACKEND(Arcane::Test::kNativeBackend);

    const bool bless = ThumbBlessRequested();

    // ---- WHICH TREE IS WHICH, and why there are two of them -------------
    // SOURCE: the repo checkout. Holds Content/ (the five assets, their
    // materials and the .glb), Intermediate/Artifacts (arccook's output) and
    // the COMMITTED references -- ArcaneTests' postbuild stages neither
    // Content/ nor the .arcproj, so [host] cases already reach the source tree
    // through this exact walk, and a bless must write here because references
    // are tracked files.
    // STAGED: ReferenceProject/Verify/ beside the exe, restaged (RMDIR then
    // COPYDIR) on every build. The COMPARE reads here, so a green run proves
    // the committed bytes actually survive staging rather than proving the
    // source file matches itself.
    const fs::path sourceRoot = Arcane::Test::FindReferenceProjectDir();
    REQUIRE_FALSE(sourceRoot.empty());   // raise the walk's bound if this ever fails
    const fs::path stagedRoot = fs::path("ReferenceProject");

    if (bless)
    {
        // THE GUARD ON A MIS-DERIVED PATH. BlessReference creates parent
        // directories, so a wrong root would happily conjure a stray
        // Verify/References/thumbs/ tree somewhere and report success. Refuse
        // instead: the References directory must ALREADY exist in the tree we
        // are about to write into -- that is the landmark that says "this is
        // the real, committed reference tree".
        const fs::path refs = sourceRoot / "Verify" / "References";
        INFO("bless target root: " << refs.string());
        REQUIRE(fs::is_directory(refs));
    }

    // ---- THE PROJECT ----------------------------------------------------
    // Runtime::OpenProject, not a bare Project::Open: MeshArtifactFor/
    // CookPending/PixelsFor all resolve through the Assets facade's installed
    // resolver, which only exists once a project is opened on a Runtime (the
    // same reasoning HostBootTest's own imported-prop case states).
    Arcane::Runtime runtime(Arcane::Test::Process());
    REQUIRE(runtime.OpenProject(sourceRoot));
    const Arcane::Project* proj = runtime.CurrentProject();
    REQUIRE(proj != nullptr);
    Arcane::Assets& assets = runtime.AssetsFacade();

    // ---- THE COMPILE SERVICE -------------------------------------------
    // Two of the five subjects are a real DXC round trip. debounce 0 so Poll
    // dispatches on the first tick -- the test drives the clock, and a quiet
    // window would only make the drain loop below longer.
    Arcane::ShaderSourceProvider sources;
    sources.AddRoot("data/shaders");   // staged beside this exe by the postbuild
    Arcane::ShaderCompiler compiler;
    REQUIRE(compiler.InitializeWithDebounce(0.0));
    REQUIRE(compiler.IsAvailable());

    // ---- THE VEHICLE ----------------------------------------------------
    Arcane::HostConfig cfg;
    cfg.backend  = Arcane::Test::kNativeBackend;
    cfg.headless = true;
    auto chrome = Arcane::OffscreenVehicle::Create(cfg, 256, 128);
    REQUIRE(chrome != nullptr);
    const Arcane::NriDeviceCaps& caps = chrome->Graph().Device().Caps();
    const std::string adapterSet =
        Arcane::ReferenceAdapterSet(kThumbBackendDir, caps.adapterName, caps.softwareAdapter);
    INFO("adapter '" << caps.adapterName << "' -> reference set '" << adapterSet << "'");

    // Never the project's own Saved/Thumbnails: this case must not write into
    // the tracked source tree, and a scratch directory also guarantees every
    // PNG read below came from THIS run rather than a stale editor session.
    const fs::path thumbs = FreshDir("thumbs_golden");

    // ---- SERVICES: EditorApp::Init's wiring, verbatim -------------------
    MaterialPreviewHarvester::Services s;
    s.chromeGraph = [&]() { return &chrome->Graph(); };
    s.hostConfig  = &cfg;
    s.compiler    = &compiler;
    s.sources     = &sources;
    s.backend     = Arcane::Test::kNativeBackend;
    s.resolveAsset = [proj](const Guid& g) -> std::optional<fs::path>
    {
        return proj->ResolveAsset(Arcane::AssetId::FromGuid(g));
    };
    s.pixelSupply = [&assets](const Guid& g) -> const Arcane::PixelData*
    {
        return assets.PixelsFor(g);
    };
    s.thumbnailDir = [thumbs]() { return thumbs; };
    // The two seams golden_prop's IMPORTED arm consults. Wired here for the
    // same reason Task 12a wired them in EditorApp: without them ResolveMeshData
    // takes the "no cooked artifact" Failed branch and an imported mesh can
    // never produce a thumbnail at all.
    s.meshArtifactFor = [&assets](const Guid& g) { return assets.MeshArtifactFor(g); };
    s.cookPending     = [&assets](const Guid& g) { return assets.CookPending(g); };

    const std::uint64_t errorsBefore = Arcane::RenderErrorCount();

    // ---- ONE HARVESTER, ALL FIVE SUBJECTS -------------------------------
    std::vector<Guid> ids;
    {
        MaterialPreviewHarvester h(std::move(s));
        chrome->Graph().SetPixelSupply([&](const Guid& g) { return h.PixelsForThumb(g); });

        for (const ThumbSubject& sub : kThumbSubjects)
        {
            const auto id = Guid::FromString(sub.guid);
            REQUIRE(id.has_value());
            ids.push_back(*id);
            INFO("subject " << sub.reference << " -- " << sub.what);
            if (sub.mesh)
                h.RequestMesh(*id);
            else
                h.Request(*id);
        }
        REQUIRE(h.PendingCount() == ids.size());

        // Drain: pump the compile service into the harvester, then pump the
        // harvester. One harvest per Pump by contract, plus a compile round
        // trip for the sprite/post subjects, so the bound is generous; the 1ms
        // sleep is what lets the compiler's worker thread actually finish
        // rather than being starved by a spin.
        double clock = 0.0;
        for (int i = 0; i < 20000 && h.PendingCount() != 0; ++i)
        {
            compiler.Poll(clock);
            for (const Arcane::ShaderCompileResult& r : compiler.Drain())
                h.OfferCompileResult(r);
            h.Pump(clock);
            clock += 1.0 / 60.0;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // A non-zero count here is a HARVEST failure, not a comparison one --
        // it means a subject never produced a picture at all (the harvester
        // logs the reason). The per-subject PNG REQUIREs below name which.
        CHECK(h.PendingCount() == 0u);

        h.Shutdown();
        // Before `h` dies: the chrome context outlives it and still holds a
        // lambda capturing it.
        chrome->Graph().SetPixelSupply(nullptr);
    }
    compiler.Shutdown();

    // ---- COMPARE (or BLESS) --------------------------------------------
    for (std::size_t i = 0; i < std::size(kThumbSubjects); ++i)
    {
        const ThumbSubject& sub = kThumbSubjects[i];
        INFO("subject " << sub.reference << " (" << sub.guid << ") -- " << sub.what);

        // THE HARVESTER'S OWN SURFACE (decision 5): the file it wrote itself.
        const fs::path harvested = thumbs / (ids[i].ToString() + ".png");
        INFO("harvested: " << harvested.string());
        REQUIRE(fs::exists(harvested));

        Arcane::PixelData actual;
        REQUIRE(Arcane::LoadPngRgba(harvested, actual.width, actual.height, actual.rgba));
        REQUIRE(actual.Valid());
        CHECK(actual.width == 64u);
        CHECK(actual.height == 64u);

        if (bless)
        {
            // Writes at the level the reference RESOLVED from -- an existing
            // backend override is overwritten in place, never flattened into
            // the shared slot, which is the host --bless rule this mirrors.
            const auto target = ResolveThumbReference(sourceRoot, sub.reference, adapterSet);
            REQUIRE_FALSE(target.blessTarget.empty());   // a refused name writes nothing
            REQUIRE(Arcane::BlessReference(target, actual.width, actual.height,
                                           actual.rgba.data()));
            WARN("thumbs bless: wrote " << target.blessTarget.string() << " ("
                 << (target.blessTarget != target.path ? "new"
                     : target.level == Arcane::ReferenceLevel::Adapter ? "adapter"
                     : target.level == Arcane::ReferenceLevel::Backend ? "backend"
                                                                       : "shared")
                 << ")");
            continue;   // a bless run compares nothing, by design
        }

        const auto resolved = ResolveThumbReference(stagedRoot, sub.reference, adapterSet);
        if (resolved.level == Arcane::ReferenceLevel::None)
        {
            // A MISSING REFERENCE IS A REFUSAL, NOT A SILENT CREATE -- the lit
            // cube's own rule. Name the path AND the way to create it, so the
            // reader does not have to find this comment to get unstuck.
            // FAIL_CHECK, not FAIL: an aborting failure would report only the
            // FIRST missing reference and hide the other four, which is the
            // opposite of useful on the run that creates the set.
            FAIL_CHECK("no committed thumbnail reference for '"
                 << sub.reference << "'. Looked for "
                 << resolved.triedPaths.front().string() << " then "
                 << resolved.triedPaths.back().string()
                 << " (relative to this exe's directory). To CREATE it: "
                    "ARCANE_THUMBS_BLESS=1 ArcaneTests.exe \"[thumbs][golden]\" "
                    "-- which writes the SOURCE tree under "
                 << (sourceRoot / "Verify" / "References" / "thumbs").string()
                 << " -- then rebuild so Verify/ restages, re-run without the "
                    "variable, and review the PNGs before committing.");
            continue;
        }

        Arcane::PixelData expected;
        REQUIRE(Arcane::LoadPngRgba(resolved.path, expected.width, expected.height,
                                    expected.rgba));
        REQUIRE(expected.Valid());

        const auto result = Arcane::CompareImages(expected, actual);   // default: budget 0
        INFO("reference " << resolved.path.string() << " -- diffCount " << result.diffCount
             << " (ratio " << result.diffRatio << ", maxLocalDifference "
             << result.maxLocalDifference << ") -- " << result.errorMessage);
        if (!result.passed)
        {
            // Under Saved/, which ReferenceProject/.gitignore already excludes,
            // so a failed run leaves nothing stageable behind. BOTH artifacts:
            // the diff says WHERE, the actual says WHAT -- and the actual is
            // also the file a human copies over the reference if the change was
            // intended and they would rather not re-run the bless.
            const fs::path dir = stagedRoot / "Saved" / "Verify" / "thumbs";
            std::error_code ec;
            fs::create_directories(dir, ec);
            const fs::path actualPath = dir / (std::string(sub.reference) + "-actual.png");
            const fs::path diffPath   = dir / (std::string(sub.reference) + "-diff.png");
            Arcane::WritePngRgba(actualPath, actual.width, actual.height, actual.rgba.data());
            if (!result.diffRgba.empty())
                Arcane::WritePngRgba(diffPath, result.width, result.height,
                                     result.diffRgba.data());
            WARN("thumbs golden: '" << sub.reference << "' does not match "
                 << resolved.path.string() << " -- artifacts written to "
                 << actualPath.string() << " and " << diffPath.string()
                 << ". If the change was INTENDED (a renderer arc moved every lit "
                    "pixel -- a uniform exposure/tonemap shift fails this whole set "
                    "by construction, see this case's header), re-bless: "
                    "ARCANE_THUMBS_BLESS=1 ArcaneTests.exe \"[thumbs][golden]\", "
                    "rebuild, re-run, review, commit.");
        }
        CHECK(result.passed);
    }

    // No OOB index read, no refused upload, no validation error across all five
    // harvests -- the same latch the control/subject case above watches.
    CHECK(Arcane::RenderErrorCount() == errorsBefore);

    std::error_code ec;
    fs::remove_all(fs::temp_directory_path() / "arcane_mesh_thumb_harvest", ec);
}
