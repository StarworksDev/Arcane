// arcbuild's core and orchestration ([build]): command + flag parsing over Arcane::Cli,
// the --sdk / ARCANE_SDK precedence, the s4.3 decision table, exit-code
// mapping, path conventions, BackendResolver's expected-based tool/context
// resolution, and every composed child command line. Nothing here spawns a
// process or reads a PE file (BackendResolver tests that need a deterministic
// "nothing on PATH" answer override PATH/PATHEXT for their own scope only);
// temporary projects cover bootstrap and missing-slot inspection. The
// opt-in [build-desk] cases at the bottom (Task 3) are the only tests that
// reach it, SKIPping unless ARCANE_BUILD_DESK names a project directory.

#include <cerrno>
#include <Arcane/Platform/Platform.hpp>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <Driver.hpp>
#include <Bootstrap.hpp>
#include <Output.hpp>
#include <Pipeline.hpp>

// TempDir + EnvOverride (shared with ToolchainTest.cpp).
#include "Helpers/TestEnvironment.hpp"

// Review F4 (multibackend hardening): the Ninja single-slot staging copy.
#include <Stage.hpp>

// Task 5 (multibackend hardening): ExeDir() locates the process-fixture exe
// beside this test exe, the same "../<project>/<project>.exe" convention
// DeskDriverExe() (bottom of this file) already uses for arcbuild.exe --
// moved up here (rather than duplicated) since the [build] process-fixture
// cases need it, not just the opt-in [build-desk] ones.
#include <Project/ModuleBuild.hpp>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
// Task 6: SIGTERM, for the POSIX "128 + signal" case at the bottom of the
// process-execution section (<csignal> is portable, but only that branch
// needs it).
#include <csignal>
#endif

namespace
{
    namespace fs = std::filesystem;
    using namespace arcbuild;

    // argv for Cli::Parse: [0] is the "program" slot Parse skips (main.cpp
    // hands it argv+1, so [0] is the command word there).
    struct Argv
    {
        std::vector<std::string> storage;
        std::vector<char*>       ptrs;
        explicit Argv(std::initializer_list<const char*> words)
        {
            for (const char* w : words) storage.emplace_back(w);
            for (std::string& s : storage) ptrs.push_back(s.data());
        }
        int    argc() const { return static_cast<int>(ptrs.size()); }
        char** argv()       { return ptrs.data(); }
    };

    ProjectLayout AphelyonProject()
    {
        ProjectLayout project;
        project.root       = "D:/dev/starworks/Gacha/Game";
        project.manifest   = "D:/dev/starworks/Gacha/Game/Aphelyon.arcproj";
        project.name       = "Aphelyon";
        project.gameModule = "Aphelyon.dll";
        return project;
    }

    fs::path PremakePath()
    {
        return "D:/dev/starworks/Arcane/ThirdParty/premake5/premake5.exe";
    }

    // Task 5: the process-fixture exe premake5.lua builds as its own
    // `arcbuild-process-fixture` project, located relative to THIS test
    // exe's own directory -- the same "../<project>/<project>.exe"
    // convention DeskDriverExe() (bottom of this file) uses for arcbuild.exe.
    // Unlike arcbuild.exe (opt-in [build-desk] only), this exe is a normal
    // build dependency of ArcaneTests (premake's dependson), so the ordinary
    // [build] cases below can rely on it always being present.
    fs::path ProcessFixtureExe()
    {
        return (Arcane::Editor::ModuleBuild::ExeDir() /
                ".." / "arcbuild-process-fixture" / "arcbuild-process-fixture.exe")
            .lexically_normal();
    }

    fs::path MsBuildPath()
    {
        return "C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe";
    }

    fs::path MakePath()
    {
        return "/usr/bin/make";
    }

    fs::path NinjaPath()
    {
        return "/usr/bin/ninja";
    }

    fs::path XcodeBuildPath()
    {
        return "/usr/bin/xcodebuild";
    }

    struct TempArcProject
    {
        fs::path root;

        explicit TempArcProject(std::string_view leaf)
            : root(fs::temp_directory_path() / "arcbuild_orchestration_test" / leaf)
        {
            std::error_code ec;
            fs::remove_all(root, ec);
            fs::create_directories(root);

            std::ofstream manifest(root / "Fixture.arcproj", std::ios::binary);
            manifest << R"({
  "formatVersion": 2,
  "name": "Fixture",
  "engine": { "abi": 37 },
  "gameModule": "Fixture.dll"
})";
        }

        ~TempArcProject()
        {
            std::error_code ec;
            fs::remove_all(root, ec);
        }
    };

    // A plain, empty scratch directory (BackendResolver's tool-lookup tests
    // use it both as a bare "no bundled Premake here" SDK root and as a
    // controlled, empty PATH entry) and the restored-on-scope-exit
    // environment override the "nothing on PATH" cases need -- both from
    // Helpers/TestEnvironment.hpp, shared with ToolchainTest.cpp.
    using Arcane::Test::TempDir;
    using Arcane::Test::EnvOverride;

    struct FakeEnvironment final : IEnvironment
    {
        std::optional<fs::path> sdk;
        mutable int             setCalls = 0;
        mutable fs::path        exported;
        bool                    setResult = true;

        std::optional<fs::path> ArcaneSdk() const override
        {
            return sdk;
        }

        bool SetArcaneSdk(const fs::path& root) const override
        {
            ++setCalls;
            exported = root;
            return setResult;
        }
    };

    struct RecordingOutput final : IOutput
    {
        bool                     quiet = false;
        mutable std::vector<std::string> messages;

        void SetQuiet(bool value) noexcept override
        {
            quiet = value;
        }

        void Info(std::string_view message) const override
        {
            if (!quiet)
                messages.emplace_back("info:" + std::string(message));
        }

        void Always(std::string_view message) const override
        {
            messages.emplace_back("always:" + std::string(message));
        }

        void Error(std::string_view message) const override
        {
            messages.emplace_back("error:" + std::string(message));
        }

        void Child(std::string_view prefix, std::string_view message) const override
        {
            messages.emplace_back(std::string(prefix) + ":" + std::string(message));
        }
    };

    struct RecordingExecutor final : IBuildExecutor
    {
        std::vector<std::string>& events;
        int generateExit = kExitOk;
        int buildExit = kExitOk;
        int cleanExit = kExitOk;

        explicit RecordingExecutor(std::vector<std::string>& recorded)
            : events(recorded)
        {
        }

        int Generate(const DriverContext&) const override
        {
            events.emplace_back("generate");
            return generateExit;
        }

        int Build(const DriverContext&, BuildOperation operation) const override
        {
            events.emplace_back(operation == BuildOperation::Rebuild
                ? "rebuild"
                : "build");
            return buildExit;
        }

        int CleanBackend(const DriverContext&) const override
        {
            events.emplace_back("backend-clean");
            return cleanExit;
        }
    };

    struct RecordingSlots final : ISlotInspector
    {
        std::vector<std::string>& events;
        SlotState state = SlotState::Match;

        explicit RecordingSlots(std::vector<std::string>& recorded)
            : events(recorded)
        {
        }

        SlotProbe Inspect(const ProjectLayout&, std::string_view) const override
        {
            events.emplace_back("inspect");
            SlotProbe probe;
            probe.state = state;
            return probe;
        }

        std::string Describe(const SlotProbe&, std::string_view, const Verdict&) const override
        {
            return "probe row";
        }
    };

    struct RecordingCleaner final : IProjectCleaner
    {
        std::vector<std::string>& events;
        int cleanExit = kExitOk;

        explicit RecordingCleaner(std::vector<std::string>& recorded)
            : events(recorded)
        {
        }

        int Clean(const ProjectLayout&, std::string_view) const override
        {
            events.emplace_back("filesystem-clean");
            return cleanExit;
        }
    };

    DriverContext PipelineContext(Command command)
    {
        DriverContext context;
        context.request.command = command;
        context.request.action = "vs2026";
        context.backend = BuildBackend::MsBuild;
        context.sdkRoot = fs::path("D:/sdk");
        context.project = AphelyonProject();
        return context;
    }
}

// ---- CLI -------------------------------------------------------------------

TEST_CASE("arcbuild::ParseCommand knows the five commands and nothing else", "[build]")
{
    CHECK(ParseCommand("generate") == Command::Generate);
    CHECK(ParseCommand("build")    == Command::Build);
    CHECK(ParseCommand("rebuild")  == Command::Rebuild);
    CHECK(ParseCommand("clean")    == Command::Clean);
    CHECK(ParseCommand("probe")    == Command::Probe);
    CHECK_FALSE(ParseCommand("Build").has_value());     // exact, lower-case
    CHECK_FALSE(ParseCommand("--project").has_value()); // a flag is not a command
    CHECK_FALSE(ParseCommand("").has_value());
    // Round-trip: the name printed in the log is the word that parses.
    for (Command c : { Command::Generate, Command::Build, Command::Rebuild, Command::Clean, Command::Probe })
        CHECK(ParseCommand(CommandName(c)) == c);
}

TEST_CASE("arcbuild action mapping only promises verified build backends", "[build]")
{
    CHECK(BackendForAction("vs2022") == BuildBackend::MsBuild);
    CHECK(BackendForAction("vs2026") == BuildBackend::MsBuild);
    CHECK(BackendForAction("gmake") == BuildBackend::Make);
    CHECK(BackendForAction("gmakelegacy") == BuildBackend::Make);
    CHECK(BackendForAction("ninja") == BuildBackend::Ninja);
    CHECK(BackendForAction("xcode4") == BuildBackend::XcodeBuild);
    CHECK(BackendForAction("vs2019") == BuildBackend::None);
    CHECK(BackendForAction("compilecommands") == BuildBackend::None);
}

TEST_CASE("arcbuild default action is host-specific", "[build]")
{
    CHECK(DefaultActionFor(HostPlatform::Windows) == "vs2026");
    CHECK(DefaultActionFor(HostPlatform::Linux) == "gmake");
    CHECK(DefaultActionFor(HostPlatform::MacOS) == "gmake");
}

TEST_CASE("arcbuild::MakeCli + RequestFromCli carry every flag of spec s3", "[build]")
{
    Arcane::Cli cli = MakeCli();
    SECTION("all flags supplied")
    {
        Argv a{ "build", "--project", "D:/dev/starworks/Gacha/Game", "--config", "Release",
                "--sdk", "D:/dev/starworks/Arcane", "--action", "gmake2", "--force-rebuild", "--quiet" };
        const Arcane::Cli::Result r = cli.Parse(a.argc(), a.argv());
        REQUIRE(r.ok);
        const Request req = RequestFromCli(Command::Build, r);
        CHECK(req.command == Command::Build);
        CHECK(req.project == fs::path("D:/dev/starworks/Gacha/Game"));
        CHECK(req.config  == "Release");
        REQUIRE(req.sdk.has_value());
        CHECK(*req.sdk == fs::path("D:/dev/starworks/Arcane"));
        CHECK(req.action == "gmake2");
        CHECK(req.forceRebuild);
        CHECK(req.quiet);
    }
    SECTION("defaults: Debug, host action, no sdk, no force, not quiet")
    {
        Argv a{ "probe", "--project", "X" };
        const Arcane::Cli::Result r = cli.Parse(a.argc(), a.argv());
        REQUIRE(r.ok);
        const Request req = RequestFromCli(Command::Probe, r);
        CHECK(req.config == "Debug");
        CHECK(req.action == DefaultActionFor(CurrentHostPlatform()));
        CHECK_FALSE(req.sdk.has_value());
        CHECK_FALSE(req.forceRebuild);
        CHECK_FALSE(req.quiet);
    }
    SECTION("an injected default action is carried through parsing")
    {
        Arcane::Cli linuxCli = MakeCli("gmake");
        Argv a{ "build", "--project", "X" };
        const Arcane::Cli::Result r = linuxCli.Parse(a.argc(), a.argv());
        REQUIRE(r.ok);
        CHECK(RequestFromCli(Command::Build, r).action == "gmake");
    }
    SECTION("--project is required")
    {
        Argv a{ "build" };
        const Arcane::Cli::Result r = cli.Parse(a.argc(), a.argv());
        CHECK_FALSE(r.ok);
        CHECK(r.exitCode == kExitRefused);
    }
    SECTION("--config is validated against the three configurations")
    {
        Argv a{ "build", "--project", "X", "--config", "Shipping" };
        const Arcane::Cli::Result r = cli.Parse(a.argc(), a.argv());
        CHECK_FALSE(r.ok);
    }
    SECTION("explicit empty --sdk is engaged-but-empty, not unsupplied")
    {
        Argv a{ "build", "--project", "X", "--sdk", "" };
        const Arcane::Cli::Result r = cli.Parse(a.argc(), a.argv());
        REQUIRE(r.ok);
        const Request req = RequestFromCli(Command::Build, r);
        REQUIRE(req.sdk.has_value());
        CHECK(req.sdk->empty());
    }
}

TEST_CASE("arcbuild::IsValidAction is a premake identifier, not a shell fragment", "[build]")
{
    CHECK(IsValidAction("vs2026"));
    CHECK(IsValidAction("gmake2"));
    CHECK(IsValidAction("ninja"));
    CHECK(IsValidAction("vs_2026"));
    CHECK(IsValidAction("gmake-2"));
    CHECK_FALSE(IsValidAction(""));
    CHECK_FALSE(IsValidAction("vs2026 & echo hi"));
    CHECK_FALSE(IsValidAction("vs2026;whoami"));
    CHECK_FALSE(IsValidAction("vs2026|dir"));
    CHECK_FALSE(IsValidAction("vs2026 && echo"));
    CHECK_FALSE(IsValidAction("vs2026.exe"));
    CHECK_FALSE(IsValidAction("vs 2026"));
}

TEST_CASE("arcbuild::ValidateRequest refuses empty --sdk, a non-identifier --action, and probe-only --force-rebuild",
          "[build]")
{
    Request ok;
    ok.command = Command::Build;
    ok.action  = "vs2026";
    CHECK_FALSE(ValidateRequest(ok).has_value());

    Request emptySdk = ok;
    emptySdk.sdk = fs::path();
    REQUIRE(ValidateRequest(emptySdk).has_value());
    CHECK(*ValidateRequest(emptySdk) == "--sdk requires a non-empty path (an empty value is not ARCANE_SDK)");

    Request badAction = ok;
    badAction.action = "vs2026 & echo hi";
    REQUIRE(ValidateRequest(badAction).has_value());
    CHECK(ValidateRequest(badAction)->find("invalid --action") != std::string::npos);

    Request probeForce;
    probeForce.command      = Command::Probe;
    probeForce.action       = "vs2026";
    probeForce.forceRebuild = true;
    REQUIRE(ValidateRequest(probeForce).has_value());
    CHECK(ValidateRequest(probeForce)->find("--force-rebuild") != std::string::npos);

    for (const Command command : { Command::Generate, Command::Build, Command::Rebuild, Command::Clean })
    {
        Request force = ok;
        force.command = command;
        force.forceRebuild = true;
        CHECK_FALSE(ValidateRequest(force).has_value());
    }
}

// ---- SDK precedence ---------------------------------------------------------

TEST_CASE("arcbuild::ResolveSdk: --sdk beats ARCANE_SDK beats refusal", "[build]")
{
    const fs::path flag = "D:/flag/sdk";
    CHECK(ResolveSdk(flag, "D:/env/sdk") == flag);
    CHECK(ResolveSdk(flag, nullptr)      == flag);
    CHECK(ResolveSdk(std::nullopt, "D:/env/sdk") == fs::path("D:/env/sdk"));
    CHECK_FALSE(ResolveSdk(std::nullopt, nullptr).has_value());
    CHECK_FALSE(ResolveSdk(std::nullopt, "").has_value());   // set-but-empty is unset
}

// ---- bootstrap --------------------------------------------------------------

TEST_CASE("arcbuild probe bootstrap needs no SDK and does not export one", "[build]")
{
    TempArcProject project("probe_without_sdk");
    FakeEnvironment environment;
    Bootstrap bootstrap(environment);

    Request request;
    request.command = Command::Probe;
    request.project = project.root;

    const BootstrapResult result = bootstrap.Prepare(request);
    REQUIRE(result.has_value());
    CHECK_FALSE(result->sdkRoot.has_value());
    CHECK(environment.setCalls == 0);
}

TEST_CASE("arcbuild non-probe bootstrap refuses a missing SDK", "[build]")
{
    TempArcProject project("build_without_sdk");
    FakeEnvironment environment;
    Bootstrap bootstrap(environment);

    Request request;
    request.command = Command::Build;
    request.project = project.root;

    const BootstrapResult result = bootstrap.Prepare(request);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("no SDK") != std::string::npos);
    CHECK(environment.setCalls == 0);
}

TEST_CASE("arcbuild explicit SDK is absolutized and exported for non-probe commands", "[build]")
{
    TempArcProject project("explicit_sdk");
    FakeEnvironment environment;
    Bootstrap bootstrap(environment);

    Request request;
    request.command = Command::Generate;
    request.project = project.root;
    request.sdk = fs::path("relative-sdk-root");

    const BootstrapResult result = bootstrap.Prepare(request);
    REQUIRE(result.has_value());
    REQUIRE(result->sdkRoot.has_value());
    CHECK(result->sdkRoot->is_absolute());
    CHECK(environment.setCalls == 1);
    CHECK(environment.exported == *result->sdkRoot);
}

// ---- the s4.3 decision table ---------------------------------------------------

TEST_CASE("arcbuild::ConfigWantsDebugCrt: Debug only; Dist maps to Release for the probe", "[build]")
{
    CHECK(ConfigWantsDebugCrt("Debug"));
    CHECK_FALSE(ConfigWantsDebugCrt("Release"));
    CHECK_FALSE(ConfigWantsDebugCrt("Dist"));
}

TEST_CASE("arcbuild::ClassifySlot lands on exactly one s4.3 row", "[build]")
{
    using Arcane::CrtFlavor;
    // absent -> nothing to be wrong about, whatever the scanner would say
    CHECK(ClassifySlot(false, CrtFlavor::Unknown, "Debug")   == SlotState::Absent);
    CHECK(ClassifySlot(false, CrtFlavor::Debug,   "Release") == SlotState::Absent);
    // present, flavor matches --config
    CHECK(ClassifySlot(true, CrtFlavor::Debug,   "Debug")   == SlotState::Match);
    CHECK(ClassifySlot(true, CrtFlavor::Release, "Release") == SlotState::Match);
    CHECK(ClassifySlot(true, CrtFlavor::Release, "Dist")    == SlotState::Match);   // Dist == release CRT
    // present, flavor mismatches
    CHECK(ClassifySlot(true, CrtFlavor::Release, "Debug")   == SlotState::Mismatch);
    CHECK(ClassifySlot(true, CrtFlavor::Debug,   "Release") == SlotState::Mismatch);
    CHECK(ClassifySlot(true, CrtFlavor::Debug,   "Dist")    == SlotState::Mismatch);
    // present, unreadable -> its own row, never silently "match"
    CHECK(ClassifySlot(true, CrtFlavor::Unknown, "Debug")   == SlotState::Unreadable);
    CHECK(ClassifySlot(true, CrtFlavor::Unknown, "Release") == SlotState::Unreadable);
}

TEST_CASE("arcbuild::Decide: plain for absent/match and rebuild for mismatch/unreadable",
          "[build]")
{
    // THE INCREMENTAL RULE (spec s4.3) -- the reason the driver exists now. A
    // game project's Binaries\ is ONE slot shared by every configuration; an
    // incremental msbuild reasons about the per-config object tree and can
    // report "up to date" over the OTHER config's DLL (observed live: a
    // 0.24s "All outputs are up-to-date" followed by "the rebuilt module
    // still failed to load"). The editor used to force /t:Rebuild on every
    // build to close that; the driver forces it ONLY when the slot's CRT
    // flavor says it must, so a wizard-made component costs one TU + a link.
    SECTION("the decision follows the slot")
    {
        CHECK_FALSE(Decide(SlotState::Absent).rebuild);
        CHECK_FALSE(Decide(SlotState::Match).rebuild);
        CHECK(Decide(SlotState::Mismatch).rebuild);
        CHECK(Decide(SlotState::Unreadable).rebuild);
    }
    SECTION("every verdict says why, and the unreadable one says so in words")
    {
        for (SlotState s : { SlotState::Absent, SlotState::Match, SlotState::Mismatch, SlotState::Unreadable })
        {
            const Verdict v = Decide(s);
            REQUIRE(v.reason != nullptr);
            CHECK(std::string(v.reason).size() > 10);
        }
        CHECK(std::string(Decide(SlotState::Unreadable).reason).find("unreadable") != std::string::npos);
        CHECK(std::string(Decide(SlotState::Mismatch).reason).find("rebuild") != std::string::npos);
    }
}

TEST_CASE("arcbuild selects rebuild only for forced or rebuilding slot verdicts", "[build]")
{
    CHECK(OperationForBuild(false, Decide(SlotState::Absent)) == BuildOperation::Build);
    CHECK(OperationForBuild(false, Decide(SlotState::Match)) == BuildOperation::Build);
    CHECK(OperationForBuild(false, Decide(SlotState::Mismatch)) == BuildOperation::Rebuild);
    CHECK(OperationForBuild(true, Decide(SlotState::Match)) == BuildOperation::Rebuild);
}

// ---- exit codes -------------------------------------------------------------

TEST_CASE("arcbuild exit codes: probe 0 on the plain rows, 3 on the rebuild rows", "[build]")
{
    CHECK(ProbeExitCode(SlotState::Absent)     == kExitOk);
    CHECK(ProbeExitCode(SlotState::Match)      == kExitOk);
    CHECK(ProbeExitCode(SlotState::Mismatch)   == kExitProbeRebuild);
    CHECK(ProbeExitCode(SlotState::Unreadable) == kExitProbeRebuild);
    CHECK(kExitRefused == 2);
    CHECK(kExitProbeRebuild == 3);
    // Child exit pass-through and launch-failure -> kExitRefused: the
    // arcbuild::ExecutePlan cases in the process-execution section below.
}

TEST_CASE("filesystem clean failure outranks backend clean failure", "[build]")
{
    CHECK(MergeCleanResults(0, 0) == 0);
    CHECK(MergeCleanResults(7, 0) == 7);
    CHECK(MergeCleanResults(0, kExitRefused) == kExitRefused);
    CHECK(MergeCleanResults(7, kExitRefused) == kExitRefused);
}

// ---- orchestration ----------------------------------------------------------

TEST_CASE("arcbuild build orchestration orders work and short-circuits generation failures", "[build]")
{
    std::vector<std::string> events;
    RecordingExecutor executor{ events };
    RecordingSlots slots{ events };
    RecordingCleaner cleaner{ events };
    RecordingOutput output;
    BuildPipeline pipeline(executor, slots, cleaner, output);

    SECTION("ordinary build generates, inspects, then builds")
    {
        CHECK(pipeline.Run(PipelineContext(Command::Build)) == kExitOk);
        CHECK(events == std::vector<std::string>{ "generate", "inspect", "build" });
    }

    SECTION("failed generation stops before inspection and build")
    {
        executor.generateExit = 17;
        CHECK(pipeline.Run(PipelineContext(Command::Build)) == 17);
        CHECK(events == std::vector<std::string>{ "generate" });
    }

    SECTION("forced build rebuilds without inspecting the slot")
    {
        DriverContext context = PipelineContext(Command::Build);
        context.request.forceRebuild = true;
        CHECK(pipeline.Run(context) == kExitOk);
        CHECK(events == std::vector<std::string>{ "generate", "rebuild" });
    }
}

TEST_CASE("arcbuild probe only inspects and reports even when output is quiet", "[build]")
{
    std::vector<std::string> events;
    RecordingExecutor executor{ events };
    RecordingSlots slots{ events };
    RecordingCleaner cleaner{ events };
    RecordingOutput output;
    output.SetQuiet(true);
    BuildPipeline pipeline(executor, slots, cleaner, output);

    DriverContext context = PipelineContext(Command::Probe);
    context.sdkRoot.reset();
    CHECK(pipeline.Run(context) == kExitOk);
    CHECK(events == std::vector<std::string>{ "inspect" });
    REQUIRE(output.messages.size() == 1);
    CHECK(output.messages[0] == "always:probe row");
}

TEST_CASE("arcbuild probe status reports an absent SDK", "[build]")
{
    std::vector<std::string> events;
    RecordingExecutor executor{ events };
    RecordingSlots slots{ events };
    RecordingCleaner cleaner{ events };
    RecordingOutput output;
    BuildPipeline pipeline(executor, slots, cleaner, output);
    DriverContext context = PipelineContext(Command::Probe);
    context.sdkRoot.reset();
    slots.state = SlotState::Mismatch;

    CHECK(pipeline.Run(context) == kExitProbeRebuild);
    CHECK(events == std::vector<std::string>{ "inspect" });
    REQUIRE(output.messages.size() == 2);
    CHECK(output.messages[0].find("against SDK <none>") != std::string::npos);
    CHECK(output.messages[1] == "always:probe row");
}

TEST_CASE("arcbuild slot inspection distinguishes a missing slot from an unreadable slot", "[build]")
{
    TempArcProject fixture("slot_inspection");
    ProjectLayout project;
    project.root = fixture.root;
    project.gameModule = "Fixture.dll";
    SlotInspector inspector;

    SECTION("missing Binaries directory")
    {
        CHECK(inspector.Inspect(project, "Debug").state == SlotState::Absent);
    }

    SECTION("missing module file")
    {
        fs::create_directory(project.root / "Binaries");
        CHECK(inspector.Inspect(project, "Debug").state == SlotState::Absent);
    }

    SECTION("directory in place of module")
    {
        fs::create_directories(project.root / "Binaries" / Arcane::Platform::ModuleFileName("Fixture"));
        CHECK(inspector.Inspect(project, "Debug").state == SlotState::Unreadable);
    }
}

TEST_CASE("arcbuild errors remain visible under quiet output", "[build]")
{
    std::vector<std::string> events;
    RecordingExecutor executor{ events };
    RecordingSlots slots{ events };
    RecordingCleaner cleaner{ events };
    RecordingOutput output;
    output.SetQuiet(true);
    BuildPipeline pipeline(executor, slots, cleaner, output);

    DriverContext context = PipelineContext(Command::Build);
    context.backend = BuildBackend::None;

    CHECK(pipeline.Run(context) == kExitRefused);
    CHECK(events.empty());
    REQUIRE(output.messages.size() == 1);
    CHECK(output.messages[0].rfind("error:", 0) == 0);
}

TEST_CASE("arcbuild clean runs filesystem cleanup after a backend failure", "[build]")
{
    std::vector<std::string> events;
    RecordingExecutor executor{ events };
    executor.cleanExit = 7;
    RecordingSlots slots{ events };
    RecordingCleaner cleaner{ events };
    RecordingOutput output;
    BuildPipeline pipeline(executor, slots, cleaner, output);

    CHECK(pipeline.Run(PipelineContext(Command::Clean)) == 7);
    CHECK(events == std::vector<std::string>{ "backend-clean", "filesystem-clean" });

    events.clear();
    cleaner.cleanExit = kExitRefused;
    CHECK(pipeline.Run(PipelineContext(Command::Clean)) == kExitRefused);
    CHECK(events == std::vector<std::string>{ "backend-clean", "filesystem-clean" });
}

// ---- paths ------------------------------------------------------------------

TEST_CASE("arcbuild::SlotPath is <root>/Binaries/<gameModule>, empty for a content-only project", "[build]")
{
    ProjectLayout project = AphelyonProject();
    CHECK(SlotPath(project) == fs::path("D:/dev/starworks/Gacha/Game") / "Binaries" / Arcane::Platform::ModuleFileName("Aphelyon"));
    project.gameModule.clear();
    CHECK(SlotPath(project).empty());
}

TEST_CASE("arcbuild::SolutionPath: a discovered workspace file wins over the <name>.slnx convention", "[build]")
{
    const ProjectLayout project = AphelyonProject();
    // An ABSOLUTE discovery is used as-is. "D:/..." is absolute only on
    // Windows; POSIX spells an absolute path with a leading '/'.
#if ARC_PLATFORM_WINDOWS
    const fs::path discovered = "D:/dev/starworks/Gacha/Game/Other.sln";
#else
    const fs::path discovered = "/dev/starworks/Gacha/Game/Other.sln";
#endif
    CHECK(SolutionPath(project, discovered) == discovered);
    CHECK(SolutionPath(project, {}) == fs::path("D:/dev/starworks/Gacha/Game") / "Aphelyon.slnx");
    // ComposeMsBuild does not cd: a relative discovery is joined onto root
    // rather than assumed absolute (DiscoverSolution is absolute iff projectRoot is).
    CHECK(SolutionPath(project, fs::path("Other.sln")) ==
          (fs::path("D:/dev/starworks/Gacha/Game") / "Other.sln").lexically_normal());
}

// ---- composition --------------------------------------------------------------
//
// Multibackend hardening Task 4: Compose* returns STRUCTURED process plans
// (ProcessSpec/ProcessPlan, Process.hpp) rather than shell strings -- these
// tests assert on the structure directly, never on a rendered command line.
// RenderProcess (below) is display-only and is never re-parsed.

TEST_CASE("arcbuild::ComposeGenerate is a structured premake invocation, cwd project root", "[build]")
{
    const ProcessSpec spec = ComposeGenerate(AphelyonProject(), PremakePath(), "vs2026");
    CHECK(spec.executable == PremakePath());
    CHECK(spec.arguments == std::vector<std::string>{ "vs2026" });
    REQUIRE(spec.workingDirectory.has_value());
    CHECK(*spec.workingDirectory == AphelyonProject().root);

    // The action is the Linux seam (spec s6): it is a parameter, not a constant.
    CHECK(ComposeGenerate(AphelyonProject(), PremakePath(), "gmake2").arguments
          == std::vector<std::string>{ "gmake2" });
}

TEST_CASE("arcbuild::ComposeMsBuild: {solution, /p:Configuration=<config>, /m, /nologo}; /t:Rebuild or /t:Clean inserted only when asked",
          "[build]")
{
    const fs::path sln = "D:/dev/starworks/Gacha/Game/Aphelyon.slnx";
    const BackendContext context { sln };

    const ProcessPlan plain = ComposeMsBuild(MsBuildPath(), context, "Debug", BuildOperation::Build);
    REQUIRE(plain.steps.size() == 1);
    CHECK(plain.steps[0].executable == MsBuildPath());
    CHECK(plain.steps[0].arguments == std::vector<std::string>{
        sln.generic_string(), "/p:Configuration=Debug", "/m", "/nologo" });
    CHECK_FALSE(plain.steps[0].workingDirectory.has_value());   // no cd -- the solution path is already absolute

    const ProcessPlan rebuild = ComposeMsBuild(MsBuildPath(), context, "Release", BuildOperation::Rebuild);
    REQUIRE(rebuild.steps.size() == 1);
    CHECK(rebuild.steps[0].arguments == std::vector<std::string>{
        sln.generic_string(), "/p:Configuration=Release", "/t:Rebuild", "/m", "/nologo" });

    const ProcessPlan clean = ComposeMsBuild(MsBuildPath(), context, "Dist", BuildOperation::Clean);
    REQUIRE(clean.steps.size() == 1);
    CHECK(clean.steps[0].arguments == std::vector<std::string>{
        sln.generic_string(), "/p:Configuration=Dist", "/t:Clean", "/m", "/nologo" });
}

TEST_CASE("arcbuild::ComposeMake: {-C, root, config=debug}; clean appends 'clean'; rebuild is clean then build",
          "[build]")
{
    const fs::path root = "D:/dev/starworks/Gacha/Game";
    const BackendContext context { root };

    const ProcessPlan build = ComposeMake(MakePath(), context, "Debug", BuildOperation::Build);
    REQUIRE(build.steps.size() == 1);
    CHECK(build.steps[0].executable == MakePath());
    CHECK(build.steps[0].arguments == std::vector<std::string>{ "-C", root.string(), "config=debug" });

    // Config is lower-cased for make's own convention regardless of the
    // engine's Debug/Release/Dist spelling.
    const ProcessPlan releaseBuild = ComposeMake(MakePath(), context, "Release", BuildOperation::Build);
    CHECK(releaseBuild.steps[0].arguments == std::vector<std::string>{ "-C", root.string(), "config=release" });

    const ProcessPlan clean = ComposeMake(MakePath(), context, "Debug", BuildOperation::Clean);
    REQUIRE(clean.steps.size() == 1);
    CHECK(clean.steps[0].arguments == std::vector<std::string>{ "-C", root.string(), "config=debug", "clean" });

    const ProcessPlan rebuild = ComposeMake(MakePath(), context, "Debug", BuildOperation::Rebuild);
    REQUIRE(rebuild.steps.size() == 2);
    CHECK(rebuild.steps[0].arguments.back() == "clean");
    CHECK(rebuild.steps[1].arguments == std::vector<std::string>{ "-C", root.string(), "config=debug" });
}

TEST_CASE("arcbuild::ComposeNinja: {-C, root, <stem>_<Config>}; clean inserts -t clean; rebuild is clean then build",
          "[build]")
{
    const fs::path root = "D:/dev/starworks/Gacha/Game";
    const BackendContext context { root, std::string("Fixture") };

    const ProcessPlan build = ComposeNinja(NinjaPath(), context, "Debug", BuildOperation::Build);
    REQUIRE(build.steps.size() == 1);
    CHECK(build.steps[0].executable == NinjaPath());
    CHECK(build.steps[0].arguments == std::vector<std::string>{ "-C", root.string(), "Fixture_Debug" });

    const ProcessPlan clean = ComposeNinja(NinjaPath(), context, "Debug", BuildOperation::Clean);
    REQUIRE(clean.steps.size() == 1);
    CHECK(clean.steps[0].arguments == std::vector<std::string>{ "-C", root.string(), "-t", "clean", "Fixture_Debug" });

    const ProcessPlan rebuild = ComposeNinja(NinjaPath(), context, "Debug", BuildOperation::Rebuild);
    REQUIRE(rebuild.steps.size() == 2);
    CHECK(rebuild.steps[0].arguments == clean.steps[0].arguments);
    CHECK(rebuild.steps[1].arguments == build.steps[0].arguments);

    // The Config seam: a different --config produces a different target.
    const ProcessPlan release = ComposeNinja(NinjaPath(), context, "Release", BuildOperation::Build);
    CHECK(release.steps[0].arguments == std::vector<std::string>{ "-C", root.string(), "Fixture_Release" });
}

TEST_CASE("arcbuild::ComposeNinja/ComposeXcodeBuild refuse (empty plan) a context with no target -- never a malformed '_Debug' or empty -target",
          "[build]")
{
    // BackendContext::target is what names the thing to build for these two
    // backends (Build.hpp). A context without it -- unreachable through the
    // real BackendResolver, which always stores the module stem, but trivially
    // constructible -- must not compose a target ninja/xcodebuild would only
    // reject one process later: an EMPTY plan is what BuildExecutor already
    // refuses with "cannot compose this operation" (kExitRefused).
    const fs::path root         = "D:/dev/starworks/Gacha/Game";
    const fs::path xcodeProject = root / "Fixture.xcodeproj";

    const BackendContext unset  { root, std::nullopt };
    const BackendContext blank  { root, std::string() };
    const BackendContext xUnset { xcodeProject, std::nullopt };
    const BackendContext xBlank { xcodeProject, std::string() };

    for (const BuildOperation operation : { BuildOperation::Build, BuildOperation::Rebuild, BuildOperation::Clean })
    {
        CHECK(ComposeNinja(NinjaPath(), unset, "Debug", operation).steps.empty());
        CHECK(ComposeNinja(NinjaPath(), blank, "Debug", operation).steps.empty());
        CHECK(ComposeXcodeBuild(XcodeBuildPath(), xUnset, "Debug", operation).steps.empty());
        CHECK(ComposeXcodeBuild(XcodeBuildPath(), xBlank, "Debug", operation).steps.empty());

        // ComposeBuild's dispatch carries the same refusal through.
        CHECK(ComposeBuild(BuildBackend::Ninja,      NinjaPath(),      unset,  "Debug", operation).steps.empty());
        CHECK(ComposeBuild(BuildBackend::XcodeBuild, XcodeBuildPath(), xUnset, "Debug", operation).steps.empty());
    }

    // And the positive control: WITH a target the same calls compose one
    // step per operation (the sibling cases above pin the exact argv).
    const BackendContext named { root, std::string("Fixture") };
    CHECK_FALSE(ComposeNinja(NinjaPath(), named, "Debug", BuildOperation::Build).steps.empty());
    const BackendContext xNamed { xcodeProject, std::string("Fixture") };
    CHECK_FALSE(ComposeXcodeBuild(XcodeBuildPath(), xNamed, "Debug", BuildOperation::Build).steps.empty());
}

TEST_CASE("arcbuild::ComposeXcodeBuild: -target (no shared scheme in beta8); rebuild is ONE invocation ending 'clean build'",
          "[build]")
{
    const fs::path xcodeProject = "D:/dev/starworks/Gacha/Game/Fixture.xcodeproj";
    const BackendContext context { xcodeProject, std::string("Fixture") };

    const ProcessPlan build = ComposeXcodeBuild(XcodeBuildPath(), context, "Debug", BuildOperation::Build);
    REQUIRE(build.steps.size() == 1);
    CHECK(build.steps[0].executable == XcodeBuildPath());
    CHECK(build.steps[0].arguments == std::vector<std::string>{
        "-project", xcodeProject.string(), "-target", "Fixture", "-configuration", "Debug", "build" });

    const ProcessPlan rebuild = ComposeXcodeBuild(XcodeBuildPath(), context, "Debug", BuildOperation::Rebuild);
    REQUIRE(rebuild.steps.size() == 1);   // ONE Xcode invocation, unlike Make/Ninja's two-step rebuild
    CHECK(rebuild.steps[0].arguments == std::vector<std::string>{
        "-project", xcodeProject.string(), "-target", "Fixture", "-configuration", "Debug", "clean", "build" });

    const ProcessPlan clean = ComposeXcodeBuild(XcodeBuildPath(), context, "Debug", BuildOperation::Clean);
    REQUIRE(clean.steps.size() == 1);
    CHECK(clean.steps[0].arguments == std::vector<std::string>{
        "-project", xcodeProject.string(), "-target", "Fixture", "-configuration", "Debug", "clean" });
}

TEST_CASE("arcbuild::RenderProcess is readable, whitespace-triggered quoting -- never re-parsed, never shell-escaped",
          "[build]")
{
    ProcessSpec spec;
    spec.executable        = fs::path("C:/Program Files/premake5/premake5.exe");
    spec.arguments         = { "vs2026", "/p:Configuration=Debug", "an arg with spaces" };
    spec.workingDirectory  = fs::path("D:/dev/starworks/Gacha/Game");

    const std::string rendered = RenderProcess(spec);

    CHECK(rendered.find("\"C:/Program Files/premake5/premake5.exe\"") != std::string::npos);
    CHECK(rendered.find(" vs2026 ") != std::string::npos);              // bare -- no internal whitespace
    CHECK(rendered.find("/p:Configuration=Debug") != std::string::npos);
    CHECK(rendered.find("\"/p:Configuration=Debug\"") == std::string::npos);  // bare -- no internal whitespace
    CHECK(rendered.find("\"an arg with spaces\"") != std::string::npos);

    // No shell metacharacter is ever escaped: an embedded quote (or `&`,
    // `|`, `;`) passes straight through, proving this text is for a human
    // reading a log, never for a shell to execute.
    ProcessSpec withQuote;
    withQuote.executable = fs::path("tool");
    withQuote.arguments  = { "value\"with\"quotes", "a&b|c;d" };
    const std::string renderedQuote = RenderProcess(withQuote);
    CHECK(renderedQuote.find("value\"with\"quotes") != std::string::npos);
    CHECK(renderedQuote.find("a&b|c;d") != std::string::npos);
}

TEST_CASE("arcbuild::CleanTargets is exactly Binaries/ and Intermediate/<config>/", "[build]")
{
    const std::vector<fs::path> t = CleanTargets(AphelyonProject(), "Debug");
    REQUIRE(t.size() == 2);
    CHECK(t[0] == fs::path("D:/dev/starworks/Gacha/Game") / "Binaries");
    CHECK(t[1] == fs::path("D:/dev/starworks/Gacha/Game") / "Intermediate" / "Debug");
    // Never Source/, Content/, Saved/, the .slnx, nor Intermediate/Artifacts (arccook's).
    for (const fs::path& p : t)
    {
        const std::string s = p.generic_string();
        CHECK(s.find("Source") == std::string::npos);
        CHECK(s.find("Content") == std::string::npos);
        CHECK(s.find("Saved") == std::string::npos);
        CHECK(s.find("Artifacts") == std::string::npos);
        CHECK(s.find(".slnx") == std::string::npos);
    }
}

// ---- Ninja single-slot staging (multibackend hardening, review F4) ----------
//
// beta8's ninja action links each configuration to its own
// Intermediate/<Config>/Ninja/Binaries/<gameModule> (build/arcane.lua) and
// arcbuild copies the result into the slot itself -- a Premake post-build
// step cannot do it on Windows (Stage.hpp has the characterization). Three
// layers, each pinned: the PATH policy (NinjaLinkOutput), the copy
// (StageBuiltModule), and the executor hooking the copy onto a successful
// Ninja plan and nothing else.

TEST_CASE("arcbuild::NinjaLinkOutput is Intermediate/<config>/Ninja/Binaries/<gameModule>, inside a CleanTargets entry",
          "[build]")
{
    const ProjectLayout project = AphelyonProject();

    const fs::path debug = NinjaLinkOutput(project, "Debug");
    CHECK(debug == fs::path("D:/dev/starworks/Gacha/Game") / "Intermediate" / "Debug" / "Ninja" / "Binaries" / Arcane::Platform::ModuleFileName("Aphelyon"));
    CHECK(NinjaLinkOutput(project, "Release").generic_string().find("/Intermediate/Release/Ninja/Binaries/") != std::string::npos);

    // Inside Intermediate/<config>/ -- the clean contract that makes a
    // soft-skipped/failed backend clean unable to strand the linked module.
    const std::vector<fs::path> clean = CleanTargets(project, "Debug");
    REQUIRE(clean.size() == 2);
    const std::string cleanIntermediate = clean[1].generic_string() + "/";
    CHECK(debug.generic_string().rfind(cleanIntermediate, 0) == 0);

    // Never the slot itself (that is what the unique location exists to
    // avoid), never the retired Intermediate/Ninja/<Config> spelling.
    CHECK(debug != SlotPath(project));
    CHECK(debug.generic_string().find("Intermediate/Ninja/") == std::string::npos);

    // No game module, no link output -- same shape as SlotPath.
    ProjectLayout noModule = project;
    noModule.gameModule.clear();
    CHECK(NinjaLinkOutput(noModule, "Debug").empty());
}

TEST_CASE("arcbuild::StageBuiltModule copies the built module (and its .pdb) over the slot, and refuses what it cannot stage",
          "[build]")
{
    TempDir root("stage_built_module");
    const fs::path built = root.path / "Intermediate" / "Debug" / "Ninja" / "Binaries" / Arcane::Platform::ModuleFileName("Fixture");
    const fs::path slot  = root.path / "Binaries" / Arcane::Platform::ModuleFileName("Fixture");

    SECTION("built module + pdb -> slot + pdb, Binaries/ created on the way")
    {
        fs::create_directories(built.parent_path());
        std::ofstream(built, std::ios::binary) << "dll-bytes-v1";
        std::ofstream(fs::path(built).replace_extension(".pdb"), std::ios::binary) << "pdb-bytes";

        const auto staged = StageBuiltModule(built, slot);
        REQUIRE(staged.has_value());
        REQUIRE(staged->copied.size() == 2);
        CHECK(staged->copied[0] == slot);
        CHECK(staged->copied[1] == fs::path(slot).replace_extension(".pdb"));

        std::ifstream slotFile(slot, std::ios::binary);
        std::string slotBytes((std::istreambuf_iterator<char>(slotFile)), std::istreambuf_iterator<char>());
        CHECK(slotBytes == "dll-bytes-v1");
        CHECK(fs::is_regular_file(fs::path(slot).replace_extension(".pdb")));

        // The single-slot rule: a second build OVERWRITES the slot.
        std::ofstream(built, std::ios::binary | std::ios::trunc) << "dll-bytes-v2";
        REQUIRE(StageBuiltModule(built, slot).has_value());
        std::ifstream slotFile2(slot, std::ios::binary);
        std::string slotBytes2((std::istreambuf_iterator<char>(slotFile2)), std::istreambuf_iterator<char>());
        CHECK(slotBytes2 == "dll-bytes-v2");
    }

    SECTION("no pdb is fine -- only the module is staged")
    {
        fs::create_directories(built.parent_path());
        std::ofstream(built, std::ios::binary) << "dll-bytes";

        const auto staged = StageBuiltModule(built, slot);
        REQUIRE(staged.has_value());
        CHECK(staged->copied == std::vector<fs::path>{ slot });
        CHECK_FALSE(fs::exists(fs::path(slot).replace_extension(".pdb")));
    }

    SECTION("the backend said success but linked nothing -> a named refusal, slot untouched")
    {
        const auto staged = StageBuiltModule(built, slot);
        REQUIRE_FALSE(staged.has_value());
        CHECK(staged.error().find(built.generic_string()) != std::string::npos);
        CHECK_FALSE(fs::exists(slot));
    }

    SECTION("empty paths are refused, not silently no-op'd")
    {
        CHECK_FALSE(StageBuiltModule({}, slot).has_value());
        CHECK_FALSE(StageBuiltModule(built, {}).has_value());
    }
}


// ---- process execution (Task 5, multibackend hardening) --------------------
//
// QuoteWindowsArgument/BuildWindowsCommandLine are pure string algorithms --
// no spawn, tested directly first (TDD). ExecutePlan is tested against a
// FakeProcessRunner (still no spawn) for its short-circuit/error-mapping
// policy, and against the REAL ProcessRunner for a guaranteed launch
// failure. The remaining cases spawn the small, dependency-free
// arcbuild-process-fixture.exe (this workspace's own build output, never an
// external tool -- premake's dependson guarantees it exists) to prove real
// CreateProcessW quoting/streaming/cwd/handle-inheritance behavior end to
// end -- see ProcessFixtureMain.cpp for its protocol.

TEST_CASE("arcbuild::QuoteWindowsArgument follows the real MSVC CRT argv parsing rules", "[build]")
{
    // Bare, no special characters: left unquoted.
    CHECK(QuoteWindowsArgument("plain") == "plain");

    // "" would vanish from the child's argv entirely if left unquoted --
    // always quoted, even though it contains nothing to escape.
    CHECK(QuoteWindowsArgument("") == "\"\"");

    // A space forces quoting; nothing inside is otherwise touched.
    CHECK(QuoteWindowsArgument("hello world") == "\"hello world\"");

    // An embedded quote is escaped as \" (0 preceding backslashes -> 1) --
    // and that alone forces quoting, even with no whitespace at all.
    CHECK(QuoteWindowsArgument(R"(a"b)") == R"("a\"b")");

    // One trailing backslash immediately before the closing quote WE add:
    // doubled to 2, so the CRT reads "2n backslashes then a quote" back as
    // n (=1) literal backslashes with the quote ending the string -- never
    // as an escaped quote. Quoting is forced here by the embedded space.
    CHECK(QuoteWindowsArgument(R"(C:\Program Files\)") == R"("C:\Program Files\\")");

    // Multiple (3) trailing backslashes: doubled to 6, same rule, n=3.
    CHECK(QuoteWindowsArgument(R"(C:\Program Files\\\)") ==
          R"("C:\Program Files\\\\\\")");

    // A non-ASCII byte sequence (UTF-8), forced into quoting by a space,
    // passes straight through untouched. Deliberately a std::string, never
    // a std::filesystem::path: path::string() on Windows narrows through
    // the ACTIVE CODE PAGE, not UTF-8 -- irrelevant to what this function
    // does, but a real trap for a test that routed a non-ASCII VALUE
    // through one on its way to becoming a std::string.
    const std::string nonAsciiWithSpace = "caf\u00e9 bar";
    CHECK(QuoteWindowsArgument(nonAsciiWithSpace) == "\"" + nonAsciiWithSpace + "\"");

    // The same non-ASCII bytes with no space: no quoting triggered at all.
    const std::string nonAsciiBare = "caf\u00e9";
    CHECK(QuoteWindowsArgument(nonAsciiBare) == nonAsciiBare);
}

TEST_CASE("arcbuild::BuildWindowsCommandLine quotes the executable and every argument, space-joined", "[build]")
{
    ProcessSpec spec;
    spec.executable = fs::path("C:/Program Files/premake5/premake5.exe");
    spec.arguments  = { "vs2026", "an arg with spaces", "bare" };

    const std::string commandLine = BuildWindowsCommandLine(spec);

    // The executable comes first and is quoted (it contains a space) --
    // exact separator rendering is not this test's concern.
    CHECK(commandLine.front() == '"');
    CHECK(commandLine.find("premake5.exe\"") != std::string::npos);
    // Bare argv entries (no whitespace) are never quoted.
    CHECK(commandLine.find(" vs2026 ") != std::string::npos);
    REQUIRE(commandLine.size() >= 4);
    CHECK(commandLine.substr(commandLine.size() - 4) == "bare");
    // An argument with a space is quoted, exactly as QuoteWindowsArgument
    // would quote it standalone.
    CHECK(commandLine.find("\"an arg with spaces\"") != std::string::npos);
}

namespace
{
    // Records every spec it was asked to run and hands back scripted
    // results in order -- never spawns anything.
    struct FakeProcessRunner final : IProcessRunner
    {
        mutable std::vector<ProcessSpec> seen;
        std::vector<ProcessResult>       results;
        mutable std::size_t              next = 0;

        ProcessResult Run(const ProcessSpec& spec, std::string_view) const override
        {
            seen.push_back(spec);
            REQUIRE(next < results.size());
            return results[next++];
        }
    };
}

TEST_CASE("arcbuild::ExecutePlan stops at the first non-zero result; a child exit code passes through unchanged",
          "[build]")
{
    ProcessSpec stepA; stepA.executable = "tool-a.exe";
    ProcessSpec stepB; stepB.executable = "tool-b.exe";

    RecordingOutput output;
    FakeProcessRunner fake;

    ProcessPlan twoStep;
    twoStep.steps = { stepA, stepB };

    fake.results = { ProcessResult{9}, ProcessResult{0} };
    CHECK(ExecutePlan(twoStep, fake, output, "[ninja]") == 9);
    CHECK(fake.seen.size() == 1);   // stepB never ran

    ProcessPlan oneStep;
    oneStep.steps = { stepA };

    fake.seen.clear();
    fake.next = 0;
    fake.results = { ProcessResult{2} };
    CHECK(ExecutePlan(oneStep, fake, output, "[tool]") == 2);
    CHECK(fake.seen.size() == 1);

    // An empty plan is a no-op success.
    fake.seen.clear();
    fake.next = 0;
    fake.results.clear();
    CHECK(ExecutePlan(ProcessPlan{}, fake, output, "[empty]") == kExitOk);
    CHECK(fake.seen.empty());
}

TEST_CASE("arcbuild::ExecutePlan maps a real launch failure to kExitRefused, never a synthetic child code",
          "[build]")
{
    ProcessSpec missing;
    missing.executable = fs::path("D:/definitely/not/a/real/tool-arcbuild-should-never-find.exe");

    RecordingOutput output;
    ProcessRunner runner(output);

    const ProcessResult direct = runner.Run(missing, "[missing]");
    REQUIRE_FALSE(direct.has_value());
    CHECK_FALSE(direct.error().message.empty());

    ProcessPlan plan;
    plan.steps = { missing };
    CHECK(ExecutePlan(plan, runner, output, "[missing]") == kExitRefused);

    // Refused, visibly: the error reached Output, not just the return code.
    bool sawError = false;
    for (const std::string& m : output.messages)
        if (m.rfind("error:", 0) == 0)
            sawError = true;
    CHECK(sawError);
}

TEST_CASE("arcbuild::BuildExecutor stages the Ninja module into the slot after a successful Build/Rebuild plan -- and only then, only for Ninja, never for Clean",
          "[build]")
{
    // A real BuildExecutor over a FAKE runner (review F4, see the Ninja
    // single-slot staging section above for the layers under it): the fake
    // ninja.exe on an overridden PATH satisfies BackendResolver::
    // ResolveBuilder (a PATH hit is a regular file on Windows), the two
    // touched .ninja files satisfy ResolveBackendContext, and the
    // FakeProcessRunner scripts the `ninja Fixture_Debug` step's exit code
    // -- nothing is ever spawned.
    TempArcProject fixture("executor_ninja_staging");
    TempDir        toolDir("executor_ninja_staging_tool");

#ifdef _WIN32
    std::ofstream(toolDir.path / "ninja.exe", std::ios::binary) << "not really";
#else
    const fs::path fakeNinja = toolDir.path / "ninja";
    std::ofstream(fakeNinja, std::ios::binary) << "#!/bin/sh\n";
    fs::permissions(fakeNinja, fs::perms::owner_all, fs::perm_options::replace);
#endif
    EnvOverride path("PATH", toolDir.path.string());
    EnvOverride pathExt("PATHEXT", ".EXE");

    std::ofstream(fixture.root / "build.ninja", std::ios::binary) << "";
    std::ofstream(fixture.root / "Fixture.ninja", std::ios::binary) << "";

    DriverContext context;
    context.request.command    = Command::Build;
    context.request.action     = "ninja";
    context.request.config     = "Debug";
    context.backend            = BuildBackend::Ninja;
    context.sdkRoot            = fs::path("D:/sdk");
    context.project.root       = fixture.root;
    context.project.manifest   = fixture.root / "Fixture.arcproj";
    context.project.name       = "Fixture";
    context.project.gameModule = "Fixture.dll";

    const fs::path built = NinjaLinkOutput(context.project, "Debug");
    const fs::path slot  = SlotPath(context.project);

    BackendResolver   backends;
    RecordingOutput   output;
    FakeProcessRunner fake;
    BuildExecutor     executor(backends, fake, output);

    SECTION("plan succeeds and the module was linked -> staged into the slot, exit 0")
    {
        fs::create_directories(built.parent_path());
        std::ofstream(built, std::ios::binary) << "linked";

        fake.results = { ProcessResult{0} };
        CHECK(executor.Build(context, BuildOperation::Build) == kExitOk);
        REQUIRE(fake.seen.size() == 1);
        CHECK(fake.seen[0].arguments.back() == "Fixture_Debug");
        CHECK(fs::is_regular_file(slot));

        bool sawStaged = false;
        for (const std::string& m : output.messages)
            if (m.rfind("info:staged ", 0) == 0)
                sawStaged = true;
        CHECK(sawStaged);
    }

    SECTION("a two-step rebuild stages once, after the build step")
    {
        fs::create_directories(built.parent_path());
        std::ofstream(built, std::ios::binary) << "linked";

        fake.results = { ProcessResult{0}, ProcessResult{0} };
        CHECK(executor.Build(context, BuildOperation::Rebuild) == kExitOk);
        CHECK(fake.seen.size() == 2);
        CHECK(fs::is_regular_file(slot));
    }

    SECTION("the child failed -> its exit code passes through unchanged and NOTHING is staged")
    {
        fs::create_directories(built.parent_path());
        std::ofstream(built, std::ios::binary) << "stale-from-an-earlier-link";

        fake.results = { ProcessResult{7} };
        CHECK(executor.Build(context, BuildOperation::Build) == 7);
        CHECK_FALSE(fs::exists(slot));
    }

    SECTION("the child exited 0 but linked nothing -> the driver's own refusal (2), never a fabricated success")
    {
        fake.results = { ProcessResult{0} };
        CHECK(executor.Build(context, BuildOperation::Build) == kExitRefused);
        CHECK_FALSE(fs::exists(slot));

        bool sawError = false;
        for (const std::string& m : output.messages)
            if (m.rfind("error:", 0) == 0 && m.find(built.generic_string()) != std::string::npos)
                sawError = true;
        CHECK(sawError);
    }

    SECTION("a Clean operation never stages, even on success (nothing was linked to stage)")
    {
        fs::create_directories(built.parent_path());
        std::ofstream(built, std::ios::binary) << "left-over";

        fake.results = { ProcessResult{0} };
        CHECK(executor.Build(context, BuildOperation::Clean) == kExitOk);
        CHECK(fake.seen.size() == 1);
        CHECK_FALSE(fs::exists(slot));
    }

    SECTION("MSBuild's plan is left alone: no staging hook (its link output IS the slot)")
    {
        // Same executor, the MSBuild backend: a touched .slnx for the
        // context, and a fake msbuild on the same overridden PATH so
        // ResolveMsBuild resolves SOMETHING on every desk (vswhere's answer
        // wins where a Visual Studio exists; the PATH fake elsewhere). The
        // scripted success must not invent a slot the fake never linked.
        std::ofstream(fixture.root / "Fixture.slnx", std::ios::binary) << "";
#ifdef _WIN32
        std::ofstream(toolDir.path / "msbuild.exe", std::ios::binary) << "not really";
#else
        const fs::path fakeMsBuild = toolDir.path / "msbuild";
        std::ofstream(fakeMsBuild, std::ios::binary) << "#!/bin/sh\n";
        fs::permissions(fakeMsBuild, fs::perms::owner_all, fs::perm_options::replace);
#endif
        context.request.action = "vs2026";
        context.backend        = BuildBackend::MsBuild;

        fake.results = { ProcessResult{0} };
        CHECK(executor.Build(context, BuildOperation::Build) == kExitOk);
        CHECK(fake.seen.size() == 1);
        CHECK_FALSE(fs::exists(slot));
    }
}

// Task 6: the three fixture-spawning cases below are Windows-only, not
// because of the Win32 APIs two of them call, but because the fixture itself
// is (wmain + windows.h -- and premake5.lua only emits the
// arcbuild-process-fixture project for a Windows target). The POSIX
// equivalents live at the bottom of this section and use /bin/sh as their
// child instead.
#ifdef _WIN32
TEST_CASE("arcbuild::ProcessRunner: exact argv, merged stdout+stderr, cwd, and a child's exit code round-trip through a real CreateProcessW launch",
          "[build]")
{
    REQUIRE(fs::is_regular_file(ProcessFixtureExe()));

    TempDir cwdDir("process_runner_cwd");

    RecordingOutput output;
    ProcessRunner runner(output);

    ProcessSpec spec;
    spec.executable       = ProcessFixtureExe();
    spec.workingDirectory = cwdDir.path;
    spec.arguments        =
    {
        "",                                 // empty argv entry
        "has space",                        // whitespace forces quoting
        R"(has"quote)",                     // embedded quote
        R"(C:\Program Files\)",             // space + one trailing backslash
        "caf\u00e9",                        // non-ASCII, bare (no quoting)
        "--stderr", "err-line",
        "--exit", "5",
    };

    const ProcessResult result = runner.Run(spec, "[fixture]");
    REQUIRE(result.has_value());
    CHECK(*result == 5);

    std::vector<std::string> childLines;
    for (const std::string& m : output.messages)
        if (m.rfind("[fixture]:", 0) == 0)
            childLines.push_back(m.substr(std::string("[fixture]:").size()));

    // argv round-trips exactly, in order, through real Win32 quoting on the
    // way out and the real CRT argv split on the fixture's own way in.
    REQUIRE(childLines.size() >= 7);
    CHECK(childLines[0] == "ARG:0:");
    CHECK(childLines[1] == "ARG:1:has space");
    CHECK(childLines[2] == R"(ARG:2:has"quote)");
    CHECK(childLines[3] == R"(ARG:3:C:\Program Files\)");
    CHECK(childLines[4] == std::string("ARG:4:") + "caf\u00e9");

    bool sawCwd = false;
    bool sawStderrLine = false;
    for (const std::string& line : childLines)
    {
        if (line.rfind("CWD:", 0) == 0)
        {
            sawCwd = true;
            CHECK(fs::equivalent(fs::path(line.substr(4)), cwdDir.path));
        }
        if (line == "err-line")
            sawStderrLine = true;
    }
    CHECK(sawCwd);
    CHECK(sawStderrLine);   // stderr is MERGED into the very same stream Child() sees
}

TEST_CASE("arcbuild::ProcessRunner: lpApplicationName pins the exact binary despite a same-named decoy earlier on PATH",
          "[build]")
{
    REQUIRE(fs::is_regular_file(ProcessFixtureExe()));

    const char* systemRoot = std::getenv("SystemRoot");
    REQUIRE(systemRoot != nullptr);
    const fs::path cmdExe = fs::path(systemRoot) / "System32" / "cmd.exe";
    REQUIRE(fs::is_regular_file(cmdExe));

    TempDir decoyDir("process_runner_path_decoy");
    const fs::path decoyExe = decoyDir.path / ProcessFixtureExe().filename();

    std::error_code copyEc;
    fs::copy_file(cmdExe, decoyExe, fs::copy_options::overwrite_existing, copyEc);
    REQUIRE_FALSE(copyEc);

    const char* originalPath = std::getenv("PATH");
    const std::string combinedPath =
        decoyDir.path.string() + ";" + (originalPath ? originalPath : "");
    EnvOverride path("PATH", combinedPath);

    RecordingOutput output;
    ProcessRunner runner(output);

    ProcessSpec spec;
    spec.executable = ProcessFixtureExe();   // the REAL fixture's absolute path
    spec.arguments  = { "sentinel-arg", "--exit", "0" };

    const ProcessResult result = runner.Run(spec, "[fixture]");
    REQUIRE(result.has_value());
    CHECK(*result == 0);

    // Only the real fixture speaks this protocol -- cmd.exe, launched with
    // the same argv, would not print an "ARG:0:sentinel-arg" line.
    bool sawFixtureProtocol = false;
    for (const std::string& m : output.messages)
        if (m == "[fixture]:ARG:0:sentinel-arg")
            sawFixtureProtocol = true;
    CHECK(sawFixtureProtocol);
}

TEST_CASE("arcbuild::ProcessRunner: PROC_THREAD_ATTRIBUTE_HANDLE_LIST inherits only the pipe write handle, never broad inheritance",
          "[build]")
{
    REQUIRE(fs::is_regular_file(ProcessFixtureExe()));

    // An unrelated, deliberately inheritable handle this process owns but
    // never puts in ANY handle list -- if ProcessRunner::Run relied on plain
    // bInheritHandles=TRUE without PROC_THREAD_ATTRIBUTE_HANDLE_LIST, this
    // would inherit into the child right alongside the pipe write handle.
    SECURITY_ATTRIBUTES sentinelAttributes{};
    sentinelAttributes.nLength       = sizeof(sentinelAttributes);
    sentinelAttributes.bInheritHandle = TRUE;

    HANDLE sentinel = ::CreateEventW(&sentinelAttributes, TRUE, FALSE, nullptr);
    REQUIRE(sentinel != nullptr);

    RecordingOutput output;
    ProcessRunner runner(output);

    ProcessSpec spec;
    spec.executable = ProcessFixtureExe();
    spec.arguments  =
    {
        // The sentinel's numeric value, as ORDINARY text -- never inherited,
        // so it names nothing valid in the child's own handle table.
        "--probe-handle", std::to_string(reinterpret_cast<std::uintptr_t>(sentinel)),
        "--exit", "0",
    };

    const ProcessResult result = runner.Run(spec, "[fixture]");
    ::CloseHandle(sentinel);

    REQUIRE(result.has_value());
    CHECK(*result == 0);   // the merged stdout/stderr pipe -- the ONE handle
                            // that IS in the list -- kept working throughout

    bool sawInvalid = false;
    for (const std::string& m : output.messages)
        if (m == "[fixture]:HANDLE:invalid")
            sawInvalid = true;
    CHECK(sawInvalid);
}
#endif

// ---- POSIX process execution (Task 6, multibackend hardening) --------------
//
// DescribeChildSetupError is the one PORTABLE piece of the POSIX runner: a
// pure {stage, errno} -> ProcessError decode with no syscall of its own, so it
// is compiled and directly tested on EVERY platform, including this Windows
// desk (ENOENT/ENOTDIR are standard <cerrno> macros, not POSIX-only ones).
// That matters because it owns the invariant the error pipe exists for -- a
// child that never managed to exec is a LAUNCH FAILURE, never the child exit
// code 127 the failed child happens to _exit with.

TEST_CASE("arcbuild::DescribeChildSetupError makes a child setup failure a launch error, never child exit 127",
          "[build]")
{
    // ENOENT at Exec -- the missing-executable case -- is an error side, and
    // nothing in it exposes the 127 the child _exit()s with: a real tool that
    // genuinely ran and exited 127 must stay distinguishable from one that
    // never started at all.
    const ProcessError missingExecutable =
        DescribeChildSetupError(ChildSetupError{ ChildSetupStage::Exec, ENOENT });

    const ProcessResult asResult = ProcessResult(std::unexpected(missingExecutable));
    REQUIRE_FALSE(asResult.has_value());
    CHECK(asResult.error().message.find("127") == std::string::npos);

    // The message names the stage and carries the real errno through, so a
    // diagnostic distinguishes "no such file" from "permission denied".
    CHECK(missingExecutable.message.find("exec") != std::string::npos);
    CHECK(missingExecutable.message.find(std::to_string(ENOENT)) != std::string::npos);

    const ProcessError deniedExecutable =
        DescribeChildSetupError(ChildSetupError{ ChildSetupStage::Exec, EACCES });
    CHECK(deniedExecutable.message != missingExecutable.message);

    // Every stage reads differently: a chdir failure and a dup2 failure must
    // never produce the same diagnostic, or the error pipe's whole point
    // (saying WHICH child-side step failed) is lost.
    const ProcessError chdir =
        DescribeChildSetupError(ChildSetupError{ ChildSetupStage::Chdir, ENOENT });
    const ProcessError dupStdout =
        DescribeChildSetupError(ChildSetupError{ ChildSetupStage::DupStdout, EBADF });
    const ProcessError dupStderr =
        DescribeChildSetupError(ChildSetupError{ ChildSetupStage::DupStderr, EBADF });

    CHECK(chdir.message.find("working directory") != std::string::npos);
    CHECK(dupStdout.message.find("stdout") != std::string::npos);
    CHECK(dupStderr.message.find("stderr") != std::string::npos);
    CHECK(dupStdout.message != dupStderr.message);
    CHECK(chdir.message != missingExecutable.message);
}

TEST_CASE("arcbuild::ExecutePlan maps a POSIX child-setup failure to kExitRefused, never to 127", "[build]")
{
    ProcessSpec step;
    step.executable = fs::path("/definitely/not/a/real/tool-arcbuild-should-never-find");

    ProcessPlan plan;
    plan.steps = { step };

    RecordingOutput  output;
    FakeProcessRunner fake;
    fake.results =
    {
        ProcessResult(std::unexpected(
            DescribeChildSetupError(ChildSetupError{ ChildSetupStage::Exec, ENOENT })))
    };

    // kExitRefused is the driver's own "never launched" verdict; 127 is a
    // value a real child could legitimately return, so the two must not be
    // the same answer.
    CHECK(ExecutePlan(plan, fake, output, "[posix]") == kExitRefused);
    CHECK(kExitRefused != 127);
}

#if !defined(_WIN32)
namespace
{
    // /bin/sh is the child EXECUTABLE in the cases below -- never an
    // intermediary. ProcessRunner launches it directly, exactly the way it
    // launches make/ninja/xcodebuild, and the script it runs is one ordinary
    // argv entry. No arcbuild command is ever routed through a shell; these
    // cases only need a program that is guaranteed present on every POSIX
    // host and can be told to exit with a chosen code, signal itself, and
    // echo its own argv back (the Task 5 process fixture is Win32-only --
    // wmain + windows.h -- so it cannot serve here).
    fs::path PosixShell()
    {
        return fs::path("/bin/sh");
    }
}

TEST_CASE("arcbuild::ProcessRunner: exact argv, merged stdout+stderr, cwd, and a child's exit code round-trip through a real fork/execv launch",
          "[build]")
{
    REQUIRE(fs::exists(PosixShell()));

    TempDir cwdDir("process_runner_posix_cwd");

    RecordingOutput output;
    ProcessRunner   runner(output);

    ProcessSpec spec;
    spec.executable       = PosixShell();
    spec.workingDirectory = cwdDir.path;
    spec.arguments        =
    {
        "-c",
        // With sh -c, the argument after the script is $0 and everything
        // after that is "$@" -- printed back one line each, so the parent can
        // compare every entry byte for byte with what it passed.
        "for a in \"$@\"; do printf 'ARG:%s\\n' \"$a\"; done; "
        "printf 'CWD:%s\\n' \"$(pwd -P)\"; "
        "printf 'to-stderr\\n' >&2; "
        "exit 5",
        "arcbuild-posix-argv0",
        "",                          // an empty argv entry survives as an entry
        "has space",                 // no quoting layer exists on this side
        "has\"quote",
        "$HOME;echo second-parse|cat",  // shell metacharacters, never re-parsed
        "café",                 // non-ASCII bytes, passed through as bytes
    };

    const ProcessResult result = runner.Run(spec, "[posix]");
    REQUIRE(result.has_value());
    CHECK(*result == 5);   // WEXITSTATUS, not a fabricated code

    std::vector<std::string> childLines;
    for (const std::string& m : output.messages)
        if (m.rfind("[posix]:", 0) == 0)
            childLines.push_back(m.substr(std::string("[posix]:").size()));

    // argv arrives EXACTLY as passed: execv takes an array, so spaces,
    // quotes and `$`/`;`/`|` are ordinary bytes with no second parse to
    // survive -- "$HOME" is still the four literal characters, and
    // "echo second-parse" never ran.
    REQUIRE(childLines.size() >= 6);
    CHECK(childLines[0] == "ARG:");
    CHECK(childLines[1] == "ARG:has space");
    CHECK(childLines[2] == "ARG:has\"quote");
    CHECK(childLines[3] == "ARG:$HOME;echo second-parse|cat");
    CHECK(childLines[4] == std::string("ARG:") + "café");

    bool sawCwd         = false;
    bool sawStderrLine  = false;
    for (const std::string& line : childLines)
    {
        if (line.rfind("CWD:", 0) == 0)
        {
            sawCwd = true;
            // `pwd -P` resolves symlinks, so this compares the physical
            // directory the child's chdir landed in (macOS /var -> /private/var).
            CHECK(fs::equivalent(fs::path(line.substr(4)), cwdDir.path));
        }
        if (line == "to-stderr")
            sawStderrLine = true;
    }
    CHECK(sawCwd);
    CHECK(sawStderrLine);   // stderr is MERGED into the very same stream Child() sees
}

TEST_CASE("arcbuild::ProcessRunner: a child killed by a signal reports 128 + signal", "[build]")
{
    REQUIRE(fs::exists(PosixShell()));

    RecordingOutput output;
    ProcessRunner   runner(output);

    ProcessSpec spec;
    spec.executable = PosixShell();
    // `kill` is a shell BUILTIN and $$ is the shell's own pid, so the very
    // process this runner forked is the one that dies by SIGTERM -- which is
    // what makes waitpid report WIFSIGNALED rather than WIFEXITED.
    spec.arguments  = { "-c", "kill -TERM $$" };

    const ProcessResult result = runner.Run(spec, "[posix]");

    // A signalled child is a child that RAN -- an ordinary result value, not
    // a launch error.
    REQUIRE(result.has_value());
    CHECK(*result == 128 + SIGTERM);
    CHECK(*result == 143);   // the number every shell and CI log shows for a SIGTERM'd child
}

TEST_CASE("arcbuild::ProcessRunner: a missing executable is a launch refusal carrying ENOENT, never child exit 127",
          "[build]")
{
    RecordingOutput output;
    ProcessRunner   runner(output);

    ProcessSpec spec;
    spec.executable = fs::path("/definitely/not/a/real/tool-arcbuild-should-never-find");

    const ProcessResult result = runner.Run(spec, "[posix]");

    // execv failed in the child, which reported {Exec, ENOENT} over the
    // error pipe and _exit(127)'d. That 127 must NOT be what the caller sees.
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("exec") != std::string::npos);
    CHECK(result.error().message.find(std::to_string(ENOENT)) != std::string::npos);
    CHECK(result.error().message.find("127") == std::string::npos);

    ProcessPlan plan;
    plan.steps = { spec };
    CHECK(ExecutePlan(plan, runner, output, "[posix]") == kExitRefused);
}

TEST_CASE("arcbuild::ProcessRunner: a bare executable name is never resolved through PATH", "[build]")
{
    REQUIRE(fs::exists(PosixShell()));

    // The POSIX counterpart of the Windows lpApplicationName decoy case.
    // /bin is on PATH here and really does hold an executable named `sh`, so
    // execvp WOULD have found and launched it from this bare name. execv does
    // no PATH search at all -- the name resolves against the cwd, finds
    // nothing, and the launch is refused. That is what keeps "executable
    // names THE binary" true even if a future caller hands over a bare tool
    // name by mistake.
    EnvOverride path("PATH", "/bin:/usr/bin");

    RecordingOutput output;
    ProcessRunner   runner(output);

    ProcessSpec spec;
    spec.executable = fs::path("sh");
    spec.arguments  = { "-c", "exit 0" };

    const ProcessResult result = runner.Run(spec, "[posix]");

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("exec") != std::string::npos);
    CHECK(result.error().message.find(std::to_string(ENOENT)) != std::string::npos);
}

TEST_CASE("arcbuild::ProcessRunner: a missing working directory refuses the launch at the chdir stage", "[build]")
{
    REQUIRE(fs::exists(PosixShell()));

    RecordingOutput output;
    ProcessRunner   runner(output);

    ProcessSpec spec;
    spec.executable       = PosixShell();
    spec.arguments        = { "-c", "exit 0" };
    spec.workingDirectory = fs::path("/definitely/not/a/real/directory-arcbuild-should-never-enter");

    const ProcessResult result = runner.Run(spec, "[posix]");

    // The executable itself is perfectly fine here -- the chdir BEFORE exec
    // is what failed, and the error pipe is the only way the parent could
    // know that, since the failure happens after fork() already succeeded.
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("working directory") != std::string::npos);
    CHECK(result.error().message.find(std::to_string(ENOENT)) != std::string::npos);
    // Never mistaken for "the shell ran and exited 0".
    CHECK(result.error().message.find("exec") == std::string::npos);
}
#endif

// ---- backend resolver --------------------------------------------------------

TEST_CASE("arcbuild::BackendResolver refuses BuildBackend::None for build resolution", "[build]")
{
    const BackendResolver resolver;

    const auto builder = resolver.ResolveBuilder(BuildBackend::None);
    REQUIRE_FALSE(builder.has_value());
    CHECK_FALSE(builder.error().empty());

    const auto context = resolver.ResolveBackendContext(BuildBackend::None, AphelyonProject());
    REQUIRE_FALSE(context.has_value());
    CHECK_FALSE(context.error().empty());
}

TEST_CASE("arcbuild::BackendResolver turns an empty low-level tool lookup into a descriptive std::unexpected",
          "[build]")
{
    // A controlled, empty PATH: none of Premake/Make/Ninja are really
    // findable there, so each lookup below exercises the "concrete
    // discovery came back empty" branch deterministically, regardless of
    // what happens to be installed on this desk.
    TempDir emptyPath("backend_resolver_missing_tools");
    EnvOverride path("PATH", emptyPath.path.string());
    EnvOverride pathExt("PATHEXT", ".COM;.EXE;.BAT;.CMD");

    const BackendResolver resolver;

    SECTION("Premake: no bundled copy in the SDK root, nothing on PATH")
    {
        TempDir sdkRoot("backend_resolver_premake_sdk");
        const auto result = resolver.ResolvePremake(sdkRoot.path);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().find("Premake") != std::string::npos);
    }

    SECTION("Make: nothing on PATH")
    {
        const auto result = resolver.ResolveBuilder(BuildBackend::Make);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().find("Make") != std::string::npos);
    }

    SECTION("Ninja: nothing on PATH")
    {
        const auto result = resolver.ResolveBuilder(BuildBackend::Ninja);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().find("Ninja") != std::string::npos);
    }

#ifdef __APPLE__
    // On a Mac the toolchain is never a PATH question: ResolveXcodeBuild
    // takes /usr/bin/xcodebuild (the xcode-select shim every macOS has)
    // first, so even the empty PATH above resolves it.
    SECTION("XcodeBuild: the fixed /usr/bin shim, whatever PATH says")
    {
        const auto result = resolver.ResolveBuilder(BuildBackend::XcodeBuild);
        REQUIRE(result.has_value());
        CHECK(result->filename() == "xcodebuild");
    }
#else
    SECTION("XcodeBuild: no macOS toolchain to find")
    {
        const auto result = resolver.ResolveBuilder(BuildBackend::XcodeBuild);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().find("xcodebuild") != std::string::npos);
    }
#endif
}

TEST_CASE("arcbuild::BackendResolver reports a missing generated build context descriptively", "[build]")
{
    TempArcProject fixture("backend_resolver_context");
    ProjectLayout project;
    project.root = fixture.root;
    project.name = "Fixture";

    const BackendResolver resolver;

    SECTION("MsBuild: no generated solution file")
    {
        const auto result = resolver.ResolveBackendContext(BuildBackend::MsBuild, project);
        REQUIRE_FALSE(result.has_value());
        CHECK_FALSE(result.error().empty());
    }

    SECTION("Make: no Makefile")
    {
        const auto result = resolver.ResolveBackendContext(BuildBackend::Make, project);
        REQUIRE_FALSE(result.has_value());
        CHECK_FALSE(result.error().empty());
    }

    SECTION("Ninja: no build.ninja")
    {
        const auto result = resolver.ResolveBackendContext(BuildBackend::Ninja, project);
        REQUIRE_FALSE(result.has_value());
        CHECK_FALSE(result.error().empty());
    }

    SECTION("XcodeBuild: no .xcodeproj directory")
    {
        const auto result = resolver.ResolveBackendContext(BuildBackend::XcodeBuild, project);
        REQUIRE_FALSE(result.has_value());
        CHECK_FALSE(result.error().empty());
    }
}

TEST_CASE("arcbuild::BackendResolver requires the module-stem .ninja file alongside build.ninja, and stores the stem in BackendContext::target",
          "[build]")
{
    TempArcProject fixture("backend_resolver_ninja_module_stem");
    ProjectLayout project;
    project.root = fixture.root;
    project.name = "Fixture";

    const BackendResolver resolver;

    SECTION("neither build.ninja nor the module .ninja exists")
    {
        const auto result = resolver.ResolveBackendContext(BuildBackend::Ninja, project);
        REQUIRE_FALSE(result.has_value());
        CHECK_FALSE(result.error().empty());
    }

    SECTION("build.ninja exists but Fixture.ninja does not -- still a descriptive refusal")
    {
        std::ofstream(project.root / "build.ninja", std::ios::binary) << "";
        const auto result = resolver.ResolveBackendContext(BuildBackend::Ninja, project);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().find("Fixture.ninja") != std::string::npos);
    }

    SECTION("both files exist -- resolves with the module stem riding in target")
    {
        std::ofstream(project.root / "build.ninja", std::ios::binary) << "";
        std::ofstream(project.root / "Fixture.ninja", std::ios::binary) << "";
        const auto result = resolver.ResolveBackendContext(BuildBackend::Ninja, project);
        REQUIRE(result.has_value());
        CHECK(result->path == project.root);
        REQUIRE(result->target.has_value());
        CHECK(*result->target == "Fixture");
    }
}

TEST_CASE("arcbuild::BackendResolver stores the module stem in BackendContext::target for Xcode (composed as -target, never -scheme)",
          "[build]")
{
    TempArcProject fixture("backend_resolver_xcode_module_stem");
    ProjectLayout project;
    project.root = fixture.root;
    project.name = "Fixture";

    std::error_code ec;
    fs::create_directories(project.root / "Fixture.xcodeproj", ec);

    const BackendResolver resolver;
    const auto result = resolver.ResolveBackendContext(BuildBackend::XcodeBuild, project);
    REQUIRE(result.has_value());
    CHECK(result->path == project.root / "Fixture.xcodeproj");
    REQUIRE(result->target.has_value());
    CHECK(*result->target == "Fixture");
}

// ---------------------------------------------------------------------------
// The opt-in desk probe. Off by default: SKIPs unless ARCANE_BUILD_DESK names
// an ABSOLUTE game-project directory (one with an .arcproj -- e.g.
// D:\dev\starworks\Gacha\Game), so the ordinary suite never spawns the
// driver. Runs the BUILT arcbuild.exe from the dev bin layout
// (../arcbuild/ beside this exe -- the editor's own ResolveDriver rule) via
// the editor's synchronous RunCapture. `probe` must answer with one row and
// exit 0 or 3; `generate` must exit 0 and leave a workspace file behind.
// Both are idempotent against a real project (premake rewrites only files
// whose content changed).
// ---------------------------------------------------------------------------

#include <cstdlib>
// ModuleBuild.hpp (RunCapture + ExeDir) is included near the top of this
// file now -- the [build] process-fixture cases need ExeDir() too.
#include <Arcane/Build/Toolchain.hpp>     // DiscoverSolution (the post-generate check)

namespace
{
    fs::path DeskDriverExe()
    {
        return (Arcane::Editor::ModuleBuild::ExeDir() / ".." / "arcbuild" / "arcbuild.exe").lexically_normal();
    }

    std::string DeskLine(const char* command, const fs::path& project)
    {
        std::string cmd = "( \"";
        cmd += DeskDriverExe().string();
        cmd += "\" ";
        cmd += command;
        cmd += " --project \"";
        cmd += project.string();
        cmd += "\" --config Debug ) 2>&1";
        return cmd;
    }
}

TEST_CASE("arcbuild probe answers one s4.3 row against a real project on this desk", "[build-desk]")
{
    const char* env = std::getenv("ARCANE_BUILD_DESK");
    if (!env || !*env)
        SKIP("ARCANE_BUILD_DESK not set -- desk-only probe");
    REQUIRE(fs::is_regular_file(DeskDriverExe()));

    const Arcane::Editor::ModuleBuild::CaptureResult r =
        Arcane::Editor::ModuleBuild::RunCapture(DeskLine("probe", fs::path(env)));
    for (const std::string& line : r.lines)
        INFO(line);
    REQUIRE(r.exit.has_value());
    CHECK((*r.exit == kExitOk || *r.exit == kExitProbeRebuild));

    bool sawRow = false;
    for (const std::string& line : r.lines)
        if (line.rfind("[arcbuild] probe:", 0) == 0 && line.find(" state=") != std::string::npos)
            sawRow = true;
    CHECK(sawRow);
}

TEST_CASE("arcbuild generate writes the project's workspace file on this desk", "[build-desk]")
{
    const char* env = std::getenv("ARCANE_BUILD_DESK");
    if (!env || !*env)
        SKIP("ARCANE_BUILD_DESK not set -- desk-only probe");
    REQUIRE(fs::is_regular_file(DeskDriverExe()));

    const Arcane::Editor::ModuleBuild::CaptureResult r =
        Arcane::Editor::ModuleBuild::RunCapture(DeskLine("generate", fs::path(env)));
    for (const std::string& line : r.lines)
        INFO(line);
    REQUIRE(r.exit.has_value());
    CHECK(*r.exit == kExitOk);

    bool sawPremake = false;
    for (const std::string& line : r.lines)
        if (line.rfind("[premake]", 0) == 0)
            sawPremake = true;
    CHECK(sawPremake);
    CHECK_FALSE(Arcane::Toolchain::DiscoverSolution(fs::path(env)).empty());
}

// ---------------------------------------------------------------------------
// Opt-in generator characterization (Task 7, multibackend hardening). Off by
// default: it runs only under an explicit "[build-generator]" tag filter,
// never under the ordinary "[build]" suite, because it spawns the REAL
// bundled Premake three times to characterize actual gmake/ninja/xcode4
// generator output -- heavier and slower than the rest of [build], which
// only ever exercises composition/decision policy or the process-fixture
// exe. Unlike [build-desk] it needs no environment variable: the SDK it
// generates against is always THIS repo (SdkRootFromExeDir walks up from
// ArcaneTests.exe's own directory, the same self-hosting trick the [build]
// process-fixture cases already rely on for ExeDir()), so it is safe to run
// on any desk or CI agent that built ArcaneTests.exe at all.
//
// Each case copies the committed, input-only ArcaneTests/data/arcbuild-
// fixture/ into its OWN unique temp directory (never generates in place,
// never touches the committed fixture) and runs the resolved bundled
// premake5 through the real ProcessRunner -- no BuildExecutor/Pipeline
// involved, since this test is about characterizing Premake's OWN output,
// not arcbuild's orchestration of it (that is the rest of [build], plus the
// live acceptance in scripts/verify-arcbuild-backends.ps1).
// ---------------------------------------------------------------------------

#include <chrono>
#include <sstream>

namespace
{
    // A directory that did not exist before this call and is guaranteed
    // distinct from every other call in this process -- a monotonic step
    // counter folded in alongside a steady_clock tick so two cases started
    // in the same tick still land on different names. create_directory's
    // own "did I just create this" return is the final word: a collision
    // (however unlikely) is retried rather than silently reused.
    std::filesystem::path MakeUniqueTempDir(std::string_view tag)
    {
        static int counter = 0;

        const fs::path base =
            fs::temp_directory_path() / "arcbuild_generator_test";

        std::error_code ec;
        fs::create_directories(base, ec);

        for (int attempt = 0; attempt < 1000; ++attempt)
        {
            std::ostringstream name;
            name << tag << "_"
                 << std::chrono::steady_clock::now()
                        .time_since_epoch()
                        .count()
                 << "_" << (++counter) << "_" << attempt;

            fs::path candidate = base / name.str();

            std::error_code createEc;
            if (fs::create_directory(candidate, createEc) && !createEc)
                return candidate;
        }

        FAIL("could not allocate a unique temp directory for '" << tag << "'");
        return {};
    }

    // RAII around MakeUniqueTempDir: deletes ONLY the directory it created,
    // and only after re-resolving both it and the system temp root through
    // weakly_canonical and confirming the directory is still a strict
    // descendant of that root -- the same "never remove anything outside a
    // validated unique temp subtree" guard scripts/verify-arcbuild-
    // backends.ps1 applies on the PowerShell side. A resolution failure (the
    // directory already gone, a permission error) leaves cleanup to the OS's
    // own temp-directory hygiene rather than guessing.
    struct UniqueTempDir
    {
        fs::path path;

        explicit UniqueTempDir(std::string_view tag)
            : path(MakeUniqueTempDir(tag))
        {
        }

        ~UniqueTempDir()
        {
            std::error_code ec;

            const fs::path root =
                fs::weakly_canonical(fs::temp_directory_path(), ec);
            if (ec) return;

            const fs::path resolved =
                fs::weakly_canonical(path, ec);
            if (ec) return;

            const std::string rootStr     = root.generic_string();
            const std::string resolvedStr = resolved.generic_string();

            const bool isStrictDescendant =
                resolvedStr.size() > rootStr.size() &&
                resolvedStr.compare(0, rootStr.size(), rootStr) == 0 &&
                resolvedStr[rootStr.size()] == '/';

            if (isStrictDescendant)
                fs::remove_all(resolved, ec);
        }

        UniqueTempDir(const UniqueTempDir&)            = delete;
        UniqueTempDir& operator=(const UniqueTempDir&) = delete;
    };
}

TEST_CASE("arcbuild's bundled Premake characterizes real gmake/ninja/xcode4 output from the fixture",
          "[build-generator]")
{
    // Self-hosting SDK root: this repo IS an Arcane checkout, so the same
    // <sdk>/bin/<cfg>-<system>-<arch>-md/<Project> inversion the editor uses
    // to find "the SDK the running exe belongs to" gives the fixture's
    // premake5.lua a real $ARCANE_SDK/build/arcane.lua to include.
    const fs::path repoRoot =
        Arcane::Editor::ModuleBuild::SdkRootFromExeDir(
            Arcane::Editor::ModuleBuild::ExeDir());

    // ... and that arcane.lua must be THIS checkout's. The fixture's
    // premake5.lua includes os.getenv("ARCANE_SDK") .. "/build/arcane.lua"
    // (the external-project contract), and the spawned premake inherits
    // this process's environment -- so without this override a desk whose
    // ARCANE_SDK names some OTHER engine checkout (the main one, while this
    // runs from a worktree) would characterize that checkout's arcane.lua,
    // not the one under test, and the link-location pin below would be
    // pinning a stranger. Scoped to this case; restored on exit.
    const EnvOverride sdkForFixture("ARCANE_SDK", repoRoot.string());

    const fs::path fixtureSource =
        repoRoot / "ArcaneTests" / "data" / "arcbuild-fixture";
    REQUIRE(fs::is_directory(fixtureSource));

    const BackendResolver resolver;
    const auto premake = resolver.ResolvePremake(repoRoot);
    REQUIRE(premake.has_value());

    RecordingOutput output;
    ProcessRunner   runner(output);

    struct GeneratorCase
    {
        const char*              label;
        std::vector<std::string> arguments;
        std::vector<fs::path>    expectedArtifacts;
    };

    // clang-format off
    const std::vector<GeneratorCase> cases =
    {
        // gmake: BackendResolver::ResolveBackendContext(Make, ...) requires
        // exactly these two files (Backend.cpp).
        { "gmake", { "gmake" },
          { "Makefile", "Fixture.make" } },

        // ninja: BackendResolver::ResolveBackendContext(Ninja, ...) requires
        // BOTH build.ninja and <name>.ninja (the module stem's own rules,
        // read back as BackendContext::target -- Task 4).
        { "ninja", { "ninja" },
          { "build.ninja", "Fixture.ninja" } },

        // xcode4: cross-generated from Windows via --os=macosx, exactly as
        // Task 4's RED-reproduction script did. beta8 emits a workspace and
        // project but -- confirmed by Task 4 -- NO shared scheme file, which
        // is why ComposeXcodeBuild drives `-target`, never `-scheme`.
        { "xcode4", { "--os=macosx", "xcode4" },
          { "Fixture.xcworkspace/contents.xcworkspacedata",
            "Fixture.xcodeproj/project.pbxproj" } },
    };
    // clang-format on

    for (const GeneratorCase& generatorCase : cases)
    {
        INFO("generator action: " << generatorCase.label);

        UniqueTempDir tempDir(
            std::string("generator_") + generatorCase.label);

        std::error_code copyEc;
        fs::copy(
            fixtureSource,
            tempDir.path,
            fs::copy_options::recursive,
            copyEc);
        REQUIRE_FALSE(copyEc);

        ProcessSpec spec;
        spec.executable       = *premake;
        spec.arguments        = generatorCase.arguments;
        spec.workingDirectory = tempDir.path;

        const ProcessResult result =
            runner.Run(spec, "[premake]");

        REQUIRE(result.has_value());
        CHECK(*result == 0);

        for (const fs::path& artifact : generatorCase.expectedArtifacts)
            CHECK(fs::exists(tempDir.path / artifact));

        // The committed fixture is read-only input: generation only ever
        // wrote into tempDir.path, never fixtureSource.
        CHECK_FALSE(fs::exists(fixtureSource / "Makefile"));
        CHECK_FALSE(fs::exists(fixtureSource / "build.ninja"));
        CHECK_FALSE(fs::exists(fixtureSource / "Fixture.xcodeproj"));

        // Ninja only: build/arcane.lua's `action:ninja` filter links each
        // configuration to a UNIQUE location (beta8 would otherwise emit
        // three identical `build Binaries/Fixture.dll` edges -- Task 4).
        // That location is ALSO what arcbuild's own staging copy reads
        // (NinjaLinkOutput -> StageBuiltModule, review F4), and it must sit
        // INSIDE one of arcbuild's two filesystem clean targets (Binaries/,
        // Intermediate/<config>/ -- CleanTargets), or a `clean` whose backend
        // step soft-skipped/failed would leave the linked DLL behind. Pinned
        // against NinjaLinkOutput and CleanTargets themselves, never a
        // hand-copied string, so arcane.lua and the driver cannot drift
        // apart silently.
        if (std::string_view(generatorCase.label) == "ninja")
        {
            std::ifstream ninjaFile(tempDir.path / "Fixture.ninja", std::ios::binary);
            REQUIRE(ninjaFile.is_open());

            std::vector<std::string> linkEdges;
            for (std::string line; std::getline(ninjaFile, line);)
            {
                if (line.rfind("build ", 0) == 0 &&
                    line.find(Arcane::Platform::ModuleFileName("Fixture")) != std::string::npos &&
                    line.find(": link_") != std::string::npos)
                {
                    // "build <output> | <implicit outputs>: link_msc ..."
                    const std::size_t start = std::string("build ").size();
                    const std::size_t end   = line.find_first_of(" |:", start);
                    linkEdges.push_back(line.substr(start, end - start));
                }
            }

            ProjectLayout layout;
            layout.root       = tempDir.path;
            layout.name       = "Fixture";
            layout.gameModule = "Fixture.dll";

            const std::vector<std::string> configs = { "Debug", "Release", "Dist" };
            REQUIRE(linkEdges.size() == configs.size());

            for (const std::string& config : configs)
            {
                INFO("configuration: " << config);

                // The generated .ninja spells its paths relative to the
                // project root; so are these two, for the comparison.
                const std::string expectedEdge =
                    fs::relative(NinjaLinkOutput(layout, config), layout.root).generic_string();

                const std::vector<fs::path> cleanTargets = CleanTargets(layout, config);
                REQUIRE(cleanTargets.size() == 2);
                const std::string intermediateForConfig =
                    fs::relative(cleanTargets[1], layout.root).generic_string() + "/";

                bool foundExactEdge = false;
                for (const std::string& edge : linkEdges)
                {
                    if (edge == expectedEdge)
                        foundExactEdge = true;
                }
                CHECK(foundExactEdge);
                CHECK(expectedEdge.rfind(intermediateForConfig, 0) == 0);
            }

            // Three DISTINCT link outputs (the Task 4 workaround's whole
            // point), and none at the retired Intermediate/Ninja/<Config>/
            // spelling that sat outside the clean contract.
            for (std::size_t i = 0; i < linkEdges.size(); ++i)
            {
                CHECK(linkEdges[i].find("Intermediate/Ninja/") == std::string::npos);
                for (std::size_t j = i + 1; j < linkEdges.size(); ++j)
                    CHECK(linkEdges[i] != linkEdges[j]);
            }
        }
    }
}
