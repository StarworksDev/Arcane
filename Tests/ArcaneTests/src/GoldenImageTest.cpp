// Golden-image / golden-layout verification (Task 10, plan-b comparator).
// This file is created HERE, ahead of Task 12 -- see that task's brief for
// what it appends.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Assets/ImageCompare.hpp>
#include <Arcane/Assets/ImageIo.hpp>
#include <Arcane/Host/HostConfig.hpp>
#include <Arcane/Render/GraphicsBackend.hpp>
#include <Arcane/Mesh/MeshBuilder.hpp>
#include <Arcane/Render/Nri/NriDevice.hpp>
#include <Arcane/Render/Nri/NriGraphContext.hpp>
#include <Arcane/Render/Nri/nodes/MeshNode.hpp>
#include <Arcane/Render/RenderDeviceDesc.hpp>
#include <Arcane/Render/RenderErrorLatch.hpp>
#include <Arcane/Scene/SceneCamera.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "Helpers/GpuCapability.hpp"

TEST_CASE("golden: the committed layout seed names windows this editor actually submits",
          "[golden]")
{
    // The placeholder failed here for weeks: every entry named a window that
    // does not exist, so LoadIniSettingsFromDisk applied nothing and the
    // "pinned layout" was really just BuildDefaultLayout every time. This is a
    // CHEAP, no-GPU guard against that exact regression.
    const std::filesystem::path seed =
        std::filesystem::path("ReferenceProject") / "Saved" / "verify-layout.ini";
    REQUIRE(std::filesystem::exists(seed));

    std::ifstream in(seed);
    const std::string text((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());

    CHECK(text.find("[Window][EditorDockHost]") != std::string::npos);
    // Task 12 (Finding D): LINE-ANCHORED, not a whole-file substring search.
    // The old bare `find("DockSpaceViewport")` matched a comment mentioning
    // the placeholder's own retired window name just as readily as an actual
    // "[Window][DockSpaceViewport]" section -- Task 10's implementer tripped
    // it twice while writing THIS FILE's prose, with no visible link between
    // "I edited a comment" and "the build broke". Anchoring on the newline +
    // bracket that only a real ini section header can produce encodes the
    // actual intent: no window SECTION is named this, said nothing about what
    // any comment elsewhere in the file is allowed to mention.
    CHECK(text.find("\n[Window][DockSpaceViewport]") == std::string::npos);

    // Panel-split spec s13: the three asset panels replaced [Window][Assets].
    CHECK(text.find("\n[Window][Asset Browser]") != std::string::npos);
    CHECK(text.find("\n[Window][Asset Graph]")   != std::string::npos);
    CHECK(text.find("\n[Window][Asset Status]")  != std::string::npos);
    CHECK(text.find("\n[Window][Assets]")        == std::string::npos);
}

// =============================================================================
// Task 12 (plan-b comparator): THE IN-PROCESS HALF OF THE GATE.
//
// THE HONEST SPLIT, stated here too because this is the file where it is easy
// to forget: this case renders through the real NRI frame graph (device ->
// NriDevice::Wrap -> NriGraphContext::CreateOffscreen -> RenderFrameOffscreen
// -> ReadCapture, the exact vehicle NriGraphPixelTest.cpp's PixelVehicle
// builds) and compares the result against a committed PNG with
// Arcane::CompareImages at a ZERO budget. That is ALL it proves. This exe
// links neither RuntimeApp nor EditorApp, so a green run here says NOTHING
// about boot, settle, --report, --compare/--bless, or the CLI -- scripts/
// golden-gate.ps1, which launches the real hosts, is the gate that covers
// that half. Do not let this case stand in for that script, and do not let a
// change here be read as evidence either host still works.
//
// WHY THIS DOES NOT COMPARE AGAINST ReferenceProject/Verify/References/
// runtime-scene.png, even though that is the file Step 1 of this task's own
// brief blessed and named: that PNG is the REAL ArcaneRuntime.exe host's
// capture of ReferenceProject/Content/scenes/main.arcscene, and reproducing
// it here is not just tedious, it is IMPOSSIBLE for this exe to attempt
// honestly, for reasons specific to this project, not a general excuse:
//   * that scene's PostProcess volume, its "PulseBox" sprite (a
//     TIME-VARYING material -- its rest appearance is whatever the real
//     host's --settle loop happened to converge to, not a fixed fact this
//     exe could ever recompute) and its MeshCube all reference material/
//     mesh/sprite GUIDs resolved out of ReferenceProject/Content/, and nothing
//     under Content/ is staged beside ArcaneTests.exe -- only
//     ReferenceProject/Verify/ is (premake5.lua's ArcaneTests postbuild
//     COPYDIR; Content/ is never named there, unlike ArcaneRuntime's and
//     ArcaneEditor's own {COPYDIR} of the WHOLE ReferenceProject/), so this
//     exe cannot even OPEN those assets, let alone shade them identically;
//   * every real host frame ALSO carries the debug HUD (RuntimeFrame.cpp's
//     BuildHud: an "ArcaneRuntime" ImGui window, unconditionally, in every
//     build config, in both windowed and --headless runs) baked into the
//     same captured pixels -- confirmed by this very task's Finding A, which
//     is why runtime-scene needed a backend-specific override at all. That
//     window's code lives in Source/Programs/ArcaneRuntime/src/ (the .exe's OWN sources,
//     per premake5.lua's `files` glob for that project), not in ArcaneClient,
//     so it is not merely unstaged here, it is not LINKED here.
// Comparing this case's readback against that PNG would therefore either
// require duplicating RuntimeApp's scene resolution, its post chain and its
// HUD by hand inside a test file (forking the exact "parallel infrastructure"
// this codebase's own directional rule warns against), or loosening the
// budget until the resulting wall of differences vanished -- and this task's
// own constraints forbid the second outright. So this case renders content it
// CAN fully author and own, and compares against a SEPARATE, equally real,
// committed reference this task blesses for exactly that content:
// ReferenceProject/Verify/References/inprocess-lit-cube.png. It is still a
// zero-budget CompareImages of a real GPU capture against a real committed
// PNG -- the render-path proof the honest split promises -- just not a
// disguised, unreliable attempt at host parity.
//
// ONE BACKEND ONLY (D3D12), deliberately: whether the two backends agree
// pixel-for-pixel is the reference HIERARCHY's own question (Tasks 6-7 and
// this task's own Step 1 dance over runtime-scene/editor-ui), answered at the
// host level where a real divergence gets a real backend-specific override.
// Re-litigating that question for a second, unrelated scene here would only
// duplicate that mechanism inside a file that has no reference hierarchy of
// its own -- one plain PNG, one backend, on purpose.
namespace
{
    constexpr std::uint32_t kGoldenW = 160;
    constexpr std::uint32_t kGoldenH = 96;

    // Mirrors NriGraphPixelTest.cpp's own PixelVehicle exactly (device ->
    // NriDevice::Wrap -> offscreen NriGraphContext) -- duplicated here rather
    // than shared because that struct lives in that file's anonymous
    // namespace, private to its own translation unit, and inventing shared
    // test-only plumbing for one more caller is not worth it.
    struct GoldenPixelVehicle
    {
        std::unique_ptr<Arcane::NativeDeviceOwner> native;
        std::unique_ptr<Arcane::NriDevice>         nri;
        std::unique_ptr<Arcane::NriGraphContext>   ctx;
    };

    GoldenPixelVehicle MakeGoldenVehicle()
    {
        ARC_REQUIRE_BACKEND(Arcane::GraphicsBackend::D3D12);

        GoldenPixelVehicle v;

        Arcane::RenderDeviceDesc desc;
        desc.backend = Arcane::GraphicsBackend::D3D12;
#if defined(ARC_BUILD_DEBUG)
        desc.enableValidation      = true;
        desc.enableD3D12DebugLayer = true;
        desc.enableSyncValidation  = true;
#endif
        v.native = Arcane::NativeDeviceOwner::Create(desc);
        REQUIRE(v.native != nullptr);

        v.nri = Arcane::NriDevice::Wrap(*v.native);
        REQUIRE(v.nri != nullptr);

        Arcane::HostConfig cfg;
        cfg.backend = Arcane::GraphicsBackend::D3D12;
        v.ctx = Arcane::NriGraphContext::CreateOffscreen(cfg, *v.nri, kGoldenW, kGoldenH, {});
        REQUIRE(v.ctx != nullptr);
        v.ctx->SetUploadModeForTests(Arcane::NriUploadMode::Immediate);   // node-level pixel case: it renders ONE frame of a scene whose mesh/textures are first used there (Plan C Task 5: the deferred timeline is UploadStagingGpuTest's)
        REQUIRE(v.ctx->IsOffscreen());
        return v;
    }
}

TEST_CASE("golden: an offscreen graph capture of a hand-authored lit cube matches its "
          "committed reference at a zero budget (d3d12)",
          "[gpu][golden]")
{
    const std::uint64_t before = Arcane::RenderErrorCount();

    GoldenPixelVehicle v = MakeGoldenVehicle();

    // The SAME lit-cube content NriGraphPixelTest.cpp's own
    // CheckMeshCubeCoversTheCentre builds (2 m cube, pure red, white
    // directional light head-on, ambient floor) -- deterministic, already
    // proven byte-stable run-to-run by that file's own case 7, and it needs
    // nothing this exe cannot supply: BuildCube is procedural geometry, not
    // an asset load.
    const Arcane::MeshData cube = Arcane::BuildCube(2.0f);
    const Arcane::Guid cubeId{ 1, 1 };
    v.ctx->SetMeshSupply(
        [&](const Arcane::Guid& id) -> Arcane::NriMeshBufferCache::SupplyResult
        {
            if (id == cubeId)
                return { &cube, Arcane::MeshResolveState::Ready };
            return { nullptr, Arcane::MeshResolveState::Failed };
        });

    Arcane::MeshInstance instance;
    instance.mesh      = cubeId;
    instance.baseColor = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
    const Arcane::MeshInstance instances[] = { instance };

    Arcane::MeshSceneDesc scene;
    scene.instances = instances;

    const float aspect = static_cast<float>(kGoldenW) / static_cast<float>(kGoldenH);
    scene.view = glm::lookAtRH(glm::vec3(0.0f, 0.0f, 4.0f),
                               glm::vec3(0.0f, 0.0f, 0.0f),
                               glm::vec3(0.0f, 1.0f, 0.0f));
    scene.projection    = Arcane::PerspectiveProjection(60.0f, aspect, 0.1f, 100.0f);
    scene.lightDirection = glm::vec3(0.0f, 0.0f, 1.0f);
    scene.lightColor     = glm::vec3(1.0f, 1.0f, 1.0f);
    scene.ambient        = glm::vec3(0.08f);

    Arcane::NriGraphContext::FrameDesc frame;
    frame.capture = true;
    frame.mesh    = &scene;
    // post / gameUi / imgui all left null, exactly as NriGraphPixelTest.cpp's
    // own byte-determinism case (7) leaves them: no post chain, no game HUD,
    // no host chrome -- see this case's header comment for why NEITHER host's
    // HUD can appear here at all, not merely why it is turned off.
    const auto outcome = v.ctx->RenderFrameOffscreen(frame);
    REQUIRE(outcome == Arcane::NriGraphContext::FrameOutcome::Presented);

    std::uint32_t width = 0, height = 0;
    std::vector<unsigned char> rgba;
    REQUIRE(v.ctx->ReadCapture(width, height, rgba));
    CHECK(width == kGoldenW);
    CHECK(height == kGoldenH);

    const std::filesystem::path referencePath =
        std::filesystem::path("ReferenceProject") / "Verify" / "References" / "inprocess-lit-cube.png";
    // Staged beside ArcaneTests.exe by the SAME {COPYDIR} of ReferenceProject/
    // Verify/ that Task 11's engine trap corpus already relies on
    // (premake5.lua's ArcaneTests postbuild) -- no premake change needed for
    // this file to be found. A missing reference is a REFUSAL, not a
    // silent re-bless: this case never writes to referencePath itself, for
    // the same reason ReferenceImagesTest.cpp and ImageCompareConformanceTest
    // .cpp both REQUIRE their fixtures exist rather than conjuring them.
    REQUIRE(std::filesystem::exists(referencePath));

    Arcane::PixelData expected;
    REQUIRE(Arcane::LoadPngRgba(referencePath, expected.width, expected.height, expected.rgba));
    REQUIRE(expected.Valid());

    Arcane::PixelData actual;
    actual.width  = width;
    actual.height = height;
    actual.rgba   = rgba;
    REQUIRE(actual.Valid());

    const auto result = Arcane::CompareImages(expected, actual);   // default: budget 0
    INFO("diffCount " << result.diffCount << " (ratio " << result.diffRatio << ") -- "
                       << result.errorMessage);
    if (!result.passed && !result.diffRgba.empty())
    {
        // Task 12 (dispatch, Step 2's own rule for the HOST-level gate, kept
        // here too): a failure that only says "failed" sends the reader
        // hunting for the artifact that would tell them why. Written under
        // Saved/ (project-gitignored -- ReferenceProject/.gitignore's
        // `Saved/*`), named after DiffArtifactPath's own convention so it
        // reads like every other diff this codebase produces.
        const std::filesystem::path diffPath =
            std::filesystem::path("ReferenceProject") / "Saved" / "Verify" / "inprocess-lit-cube-diff.png";
        std::filesystem::create_directories(diffPath.parent_path());
        if (Arcane::WritePngRgba(diffPath, result.width, result.height, result.diffRgba.data()))
            WARN("golden: mismatch -- diff artifact written to " << diffPath.string());
        else
            WARN("golden: mismatch, AND the diff artifact failed to write to " << diffPath.string());
    }
    CHECK(result.passed);

    CHECK(Arcane::RenderErrorCount() == before);
}
