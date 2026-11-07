// Plan C final review C1: a texture eviction must never leave a baked consumer holding a dead view, and a
// texture a baked consumer draws every frame must not look cold to the account. [gpu][eviction]: run BY NAME,
// one GPU suite at a time on the desk. Every case asserts RenderErrorCount() unchanged; the D3D12 debug layer
// stays forced (test_main) and Vulkan runs core + sync validation.
//
// THE CAPS. ResidencyForTests latches tiny caps BEFORE the context exists, so a 64x64 RGBA8 image (16 KiB) is
// enough to force an eviction. The reference pictures come from the blocking path on a device with the normal caps.
//
// Include order: NRI headers first, ALWAYS -- see NriCommon.hpp.
#include <NRI.h>

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Assets/ImageCompare.hpp>
#include <Arcane/Assets/ImageIo.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Host/HostConfig.hpp>
#include <Arcane/ImGui/ImGuiNriNode.hpp>
#include <Arcane/Material/MaterialInstance.hpp>
#include <Arcane/Material/MaterialSource.hpp>
#include <Arcane/Mesh/MeshBuilder.hpp>
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/FramePacing.hpp>
#include <Arcane/Render/Generated/BindlessCapacity.gen.hpp>
#include <Arcane/Render/Nri/BindlessTable.hpp>
#include <Arcane/Render/Nri/NriDevice.hpp>
#include <Arcane/Render/Nri/NriGraphContext.hpp>
#include <Arcane/Render/Nri/NriMeshBufferCache.hpp>
#include <Arcane/Render/Nri/NriTextureCache.hpp>
#include <Arcane/Render/Nri/ResidencyAccount.hpp>
#include <Arcane/Render/PostChainCache.hpp>
#include <Arcane/Render/RenderDeviceDesc.hpp>
#include <Arcane/Render/RenderErrorLatch.hpp>
#include <Arcane/Render/ShaderCompiler.hpp>
#include <Arcane/Render/ShaderConventions.hpp>
#include <Arcane/Render/ShaderSourceProvider.hpp>
#include <Arcane/Scene/SceneCamera.hpp>

#undef ERROR

#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "Helpers/GpuCapability.hpp"

namespace
{
    constexpr std::uint32_t kW = 160;
    constexpr std::uint32_t kH = 96;
    constexpr std::uint32_t kSide = 64;
    constexpr std::uint64_t kImageBytes = static_cast<std::uint64_t>(kSide) * kSide * 4u;

    using FrameOutcome = Arcane::NriGraphContext::FrameOutcome;
    using FrameDesc    = Arcane::NriGraphContext::FrameDesc;

    Arcane::Guid Id(std::uint32_t n)
    {
        char text[64];
        std::snprintf(text, sizeof(text), "f7f7f7f7-0000-0000-0000-%012x", n);
        return *Arcane::Guid::FromString(text);
    }

    // A gradient per id, so a stale or missing image cannot equal the right one.
    Arcane::PixelData MakeImage(std::uint32_t n)
    {
        Arcane::PixelData p;
        p.width = p.height = kSide;
        p.rgba.resize(static_cast<std::size_t>(kImageBytes));
        for (std::uint32_t y = 0; y < kSide; ++y)
            for (std::uint32_t x = 0; x < kSide; ++x)
            {
                unsigned char* t = p.rgba.data() + (static_cast<std::size_t>(y) * kSide + x) * 4;
                t[0] = static_cast<unsigned char>((x * 200u) / kSide + 20u * (n % 3u));
                t[1] = static_cast<unsigned char>((y * 200u) / kSide + 15u * (n % 5u));
                t[2] = static_cast<unsigned char>(40u + 50u * (n % 4u));
                t[3] = 255;
            }
        return p;
    }

    struct Images
    {
        std::map<Arcane::Guid, Arcane::PixelData> byId;
        void Add(std::uint32_t n) { byId[Id(n)] = MakeImage(n); }
        const Arcane::PixelData* Find(const Arcane::Guid& id) const
        {
            const auto it = byId.find(id);
            return it == byId.end() ? nullptr : &it->second;
        }
    };

    struct Vehicle
    {
        std::unique_ptr<Arcane::NativeDeviceOwner> native;
        std::unique_ptr<Arcane::NriDevice>         nri;
        std::unique_ptr<Arcane::NriGraphContext>   ctx;
        ~Vehicle()
        {
            ctx.reset();
            nri.reset();
            native.reset();
        }
    };

    // `caps` null = the normal latched caps (the reference pictures).
    std::unique_ptr<Vehicle> MakeVehicle(Arcane::GraphicsBackend backend, const Arcane::ResidencyCaps* caps,
                                         const Arcane::NriGraphContext::NodeSet& nodes = {})
    {
        Arcane::RenderDeviceDesc desc;
        desc.backend = backend;
#if defined(ARC_BUILD_DEBUG)
        desc.enableValidation      = true;
        desc.enableD3D12DebugLayer = true;
        desc.enableSyncValidation  = true;
#endif
        auto v = std::make_unique<Vehicle>();
        v->native = Arcane::NativeDeviceOwner::Create(desc);
        REQUIRE(v->native != nullptr);
        v->nri = Arcane::NriDevice::Wrap(*v->native);
        REQUIRE(v->nri != nullptr);
        if (caps)
            (void)v->nri->ResidencyForTests(*caps);   // BEFORE the context: its caches latch this account
        Arcane::HostConfig cfg;
        cfg.backend = backend;
        v->ctx = Arcane::NriGraphContext::CreateOffscreen(cfg, *v->nri, kW, kH, nodes);
        REQUIRE(v->ctx != nullptr);
        return v;
    }

    Arcane::ResidencyCaps TextureCaps(std::uint64_t textureCap)
    {
        Arcane::ResidencyCaps caps;
        caps.meshCap = 4096;       // no mesh in these scenes; its surplus (the half above its reservation) is small
        caps.textureCap = textureCap;
        caps.reservationPercent = 50;
        return caps;
    }

    Arcane::PixelData Capture(Arcane::NriGraphContext& ctx)
    {
        Arcane::PixelData out;
        REQUIRE(ctx.ReadCapture(out.width, out.height, out.rgba));
        return out;
    }

    bool SamePicture(const Arcane::PixelData& a, const Arcane::PixelData& b)
    {
        return Arcane::CompareImages(a, b).diffCount == 0u;
    }

    // ---- plain sprites -------------------------------------------------------------------------------------
    // One slot on screen per position in `ids`; an invalid Guid leaves that slot empty, so a sprite keeps its place
    // when others come and go and the reference needs only the final list.
    FrameDesc DrawSprites(Arcane::Batcher2D& batch, const std::vector<Arcane::Guid>& ids)
    {
        batch.Begin(kW, kH);
        batch.SetLayer(0, 0);
        batch.Rect(glm::vec2(4.0f, 4.0f), glm::vec2(30.0f, 12.0f), glm::vec4(0.1f, 0.8f, 0.2f, 1.0f));
        for (std::size_t i = 0; i < ids.size(); ++i)
            if (ids[i].IsValid())
                batch.QuadTextured(Arcane::Batcher2D::kMaterialSprite, ids[i],
                                   glm::vec2(6.0f + 38.0f * float(i), 30.0f), glm::vec2(32.0f),
                                   glm::vec2(0.0f), glm::vec2(1.0f), glm::vec4(1.0f));
        FrameDesc frame;
        frame.batch   = &batch;
        frame.capture = true;
        return frame;
    }

    Arcane::PixelData ReferenceSprites(Arcane::GraphicsBackend backend, const Images& images,
                                       const std::vector<Arcane::Guid>& ids)
    {
        auto v = MakeVehicle(backend, nullptr);
        v->ctx->SetPixelSupply([&](const Arcane::Guid& id) { return images.Find(id); });
        v->ctx->SetUploadModeForTests(Arcane::NriUploadMode::Immediate);
        auto batch = Arcane::Batcher2D::Create();
        REQUIRE(batch != nullptr);
        REQUIRE(v->ctx->RenderFrameOffscreen(DrawSprites(*batch, ids)) == FrameOutcome::Presented);
        return Capture(*v->ctx);
    }

    // Renders until nothing is pending, then far enough that every submit which could still name an evicted object
    // has retired (each capture idles, and the next frame reaps). Returns the last picture.
    Arcane::PixelData Settle(Arcane::NriGraphContext& ctx, const std::function<FrameDesc()>& draw)
    {
        const std::uint32_t pastTheFence = Arcane::FramesInFlight() + 2u;
        Arcane::PixelData last;
        std::uint32_t settled = 0;
        for (int frame = 0; frame < 32 && settled < pastTheFence; ++frame)
        {
            REQUIRE(ctx.RenderFrameOffscreen(draw()) == FrameOutcome::Presented);
            last = Capture(ctx);
            settled = ctx.HasPendingUploads() ? 0u : settled + 1u;
        }
        REQUIRE(settled >= pastTheFence);
        return last;
    }

    // ---- 1. A sprite drawn every frame survives another image's admission -----------------------------------
    void CheckDrawnSpriteSurvives(Arcane::GraphicsBackend backend)
    {
        const std::uint64_t errorsBefore = Arcane::RenderErrorCount();
        Images images;
        images.Add(1);
        images.Add(2);
        const Arcane::Guid s = Id(1), t = Id(2);
        const Arcane::PixelData reference = ReferenceSprites(backend, images, { s, t });

        // One image fits, two do not.
        const Arcane::ResidencyCaps caps = TextureCaps(kImageBytes + 4096u);
        auto v = MakeVehicle(backend, &caps);
        v->ctx->SetPixelSupply([&](const Arcane::Guid& id) { return images.Find(id); });
        auto batch = Arcane::Batcher2D::Create();
        REQUIRE(batch != nullptr);

        (void)Settle(*v->ctx, [&] { return DrawSprites(*batch, { s, Arcane::Guid{} }); });
        CHECK(v->ctx->Textures()->View(s) != nullptr);

        // T is admitted while S is on screen. S is DRAWN, so it is protected: the over-budget frame renders intact.
        const Arcane::PixelData picture = Settle(*v->ctx, [&] { return DrawSprites(*batch, { s, t }); });
        CHECK(v->ctx->Textures()->View(s) != nullptr);
        CHECK(v->ctx->Textures()->View(t) != nullptr);
        CHECK(SamePicture(picture, reference));
        CHECK(Arcane::RenderErrorCount() == errorsBefore);
    }

    // ---- 2. The victim is the cold image, not the older one a baked consumer still draws --------------------
    void CheckColdImageIsTheVictim(Arcane::GraphicsBackend backend)
    {
        const std::uint64_t errorsBefore = Arcane::RenderErrorCount();
        Images images;
        images.Add(1);
        images.Add(2);
        images.Add(3);
        // S has the smaller Guid, so on a stamp tie (both first bound on one frame) a stampless S is chosen.
        const Arcane::Guid s = Id(1), cold = Id(2), t = Id(3);
        const Arcane::PixelData reference = ReferenceSprites(backend, images, { s, Arcane::Guid{}, t });

        // Two images fit, three do not.
        const Arcane::ResidencyCaps caps = TextureCaps(kImageBytes * 2u + 4096u);
        auto v = MakeVehicle(backend, &caps);
        v->ctx->SetPixelSupply([&](const Arcane::Guid& id) { return images.Find(id); });
        auto batch = Arcane::Batcher2D::Create();
        REQUIRE(batch != nullptr);

        (void)Settle(*v->ctx, [&] { return DrawSprites(*batch, { s, cold }); });
        // `cold` leaves the screen, S stays.
        (void)Settle(*v->ctx, [&] { return DrawSprites(*batch, { s, Arcane::Guid{} }); });
        const Arcane::PixelData picture = Settle(*v->ctx, [&] { return DrawSprites(*batch, { s, Arcane::Guid{}, t }); });

        CHECK(v->ctx->Textures()->View(s) != nullptr);          // drawn every frame: kept
        CHECK(v->ctx->Textures()->View(cold) == nullptr);       // cold: the victim
        CHECK(v->ctx->Textures()->View(t) != nullptr);
        CHECK(SamePicture(picture, reference));
        CHECK(Arcane::RenderErrorCount() == errorsBefore);
    }

    // ---- 3. An evicted sprite comes back: its baked set is replaced, it never binds the dead view -----------
    void CheckEvictedSpriteRecovers(Arcane::GraphicsBackend backend)
    {
        const std::uint64_t errorsBefore = Arcane::RenderErrorCount();
        Images images;
        images.Add(1);
        images.Add(2);
        const Arcane::Guid s = Id(1), t = Id(2);
        const Arcane::PixelData reference = ReferenceSprites(backend, images, { s });

        const Arcane::ResidencyCaps caps = TextureCaps(kImageBytes + 4096u);
        auto v = MakeVehicle(backend, &caps);
        v->nri->EnableDeviceWaitIdleCounting();
        v->ctx->SetPixelSupply([&](const Arcane::Guid& id) { return images.Find(id); });
        auto batch = Arcane::Batcher2D::Create();
        REQUIRE(batch != nullptr);

        (void)Settle(*v->ctx, [&] { return DrawSprites(*batch, { s }); });
        // S leaves the screen and T evicts it. Past the fence its view is gone.
        (void)Settle(*v->ctx, [&] { return DrawSprites(*batch, { Arcane::Guid{}, t }); });
        CHECK(v->ctx->Textures()->View(s) == nullptr);

        // S is drawn again: re-queued, uploaded, a new set. No device idle, and no frame binds the dead view.
        int framesWithoutIdleRise = 0, frames = 0;
        std::uint32_t settled = 0;
        const std::uint32_t pastTheFence = Arcane::FramesInFlight() + 2u;
        Arcane::PixelData picture;
        for (int f = 0; f < 32 && settled < pastTheFence; ++f)
        {
            const std::uint64_t idleBefore = Arcane::NriDevice::DeviceWaitIdleCount();
            REQUIRE(v->ctx->RenderFrameOffscreen(DrawSprites(*batch, { s })) == FrameOutcome::Presented);
            framesWithoutIdleRise += Arcane::NriDevice::DeviceWaitIdleCount() == idleBefore ? 1 : 0;
            ++frames;
            picture = Capture(*v->ctx);
            settled = v->ctx->HasPendingUploads() ? 0u : settled + 1u;
        }
        CHECK(framesWithoutIdleRise == frames);
        CHECK(v->ctx->Textures()->View(s) != nullptr);
        CHECK(SamePicture(picture, reference));
        CHECK(Arcane::RenderErrorCount() == errorsBefore);
    }

    // ---- registered materials ------------------------------------------------------------------------------
    struct MaterialKit
    {
        std::shared_ptr<const Arcane::MaterialTemplate> templ;
        std::shared_ptr<const Arcane::MaterialInstance> instance;
        std::shared_ptr<const std::vector<std::uint8_t>> vs, ps;
    };

    // One declared image, and the sprite's own t0: both show in the picture.
    MaterialKit CompileDetailMaterial(Arcane::GraphicsBackend backend, const Arcane::Guid& detail)
    {
        Arcane::ShaderSourceProvider provider;
        provider.AddRoot("data/shaders");
        const auto templateText = provider.Get(Arcane::MaterialTemplateFile(Arcane::MaterialSurface::Sprite));
        REQUIRE(templateText.has_value());
        const Arcane::MaterialBuildResult build = Arcane::BuildMaterialShaderSource(
            *templateText,
            "//@param texture Detail\n"
            "float4 shade(Varyings v) { float4 a = Detail.Sample(MaterialSampler, v.uv); "
            "float4 b = SpriteTexture.Sample(MaterialSampler, v.uv); return float4(a.r, b.g, 0.5 * (a.b + b.b), 1.0); }",
            "eviction_detail", Arcane::MaterialSurface::Sprite);
        REQUIRE(build.errors.empty());

        Arcane::ShaderCompiler compiler;
        REQUIRE(compiler.InitializeWithDebounce(0.0));
        const auto compileStage = [&](const char* entry, const char* profile)
        {
            Arcane::ShaderCompileRequest request;
            request.debugName = "eviction_detail.hlsl";
            request.sourceUtf8 = build.hlsl;
            request.entry = entry;
            request.profile = profile;
            return compiler.CompileNow(request);
        };
        const Arcane::ShaderCompileResult vs = compileStage(Arcane::kVsEntry, Arcane::kVsProfile);
        const Arcane::ShaderCompileResult ps = compileStage(Arcane::kPsEntry, Arcane::kPsProfile);
        const bool vulkan = backend == Arcane::GraphicsBackend::Vulkan;
        const Arcane::ShaderTargetResult& vsTarget = vulkan ? vs.spirv : vs.dxil;
        const Arcane::ShaderTargetResult& psTarget = vulkan ? ps.spirv : ps.dxil;
        REQUIRE(vsTarget.succeeded);
        REQUIRE(psTarget.succeeded);

        MaterialKit kit;
        kit.templ = std::make_shared<const Arcane::MaterialTemplate>(build.templ);
        auto instance = std::make_shared<Arcane::MaterialInstance>(kit.templ);
        REQUIRE(instance->SetTexture("Detail", detail));
        kit.instance = instance;
        kit.vs = std::make_shared<const std::vector<std::uint8_t>>(vsTarget.bytecode);
        kit.ps = std::make_shared<const std::vector<std::uint8_t>>(psTarget.bytecode);
        compiler.Shutdown();
        return kit;
    }

    std::uint16_t Register(Arcane::Batcher2D& batch, const MaterialKit& kit)
    {
        Arcane::Material2DDesc desc;
        desc.templ = kit.templ;
        desc.instance = kit.instance;
        desc.vsBytes = kit.vs;
        desc.psBytes = kit.ps;
        const std::uint16_t id = batch.RegisterMaterial(std::move(desc));
        REQUIRE(id != Arcane::Batcher2D::kInvalidMaterialId);
        return id;
    }

    FrameDesc DrawMaterial(Arcane::Batcher2D& batch, std::uint16_t material, const Arcane::Guid& sprite)
    {
        batch.Begin(kW, kH);
        batch.SetLayer(0, 0);
        batch.Rect(glm::vec2(4.0f, 4.0f), glm::vec2(30.0f, 12.0f), glm::vec4(0.1f, 0.8f, 0.2f, 1.0f));
        batch.QuadTextured(material, sprite, glm::vec2(44.0f, 30.0f), glm::vec2(48.0f), glm::vec2(0.0f), glm::vec2(1.0f),
                           glm::vec4(1.0f));
        FrameDesc frame;
        frame.batch   = &batch;
        frame.capture = true;
        return frame;
    }

    enum class MaterialVictim { Declared, SpriteVariant };

    // ---- 4. An evicted declared param / sprite variant of a material comes back through the rebuild or a new set ----
    void CheckMaterialRecovers(Arcane::GraphicsBackend backend, MaterialVictim which)
    {
        const std::uint64_t errorsBefore = Arcane::RenderErrorCount();
        Images images;
        images.Add(1);
        images.Add(2);
        images.Add(3);
        // Equal stamps evict the smaller Guid first, so the Guid picks the victim.
        const Arcane::Guid victim = Id(1), survivor = Id(2), t = Id(3);
        const Arcane::Guid detail = which == MaterialVictim::Declared ? victim : survivor;
        const Arcane::Guid sprite = which == MaterialVictim::Declared ? survivor : victim;
        const MaterialKit kit = CompileDetailMaterial(backend, detail);
        const auto supply = [&](const Arcane::Guid& id) { return images.Find(id); };

        Arcane::PixelData reference;
        {
            auto v = MakeVehicle(backend, nullptr);
            v->ctx->SetPixelSupply(supply);
            v->ctx->SetUploadModeForTests(Arcane::NriUploadMode::Immediate);
            auto batch = Arcane::Batcher2D::Create();
            REQUIRE(batch != nullptr);
            const std::uint16_t material = Register(*batch, kit);
            REQUIRE(v->ctx->RenderFrameOffscreen(DrawMaterial(*batch, material, sprite)) == FrameOutcome::Presented);
            reference = Capture(*v->ctx);
        }

        // Two images fit, three do not.
        const Arcane::ResidencyCaps caps = TextureCaps(kImageBytes * 2u + 4096u);
        auto v = MakeVehicle(backend, &caps);
        v->ctx->SetPixelSupply(supply);
        auto batch = Arcane::Batcher2D::Create();
        REQUIRE(batch != nullptr);
        const std::uint16_t material = Register(*batch, kit);

        const Arcane::PixelData first = Settle(*v->ctx, [&] { return DrawMaterial(*batch, material, sprite); });
        CHECK(SamePicture(first, reference));

        // The material leaves the screen, T evicts the victim.
        (void)Settle(*v->ctx, [&] { return DrawSprites(*batch, { t }); });
        CHECK(v->ctx->Textures()->View(victim) == nullptr);

        // Back on screen: it holds or skips while the image is queued, then draws the same picture.
        const Arcane::PixelData again = Settle(*v->ctx, [&] { return DrawMaterial(*batch, material, sprite); });
        CHECK(v->ctx->Textures()->View(victim) != nullptr);
        CHECK(SamePicture(again, reference));
        CHECK(Arcane::RenderErrorCount() == errorsBefore);
    }

    // ---- 5. THE 1-THEN-1 HOLE: the image is evicted while the material is off screen, and ANOTHER consumer re-resolves
    // it before the material returns. "Is it bindable" reads yes both times; the view is a different one. ----
    void CheckMaterialSeesAChangedView(Arcane::GraphicsBackend backend)
    {
        const std::uint64_t errorsBefore = Arcane::RenderErrorCount();
        Images images;
        images.Add(1);
        images.Add(2);
        images.Add(3);
        const Arcane::Guid detail = Id(1), sprite = Id(2), t = Id(3);
        const MaterialKit kit = CompileDetailMaterial(backend, detail);
        const auto supply = [&](const Arcane::Guid& id) { return images.Find(id); };

        Arcane::PixelData reference;
        {
            auto v = MakeVehicle(backend, nullptr);
            v->ctx->SetPixelSupply(supply);
            v->ctx->SetUploadModeForTests(Arcane::NriUploadMode::Immediate);
            auto batch = Arcane::Batcher2D::Create();
            REQUIRE(batch != nullptr);
            const std::uint16_t material = Register(*batch, kit);
            REQUIRE(v->ctx->RenderFrameOffscreen(DrawMaterial(*batch, material, sprite)) == FrameOutcome::Presented);
            reference = Capture(*v->ctx);
        }

        const Arcane::ResidencyCaps caps = TextureCaps(kImageBytes * 2u + 4096u);
        auto v = MakeVehicle(backend, &caps);
        v->nri->EnableDeviceWaitIdleCounting();
        v->ctx->SetPixelSupply(supply);
        auto batch = Arcane::Batcher2D::Create();
        REQUIRE(batch != nullptr);
        const std::uint16_t material = Register(*batch, kit);

        (void)Settle(*v->ctx, [&] { return DrawMaterial(*batch, material, sprite); });
        (void)Settle(*v->ctx, [&] { return DrawSprites(*batch, { t }); });          // evicts `detail` (smallest Guid)
        CHECK(v->ctx->Textures()->View(detail) == nullptr);
        (void)Settle(*v->ctx, [&] { return DrawSprites(*batch, { detail }); });     // a plain sprite re-resolves it
        CHECK(v->ctx->Textures()->View(detail) != nullptr);

        // The material returns with its image bindable. The baked set names the DEAD view, so it must rebuild.
        std::uint64_t idles = 0;
        Arcane::PixelData picture;
        for (int f = 0; f < 8; ++f)
        {
            const std::uint64_t idleBefore = Arcane::NriDevice::DeviceWaitIdleCount();
            REQUIRE(v->ctx->RenderFrameOffscreen(DrawMaterial(*batch, material, sprite)) == FrameOutcome::Presented);
            idles += Arcane::NriDevice::DeviceWaitIdleCount() - idleBefore;
            picture = Capture(*v->ctx);
        }
        CHECK(idles >= 1u);   // the rebuild, behind the idle the Task 5 ruling accepts for an eviction
        CHECK(SamePicture(picture, reference));
        CHECK(Arcane::RenderErrorCount() == errorsBefore);
    }

    // ---- post chains ---------------------------------------------------------------------------------------
    Arcane::PostChainDesc CompilePostChain(Arcane::GraphicsBackend backend, const Arcane::Guid& detail)
    {
        Arcane::ShaderSourceProvider provider;
        provider.AddRoot("data/shaders");
        const auto templateText = provider.Get("materials/fullscreen_material.hlsl");
        REQUIRE(templateText.has_value());
        const char* shade =
            "//@param texture Detail\n"
            "float4 shade(Varyings v) { return InputTexture.Sample(MaterialSampler, v.uv) * Detail.Sample(MaterialSampler, v.uv); }\n";
        const std::uint32_t sceneIn[] = { Arcane::kSceneInput };
        const Arcane::MaterialChainPassDesc authored[] = { { shade, sceneIn } };
        Arcane::MaterialChainBuildResult built = Arcane::BuildMaterialChainSource(
            *templateText, authored, "eviction_post_detail", {}, /*externalInput=*/true);
        REQUIRE(built.Ok());

        Arcane::ShaderCompiler compiler;
        REQUIRE(compiler.InitializeWithDebounce(0.0));
        const auto compileStage = [&](const char* entry, const char* profile)
        {
            Arcane::ShaderCompileRequest req;
            req.debugName = "eviction_post_detail.hlsl";
            req.sourceUtf8 = built.hlsl[0];
            req.entry = entry;
            req.profile = profile;
            const Arcane::ShaderCompileResult cr = compiler.CompileNow(req);
            REQUIRE(cr.AllSucceeded());
            const Arcane::ShaderTargetResult& target = backend == Arcane::GraphicsBackend::D3D12 ? cr.dxil : cr.spirv;
            REQUIRE(target.succeeded);
            return std::make_shared<const std::vector<std::uint8_t>>(target.bytecode);
        };
        auto templ = std::make_shared<Arcane::MaterialTemplate>(std::move(built.templ));
        auto instance = std::make_shared<Arcane::MaterialInstance>(std::shared_ptr<const Arcane::MaterialTemplate>(templ));
        REQUIRE(instance->SetTexture("Detail", detail));
        Arcane::PostChainDesc chain;
        chain.templ = templ;
        chain.instance = instance;
        chain.chainInputSlots = built.chainInputSlots;
        Arcane::PostChainPassDesc pass;
        pass.vsBytes = compileStage(Arcane::kVsEntry, Arcane::kVsProfile);
        pass.psBytes = compileStage(Arcane::kPsEntry, Arcane::kPsProfile);
        pass.inputs = built.passInputs[0];
        chain.passes.push_back(std::move(pass));
        compiler.Shutdown();
        return chain;
    }

    FrameDesc DrawThroughChain(Arcane::Batcher2D& batch, const Arcane::PostChainDesc& chain)
    {
        batch.Begin(kW, kH);
        batch.SetLayer(0, 0);
        batch.Rect(glm::vec2(4.0f, 4.0f), glm::vec2(30.0f, 12.0f), glm::vec4(0.1f, 0.8f, 0.2f, 1.0f));
        batch.Rect(glm::vec2(50.0f, 20.0f), glm::vec2(80.0f, 50.0f), glm::vec4(0.9f, 0.9f, 0.3f, 1.0f));
        FrameDesc frame;
        frame.batch   = &batch;
        frame.post    = &chain;
        frame.capture = true;
        return frame;
    }

    // ---- 6. A post chain's declared image: evicted while the chain is off, and the 1-then-1 hole -------------
    void CheckPostChainRecovers(Arcane::GraphicsBackend backend, bool anotherConsumerReResolves)
    {
        const std::uint64_t errorsBefore = Arcane::RenderErrorCount();
        Images images;
        images.Add(1);
        images.Add(2);
        const Arcane::Guid detail = Id(1), t = Id(2);
        const Arcane::PostChainDesc chain = CompilePostChain(backend, detail);
        const auto supply = [&](const Arcane::Guid& id) { return images.Find(id); };

        Arcane::PixelData reference;
        {
            auto v = MakeVehicle(backend, nullptr);
            v->ctx->SetPixelSupply(supply);
            v->ctx->SetUploadModeForTests(Arcane::NriUploadMode::Immediate);
            auto batch = Arcane::Batcher2D::Create();
            REQUIRE(batch != nullptr);
            REQUIRE(v->ctx->RenderFrameOffscreen(DrawThroughChain(*batch, chain)) == FrameOutcome::Presented);
            reference = Capture(*v->ctx);
        }

        const Arcane::ResidencyCaps caps = TextureCaps(kImageBytes + 4096u);
        auto v = MakeVehicle(backend, &caps);
        v->nri->EnableDeviceWaitIdleCounting();
        v->ctx->SetPixelSupply(supply);
        auto batch = Arcane::Batcher2D::Create();
        REQUIRE(batch != nullptr);

        const Arcane::PixelData first = Settle(*v->ctx, [&] { return DrawThroughChain(*batch, chain); });
        CHECK(SamePicture(first, reference));

        (void)Settle(*v->ctx, [&] { return DrawSprites(*batch, { t }); });    // the chain is off; T evicts `detail`
        CHECK(v->ctx->Textures()->View(detail) == nullptr);
        if (anotherConsumerReResolves)
        {
            (void)Settle(*v->ctx, [&] { return DrawSprites(*batch, { detail }); });
            CHECK(v->ctx->Textures()->View(detail) != nullptr);
        }

        std::uint64_t idles = 0;
        Arcane::PixelData picture;
        for (int f = 0; f < 8; ++f)
        {
            const std::uint64_t idleBefore = Arcane::NriDevice::DeviceWaitIdleCount();
            REQUIRE(v->ctx->RenderFrameOffscreen(DrawThroughChain(*batch, chain)) == FrameOutcome::Presented);
            idles += Arcane::NriDevice::DeviceWaitIdleCount() - idleBefore;
            picture = Capture(*v->ctx);
        }
        CHECK(idles >= 1u);   // the rebuild, behind the idle the Task 5 ruling accepts for an eviction
        CHECK(SamePicture(picture, reference));
        CHECK(Arcane::RenderErrorCount() == errorsBefore);
    }

    // ---- 7. ImGui: the drop path takes the backend's cached entry with it -----------------------------------
    struct ImGuiScope
    {
        ImGuiContext* previous = nullptr;
        ImGuiContext* imgui = nullptr;
        ImGuiScope()
        {
            previous = ImGui::GetCurrentContext();
            imgui = ImGui::CreateContext();
            REQUIRE(imgui != nullptr);
            ImGui::SetCurrentContext(imgui);
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.DisplaySize = ImVec2(float(kW), float(kH));
            io.DeltaTime = 1.0f / 60.0f;
        }
        ~ImGuiScope()
        {
            ImGui::DestroyContext(imgui);
            ImGui::SetCurrentContext(previous);
        }
    };

    void CheckImGuiEntryFollowsTheDrop(Arcane::GraphicsBackend backend)
    {
        const std::uint64_t errorsBefore = Arcane::RenderErrorCount();
        constexpr auto kDisplay = Arcane::NriTextureCache::ColorSpace::Display;
        Images images;
        images.Add(1);
        images.Add(2);
        images.Add(3);
        const Arcane::Guid logo = Id(1), thumb = Id(2), t = Id(3);

        ImGuiScope scope;
        Arcane::NriGraphContext::NodeSet nodes;
        nodes.hostHud = true;
        // Two images fit, three do not.
        const Arcane::ResidencyCaps caps = TextureCaps(kImageBytes * 2u + 4096u);
        auto v = MakeVehicle(backend, &caps, nodes);
        auto* const hud = dynamic_cast<Arcane::ImGuiNriNode*>(v->ctx->ImGuiHud());
        REQUIRE(hud != nullptr);
        hud->AdoptImGuiContext(scope.imgui);
        v->ctx->SetPixelSupply([&](const Arcane::Guid& id) { return images.Find(id); });
        Arcane::NriTextureCache& cache = *v->ctx->Textures();
        auto batch = Arcane::Batcher2D::Create();
        REQUIRE(batch != nullptr);

        // What a raw-handle holder does (the toolbar logo, a thumbnail): ask every frame it draws, draw what it is told.
        const auto frameOf = [&](const std::vector<Arcane::Guid>& ids) -> nri::Texture*
        {
            ImGui::NewFrame();
            ImDrawList* draw = ImGui::GetForegroundDrawList();
            nri::Texture* last = nullptr;
            float x = 8.0f;
            for (const Arcane::Guid& id : ids)
            {
                if (nri::Texture* texture = cache.Resolve(id, kDisplay))
                {
                    draw->AddImage((ImTextureID)(intptr_t)texture, ImVec2(x, 8.0f), ImVec2(x + 24.0f, 32.0f));
                    last = texture;
                }
                x += 28.0f;
            }
            ImGui::Render();
            batch->Begin(kW, kH);
            FrameDesc frame;
            frame.batch   = batch.get();
            frame.imgui   = ImGui::GetDrawData();
            frame.capture = true;
            REQUIRE(v->ctx->RenderFrameOffscreen(frame) == FrameOutcome::Presented);
            (void)Capture(*v->ctx);
            return last;
        };

        const std::uint32_t pastTheFence = Arcane::FramesInFlight() + 2u;
        nri::Texture* oldThumb = nullptr;
        for (std::uint32_t f = 0; f < pastTheFence + 4u; ++f)
            oldThumb = frameOf({ logo, thumb });
        REQUIRE(oldThumb != nullptr);
        REQUIRE(hud->Renderer().HasEntryFor(oldThumb));
        nri::Texture* const logoTexture = cache.Resolve(logo, kDisplay);
        REQUIRE(logoTexture != nullptr);

        // The thumbnail leaves the screen, the logo stays, T evicts the thumbnail.
        for (std::uint32_t f = 0; f < pastTheFence + 4u; ++f)
            (void)frameOf({ logo, t });
        CHECK(cache.Resolve(logo, kDisplay) == logoTexture);   // asked every frame: never the victim
        CHECK(cache.View(thumb, kDisplay) == nullptr);
        CHECK_FALSE(hud->Renderer().HasEntryFor(oldThumb));    // the cached entry went with the drop

        // The thumbnail is drawn again: a new texture and a new entry, whatever address it landed on.
        nri::Texture* newThumb = nullptr;
        for (std::uint32_t f = 0; f < pastTheFence + 4u; ++f)
            newThumb = frameOf({ logo, thumb });
        CHECK(newThumb != nullptr);
        CHECK(hud->Renderer().HasEntryFor(newThumb));
        CHECK(Arcane::RenderErrorCount() == errorsBefore);

        v->ctx.reset();
    }

    // ---- meshes that sample through bindless slots ------------------------------------------------------------
    // What SceneRenderResolver and MeshMaterialCache do per frame, without a scene: stamp, re-acquire whatever the
    // device released, flush, draw one cube per albedo.
    struct AlbedoScene
    {
        std::vector<Arcane::Guid>  ids;
        std::vector<std::uint32_t> slots;
        Arcane::MeshData           cube = Arcane::BuildCube(1.0f);
        const Arcane::Guid         meshId = *Arcane::Guid::FromString("f8f8f8f8-0000-0000-0000-000000000001");
        int                        released = 0;

        void Install(Vehicle& v, const Images& images)
        {
            v.ctx->SetPixelSupply([&images](const Arcane::Guid& id) { return images.Find(id); });
            v.ctx->SetMeshSupply([this](const Arcane::Guid& id) -> Arcane::NriMeshBufferCache::SupplyResult {
                if (id == meshId)
                    return { &cube, Arcane::MeshResolveState::Ready };
                return { nullptr, Arcane::MeshResolveState::Failed };
            });
            Arcane::BindlessTable* table = v.nri->Bindless();
            REQUIRE(table != nullptr);
            table->SetObserver(this, [this](std::uint32_t slot)
            {
                for (std::uint32_t& held : slots)
                    if (held == slot)
                    {
                        held = 0xFFFFFFFFu;
                        ++released;
                    }
            });
        }

        void Add(const Arcane::Guid& id)
        {
            ids.push_back(id);
            slots.push_back(0xFFFFFFFFu);
        }

        FrameOutcome Frame(Arcane::NriGraphContext& ctx, Arcane::Batcher2D& batch)
        {
            ctx.StampReferencedMeshAlbedos();
            for (std::size_t i = 0; i < ids.size(); ++i)
                if (slots[i] == 0xFFFFFFFFu)
                    slots[i] = ctx.AcquireMeshAlbedoSlot(ids[i]);
            ctx.FlushMeshAlbedoReleases();

            std::vector<Arcane::MeshInstance> instances;
            for (std::size_t i = 0; i < ids.size(); ++i)
            {
                Arcane::MeshInstance instance;
                instance.mesh = meshId;
                instance.model = glm::translate(glm::mat4(1.0f), glm::vec3(-3.0f + 1.5f * float(i), 0.0f, 0.0f));
                instance.baseColor = glm::vec4(1.0f);
                instance.materialSlot = slots[i];
                instances.push_back(instance);
            }
            Arcane::MeshSceneDesc scene;
            scene.instances = instances;
            scene.view = glm::lookAtRH(glm::vec3(0.0f, 0.0f, 6.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
            scene.projection     = Arcane::PerspectiveProjection(60.0f, float(kW) / float(kH), 0.1f, 100.0f);
            scene.lightDirection = glm::vec3(0.0f, 0.0f, 1.0f);
            scene.lightColor     = glm::vec3(1.0f);
            scene.ambient        = glm::vec3(0.08f);

            batch.Begin(kW, kH);
            FrameDesc frame;
            frame.batch   = &batch;
            frame.mesh    = &scene;
            frame.capture = true;
            return ctx.RenderFrameOffscreen(frame);
        }
    };

    Arcane::PixelData ReferenceAlbedos(Arcane::GraphicsBackend backend, const Images& images,
                                       const std::vector<Arcane::Guid>& ids)
    {
        auto v = MakeVehicle(backend, nullptr);
        v->ctx->SetUploadModeForTests(Arcane::NriUploadMode::Immediate);
        AlbedoScene scene;
        for (const Arcane::Guid& id : ids)
            scene.Add(id);
        scene.Install(*v, images);
        auto batch = Arcane::Batcher2D::Create();
        REQUIRE(batch != nullptr);
        REQUIRE(scene.Frame(*v->ctx, *batch) == FrameOutcome::Presented);
        const Arcane::PixelData picture = Capture(*v->ctx);
        v->nri->Bindless()->RemoveObserver(&scene);
        return picture;
    }

    // ---- 8. I1: a visible albedo set over the allowance renders intact, with no churn --------------------------
    void CheckVisibleAlbedosDoNotChurn(Arcane::GraphicsBackend backend)
    {
        const std::uint64_t errorsBefore = Arcane::RenderErrorCount();
        Images images;
        std::vector<Arcane::Guid> ids;
        for (std::uint32_t n = 1; n <= 5; ++n)
        {
            images.Add(n);
            ids.push_back(Id(n));
        }
        const Arcane::PixelData reference = ReferenceAlbedos(backend, images, ids);

        // Three images fit and five are visible. The mesh slice lends nothing (a 100% reservation).
        Arcane::ResidencyCaps caps;
        caps.meshCap = 1ull << 20;
        caps.textureCap = kImageBytes * 3u + 4096u;
        caps.reservationPercent = 100;
        auto v = MakeVehicle(backend, &caps);
        AlbedoScene scene;
        for (std::size_t i = 0; i < 4; ++i)
            scene.Add(ids[i]);
        scene.Install(*v, images);
        auto batch = Arcane::Batcher2D::Create();
        REQUIRE(batch != nullptr);

        // Four visible, all charged while they were all drawn: over budget already, and kept.
        for (int f = 0; f < 8; ++f)
        {
            REQUIRE(scene.Frame(*v->ctx, *batch) == FrameOutcome::Presented);
            (void)Capture(*v->ctx);
        }
        CHECK(scene.released == 0);

        // The fifth appears. Its Charge must not take a visible albedo for a cold one, or the re-resolve of that one
        // evicts the next, for ever.
        scene.Add(ids[4]);
        Arcane::PixelData picture;
        for (int f = 0; f < 16; ++f)
        {
            REQUIRE(scene.Frame(*v->ctx, *batch) == FrameOutcome::Presented);
            picture = Capture(*v->ctx);
        }
        CHECK(scene.released == 0);
        CHECK(v->ctx->Textures()->ObjectCreateCount() == 5u);   // no re-upload: one texture per image, ever
        CHECK(SamePicture(picture, reference));
        CHECK(Arcane::RenderErrorCount() == errorsBefore);
        v->nri->Bindless()->RemoveObserver(&scene);
    }

    // ---- 9. I2: a table full of unreferenced mappings does not starve a referenced acquire --------------------
    void CheckFullTableServesAReferencedAcquire(Arcane::GraphicsBackend backend)
    {
        const std::uint64_t errorsBefore = Arcane::RenderErrorCount();
        Images images;
        images.Add(1);
        for (std::uint32_t n = 0; n < Arcane::kBindlessCapacity; ++n)
            images.Add(1000u + n);
        const Arcane::Guid wanted = Id(1);
        const Arcane::Guid newest = Id(1000u + Arcane::kBindlessCapacity - 1u);
        const Arcane::PixelData reference = ReferenceAlbedos(backend, images, { newest, wanted });

        auto v = MakeVehicle(backend, nullptr);
        v->ctx->SetUploadModeForTests(Arcane::NriUploadMode::Immediate);
        AlbedoScene scene;
        scene.Install(*v, images);
        auto batch = Arcane::Batcher2D::Create();
        REQUIRE(batch != nullptr);

        // What scrolling an asset browser through textured materials did: plain resolves, with no reference.
        Arcane::BindlessTable* table = v->nri->Bindless();
        for (std::uint32_t n = 0; n < Arcane::kBindlessCapacity; ++n)
            REQUIRE(v->ctx->ResolveMeshAlbedoSlot(Id(1000u + n)) != Arcane::BindlessTable::kInvalidSlot);
        REQUIRE(table->LiveCount() == Arcane::kBindlessCapacity);

        // A frame in flight samples every live slot, so the index that gets reclaimed has a reader to wait for.
        scene.Add(newest);
        for (int f = 0; f < 2; ++f)
            REQUIRE(scene.Frame(*v->ctx, *batch) == FrameOutcome::Presented);

        // The referenced acquire: no capture between these frames, so it may take a few of them. It never idles the
        // table's retire rule away, and the caller only re-asks.
        scene.Add(wanted);
        Arcane::PixelData picture;
        for (int f = 0; f < 8; ++f)
        {
            REQUIRE(scene.Frame(*v->ctx, *batch) == FrameOutcome::Presented);
            picture = Capture(*v->ctx);
        }
        CHECK(scene.slots[1] != 0xFFFFFFFFu);
        CHECK(table->Lookup(Id(1000u), Arcane::BindlessTable::ColorSpace::Srgb) == Arcane::BindlessTable::kInvalidSlot);   // the oldest went
        CHECK(table->Lookup(Id(1001u), Arcane::BindlessTable::ColorSpace::Srgb) != Arcane::BindlessTable::kInvalidSlot);   // and only as many as asked
        CHECK(SamePicture(picture, reference));
        CHECK(Arcane::RenderErrorCount() == errorsBefore);
        table->RemoveObserver(&scene);
    }

    // ---- 10. The same stale set on the cook path (the review: it exists at base) ------------------------------
    void CheckInvalidatedSpriteRecovers(Arcane::GraphicsBackend backend)
    {
        const std::uint64_t errorsBefore = Arcane::RenderErrorCount();
        Images images;
        images.Add(1);
        const Arcane::Guid s = Id(1);

        auto v = MakeVehicle(backend, nullptr);
        v->ctx->SetPixelSupply([&](const Arcane::Guid& id) { return images.Find(id); });
        auto batch = Arcane::Batcher2D::Create();
        REQUIRE(batch != nullptr);
        (void)Settle(*v->ctx, [&] { return DrawSprites(*batch, { s }); });

        // A cook landed: the image changed and the device-wide invalidation dropped it.
        images.byId[s] = MakeImage(7);
        const Arcane::PixelData reference = ReferenceSprites(backend, images, { s });
        v->ctx->InvalidateTextureOnDevice(s);
        const Arcane::PixelData picture = Settle(*v->ctx, [&] { return DrawSprites(*batch, { s }); });
        CHECK(v->ctx->Textures()->View(s) != nullptr);
        CHECK(SamePicture(picture, reference));
        CHECK(Arcane::RenderErrorCount() == errorsBefore);
    }
}

#define EVICTION_GPU_CASE(Name, Backend, BackendTag, ...)                                                   \
    TEST_CASE("eviction: " Name " (" BackendTag ")", "[gpu][eviction]")                                     \
    {                                                                                                       \
        ARC_REQUIRE_BACKEND(Arcane::GraphicsBackend::Backend);                                              \
        __VA_ARGS__;                                                                                        \
    }
#define EVICTION_GPU_BOTH(Name, Fn, ...)                                                                    \
    EVICTION_GPU_CASE(Name, D3D12, "d3d12", Fn(Arcane::GraphicsBackend::D3D12 __VA_ARGS__))                 \
    EVICTION_GPU_CASE(Name, Vulkan, "vulkan", Fn(Arcane::GraphicsBackend::Vulkan __VA_ARGS__))

EVICTION_GPU_BOTH("a sprite drawn every frame survives another image's admission and renders intact", CheckDrawnSpriteSurvives)
EVICTION_GPU_BOTH("the victim is the cold image, not the older one a baked sprite set still draws", CheckColdImageIsTheVictim)
EVICTION_GPU_BOTH("an evicted sprite returns through a new set, with no idle and no dead view", CheckEvictedSpriteRecovers)
EVICTION_GPU_BOTH("an evicted declared image of a material returns and draws the same picture", CheckMaterialRecovers, , MaterialVictim::Declared)
EVICTION_GPU_BOTH("an evicted sprite variant of a material returns and draws the same picture", CheckMaterialRecovers, , MaterialVictim::SpriteVariant)
EVICTION_GPU_BOTH("a material notices a changed view that another consumer re-resolved (the 1-then-1 hole)", CheckMaterialSeesAChangedView)
EVICTION_GPU_BOTH("a post chain's evicted declared image returns and draws the same picture", CheckPostChainRecovers, , false)
EVICTION_GPU_BOTH("a post chain notices a changed view that another consumer re-resolved (the 1-then-1 hole)", CheckPostChainRecovers, , true)
EVICTION_GPU_BOTH("a dropped texture takes its ImGui entry with it, and a raw handle holder re-asks", CheckImGuiEntryFollowsTheDrop)
EVICTION_GPU_BOTH("a device-wide invalidation (a cook) reaches a sprite's baked set", CheckInvalidatedSpriteRecovers)
EVICTION_GPU_BOTH("a visible albedo set over the allowance renders intact, with no churn", CheckVisibleAlbedosDoNotChurn)
EVICTION_GPU_BOTH("a table full of unreferenced mappings serves a referenced acquire, which renders", CheckFullTableServesAReferencedAcquire)
