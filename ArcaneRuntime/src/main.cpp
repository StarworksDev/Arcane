// ArcaneRuntime -- the thin entry point. Parses argv into a HostConfig, constructs
// the RuntimeApp application object, and returns its Run() exit code. All engine
// boot, the frame loop, and the load-bearing teardown order now live in the
// RuntimeApp class (RuntimeApp.hpp/.cpp); main is just the wire-up.

#include <Arcane/Base/Assert.hpp>
#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Base/DiagnosticsSettings.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Platform/Platform.hpp>
#include <Arcane/Config/CVarConfig.hpp>
#include <Arcane/Host/BootSplashWindow.hpp>
#include <Arcane/Host/EarlyConfig.hpp>
#include <Arcane/Host/HostConfig.hpp>
#include <Arcane/Render/AgilitySdk.hpp>
#include "RuntimeApp.hpp"
#include <Arcane/Host/ProjectBoot.hpp>   // HostBoot::EngineInfoJson (the --print-engine-info probe)

#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// Agility SDK handshake: the exported version/path pair (Arcane/Render/AgilitySdk.hpp).
// The macro is empty off Windows.
ARC_AGILITY_SDK_EXPORTS();

namespace
{
    // The reporter's window title (Diagnostics::Config::productName, spec
    // S5.1: "the runtime the project's name"). Derived from --project, which
    // is all this host knows before the project is even opened -- a
    // ".../MyGame/MyGame.arcproj" and a ".../MyGame" folder both read
    // "MyGame". Falls back to the app name for a project-less run (which
    // ArcaneRuntime refuses at plugin_load anyway, but productName is set
    // before that refusal and must not be empty).
    std::string ProductNameFor(const std::string& projectPath)
    {
        if (projectPath.empty()) return "Arcane Runtime";
        std::filesystem::path p = std::filesystem::path(projectPath).lexically_normal();
        // A trailing separator makes filename() empty -- step up once so a
        // "D:/games/MyGame/" reads the same as "D:/games/MyGame".
        if (p.filename().empty()) p = p.parent_path();
        const std::string stem = p.stem().string();
        return stem.empty() ? "Arcane Runtime" : stem;
    }
}

int main(int argc, char** argv)
{
    Arcane::Log::Init();
    Arcane::Log::InstallMosaicSink();
    Arcane::Assert::InstallMosaicHandler();
    Arcane::HostConfig::ParseOutcome parsed = Arcane::HostConfig::Parse(argc, argv);   // non-const: the early rungs fill backend/vsync
    if (!parsed.config) return parsed.exitCode;   // --help => 0, bad args => 2

    // Same probe as the editor: identity to stdout, no window, no device. The
    // flag lives in the SHARED HostConfig, so a flag that parsed on both hosts
    // but only worked on one would be a trap. A bare run (no --project, no
    // --plugin) refuses at plugin_load with usage guidance -- the runtime's one
    // job is running a game (the old Sandbox.dll default was retired 2026-08-11).
    //
    // AND BEFORE Diagnostics::Install BELOW (R25), the ONE path that exits
    // before arming -- see ArcaneEditor/src/main.cpp's copy of this note: a
    // pure query that prints one line and exits must not start the crash
    // thread and the watchdog, rotate a log file and leave an empty
    // diagnostics/ directory beside the exe.
    if (parsed.config->printEngineInfo)
    {
        // ExecutablePathUtf8, NOT argv[0]: argv[0] is whatever the launcher typed
        // (a bare relative name under the documented cd-then-run workflow) and is
        // ANSI-codepage bytes under MSVC, which a strict-UTF-8 dump() rejects.
        std::printf("%s\n", Arcane::HostBoot::EngineInfoJson(Arcane::ExecutablePathUtf8()).c_str());
        return 0;
    }

    // POST-MORTEM CAPTURE, FIRST (bar the probe above) -- crash window plan 1,
    // task 9; spec S5.1's closing paragraph. Same arming, same reasoning and
    // now the same POSITION as ArcaneEditor -- see that file's block for the
    // full account of why the "after every refusal" placement is gone: the
    // watchdog is a raw thread stopped from an atexit hook Install registers,
    // so an early `return` is clean and the boot itself is finally covered.
    // AFTER the Log::Init/Mosaic trio above, which is still load-bearing (R16).
    // I2: engine/project/EditorUser/user/--set rungs before Install and Runtime.
    Arcane::HostBoot::ApplyEarlyConfigRungs(*parsed.config, Arcane::CommandLineCVarContext(),
                                            /*editor*/ false);
    {
        // The tunables are diagnostics.* (settings arc S6-2), read after the
        // early rungs above so a project, user or --set value reaches Install.
        Arcane::Diagnostics::Config diag =
            Arcane::Diagnostics::ConfigFromSettings(Arcane::Settings<Arcane::DiagnosticsSettings>());
        diag.appName     = "ArcaneRuntime";
        diag.productName = ProductNameFor(parsed.config->projectPath);
        diag.unattended  = parsed.config->headless;   // nobody to answer a reporter window
        // Spec S5.8 (plan 2, task 9): windowed runs pre-launch the crash monitor;
        // headless runs, build machines and debugged runs launch none.
        diag.launchMonitor = !parsed.config->headless;
        // Element 0 is ExecutablePathUtf8(), NOT argv[0] -- the same reason the
        // probe above gives for not printing argv[0]: a bare relative name in
        // ANSI-codepage bytes is not something a reporter can relaunch or put
        // in a UTF-8 envelope. The sanitizer stays pure; only the host knows
        // its own exe.
        std::vector<std::string> args(argv, argv + argc);
        if (args.empty()) args.emplace_back();
        args[0] = Arcane::ExecutablePathUtf8();
        diag.commandLine = Arcane::SanitizeRelaunchLine(args);
        Arcane::Diagnostics::Install(diag);
    }
    // Every host installs one (R24): with the slot empty a first Ctrl-C is
    // declined and Windows terminates the process outright, so the two-step
    // clean exit only exists for hosts that opt in. This one stops the frame
    // loop the same way the window's close box does.
    RuntimeApp::InstallCleanExitHook();

    // --dump-layout: THIS HOST'S FIRST REFUSAL (Task 10, plan-b comparator).
    // HostConfig is shared with ArcaneEditor, which implements the flag
    // (EditorApp::Shutdown dumps the live ImGui layout to this path) --
    // ArcaneRuntime has no ImGui layout at all, so parsing it and shrugging
    // would exit 0 having silently done nothing, the exact failure Plan A's
    // Task 12 closed everywhere else in this file's sibling (see
    // ArcaneEditor/src/main.cpp's own refusal table for the established
    // idiom this one-line table borrows).
    //
    // ITS POSITION RELATIVE TO Diagnostics::Install NO LONGER MATTERS (crash
    // window plan 1, task 9) -- same reasoning ArcaneEditor/src/main.cpp's
    // refusal-table comment states in full. It USED to have to run first: an
    // early `return` taken after Install left the hang watchdog's std::thread
    // joinable at static destruction, which is std::terminate -> abort() (a
    // BLOCKING dialog under a Debug CRT, not a clean exit). The watchdog is
    // now a raw thread stopped from Install's atexit hook, so a plain
    // `return 2;` is clean wherever it sits.
    if (!parsed.config->dumpLayoutPath.empty())
    {
        std::fprintf(stderr, "error: --dump-layout is an EDITOR-only flag (there is no ImGui "
                             "layout on this host to dump). Use ArcaneEditor.exe.\n");
        return 2;
    }

    // --play-as: THIS HOST'S SECOND REFUSAL, in the same table and for the same
    // reason (Core-DLL split, plan 1 Task 7). The flag asks an editor to LEAVE
    // Edit mode at boot in a chosen topology; this host has no Edit mode to
    // leave and no PlaySession to enter -- it is always simply running the game
    // -- so parsing it and shrugging would exit 0 having silently ignored a
    // topology the caller explicitly asked for. Its `return 2;` is clean on
    // its own (see the --dump-layout block above).
    if (!parsed.config->playAs.empty())
    {
        std::fprintf(stderr, "error: --play-as is an EDITOR-only flag (this host has no Edit "
                             "mode to leave and no play session to enter; it always runs the "
                             "game). Use ArcaneEditor.exe, or ArcaneServer.exe for a dedicated "
                             "server.\n");
        return 2;
    }

    // --view-mode: the THIRD editor-only refusal, same table, same reasoning
    // (F4 plan 1 T7). The flag seeds the EDITOR's viewport camera mode; this
    // host has no editor camera -- the game's own SetView is the only view --
    // so accepting it would exit 0 having seeded nothing.
    if (!parsed.config->viewMode.empty())
    {
        std::fprintf(stderr, "error: --view-mode is an EDITOR-only flag (this host has no editor "
                             "viewport camera to seed; the game drives its own view). Use "
                             "ArcaneEditor.exe.\n");
        return 2;
    }
    if (!parsed.config->selectName.empty())
    {
        std::fprintf(stderr, "error: --select-name is an EDITOR-only flag (this host has no "
                             "editor selection). Use ArcaneEditor.exe.\n");
        return 2;
    }
    if (!parsed.config->tool.empty())
    {
        std::fprintf(stderr, "error: --tool is an EDITOR-only flag (this host has no viewport "
                             "tool). Use ArcaneEditor.exe.\n");
        return 2;
    }
    if (!parsed.config->openAsset.empty())
    {
        std::fprintf(stderr, "error: --open-asset is an EDITOR-only flag (this host has no "
                             "documents). Use ArcaneEditor.exe.\n");
        return 2;
    }
    if (!parsed.config->selectInDocument.empty())
    {
        std::fprintf(stderr, "error: --select-in-document is an EDITOR-only flag (this host has no "
                             "documents). Use ArcaneEditor.exe.\n");
        return 2;
    }
    if (!parsed.config->selectAsset.empty())
    {
        std::fprintf(stderr, "error: --select-asset is an EDITOR-only flag (this host has no "
                             "Asset Browser). Use ArcaneEditor.exe.\n");
        return 2;
    }

    // (Diagnostics::Install USED TO BE HERE, after the refusals above. It now
    // runs right after the --print-engine-info probe -- see the block at the
    // top of main() and the reciprocal note in the --dump-layout refusal.)

    // Before ANY engine boot: something on screen within ~100ms. The probe
    // return above stays free of any window on purpose. Never fails boot --
    // BootSplashWindow's whole contract is "every error path degrades to no
    // splash, silently".
    // ...UNLESS --headless, where the whole point is that this process maps
    // no window. The splash is a real WS_POPUP on its own thread, so
    // constructing it unconditionally would make "--headless" a lie for the
    // ~seconds boot takes -- the one window a --headless run would still
    // flash on screen. std::optional is what it takes: BootSplashWindow's
    // only constructor takes an image path and there is no "disabled" state,
    // and every consumer downstream is already null-tolerant by contract
    // (BootSplashPresenter's ctor comment: "`splash` may be null ... every
    // call then degrades to do nothing"; RuntimeApp takes a non-owning
    // pointer that defaults to nullptr). See ArcaneEditor/src/main.cpp for
    // the identical pattern. Destruction order is unchanged -- `app` still
    // lives in the nested scope below and is destroyed before this object.
    std::optional<Arcane::BootSplashWindow> splash;
    if (!parsed.config->headless)
        splash.emplace("data/images/arcane_logo.png");

    // Scoped so ~RuntimeApp runs while the watchdog is still armed, then joined
    // before main returns -- see ArcaneEditor/src/main.cpp for the full reason
    // (teardown is a suspect; a joinable std::thread at static destruction
    // calls std::terminate).
    int rc = 0;
    {
        RuntimeApp app(*parsed.config, splash ? &*splash : nullptr);
        rc = app.Run();
        Arcane::Diagnostics::SetPhase("runtime teardown");
        Arcane::Diagnostics::Heartbeat();
    }
    Arcane::Diagnostics::Shutdown();
    return rc;
}
