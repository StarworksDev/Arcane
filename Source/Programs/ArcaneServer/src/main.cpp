// ArcaneServer -- the Core-only dedicated-server host's entry point (Core-DLL
// split, spec docs/specs/2026-09-15-core-dll-split-design.md s6, plan 1
// Task 6). Parses argv into a ServerConfig, constructs the ServerApp
// application object, and returns its Run() exit code -- mirrors
// Source/Programs/ArcaneRuntime/src/main.cpp's own shape, minus everything presentation-only:
// no boot splash (there is no window), no --dump-layout refusal (there is no
// ImGui layout), and no D3D12 Agility SDK export (there is no D3D12 device in
// this process at all). The link line is the construction proof this host is
// Core-only: see premake5.lua's ArcaneServer block (links ArcaneCore, NOT
// ArcaneClient).

#include <Arcane/Base/Assert.hpp>
#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Base/Engine.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Plugin/PluginABI.hpp>   // Arcane::PluginABIVersion (the --print-engine-info probe)
#include "ServerApp.hpp"
#include "ServerConfig.hpp"

#include <Json.hpp>

#include <cstdio>
#include <string>

int main(int argc, char** argv)
{
    Arcane::Log::Init();
    Arcane::Log::InstallMosaicSink();
    Arcane::Assert::InstallMosaicHandler();

    const Arcane::Server::ServerConfig::ParseOutcome parsed =
        Arcane::Server::ServerConfig::Parse(argc, argv);
    if (!parsed.config) return parsed.exitCode;   // --help => 0, bad args => 2

    // Same probe every host offers: identity to stdout, no window, no device,
    // no project, no ProcessContext. `EngineInfoJson` (Source/Client/ArcaneClient/src/Arcane/
    // Host/ProjectBoot.hpp) stays Client-side deliberately -- this Core-only
    // host does not link Client for one string function, so it prints the
    // SAME three keys itself, straight from the Core-side identity probes
    // (Arcane::PluginABIVersion/BuildInfo/ExecutablePathUtf8) EngineInfoJson
    // is built from.
    //
    // AND BEFORE Diagnostics::Install BELOW (R25), the ONE path that exits
    // before arming -- see Source/Programs/ArcaneEditor/src/main.cpp's copy of this note: a
    // pure query that prints one line and exits must not start the crash
    // thread and the watchdog, rotate a log file and leave an empty
    // diagnostics/ directory beside the exe.
    if (parsed.config->printEngineInfo)
    {
        nlohmann::json j;
        j["engineAbi"] = Arcane::PluginABIVersion();
        j["build"]     = Arcane::BuildInfo();
        j["exePath"]   = Arcane::ExecutablePathUtf8();
        std::printf("%s\n", j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace).c_str());
        return 0;
    }

    // POST-MORTEM CAPTURE, FIRST (bar the probe above) -- crash window plan 1,
    // task 9; spec S5.1's closing paragraph. Same arming, same reasoning and
    // the same position as the other two hosts, which is the whole point: a
    // crash or a hang on this host must leave the same evidence behind.
    {
        Arcane::Diagnostics::Config diag;
        diag.appName     = "ArcaneServer";
        diag.productName = "Arcane Server";
        // ALWAYS unattended, unlike the other two: a dedicated server has no
        // desktop session to put a reporter window on, whatever its flags say.
        diag.unattended  = true;
        diag.commandLine = Arcane::Server::SanitizedRelaunchLine(Arcane::ExecutablePathUtf8(), argc, argv);
        Arcane::Diagnostics::Install(diag);
    }
    // Every host installs one (R24): with the slot empty a first Ctrl-C is
    // declined and Windows terminates outright. This is the host where it
    // matters most -- a dedicated server's ordinary stop IS a Ctrl-C or a
    // service shutdown, and its tick loop is otherwise open-ended.
    Arcane::Server::ServerApp::InstallCleanExitHook();

    int rc = 0;
    {
        Arcane::Server::ServerApp app(*parsed.config);
        rc = app.Run();
        Arcane::Diagnostics::SetPhase("server teardown");
        Arcane::Diagnostics::Heartbeat();
    }
    Arcane::Diagnostics::Shutdown();
    return rc;
}
