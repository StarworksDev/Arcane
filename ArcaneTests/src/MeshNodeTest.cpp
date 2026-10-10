// Task 8 (F2a): pins MeshNode.hpp's NormalMatrixFor -- the inverse transpose
// of a model matrix's upper 3x3, and the fix for the defect MeshInstance::
// model used to document: a non-uniformly-scaled instance's normals need the
// inverse transpose, not the upper 3x3 mesh.hlsl's vs_main applied directly
// until this task. Pure math, no NRI device, no Registry -- same discipline
// PerspectiveCameraTest.cpp's projection-only cases take for
// SceneCamera::PerspectiveProjection (both are header-only pure functions for
// exactly that reason).
//
// The three properties task-8-brief.md (Step 1) requires:
//   1. A NON-UNIFORM scale (the brief's own worked example, scale(2,1,1))
//      sends normalize(1,1,0) to normalize(0.5,1,0) -- NOT normalize(2,1,0),
//      which is what the upper-3x3 shortcut this task removes would produce.
//   2. A UNIFORM scale leaves a normal's DIRECTION unchanged, so
//      NriGraphPixelTest.cpp's existing (Task 7) [pixel] mesh cases -- which
//      light an unscaled cube -- cannot regress silently: a normal-matrix bug
//      that broke the uniform case too would show here first, device-less.
//   3. A SINGULAR model matrix (a collapsed/zero-scaled axis) returns
//      IDENTITY, not NaN -- a NaN clip position is undefined behaviour on the
//      GPU rather than a wrong picture, the same class of guard
//      SceneCamera.hpp's degenerate-basis fallback takes.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Render/Nri/nodes/MeshNode.hpp>
#include <Arcane/Platform/Platform.hpp>

#include <Arcane/Assets/ImageCompare.hpp>
#include <Arcane/Assets/ImageIo.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Host/HostConfig.hpp>
#include <Arcane/Mesh/MeshBuilder.hpp>
#include <Arcane/Render/Nri/NriDevice.hpp>
#include <Arcane/Render/Nri/NriGraphContext.hpp>
#include <Arcane/Render/Nri/NriMeshBufferCache.hpp>
#include <Arcane/Render/RenderDeviceDesc.hpp>
#include <Arcane/Render/RenderDeviceSettings.hpp>   // RenderDebugSettings -- the per-configuration validation default
#include <Arcane/Render/RenderErrorLatch.hpp>
#include <Arcane/Render/ShaderPaths.hpp>
#include <Arcane/Scene/SceneCamera.hpp>

#include <Arcane/Render/Nri/GpuScene.hpp>   // GpuScene::ScratchRows() -- the ad-hoc overflow pin

#include <spdlog/sinks/callback_sink.h>
#include <Arcane/Base/Log.hpp>

#undef ERROR

#include <glm/glm.hpp>
#include <glm/gtc/epsilon.hpp>
#include <glm/gtc/matrix_transform.hpp>   // glm::scale

#include <algorithm>
#include <string>

#include <cmath>
#include <cstddef>       // offsetof -- the MeshRootConstants pin
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>          // MeshSceneDesc::instances
#include <type_traits>
#include <vector>

#include "Helpers/GpuCapability.hpp"

TEST_CASE("NormalMatrixFor transforms a non-uniformly-scaled normal by the "
          "inverse transpose, not the upper 3x3",
          "[mesh]")
{
    // scale(2,1,1): the brief's own worked example. A surface tangent scales
    // WITH the object, so for dot(normal, tangent) to stay zero after a
    // non-uniform scale, the normal has to scale by the INVERSE along each
    // axis -- the upper-3x3 shortcut (mesh.hlsl's defect before this task)
    // does the opposite, stretching the normal TOWARD the scaled axis instead
    // of leaning it away.
    const glm::mat4 model = glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 1.0f, 1.0f));
    const glm::mat3 normalMatrix = Arcane::NormalMatrixFor(model);

    const glm::vec3 n = glm::normalize(glm::vec3(1.0f, 1.0f, 0.0f));
    const glm::vec3 transformed = glm::normalize(normalMatrix * n);

    const glm::vec3 expectedCorrect = glm::normalize(glm::vec3(0.5f, 1.0f, 0.0f));
    const glm::vec3 wrongUpperOnly  = glm::normalize(glm::vec3(2.0f, 1.0f, 0.0f));

    CHECK(glm::epsilonEqual(transformed.x, expectedCorrect.x, 1e-5f));
    CHECK(glm::epsilonEqual(transformed.y, expectedCorrect.y, 1e-5f));
    CHECK(glm::epsilonEqual(transformed.z, expectedCorrect.z, 1e-5f));

    // NOT the upper-3x3 result -- stated explicitly (rather than just pinning
    // the correct answer) so a regression back to `glm::mat3(model)` fails
    // LOUDLY as a wrong-direction defect, not by a margin a future reader
    // might mistake for float noise on an otherwise-passing case.
    CHECK_FALSE(glm::epsilonEqual(transformed.x, wrongUpperOnly.x, 1e-3f));
}

TEST_CASE("NormalMatrixFor leaves a uniformly-scaled normal's direction "
          "unchanged (Task 7's [pixel] mesh cases must not regress)",
          "[mesh]")
{
    // A uniform scale's inverse transpose is (1/s)*I -- a positive multiple
    // of identity that vs_main's normalize() divides straight back out, so
    // the DIRECTION survives even though NormalMatrixFor now runs
    // unconditionally (every instance, not just non-uniformly-scaled ones).
    // NriGraphPixelTest.cpp's existing mesh [pixel] cases light an UNSCALED
    // cube and would not notice a normal-matrix regression confined to the
    // uniform/identity case -- this device-less case is what catches that one.
    const glm::mat4 model = glm::scale(glm::mat4(1.0f), glm::vec3(3.0f, 3.0f, 3.0f));
    const glm::mat3 normalMatrix = Arcane::NormalMatrixFor(model);

    const glm::vec3 n = glm::normalize(glm::vec3(1.0f, 2.0f, 3.0f));
    const glm::vec3 transformed = glm::normalize(normalMatrix * n);

    CHECK(glm::epsilonEqual(transformed.x, n.x, 1e-5f));
    CHECK(glm::epsilonEqual(transformed.y, n.y, 1e-5f));
    CHECK(glm::epsilonEqual(transformed.z, n.z, 1e-5f));
}

TEST_CASE("NormalMatrixFor returns identity for a singular model matrix, "
          "not NaN",
          "[mesh]")
{
    // A collapsed axis (an authored zero scale -- the same fixture shape
    // SceneCamera.hpp's own degenerate-basis test uses) has no inverse.
    // glm::inverse divides by the determinant unconditionally and would hand
    // back Inf/NaN -- undefined behaviour on the GPU rather than a wrong
    // picture, per NormalMatrixFor's own guard comment. Checked over all nine
    // elements against BOTH properties (finite AND identity), not just one:
    // finite-but-wrong would still pass a bare isfinite sweep.
    const glm::mat4 model = glm::scale(glm::mat4(1.0f), glm::vec3(0.0f, 1.0f, 1.0f));
    const glm::mat3 normalMatrix = Arcane::NormalMatrixFor(model);
    const glm::mat3 identity(1.0f);

    for (int c = 0; c < 3; ++c)
    {
        for (int r = 0; r < 3; ++r)
        {
            CHECK(std::isfinite(normalMatrix[c][r]));
            CHECK(glm::epsilonEqual(normalMatrix[c][r], identity[c][r], 1e-6f));
        }
    }
}

TEST_CASE("NormalMatrixFor is not fooled by a small NON-UNIFORM scale into "
          "falling back to identity",
          "[mesh]")
{
    // Review finding (Task 8, fix round 1): a FIXED determinant threshold is
    // not scale-invariant -- a 3x3 determinant scales as s^3 under a uniform
    // scale s, so any fixed cutoff misclassifies some genuinely-invertible
    // small matrix as singular. Concretely: scale(0.001, 0.002, 0.003) has
    // determinant 0.001*0.002*0.003 = 6e-9, comfortably below a naive 1e-8
    // cutoff (any model whose geometric-mean scale is under ~2.15mm in this
    // engine's MKS meters would trip it) -- yet this matrix is perfectly
    // invertible and non-uniform, i.e. EXACTLY the case this function has to
    // get right. A guard that fell back to identity here would silently
    // reinstate the original upper-3x3 defect at small scales, with no
    // diagnostic. NormalMatrixFor's guard checks the INVERSE'S finiteness,
    // not a determinant pre-screen, specifically so this case is not
    // singular to it.
    const glm::mat4 model = glm::scale(glm::mat4(1.0f), glm::vec3(0.001f, 0.002f, 0.003f));
    const glm::mat3 normalMatrix = Arcane::NormalMatrixFor(model);

    const glm::vec3 n = glm::normalize(glm::vec3(1.0f, 1.0f, 0.0f));
    const glm::vec3 transformed = glm::normalize(normalMatrix * n);

    // inverseTranspose(diag(a,b,c)) == diag(1/a,1/b,1/c) for a diagonal
    // matrix, applied to n gives a direction proportional to
    // (1/a, 1/b, 0) == (1000, 500, 0) -- ratio 2:1 -- normalized to
    // (2,1,0)/sqrt(5).
    const glm::vec3 expected = glm::normalize(glm::vec3(2.0f, 1.0f, 0.0f));

    CHECK(glm::epsilonEqual(transformed.x, expected.x, 1e-4f));
    CHECK(glm::epsilonEqual(transformed.y, expected.y, 1e-4f));
    CHECK(glm::epsilonEqual(transformed.z, expected.z, 1e-4f));

    // NOT identity (i.e. not `n` unchanged) -- the exact wrong answer a
    // determinant-threshold guard would have produced for this input.
    CHECK_FALSE(glm::epsilonEqual(transformed.x, n.x, 1e-3f));
}

TEST_CASE("mesh node: an instance names its mesh by GUID, not by borrowed pointer",
          "[nri]")
{
    Arcane::MeshInstance instance;
    instance.mesh = Arcane::Guid{ 1, 0 };
    CHECK(instance.mesh.IsValid());
    static_assert(std::is_same_v<decltype(Arcane::MeshInstance::mesh), Arcane::Guid>);
}

// F3 plan 1 T7: MeshSceneDesc::Empty() is "no mesh pass this frame". NOT
// empty when there are ad-hoc rows, when the registry scene EMITTED a batch,
// or (ruling R-D) when the registry scene STAGED rows even though nothing was
// emitted -- a frame whose entities are all culled still has to upload its
// dirty rows, because the mirror's lastModel history has already advanced
// past them and a never-written row would draw stale (or never-written)
// bytes the moment it comes into view. That frame declares the pass and
// records only its depth clear.
TEST_CASE("MeshSceneDesc::Empty: no ad-hoc rows, no scene draws, no staged rows", "[mesh][node]")
{
    Arcane::MeshSceneDesc d;
    CHECK(d.Empty());
    Arcane::GpuSceneFrame f;
    d.scene = &f;
    CHECK(d.Empty());                       // a frame with no emitted batches AND no staged rows is empty
    f.batches.push_back(Arcane::GpuBatchDraw{});
    CHECK_FALSE(d.Empty());                 // an emitted batch: draws
    f.batches.clear();
    CHECK(d.Empty());
    f.stage.rows.push_back(0u);             // R-D: staged rows with nothing emitted (everything culled)
    f.stage.values.push_back(Arcane::GpuInstance{});
    CHECK_FALSE(d.Empty());
    f.stage.Clear();
    CHECK(d.Empty());
    f.stage.fullRebuild = true;             // R-D: a full rebuild uploads (and stamps the generation) even with no rows
    CHECK_FALSE(d.Empty());
    f.stage.Clear();
    d.scene = nullptr;
    const Arcane::MeshInstance one{};
    d.instances = std::span<const Arcane::MeshInstance>(&one, 1);
    CHECK_FALSE(d.Empty());                 // an ad-hoc row, no scene
}

TEST_CASE("mesh node: registry refusal suppresses indirect and transparent draws without suppressing ad-hoc draws",
          "[mesh][node]")
{
    Arcane::GpuSceneFrameReadiness readiness;
    Arcane::MeshDrawSelection selected = Arcane::SelectMeshDrawPaths(
        /*registryHasDraws*/ true, /*adHocHasDraws*/ true, readiness);
    CHECK_FALSE(selected.registry);
    CHECK_FALSE(selected.adHoc);

    readiness.adHocReady = true;
    selected = Arcane::SelectMeshDrawPaths(true, true, readiness);
    CHECK_FALSE(selected.registry);
    CHECK(selected.adHoc);   // preview/scratch rendering is independent

    readiness.registryReady = true;
    selected = Arcane::SelectMeshDrawPaths(true, true, readiness);
    CHECK(selected.registry);
    CHECK(selected.adHoc);
}

TEST_CASE("MeshRootConstants is the 8-byte {firstOutput, flags} block", "[mesh][node]")
{
    // mesh.hlsl's MeshRoot, pushed once per draw: an indirect batch draw reads
    // row = g_VisibleIndices[firstOutput + SV_InstanceID]; a DIRECT draw
    // (flags & kMeshRootDirect) reads row = firstOutput itself. The 128-byte
    // MeshConstants block this replaces is gone with the per-instance push.
    CHECK(sizeof(Arcane::MeshRootConstants) == 8);
    CHECK(offsetof(Arcane::MeshRootConstants, firstOutput) == 0);
    CHECK(offsetof(Arcane::MeshRootConstants, flags) == 4);
    CHECK(Arcane::kMeshRootDirect == 1u);
    const Arcane::MeshRootConstants defaults{};
    CHECK(defaults.firstOutput == 0u);
    CHECK(defaults.flags == 0u);
}

TEST_CASE("mesh node: blend and sidedness select all six fixed pipeline states", "[mesh][node]")
{
    using Blend = Arcane::MaterialBlendMode;
    using Pixel = Arcane::MeshPixelShader;

    const struct
    {
        Blend blend;
        bool twoSided;
        Pixel pixel;
        Arcane::NriPipelineCache::GraphicsKey::Blend pipelineBlend;
        bool depthWrite;
        nri::CullMode cull;
    } cases[] = {
        { Blend::Opaque, false, Pixel::Opaque,      Arcane::NriPipelineCache::GraphicsKey::Blend::Opaque,    true,  nri::CullMode::BACK },
        { Blend::Opaque, true,  Pixel::Opaque,      Arcane::NriPipelineCache::GraphicsKey::Blend::Opaque,    true,  nri::CullMode::NONE },
        { Blend::Masked, false, Pixel::Masked,      Arcane::NriPipelineCache::GraphicsKey::Blend::Opaque,    true,  nri::CullMode::BACK },
        { Blend::Masked, true,  Pixel::Masked,      Arcane::NriPipelineCache::GraphicsKey::Blend::Opaque,    true,  nri::CullMode::NONE },
        { Blend::Transparent, false, Pixel::Transparent, Arcane::NriPipelineCache::GraphicsKey::Blend::AlphaOver, false, nri::CullMode::BACK },
        { Blend::Transparent, true,  Pixel::Transparent, Arcane::NriPipelineCache::GraphicsKey::Blend::AlphaOver, false, nri::CullMode::NONE },
    };

    for (const auto& c : cases)
    {
        const Arcane::MeshPipelineState state = Arcane::MeshNode::PipelineStateFor(c.blend, c.twoSided);
        CHECK(state.pixel == c.pixel);
        CHECK(state.blend == c.pipelineBlend);
        CHECK(state.depthWrite == c.depthWrite);
        CHECK(state.cullMode == c.cull);
    }
}

namespace
{
    constexpr std::uint32_t kParityW = 160;
    constexpr std::uint32_t kParityH = 96;

    // The parity vehicle OWNS its device pair, torn down in reverse
    // (context, wrap, native) when the case's local goes out of scope.
    //
    // It used to keep the pair in function-local statics so one case's device
    // outlived it until the next call reset it. That leaves the LAST case's
    // device to a static destructor at process exit, which runs after
    // everything registered later -- and the Vulkan validation layer is
    // dlopened (its globals registered) only once the first device is
    // created, so it was gone by then: vkDestroyDevice from __run_exit_handlers
    // spun forever inside the layer's dispatch lookup (seen on Linux with
    // VK_LAYER_KHRONOS_validation 1.4.328; the D3D12 debug layer let it pass).
    struct ParityVehicle
    {
        std::unique_ptr<Arcane::NativeDeviceOwner> native;
        std::unique_ptr<Arcane::NriDevice>         nri;
        std::unique_ptr<Arcane::NriGraphContext>   ctx;

        ParityVehicle() = default;
        ParityVehicle(ParityVehicle&&) = default;
        ParityVehicle& operator=(ParityVehicle&&) = delete;
        ~ParityVehicle()
        {
            ctx.reset();      // WRAP BEFORE OWNER: the context borrows *nri,
            nri.reset();      // which wraps *native -- releasing the owner first
            native.reset();   // under a live wrap SIGSEGV'd the next case
        }

        Arcane::NriGraphContext* operator->() const noexcept { return ctx.get(); }
        Arcane::NriGraphContext& operator*() const noexcept { return *ctx; }
    };

    ParityVehicle MakeParityContext()
    {
        Arcane::RenderDeviceDesc desc;
        desc.backend = Arcane::Test::kNativeBackend;
#if defined(ARC_BUILD_DEBUG)
        desc.enableValidation      = true;
        desc.enableD3D12DebugLayer = true;
        desc.enableSyncValidation  = true;
#endif
        ParityVehicle v;
        v.native = Arcane::NativeDeviceOwner::Create(desc);
        REQUIRE(v.native != nullptr);
        v.nri = Arcane::NriDevice::Wrap(*v.native);
        REQUIRE(v.nri != nullptr);
        Arcane::HostConfig cfg;
        cfg.backend = Arcane::Test::kNativeBackend;
        v.ctx = Arcane::NriGraphContext::CreateOffscreen(cfg, *v.nri, kParityW, kParityH, {});
        REQUIRE(v.ctx != nullptr);
        return v;
    }

    class ScopedEnvironmentValue
    {
    public:
        ScopedEnvironmentValue(const char* name, const std::string& value) : m_name(name)
        {
            if (const char* old = std::getenv(name))
            {
                m_hadOld = true;
                m_old = old;
            }
#if ARC_PLATFORM_WINDOWS
            _putenv_s(name, value.c_str());
#else
            ::setenv(name, value.c_str(), 1);
#endif
        }

        ~ScopedEnvironmentValue()
        {
#if ARC_PLATFORM_WINDOWS
            _putenv_s(m_name, m_hadOld ? m_old.c_str() : "");
#else
            // _putenv_s(name, "") REMOVES the variable; POSIX spells that unsetenv.
            if (m_hadOld) ::setenv(m_name, m_old.c_str(), 1);
            else          ::unsetenv(m_name);
#endif
        }

    private:
        const char* m_name = nullptr;
        bool m_hadOld = false;
        std::string m_old;
    };
}

TEST_CASE("mesh node: creation refuses when a fixed required shader artifact is missing", "[gpu][meshnode]")
{
    ARC_REQUIRE_BACKEND(Arcane::Test::kNativeBackend);

    const std::filesystem::path source = Arcane::ShaderPaths::ResolveFlavorDir(
        Arcane::Test::kNativeBackend, "data/shaders");
    REQUIRE_FALSE(source.empty());

    const std::filesystem::path root = std::filesystem::temp_directory_path() / "arcane-task3-missing-mesh-artifact";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root / "dxil");
    std::filesystem::copy(source, root / "dxil", std::filesystem::copy_options::recursive);
    REQUIRE(std::filesystem::remove(root / "dxil" / "mesh_masked_ps.bin"));

    Arcane::RenderDeviceDesc desc;
    desc.backend = Arcane::Test::kNativeBackend;
    desc.enableValidation = Arcane::RenderDebugSettings{}.validation;   // the per-configuration default (on in Debug)
    auto native = Arcane::NativeDeviceOwner::Create(desc);
    REQUIRE(native != nullptr);
    auto nri = Arcane::NriDevice::Wrap(*native);
    REQUIRE(nri != nullptr);
    Arcane::HostConfig config;
    config.backend = Arcane::Test::kNativeBackend;
    {
        const ScopedEnvironmentValue shaderDir("ARCANE_SHADER_DIR", root.string());
        CHECK(Arcane::NriGraphContext::CreateOffscreen(config, *nri, 16, 16, {}) == nullptr);
    }

    std::filesystem::remove_all(root, ec);
}

TEST_CASE("pixel: a cube drawn from the resident cache matches the ring's own pixels",
          "[gpu][meshnode]")
{
    ARC_REQUIRE_BACKEND(Arcane::Test::kNativeBackend);
    const std::uint64_t before = Arcane::RenderErrorCount();

    auto ctx = MakeParityContext();
    const Arcane::MeshData cube = Arcane::BuildCube(2.0f);
    const Arcane::Guid cubeId{ 1, 1 };
    ctx->SetMeshSupply(
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
    const float aspect = static_cast<float>(kParityW) / static_cast<float>(kParityH);
    scene.view = glm::lookAtRH(glm::vec3(0.0f, 0.0f, 4.0f),
                               glm::vec3(0.0f, 0.0f, 0.0f),
                               glm::vec3(0.0f, 1.0f, 0.0f));
    scene.projection     = Arcane::PerspectiveProjection(60.0f, aspect, 0.1f, 100.0f);
    scene.lightDirection = glm::vec3(0.0f, 0.0f, 1.0f);
    scene.lightColor     = glm::vec3(1.0f, 1.0f, 1.0f);
    scene.ambient        = glm::vec3(0.08f);

    Arcane::NriGraphContext::FrameDesc frame;
    frame.capture = true;
    frame.mesh    = &scene;
    REQUIRE(ctx->RenderFrameOffscreen(frame) == Arcane::NriGraphContext::FrameOutcome::Presented);

    std::uint32_t width = 0, height = 0;
    std::vector<unsigned char> rgba;
    REQUIRE(ctx->ReadCapture(width, height, rgba));

    const std::filesystem::path referencePath =
        std::filesystem::path("ReferenceProject") / "Verify" / "References" / "inprocess-lit-cube.png";
    REQUIRE(std::filesystem::exists(referencePath));
    Arcane::PixelData expected;
    REQUIRE(Arcane::LoadPngRgba(referencePath, expected.width, expected.height, expected.rgba));
    Arcane::PixelData actual;
    actual.width  = width;
    actual.height = height;
    actual.rgba   = rgba;
    const auto result = Arcane::CompareImages(expected, actual);
    INFO("diffCount " << result.diffCount << " (ratio " << result.diffRatio << ") -- "
                       << result.errorMessage);
    CHECK(result.passed);
    CHECK(Arcane::RenderErrorCount() == before);
}

// F3 plan 1 T7, review round 1: ad-hoc instances past GpuScene::ScratchRows()
// are DROPPED by MeshNode::Prepare (the sync node only ever sees the capped
// span, so GpuScene::Reserve's own overflow guard cannot fire on this path)
// and Prepare WARNS for it exactly ONCE per node, naming the dropped count.
// NOT device-free: MeshNode is only constructible through Create(context),
// so this rides the D3D12 parity vehicle and asserts on the log directly
// (the AttachLogCapture idiom from BindlessTableTest.cpp).
TEST_CASE("mesh node: ad-hoc instances past ScratchRows() are dropped by Prepare with ONE warn",
          "[gpu][meshnode][mesh][node]")
{
    ARC_REQUIRE_BACKEND(Arcane::Test::kNativeBackend);
    const std::uint64_t before = Arcane::RenderErrorCount();

    auto ctx = MakeParityContext();
    const Arcane::MeshData cube = Arcane::BuildCube(2.0f);
    const Arcane::Guid cubeId{ 1, 1 };
    ctx->SetMeshSupply(
        [&](const Arcane::Guid& id) -> Arcane::NriMeshBufferCache::SupplyResult
        {
            if (id == cubeId)
                return { &cube, Arcane::MeshResolveState::Ready };
            return { nullptr, Arcane::MeshResolveState::Failed };
        });

    // The cap is the context's latched render.gpuScene.scratchRowsPerFrame.
    REQUIRE(ctx->Scene() != nullptr);
    const std::uint32_t scratchRows = ctx->Scene()->ScratchRows();
    REQUIRE(scratchRows > 0u);

    constexpr std::size_t kOver = 3;
    std::vector<Arcane::MeshInstance> instances(scratchRows + kOver);
    for (Arcane::MeshInstance& i : instances)
        i.mesh = cubeId;
    // A nil-mesh instance is skipped BEFORE the cap and must not count as dropped.
    instances.push_back(Arcane::MeshInstance{});

    Arcane::MeshSceneDesc scene;
    scene.instances = instances;
    const float aspect = static_cast<float>(kParityW) / static_cast<float>(kParityH);
    scene.view = glm::lookAtRH(glm::vec3(0.0f, 0.0f, 4.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    scene.projection = Arcane::PerspectiveProjection(60.0f, aspect, 0.1f, 100.0f);

    int overflowWarns = 0;
    std::string lastOverflow;
    auto sink = std::make_shared<spdlog::sinks::callback_sink_mt>(
        [&](const spdlog::details::log_msg& m)
        {
            const std::string text(m.payload.data(), m.payload.size());
            if (text.find("scratch rows") != std::string::npos && text.find("MeshNode") != std::string::npos)
            {
                ++overflowWarns;
                lastOverflow = text;
            }
        });
    Arcane::Log::Engine()->sinks().push_back(sink);

    // TWO frames with the same overflow: the WARN is once per NODE, not per frame.
    for (int frame = 0; frame < 2; ++frame)
    {
        Arcane::NriGraphContext::FrameDesc fd;
        fd.mesh = &scene;
        REQUIRE(ctx->RenderFrameOffscreen(fd) == Arcane::NriGraphContext::FrameOutcome::Presented);
        REQUIRE(ctx->Mesh() != nullptr);
        CHECK(ctx->Mesh()->AdHocRows().size() == scratchRows);   // capped, never more
    }

    auto& sinks = Arcane::Log::Engine()->sinks();
    sinks.erase(std::remove(sinks.begin(), sinks.end(), sink), sinks.end());

    CHECK(overflowWarns == 1);
    CHECK(lastOverflow.find(std::to_string(kOver) + " ad-hoc instance") != std::string::npos);
    CHECK(lastOverflow.find(std::to_string(scratchRows) + " scratch rows") != std::string::npos);
    CHECK(Arcane::RenderErrorCount() == before);   // a drop is a WARN, never a latched error
}

TEST_CASE("pixel: an instance whose mesh is not resident is SKIPPED, not drawn wrong",
          "[gpu][meshnode]")
{
    ARC_REQUIRE_BACKEND(Arcane::Test::kNativeBackend);
    const std::uint64_t before = Arcane::RenderErrorCount();

    auto ctx = MakeParityContext();
    const Arcane::MeshData cube = Arcane::BuildCube(2.0f);
    const Arcane::Guid cubeId{ 1, 1 };
    const Arcane::Guid missing{ 9, 9 };
    ctx->SetMeshSupply(
        [&](const Arcane::Guid& id) -> Arcane::NriMeshBufferCache::SupplyResult
        {
            if (id == cubeId)
                return { &cube, Arcane::MeshResolveState::Ready };
            return { nullptr, Arcane::MeshResolveState::Failed };
        });

    Arcane::MeshInstance good;
    good.mesh      = cubeId;
    good.baseColor = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
    Arcane::MeshInstance bad;
    bad.mesh      = missing;
    bad.baseColor = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f);
    const Arcane::MeshInstance instances[] = { bad, good };

    Arcane::MeshSceneDesc scene;
    scene.instances = instances;
    const float aspect = static_cast<float>(kParityW) / static_cast<float>(kParityH);
    scene.view = glm::lookAtRH(glm::vec3(0.0f, 0.0f, 4.0f),
                               glm::vec3(0.0f, 0.0f, 0.0f),
                               glm::vec3(0.0f, 1.0f, 0.0f));
    scene.projection     = Arcane::PerspectiveProjection(60.0f, aspect, 0.1f, 100.0f);
    scene.lightDirection = glm::vec3(0.0f, 0.0f, 1.0f);
    scene.lightColor     = glm::vec3(1.0f, 1.0f, 1.0f);
    scene.ambient        = glm::vec3(0.08f);

    Arcane::NriGraphContext::FrameDesc frame;
    frame.capture = true;
    frame.mesh    = &scene;
    REQUIRE(ctx->RenderFrameOffscreen(frame) == Arcane::NriGraphContext::FrameOutcome::Presented);
    CHECK(Arcane::RenderErrorCount() == before);
}

TEST_CASE("pixel: two sections of one mesh draw with distinct base colours",
          "[gpu][meshnode]")
{
    ARC_REQUIRE_BACKEND(Arcane::Test::kNativeBackend);
    auto ctx = MakeParityContext();

    // Two 1 m cubes as sections of ONE mesh, offset like NriGraphPixelTest's
    // four-cube case so their silhouettes do not overlap at this camera.
    Arcane::MeshData leftCube  = Arcane::BuildCube(1.0f);
    Arcane::MeshData rightCube = Arcane::BuildCube(1.0f);
    for (Arcane::MeshVertex& v : leftCube.vertices)
        v.position.x -= 1.6f;
    for (Arcane::MeshVertex& v : rightCube.vertices)
        v.position.x += 1.6f;
    Arcane::MeshData two;
    two.vertices = leftCube.vertices;
    two.vertices.insert(two.vertices.end(), rightCube.vertices.begin(), rightCube.vertices.end());
    two.indices = leftCube.indices;
    const std::uint32_t leftIndexCount = static_cast<std::uint32_t>(leftCube.indices.size());
    const std::uint32_t vertexBase     = static_cast<std::uint32_t>(leftCube.vertices.size());
    for (std::uint32_t i : rightCube.indices)
        two.indices.push_back(i + vertexBase);
    two.sections = {
        { "L", 0, leftIndexCount, 0 },
        { "R", leftIndexCount, static_cast<std::uint32_t>(rightCube.indices.size()), 1 },
    };

    const Arcane::Guid id{ 1, 1 };
    ctx->SetMeshSupply(
        [&](const Arcane::Guid& g) -> Arcane::NriMeshBufferCache::SupplyResult
        {
            if (g == id)
                return { &two, Arcane::MeshResolveState::Ready };
            return { nullptr, Arcane::MeshResolveState::Failed };
        });

    Arcane::MeshInstance left;
    left.mesh        = id;
    left.baseColor   = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
    left.indexOffset = 0;
    left.indexCount  = leftIndexCount;
    Arcane::MeshInstance right;
    right.mesh        = id;
    right.baseColor   = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f);
    right.indexOffset = leftIndexCount;
    right.indexCount  = static_cast<std::uint32_t>(rightCube.indices.size());
    const Arcane::MeshInstance instances[] = { left, right };

    Arcane::MeshSceneDesc scene;
    scene.instances = instances;
    const float aspect = static_cast<float>(kParityW) / static_cast<float>(kParityH);
    scene.view = glm::lookAtRH(glm::vec3(0.0f, 0.0f, 4.0f),
                               glm::vec3(0.0f, 0.0f, 0.0f),
                               glm::vec3(0.0f, 1.0f, 0.0f));
    scene.projection     = Arcane::PerspectiveProjection(60.0f, aspect, 0.1f, 100.0f);
    scene.lightDirection = glm::vec3(0.0f, 0.0f, 1.0f);
    scene.lightColor     = glm::vec3(1.0f);
    scene.ambient        = glm::vec3(0.08f);

    Arcane::NriGraphContext::FrameDesc frame;
    frame.capture = true;
    frame.mesh    = &scene;
    REQUIRE(ctx->RenderFrameOffscreen(frame) == Arcane::NriGraphContext::FrameOutcome::Presented);

    std::uint32_t w = 0, h = 0;
    std::vector<unsigned char> rgba;
    REQUIRE(ctx->ReadCapture(w, h, rgba));
    REQUIRE(w == kParityW);
    REQUIRE(h == kParityH);

    const auto at = [&](std::uint32_t x, std::uint32_t y) {
        const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4u;
        return glm::ivec3(rgba[i], rgba[i + 1], rgba[i + 2]);
    };
    const glm::ivec3 leftPx  = at(40u, 48u);
    const glm::ivec3 rightPx = at(120u, 48u);
    CHECK(leftPx.r > leftPx.g + 40);
    CHECK(rightPx.g > rightPx.r + 40);
}
