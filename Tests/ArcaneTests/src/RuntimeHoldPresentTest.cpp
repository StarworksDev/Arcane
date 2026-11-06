// Held runtime frame (review round 1, Task 18): the Hold branch of
// RuntimeFrame presents derived state (WorldTransform + WorldBounds) without
// advancing the sim. The pin is the RED; the behavior case documents the pair
// of systems the Hold branch calls (the same pair EditModeSchedule runs).
#include <Arcane/Scene/BoundsSystem.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Scene/TransformSystems.hpp>

#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Registry/Registry.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "Helpers/ConstantScan.hpp"

#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

namespace
{
    std::string ReadRepoFile(const char* relative)
    {
        std::ifstream in(Arcane::Test::RepoRoot() / relative);
        REQUIRE(in.good());
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }
}

TEST_CASE("Hold presents world transform and bounds without advancing frameCount", "[runtime][hold]")
{
    const auto text = ReadRepoFile("Source/Programs/ArcaneRuntimeHost/src/RuntimeFrame.cpp");
    const auto hold = text.find("if (pace == SimPace::Hold)");
    REQUIRE(hold != std::string::npos);
    const auto ret = text.find("return;", hold);
    REQUIRE(ret != std::string::npos);
    const auto body = text.substr(hold, ret - hold);
    CHECK(body.find("PresentDerivedState") != std::string::npos);
    CHECK(body.find("RunLoop::Advance") == std::string::npos);
    CHECK(body.find("frameCount") == std::string::npos);
}

TEST_CASE("the propagation + bounds pair gives a child its world translation and a sprite its box", "[runtime][hold]")
{
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg{ components };
    Arcane::RegisterSceneComponents(reg);
    const Astra::Entity root = reg.CreateEntity();
    reg.AddComponent<Arcane::Transform>(root, Arcane::Transform{});
    reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{ root });

    const Astra::Entity parent = reg.CreateEntity();
    Arcane::Transform pt; pt.position = glm::vec3(2, 0, 0);
    reg.AddComponent<Arcane::Transform>(parent, pt);
    reg.SetParent(parent, root);

    const Astra::Entity child = reg.CreateEntity();
    Arcane::Transform ct; ct.position = glm::vec3(1, 0, 0);
    reg.AddComponent<Arcane::Transform>(child, ct);
    reg.SetParent(child, parent);
    Arcane::SpriteRenderer s; s.shape = Arcane::SpriteShape::Rect;
    reg.AddComponent<Arcane::SpriteRenderer>(child, s);

    Arcane::TransformPropagationSystem{}.operator()(reg);
    Arcane::BoundsSystem{}.operator()(reg);

    const Arcane::WorldTransform* wt = std::as_const(reg).GetComponent<Arcane::WorldTransform>(child);
    REQUIRE(wt);
    CHECK(wt->matrix[3].x == Catch::Approx(3.0f));
    const Arcane::WorldBounds* wb = std::as_const(reg).GetComponent<Arcane::WorldBounds>(child);
    REQUIRE(wb);
    CHECK(wb->box.max.x > wb->box.min.x);
}
