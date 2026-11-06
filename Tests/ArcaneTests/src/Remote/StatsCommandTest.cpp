#include <catch2/catch_test_macros.hpp>
#include <Arcane/Remote/FrameStats.hpp>
#include <Arcane/Remote/Commands/StatsCommands.hpp>
#include <Arcane/Remote/Registry.hpp>
#include <Arcane/Remote/Protocol.hpp>
#include "RemoteTestSupport.hpp"

#if !defined(ARC_BUILD_DIST)

TEST_CASE("stats.frame reads the last advance and ignores a later idle", "[remote]") {
    Arcane::Remote::FrameStats stats;
    Arcane::Remote::NoteAdvance(stats, 10, 0.016, 1.5, 0.0);
    // Null again on scope exit: Task 29's cases must never read this dead stack object (ruling F11).
    RemoteTest::ScopedRemotePointer<Arcane::Remote::FrameStats> statsGuard(&Arcane::Remote::RegisterStatsCommand, &stats);
    Arcane::Remote::Call call;
    call.command = "stats.frame";
    const auto env = Arcane::Remote::CommandRegistry::Get().Invoke(call).envelope;
    CHECK(env.ok);
    CHECK(env.data["frame"] == 10);
    CHECK(env.data["dtSeconds"] == 0.016);
    CHECK(env.data["tickMs"] == 1.5);
    CHECK(env.data["renderMs"] == 0.0);
    const auto again = Arcane::Remote::CommandRegistry::Get().Invoke(call).envelope;
    CHECK(again.data["frame"] == 10);
    Arcane::Remote::FrameStats later;
    Arcane::Remote::NoteAdvance(later, 42, 0.016, 1.0, 2.0);
    Arcane::Remote::RegisterStatsCommand(&later);              // a second call replaces the pointer
    CHECK(Arcane::Remote::CommandRegistry::Get().Invoke(call).envelope.data["frame"] == 42);
    Arcane::Remote::RegisterStatsCommand(nullptr);
    const auto none = Arcane::Remote::CommandRegistry::Get().Invoke(call).envelope;
    CHECK(none.errors[0].code == "host.unavailable");
    CHECK(Arcane::Remote::ExitFromHost(none) == 5);
}

TEST_CASE("stats.frame refuses arguments and releases only its own pointer", "[remote]") {
    Arcane::Remote::FrameStats stats;
    Arcane::Remote::NoteAdvance(stats, 7, 0.01, 0.5, 0.25);
    RemoteTest::ScopedRemotePointer<Arcane::Remote::FrameStats> statsGuard(&Arcane::Remote::RegisterStatsCommand, &stats);
    Arcane::Remote::Call call;
    call.command = "stats.frame";
    call.args = nlohmann::json{{"bogus", 1}};
    const auto refused = Arcane::Remote::CommandRegistry::Get().Invoke(call).envelope;
    REQUIRE_FALSE(refused.errors.empty());
    CHECK(refused.errors[0].code == "arg.unknown");
    CHECK(Arcane::Remote::ExitFromHost(refused) == 2);

    call.args = nlohmann::json::object();
    Arcane::Remote::FrameStats other;
    Arcane::Remote::ReleaseStatsCommand(&other);   // not the stored pointer: left alone
    CHECK(Arcane::Remote::CommandRegistry::Get().Invoke(call).envelope.data["frame"] == 7);
    Arcane::Remote::ReleaseStatsCommand(&stats);
    CHECK(Arcane::Remote::CommandRegistry::Get().Invoke(call).envelope.errors[0].code == "host.unavailable");
}

#endif
