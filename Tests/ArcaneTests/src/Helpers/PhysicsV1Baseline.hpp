#pragma once

// The v1 physics baselines (3D-readiness Plan A, Tasks 1-2). One scene, one
// writer per snapshot. Later tasks compare these exact bytes; do not add a
// second writer. Definitions live in PhysicsV1BaselineTest.cpp (not in an
// anonymous namespace) so other test TUs can include this header and call them.

#include <Arcane/Base/Runtime.hpp>
#include <Arcane/Ecs.hpp>

#include <array>
#include <filesystem>
#include <string>

namespace Arcane::Test
{
    struct V1PhysicsScene
    {
        // Creation order: floor, circle, capsule, box, sensor.
        std::array<Arcane::Entity, 5> entities{};
        [[nodiscard]] Arcane::Entity Floor()  const { return entities[0]; }
        [[nodiscard]] Arcane::Entity Circle() const { return entities[1]; }
    };

    V1PhysicsScene BuildV1PhysicsScene(Arcane::Registry& reg);

    // `steps` fixed steps (one Advance(1/60) each); returns the whole trace file.
    std::string WriteV1StepTrace(Arcane::Runtime& rt, int steps);

    // Which debug request a dump reproduces (see Task 2 of the plan).
    // AltView is dumped at step 20 (bodies moving). AltViewAlpha0 is the same affine at alpha 0,
    // used only to prove alpha is pinned.
    enum class V1DebugView { WholeWorld, SelectedOnly, AltView, AltViewAlpha0 };

    // Header line plus one primitive per line. Call after the steps.
    std::string DumpV1Debug(Arcane::Runtime& rt, const V1PhysicsScene& scene, V1DebugView view);
    // Narrowphase inspector overlay: floor fixture 0 vs circle fixture 0.
    std::string DumpV1Inspector(Arcane::Runtime& rt, const V1PhysicsScene& scene);
    // CollectPickables over the (sprite-free) scene.
    std::string DumpV1Pick(Arcane::Runtime& rt);

    std::string ReadFileBinary(const std::filesystem::path& path);
    void WriteFileBinary(const std::filesystem::path& path, const std::string& bytes);
}
