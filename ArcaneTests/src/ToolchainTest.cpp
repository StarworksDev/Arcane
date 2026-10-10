// Arcane::Toolchain's PURE-ENOUGH halves ([build]): workspace-file discovery,
// the shell-free FindOnPath search, and the concrete Premake/MsBuild/Make/
// Ninja/XcodeBuild resolvers -- all driven over a real temp directory and an
// explicit (or, for the Resolve* PATH-fallback cases, temporarily overridden)
// PATH/PATHEXT. The vswhere-backed lookups (VsWhere itself, and the vswhere
// HIT path inside ResolveMsBuild/ResolveDevenv) spawn vswhere.exe and are
// desk-verify territory -- the same "no spawn test" split ModuleBuild and
// RuntimeLaunch draw.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Platform/Platform.hpp>

#include <Arcane/Build/Toolchain.hpp>

// TempDir (unique temp dir per SECTION run, removed by the guard) and
// EnvOverride (the Resolve* wrappers, unlike FindOnPath itself, read
// PATH/PATHEXT live via std::getenv, so proving their fallback precedence
// needs a real, restored-on-scope-exit env mutation rather than a mock) --
// shared with BuildDriverTest.cpp.
#include "Helpers/TestEnvironment.hpp"

namespace
{
    namespace fs = std::filesystem;

    using Arcane::Test::TempDir;
    using Arcane::Test::EnvOverride;

    void Touch(const fs::path& p)
    {
        fs::create_directories(p.parent_path());
        std::ofstream(p.string()) << "x";
#if !ARC_PLATFORM_WINDOWS
        // POSIX FindOnPath only accepts a file with an executable bit (the
        // PATHEXT analogue); a case that needs a NON-executable file sets
        // its permissions explicitly after this.
        fs::permissions(p, fs::perms::owner_all, fs::perm_options::replace);
#endif
    }
}

TEST_CASE("Toolchain::DiscoverSolution prefers .slnx over .sln, first lexicographic within a bucket",
          "[build]")
{
    TempDir dir("discover");
    Touch(dir.path / "Zeta.sln");
    Touch(dir.path / "beta.slnx");
    Touch(dir.path / "Alpha.slnx");
    Touch(dir.path / "notes.txt");

    CHECK(Arcane::Toolchain::DiscoverSolution(dir.path).filename() == "Alpha.slnx");
}

TEST_CASE("Toolchain::DiscoverSolution falls back to .sln, and to empty when neither exists",
          "[build]")
{
    TempDir dir("fallback");
    SECTION("only a .sln")
    {
        Touch(dir.path / "Game.sln");
        CHECK(Arcane::Toolchain::DiscoverSolution(dir.path).filename() == "Game.sln");
    }
    SECTION("a hand-generated upper-case extension still counts")
    {
        Touch(dir.path / "Game.SLNX");
        CHECK(Arcane::Toolchain::DiscoverSolution(dir.path).filename() == "Game.SLNX");
    }
    SECTION("neither")
    {
        CHECK(Arcane::Toolchain::DiscoverSolution(dir.path).empty());
    }
    SECTION("a directory that does not exist")
    {
        CHECK(Arcane::Toolchain::DiscoverSolution(dir.path / "missing").empty());
    }
    SECTION("non-recursive: a vendor solution one level down is not ours")
    {
        Touch(dir.path / "ThirdParty" / "vendor.slnx");
        CHECK(Arcane::Toolchain::DiscoverSolution(dir.path).empty());
    }
}

TEST_CASE("Toolchain::ResolvePremake returns the SDK's bundled premake, else a concrete PATH file", "[build]")
{
    TempDir sdk("premake");
    SECTION("bundled copy present")
    {
        Touch(sdk.path / "ThirdParty" / "premake5" / Arcane::Platform::ExecutableFileName("premake5"));
        const fs::path found = Arcane::Toolchain::ResolvePremake(sdk.path);
        CHECK(found.filename() == Arcane::Platform::ExecutableFileName("premake5"));
        CHECK(found.is_absolute());
        // lexically_normal'd: no "." / ".." elements survive.
        CHECK(found == found.lexically_normal());
    }
    SECTION("bundled copy present, sdkRoot given RELATIVE -- the answer is still absolute (the file's 'absolute or empty' contract)")
    {
        Touch(sdk.path / "ThirdParty" / "premake5" / Arcane::Platform::ExecutableFileName("premake5"));

        // A relative sdkRoot only makes sense against the process cwd, so
        // enter the temp dir's parent for the scope and hand in the leaf.
        const fs::path previousCwd = fs::current_path();
        fs::current_path(sdk.path.parent_path());
        const fs::path found = Arcane::Toolchain::ResolvePremake(sdk.path.filename());
        fs::current_path(previousCwd);

        REQUIRE_FALSE(found.empty());
        CHECK(found.is_absolute());
        CHECK(found == found.lexically_normal());
        // equivalent(), not ==: the relative root resolves against the cwd,
        // which the OS reports through any symlink (macOS's /var ->
        // /private/var), so the spelling may differ while the file is the same.
        CHECK(fs::equivalent(found, sdk.path / "ThirdParty" / "premake5" / Arcane::Platform::ExecutableFileName("premake5")));
    }
    SECTION("bundled copy absent -> the concrete PATH hit, never a bare 'premake5'")
    {
        TempDir onPath("premake_onpath");
        Touch(onPath.path / Arcane::Platform::ExecutableFileName("premake5"));
        EnvOverride path("PATH", onPath.path.string());
        EnvOverride pathExt("PATHEXT", ".COM;.EXE;.BAT;.CMD");

        const fs::path found = Arcane::Toolchain::ResolvePremake(sdk.path);
        CHECK(found == (onPath.path / Arcane::Platform::ExecutableFileName("premake5")).lexically_normal());
        CHECK(found != fs::path("premake5"));
    }
    SECTION("neither bundled nor PATH has one -> empty, never an optimistic bare name")
    {
        TempDir onPath("premake_missing");
        EnvOverride path("PATH", onPath.path.string());
        EnvOverride pathExt("PATHEXT", ".COM;.EXE;.BAT;.CMD");

        CHECK(Arcane::Toolchain::ResolvePremake(sdk.path).empty());
    }
}

TEST_CASE("Toolchain::ResolvePremake: the bundled copy wins even when PATH also has one", "[build]")
{
    TempDir sdk("premake_bundled_wins");
    TempDir onPath("premake_bundled_wins_path");
    Touch(sdk.path / "ThirdParty" / "premake5" / Arcane::Platform::ExecutableFileName("premake5"));
    Touch(onPath.path / Arcane::Platform::ExecutableFileName("premake5"));
    EnvOverride path("PATH", onPath.path.string());
    EnvOverride pathExt("PATHEXT", ".COM;.EXE;.BAT;.CMD");

    const fs::path found = Arcane::Toolchain::ResolvePremake(sdk.path);
    CHECK(found == (sdk.path / "ThirdParty" / "premake5" / Arcane::Platform::ExecutableFileName("premake5")).lexically_normal());
}

TEST_CASE("Toolchain::ResolveMake/ResolveNinja are empty (not a bare name) when PATH has neither", "[build]")
{
    TempDir onPath("missing_make_ninja");
    EnvOverride path("PATH", onPath.path.string());
    EnvOverride pathExt("PATHEXT", ".COM;.EXE;.BAT;.CMD");

    CHECK(Arcane::Toolchain::ResolveMake().empty());
    CHECK(Arcane::Toolchain::ResolveNinja().empty());
}

TEST_CASE("Toolchain::ResolveNinja returns the concrete PATH hit when ninja IS there", "[build]")
{
    TempDir onPath("ninja_onpath");
    // The on-disk tool is the platform's executable spelling: ninja.exe found
    // through PATHEXT on Windows; ninja with its executable bit on POSIX
    // (FindOnPath's POSIX contract skips a non-executable file).
    const fs::path ninja = onPath.path / Arcane::Platform::ExecutableFileName("ninja");
    Touch(ninja);
#if !ARC_PLATFORM_WINDOWS
    fs::permissions(ninja, fs::perms::owner_all, fs::perm_options::replace);
#endif
    EnvOverride path("PATH", onPath.path.string());
    EnvOverride pathExt("PATHEXT", ".COM;.EXE;.BAT;.CMD");

    CHECK(Arcane::Toolchain::ResolveNinja() == ninja.lexically_normal());
}

TEST_CASE("Toolchain::ResolveXcodeBuild is empty off macOS, even when something answers to the name on PATH",
          "[build]")
{
#if !defined(__APPLE__)
    TempDir onPath("xcodebuild_offmac");
    Touch(onPath.path / "xcodebuild.exe");
    EnvOverride path("PATH", onPath.path.string());

    CHECK(Arcane::Toolchain::ResolveXcodeBuild().empty());
#else
    SUCCEED("macOS-only branch; exercised on a macOS agent instead");
#endif
}

// ---- FindOnPath --------------------------------------------------------------

TEST_CASE("Toolchain::FindOnPath is a shell-free, explicit-search-path lookup", "[build]")
{
#if defined(_WIN32)
    SECTION("PATHEXT precedence: the bare name misses, an appended extension hits")
    {
        TempDir dir("findonpath_pathext");
        Touch(dir.path / "ninja.exe");
        const fs::path expectedNinjaExe = (dir.path / "ninja.exe").lexically_normal();

        CHECK(Arcane::Toolchain::FindOnPath("ninja", dir.path.string(), ".COM;.EXE;.BAT;.CMD") == expectedNinjaExe);
        CHECK(Arcane::Toolchain::FindOnPath("missing", dir.path.string(), ".EXE").empty());
    }

    SECTION("first-directory precedence: an earlier directory's match wins")
    {
        TempDir first("findonpath_first");
        TempDir second("findonpath_second");
        Touch(first.path / "tool.exe");
        Touch(second.path / "tool.exe");
        const std::string search = first.path.string() + ";" + second.path.string();

        CHECK(Arcane::Toolchain::FindOnPath("tool", search, ".EXE") == (first.path / "tool.exe").lexically_normal());
    }

    SECTION("a later directory is reached when the earlier one has no match")
    {
        TempDir first("findonpath_skip_first");
        TempDir second("findonpath_skip_second");
        Touch(second.path / "tool.exe");
        const std::string search = first.path.string() + ";" + second.path.string();

        CHECK(Arcane::Toolchain::FindOnPath("tool", search, ".EXE") == (second.path / "tool.exe").lexically_normal());
    }

    SECTION("the result is absolute and lexically normal")
    {
        TempDir dir("findonpath_absolute");
        Touch(dir.path / "tool.exe");

        const fs::path found = Arcane::Toolchain::FindOnPath("tool", dir.path.string(), ".EXE");
        REQUIRE_FALSE(found.empty());
        CHECK(found.is_absolute());
        CHECK(found == found.lexically_normal());
    }

    SECTION("missing is empty, not a fabricated path")
    {
        TempDir dir("findonpath_missing");
        CHECK(Arcane::Toolchain::FindOnPath("missing", dir.path.string(), ".EXE").empty());
    }
#else
    SECTION("POSIX requires an executable bit and ignores a non-executable regular file")
    {
        TempDir dir("findonpath_posix_exec");
        const fs::path candidate = dir.path / "tool";
        Touch(candidate);
        fs::permissions(candidate,
                         fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read,
                         fs::perm_options::replace);

        // Readable but not executable: not a match.
        CHECK(Arcane::Toolchain::FindOnPath("tool", dir.path.string(), "").empty());

        fs::permissions(candidate, fs::perms::owner_all, fs::perm_options::replace);
        CHECK(Arcane::Toolchain::FindOnPath("tool", dir.path.string(), "") == candidate.lexically_normal());
    }

    SECTION("a non-executable match in an earlier directory is skipped for a later executable one")
    {
        TempDir first("findonpath_posix_first");
        TempDir second("findonpath_posix_second");
        Touch(first.path / "tool");
        fs::permissions(first.path / "tool", fs::perms::owner_read, fs::perm_options::replace);
        Touch(second.path / "tool");
        fs::permissions(second.path / "tool", fs::perms::owner_all, fs::perm_options::replace);
        const std::string search = first.path.string() + ":" + second.path.string();

        CHECK(Arcane::Toolchain::FindOnPath("tool", search, "") == (second.path / "tool").lexically_normal());
    }

    SECTION("missing is empty")
    {
        TempDir dir("findonpath_posix_missing");
        CHECK(Arcane::Toolchain::FindOnPath("missing", dir.path.string(), "").empty());
    }
#endif
}
