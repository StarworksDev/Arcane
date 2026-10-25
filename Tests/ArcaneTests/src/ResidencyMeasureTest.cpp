// ResidencyMeasureTest.cpp -- 3D-readiness Plan C Task 3. The texture slice cap and the
// reservation percent are registered settings (the residency account latches them).

#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Config/Settings.hpp>
#include <Arcane/Render/RenderBudgetSettings.hpp>

#include <Arcane/Mesh/MeshBuilder.hpp>
#include <Arcane/Render/Nri/MeshResidencyBudget.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>

namespace
{
    // Resident bytes of one RGBA8 texture with a full mip chain at D3D12's upload
    // footprint (row pitch to 256, each subresource to 512). `packed` is the same
    // chain with no alignment (what the artifact payload stores).
    void TextureChainBytes(std::uint32_t w, std::uint32_t h, std::uint64_t& packed, std::uint64_t& aligned)
    {
        packed = aligned = 0;
        for (;;)
        {
            packed += std::uint64_t(w) * h * 4u;
            const std::uint64_t rowPitch = (std::uint64_t(w) * 4u + 255u) / 256u * 256u;
            aligned += (rowPitch * h + 511u) / 512u * 512u;
            if (w == 1u && h == 1u) break;
            w = std::max(1u, w / 2u);
            h = std::max(1u, h / 2u);
        }
    }
}

// Throwaway measurement (hidden: run by name, "[.residency-measure]"). Prints the
// numbers the Task 3 commit message quotes; asserts nothing.
TEST_CASE("residency measurement: texture chains and primitive meshes", "[.][residency-measure]")
{
    std::uint64_t p = 0, a = 0;
    TextureChainBytes(64, 64, p, a);     // ReferenceProject uv_marker.png
    std::printf("uv_marker 64x64 RGBA8 chain: packed %llu aligned %llu\n", (unsigned long long)p, (unsigned long long)a);
    TextureChainBytes(550, 550, p, a);   // Aphelyon arcane_logo.png
    std::printf("arcane_logo 550x550 RGBA8 chain: packed %llu aligned %llu\n", (unsigned long long)p, (unsigned long long)a);

    const Arcane::MeshData cube = Arcane::BuildCube(1.0f);
    std::size_t sectionBytes = cube.sections.size() * sizeof(Arcane::MeshSection);
    for (const auto& s : cube.sections) sectionBytes += s.name.size();
    std::printf("cube: %zu verts %zu idx -> %llu bytes\n", cube.vertices.size(), cube.indices.size(),
                (unsigned long long)Arcane::MeshResidencyBytes(cube.vertices.size() * sizeof(Arcane::MeshVertex),
                                                               cube.indices.size() * sizeof(std::uint32_t),
                                                               sectionBytes));
}

TEST_CASE("texture and residency settings are registered", "[cvar][residency]")
{
    CHECK_FALSE(Arcane::CVarRegistry::Get().Find("render.texture.residencyBudgetBytes").IsStale());
    CHECK_FALSE(Arcane::CVarRegistry::Get().Find("render.residency.reservationPercent").IsStale());
    CHECK(Arcane::Settings<Arcane::RenderTextureSettings>().residencyBudgetBytes >= (64ull << 20));
    CHECK(Arcane::Settings<Arcane::RenderTextureSettings>().residencyBudgetBytes <= (16ull << 30));
    CHECK(Arcane::Settings<Arcane::RenderResidencySettings>().reservationPercent <= 100u);
}
