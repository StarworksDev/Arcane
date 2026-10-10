// THE BEHAVIOUR GATE of the input-seam work (spec 2026-10-02 s8 T6).
//
// ReferenceProject's REAL module sources (built as ReferenceGameUnderTest.dll,
// premake5.lua) run headless the way ArcaneRuntime's frame does
// (ArcaneRuntime/src/RuntimeFrame.cpp: SetInputSnapshot/UpdateGameInput, then
// EnsurePhysics, then Loop().Advance with BeginGameInputFixedStep +
// FixedUpdateAll), against the authored physics scene, under a fixed scripted
// input. The player's position and velocity at every frame must equal the
// recording bit for bit. The recording was captured BEFORE the rewrite of
// ReferenceGame.cpp / PlayerController2D* (input-seam plan Task 1), so a
// green run after it is the proof that moving input and time into resources
// changed no gameplay.
//
// To re-record (only ever on purpose, and never in the same commit as a
// gameplay change): set ARCANE_RECORD_TRAJECTORY=1 and run "[trajectory]".

#include <catch2/catch_test_macros.hpp>
#include "Helpers/ModuleNames.hpp"   // fixture module file names per platform

#include <Arcane/Client/ClientRuntime.hpp>
#include <Arcane/Host/ProjectBoot.hpp>
#include <Arcane/Input/InputSnapshot.hpp>
#include <Arcane/Plugin/PluginHost.hpp>
#include <Arcane/Project/Project.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Serialization/SceneAsset.hpp>

#include <Astra/Registry/Registry.hpp>

#include <Json.hpp>

#include <bit>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "Helpers/ReferenceProjectDir.hpp"
#include "Helpers/TestTypeContext.hpp"

namespace
{
    constexpr std::uint32_t kScancodeA = 4;    // SDL_SCANCODE_A -- Player.Move negative
    constexpr std::uint32_t kScancodeD = 7;    // SDL_SCANCODE_D -- Player.Move positive
    constexpr std::uint32_t kScancodeW = 26;   // SDL_SCANCODE_W -- Player.Jump
    constexpr int    kFrames = 240;
    constexpr double kDt     = 1.0 / 60.0;

    struct Sample { float x, y, vx, vy; };

    // Sized to the authored scene (physics.arcscene): the Pill starts at x = 2
    // on a Ground slab spanning x = [-3, 3], with the dynamic Crate at x = 0.
    // Settle; run left with a FULL jump (W held past the apex); turn right in
    // the air and land on the Crate; keep running right off it back to the
    // ground; brake to a stop; then a SHORT hop (W released early, exercising
    // the jump cut). Every frame stays over the slab.
    Arcane::InputSnapshot ScriptedInput(int f)
    {
        Arcane::InputSnapshot s;
        if (f >= 30 && f < 75)   s.SetScancode(kScancodeA);
        if (f >= 40 && f < 80)   s.SetScancode(kScancodeW);
        if (f >= 75 && f < 138)  s.SetScancode(kScancodeD);
        if (f >= 150 && f < 153) s.SetScancode(kScancodeW);
        return s;
    }

    std::filesystem::path FixturePath()
    {
        return Arcane::Test::FindReferenceProjectDir().parent_path() /
               "ArcaneTests" / "data" / "trajectory" / "reference_player.json";
    }

    Astra::Entity FindByName(Astra::Registry& reg, const std::string& name)
    {
        Astra::Entity found = Astra::Entity::Invalid();
        reg.CreateView<Arcane::Identity>().ForEach([&](Astra::Entity e, Arcane::Identity& id)
        {
            if (id.name == name) found = e;
        });
        return found;
    }

    std::vector<Sample> RunScript()
    {
        Arcane::ClientRuntime client(Arcane::Test::Process());
        const std::filesystem::path projectDir = Arcane::Test::FindReferenceProjectDir();
        REQUIRE_FALSE(projectDir.empty());
        auto project = Arcane::Project::Open(projectDir);
        REQUIRE(project);
        REQUIRE(Arcane::HostBoot::LoadGameplayInput(client, *project).status ==
                Arcane::HostBoot::GameplayInputLoadResult::Status::Loaded);

        Arcane::PluginHost host(Arcane::Test::Process(),
                                std::filesystem::path(Arcane::Test::ModuleFile("ReferenceGameUnderTest")));
        REQUIRE(host.AttachRuntime(client.Core()));
        REQUIRE(host.Load());

        std::string error;
        const auto scene = Arcane::Scene::ReadSceneFile(
            projectDir / "Content" / "scenes" / "physics.arcscene", &error);
        INFO(error);
        REQUIRE(scene);
        client.ResetRegistry();
        REQUIRE(Arcane::Scene::ApplySceneDocument(*scene, client.Registry()));

        const Astra::Entity player = FindByName(client.Registry(), "Pill");
        REQUIRE(player != Astra::Entity::Invalid());

        std::vector<Sample> out;
        out.reserve(kFrames);
        for (int f = 0; f < kFrames; ++f)
        {
            const Arcane::InputSnapshot snap = ScriptedInput(f);
            client.SetInputSnapshot(snap);
            client.UpdateGameInput(kDt, snap);
            client.EnsurePhysics();
            client.Loop().Advance(kDt,
                [&](double dt)           { client.BeginGameInputFixedStep(); host.FixedUpdateAll(dt); },
                [&](double dt, double a) { host.UpdateAll(dt, a); });

            const auto* t  = client.Registry().GetComponent<Arcane::Transform>(player);
            const auto* rb = client.Registry().GetComponent<Arcane::RigidBody2D>(player);
            REQUIRE(t);
            REQUIRE(rb);
            out.push_back({ t->position.x, t->position.y, rb->velocity.x, rb->velocity.y });
        }
        host.Unload();
        return out;
    }

    bool SameBits(float a, float b) { return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b); }
}

TEST_CASE("ReferenceProject player: the scripted trajectory matches the recording bit for bit",
          "[trajectory][reference]")
{
    const std::vector<Sample> first = RunScript();
    // Self-consistency first: a run that does not reproduce ITSELF cannot be
    // compared against anything (a nondeterministic step would show up here,
    // not as a phantom "behaviour change" against the fixture).
    const std::vector<Sample> second = RunScript();
    REQUIRE(first.size() == second.size());
    for (std::size_t i = 0; i < first.size(); ++i)
    {
        INFO("self-consistency, frame " << i);
        REQUIRE(SameBits(first[i].x, second[i].x));
        REQUIRE(SameBits(first[i].y, second[i].y));
        REQUIRE(SameBits(first[i].vx, second[i].vx));
        REQUIRE(SameBits(first[i].vy, second[i].vy));
    }

    if (const char* rec = std::getenv("ARCANE_RECORD_TRAJECTORY"); rec && std::string(rec) == "1")
    {
        nlohmann::json j;
        j["frames"] = kFrames;
        j["dt"] = kDt;
        j["entity"] = "Pill";
        j["samples"] = nlohmann::json::array();
        for (const Sample& s : first)
            j["samples"].push_back({ static_cast<double>(s.x), static_cast<double>(s.y),
                                     static_cast<double>(s.vx), static_cast<double>(s.vy) });
        std::filesystem::create_directories(FixturePath().parent_path());
        std::ofstream(FixturePath()) << j.dump(1) << '\n';
        WARN("recorded " << first.size() << " samples to " << FixturePath().generic_string());
        return;
    }

    std::ifstream in(FixturePath());
    REQUIRE(in);
    const nlohmann::json j = nlohmann::json::parse(in);
    REQUIRE(j.at("frames").get<int>() == kFrames);
    const auto& samples = j.at("samples");
    REQUIRE(samples.size() == first.size());
    for (std::size_t i = 0; i < first.size(); ++i)
    {
        INFO("frame " << i);
        CHECK(SameBits(static_cast<float>(samples[i][0].get<double>()), first[i].x));
        CHECK(SameBits(static_cast<float>(samples[i][1].get<double>()), first[i].y));
        CHECK(SameBits(static_cast<float>(samples[i][2].get<double>()), first[i].vx));
        CHECK(SameBits(static_cast<float>(samples[i][3].get<double>()), first[i].vy));
    }
}
