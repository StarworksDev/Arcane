// IdeLaunch's PURE halves ([editor]): the Running-Object-Table moniker
// predicate, the solution-path equivalence DTE's Solution.FullName is
// compared under, and the devenv argv composition. The COM half (ROT
// enumeration, late-bound IDispatch calls into a live Visual Studio) and the
// CreateProcessW launch are desk-verify territory -- the same "no spawn
// test" split ModuleBuild and RuntimeLaunch draw -- EXCEPT for the one
// opt-in [ide-desk] case at the bottom, which SKIPs unless
// ARCANE_IDE_DESK is set and then drives the real COM path against whatever
// Visual Studio is actually running on the desk.

#include <cstdlib>
#include <Arcane/Platform/Platform.hpp>
#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <Project/IdeLaunch.hpp>

namespace
{
    namespace fs = std::filesystem;
    using namespace Arcane::Editor;
}

TEST_CASE("IdeLaunch::IsVisualStudioMoniker accepts VisualStudio.DTE monikers of any major version only", "[editor]")
{
    // The ROT display-name shape UE's accessor matches on (its ROTMoniker is
    // "!VisualStudio.DTE.<major>.0"): a DTE moniker suffixed with the pid.
    CHECK(IdeLaunch::IsVisualStudioMoniker(L"!VisualStudio.DTE.18.0:12345"));
    CHECK(IdeLaunch::IsVisualStudioMoniker(L"!VisualStudio.DTE.17.0:9"));
    // Prefix must be exact -- Express editions register a different progid,
    // and any other ROT entry (file monikers, other automation servers) is
    // not ours.
    CHECK_FALSE(IdeLaunch::IsVisualStudioMoniker(L"!WDExpress.DTE.12.0:1"));
    CHECK_FALSE(IdeLaunch::IsVisualStudioMoniker(L"!{6C736DB1-BD94-11D0-8A23-00AA00B58E10}"));
    CHECK_FALSE(IdeLaunch::IsVisualStudioMoniker(L"VisualStudio.DTE.18.0:1"));   // no leading '!'
    CHECK_FALSE(IdeLaunch::IsVisualStudioMoniker(L""));
}

TEST_CASE("IdeLaunch::SameSolutionPath compares case- and separator-insensitively after normalising", "[editor]")
{
    // DTE hands back a backslash, mixed-case Windows path; DiscoverSolution
    // hands back whatever the directory iterator produced. Same file either way.
#if ARC_PLATFORM_WINDOWS
    CHECK(IdeLaunch::SameSolutionPath(L"D:\\dev\\Game\\Game.slnx", L"d:/dev/game/game.slnx"));
    CHECK(IdeLaunch::SameSolutionPath(L"D:\\dev\\.\\Game\\Game.slnx", L"D:\\dev\\Game\\Game.slnx"));
    CHECK(IdeLaunch::SameSolutionPath(L"D:\\dev\\Other\\..\\Game\\Game.slnx", L"D:\\dev\\Game\\Game.slnx"));

    CHECK_FALSE(IdeLaunch::SameSolutionPath(L"D:\\dev\\Game\\Game.slnx", L"D:\\dev\\Game\\Other.slnx"));
    CHECK_FALSE(IdeLaunch::SameSolutionPath(L"D:\\dev\\Game\\Game.slnx", L"D:\\dev\\Game2\\Game.slnx"));
#else
    // Native POSIX paths: '\\' is an ordinary file-name byte there, so the
    // normalising half ("." and ".." segments, the case fold) is what is
    // exercised. (The Visual Studio half of IdeLaunch is Windows-only.)
    CHECK(IdeLaunch::SameSolutionPath(L"/dev/Game/Game.slnx", L"/dev/game/game.slnx"));
    CHECK(IdeLaunch::SameSolutionPath(L"/dev/./Game/Game.slnx", L"/dev/Game/Game.slnx"));
    CHECK(IdeLaunch::SameSolutionPath(L"/dev/Other/../Game/Game.slnx", L"/dev/Game/Game.slnx"));

    CHECK_FALSE(IdeLaunch::SameSolutionPath(L"/dev/Game/Game.slnx", L"/dev/Game/Other.slnx"));
    CHECK_FALSE(IdeLaunch::SameSolutionPath(L"/dev/Game/Game.slnx", L"/dev/Game2/Game.slnx"));
#endif
    // An empty side never matches -- a VS with NO solution open reports an
    // empty FullName, and that must never equal anything we ask about.
    CHECK_FALSE(IdeLaunch::SameSolutionPath(L"", L"D:\\dev\\Game\\Game.slnx"));
    CHECK_FALSE(IdeLaunch::SameSolutionPath(L"", L""));
}

TEST_CASE("IdeLaunch::ComposeLaunchArgs is the solution, then the file when there is one", "[editor]")
{
    // UE's RunVisualStudioAndOpenSolutionAndFiles shape: `devenv "<sln>" "<file>"`
    // -- one devenv launch opens the solution AND the file in it. Tokens are
    // UNQUOTED (RuntimeLaunch::QuoteArg does the Win32 escaping at spawn),
    // same contract as RuntimeLaunch::BuildArgs.
    const fs::path sln  = L"D:\\dev\\My Game\\MyGame.slnx";
    const fs::path file = L"D:\\dev\\My Game\\Source\\Game.cpp";

    const std::vector<std::wstring> withFile = IdeLaunch::ComposeLaunchArgs(sln, file);
    REQUIRE(withFile.size() == 2);
    CHECK(withFile[0] == sln.wstring());
    CHECK(withFile[1] == file.wstring());

    const std::vector<std::wstring> solutionOnly = IdeLaunch::ComposeLaunchArgs(sln, {});
    REQUIRE(solutionOnly.size() == 1);
    CHECK(solutionOnly[0] == sln.wstring());
}

TEST_CASE("IdeLaunch::Describe names every outcome", "[editor]")
{
    // Each outcome is what the Console line says after a click; none may be
    // blank, and no two may collapse onto the same wording.
    std::vector<std::string> seen;
    for (int i = 0; i < static_cast<int>(IdeLaunch::Outcome::Count); ++i)
    {
        const std::string s = IdeLaunch::Describe(static_cast<IdeLaunch::Outcome>(i));
        CHECK_FALSE(s.empty());
        for (const std::string& prior : seen)
            CHECK(prior != s);
        seen.push_back(s);
    }
}

// ---------------------------------------------------------------------------
// The opt-in desk probe. Off by default: SKIPs unless ARCANE_IDE_DESK names
// a solution path, so the ordinary ~[gpu] gate never touches COM or a live
// Visual Studio. Set ARCANE_IDE_DESK=<abs path to a .slnx> and run
// "[ide-desk]" to exercise the ROT + IDispatch route for real:
//   - with THAT solution open in VS            -> Access::Open
//   - with VS closed, or a different solution -> Access::NotOpen
// The assertion is only that the probe RETURNS one of the two decidable
// answers (never Unknown -- the ROT was reachable) -- which one is a fact
// about the desk, printed for the human to compare against what they see.
// ---------------------------------------------------------------------------
TEST_CASE("IdeLaunch::Probe reaches the Running Object Table on this desk", "[ide-desk]")
{
    const char* env = std::getenv("ARCANE_IDE_DESK");
    if (!env || !*env)
        SKIP("ARCANE_IDE_DESK not set -- desk-only probe");

    const IdeLaunch::Access access = IdeLaunch::Probe(fs::path(env));
    INFO("probe answered: " << static_cast<int>(access) << " (0=Open 1=NotOpen 2=Blocked 3=Unknown)");
    CHECK(access != IdeLaunch::Access::Unknown);
}

// The second half of the desk probe: with ARCANE_IDE_DESK_FILE naming a
// source file, actually drive ItemOperations.OpenFile into the instance that
// has ARCANE_IDE_DESK's solution open. Expected OpenedInInstance -- and the
// file visibly opening in Visual Studio is the human half of the check. No
// devenv is handed in on purpose: if the instance is NOT open this must come
// back NoDevenv rather than launch a second Visual Studio mid-test-run.
TEST_CASE("IdeLaunch::OpenFile opens a file into the running instance on this desk", "[ide-desk]")
{
    const char* sln  = std::getenv("ARCANE_IDE_DESK");
    const char* file = std::getenv("ARCANE_IDE_DESK_FILE");
    if (!sln || !*sln || !file || !*file)
        SKIP("ARCANE_IDE_DESK / ARCANE_IDE_DESK_FILE not set -- desk-only probe");

    const IdeLaunch::Outcome outcome = IdeLaunch::OpenFile({}, fs::path(sln), fs::path(file));
    INFO("outcome: " << IdeLaunch::Describe(outcome));
    CHECK(outcome == IdeLaunch::Outcome::OpenedInInstance);
}

TEST_CASE("IdeLaunch::ComposeLaunchArgs appends /Command \"Edit.GoTo <line>\" only for a line", "[editor]")
{
    const fs::path sln  = L"D:\\dev\\My Game\\MyGame.slnx";
    const fs::path file = L"D:\\dev\\My Game\\Source\\Game.cpp";
    const std::vector<std::wstring> atLine = IdeLaunch::ComposeLaunchArgs(sln, file, 42);
    REQUIRE(atLine.size() == 4);
    CHECK(atLine[0] == sln.wstring());
    CHECK(atLine[1] == file.wstring());
    CHECK(atLine[2] == L"/Command");
    CHECK(atLine[3] == L"Edit.GoTo 42");             // one token; QuoteArg quotes it at spawn
    CHECK(IdeLaunch::ComposeLaunchArgs(sln, file, 0).size() == 2);
    CHECK(IdeLaunch::ComposeLaunchArgs(sln, {}, 42).size() == 1);   // no file, no jump
}
