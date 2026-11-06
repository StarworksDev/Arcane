#if !defined(ARC_BUILD_DIST)

#include <catch2/catch_test_macros.hpp>
#include <Arcane/Base/DiagnosticStore.hpp>
#include <Arcane/Base/Diagnostics.hpp>

TEST_CASE("Core DiagnosticStore installs, counts by severity, and uninstalls", "[remote]") {
    Arcane::DiagnosticStore store;
    store.InstallAsEngineSink();
    Arcane::Diagnostic row;
    row.severity = Arcane::DiagSeverity::Error;
    row.scope = Arcane::DiagScope::Project;
    row.code = "remote.move";
    row.message = "hello store";
    const Arcane::Diagnostic rows[] = { row };
    Arcane::Diagnostics::Publish("remote-move", rows);
    CHECK(store.Count(Arcane::DiagSeverity::Error) == 1);
    CHECK(store.Count(Arcane::DiagSeverity::Warning) == 0);
    const auto snap = store.Snapshot();
    REQUIRE(snap.size() == 1);
    CHECK(snap[0].message == "hello store");
    CHECK(snap[0].code == "remote.move");
    const auto filtered = store.Filtered(Arcane::SeverityMask::Error, "hello");
    REQUIRE(filtered.size() == 1);
    CHECK(Arcane::MatchesDiagnosticFilter(snap[0], Arcane::SeverityMask::Error, "hello"));
    CHECK_FALSE(Arcane::MatchesDiagnosticFilter(snap[0], Arcane::SeverityMask::Warning, "hello"));
    CHECK_FALSE(Arcane::MatchesDiagnosticFilter(snap[0], Arcane::SeverityMask::Error, "absent"));
    store.UninstallEngineSink();
    Arcane::Diagnostic later = row;
    later.message = "after uninstall";
    const Arcane::Diagnostic laterRows[] = { later };
    Arcane::Diagnostics::Publish("remote-move", laterRows);
    CHECK(store.Count(Arcane::DiagSeverity::Error) == 1);
    CHECK(store.Snapshot()[0].message == "hello store");
}

#endif
