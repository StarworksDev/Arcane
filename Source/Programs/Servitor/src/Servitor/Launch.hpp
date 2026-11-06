#pragma once
// Servitor/Launch.hpp (rev 8.3 V3/V7; Servitor P0 Task 16): the client half of
// `servitor launch`. Spawns a host binary from the engine root HostsDir names,
// detached with no shell (ruling E-2), and waits for that exact process's
// descriptor to read "ready". Program code: no export macro (V4); includes no
// ArcaneRemote host header (V3).
#include <Servitor/Client.hpp>
#include <Servitor/Verbs.hpp>
#include <Arcane/Core/Constant.hpp>
#include <Arcane/Remote/Protocol.hpp>
#include <Json.hpp>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#if !defined(ARC_BUILD_DIST)

namespace Servitor {

ARC_CONSTANT("timing: how often launch re-reads the new host's descriptor and liveness while it boots")
inline constexpr int kLaunchPollMs = 50;

// <HostsDir>/<Host>/<Host>[.exe]; nullopt fills *error with engine.unresolved (exit 4) and the
// path it tried. Launch calls this first and spawns nothing when it fails.
std::optional<std::filesystem::path> HostExePath(Arcane::Remote::HostKind kind, const GlobalOptions& opt,
                                                 Arcane::Remote::Envelope* error);

// Everything Launch decides before it spawns, so it can be checked without a child.
struct LaunchPlan {
    std::filesystem::path exe;            // HostExePath, gated by Arcane::CheckLaunchPath
    std::filesystem::path logDir;         // <descriptor dir>/logs (the project's Saved/Remote/logs; spec s4.7)
    std::filesystem::path log;            // <logDir>/<Host>.servitor-launch-starting-<servitor pid>.log; Launch
                                          // renames it <Host>.servitor-launch-<child pid>.log after the spawn
    std::filesystem::path projectRoot;    // the host's project (ProjectRootFor); empty = projectless
    std::filesystem::path descriptorDir;  // where the host writes <kind>-<pid>.json (its project's Saved/Remote)
    std::filesystem::path descriptor;     // --remote-descriptor among the host flags (absolute); empty = the default
    std::vector<std::string> argv;        // the child's arguments after the exe
    bool hideConsole = false;             // a headless host or the server: CREATE_NO_WINDOW on Windows
};

// The plan for `hostArgs` (passed to the host verbatim, except that the values
// of --project and --remote-descriptor are made absolute: the host runs in its
// own folder). Adds `--project <root>` in front when the host flags carry no
// --project and `opt` names a project (ProjectRootFor), and `--remote` at the
// end unless the flags already carry it. --project both in `opt` and in the
// host flags is arg.invalid (exit 2). nullopt fills *error.
std::optional<LaunchPlan> PlanLaunch(Arcane::Remote::HostKind kind, const GlobalOptions& opt,
                                     std::span<const std::string> hostArgs, Arcane::Remote::Envelope* error);

// Spawns detached (ruling E-2) with --remote (+ --remote-descriptor when given), then polls the
// descriptor until it is not IsStale and its state is ready (Arcane::Remote::IsReadyState: ready,
// playing or paused), or opt.timeoutMs (LaunchTimedOut: launch.timeout, exit 8). Returns the exit code and fills *out.
//  - ok: MakeOk("launch", {pid, port, descriptor, hostKind, log, exe}) with the host block (exit 0);
//  - the child exits before it is ready: host.died (exit 7), detail {pid, log, descriptor,
//    lastState[, exitCode][, arcdiag]} (spec s4.7: the newest .arcdiag written after the spawn);
//  - the spawn itself fails: internal (exit 1) with the OS error.
// The child keeps running on a timeout: detail.pid names it, and the remediation says how to stop it.
int Launch(Arcane::Remote::HostKind kind, const GlobalOptions& opt, std::span<const std::string> hostArgs,
           Arcane::Remote::Envelope* out);

// The answer when the spawned host (pid) did not read ready within
// timeoutMs: launch.timeout, exit 8 (a check ran and did not pass; never the
// "provably not run, retry is safe" exit 5, because a process IS running).
// detail {pid, log, descriptor, state}; the remediation names how to wait for
// it (servitor wait ready --pid N) and how to stop it (servitor quit --pid N).
Arcane::Remote::Envelope LaunchTimedOut(Arcane::Remote::HostKind kind, std::uint32_t pid, std::uint32_t timeoutMs,
                                        const std::filesystem::path& log, const std::filesystem::path& descriptor,
                                        const std::string& lastState);

// The launch row's flags (Verbs.cpp): --headless, --backend <b>, --remote-descriptor <path>.
std::span<const VerbFlag> LaunchFlags();
// `launch editor|runtime|server [flags] [-- host flags]` -> {kind, hostArgs}: the
// launch flags in order, then everything after "--". False is a usage error.
bool LaunchArgs(std::span<const std::string> rest, nlohmann::json& out, std::string* error);
// The launch row's client-side verb: LaunchArgs, Launch, print the envelope.
int LaunchVerb(std::span<const std::string> rest, const GlobalOptions& opt);

} // namespace Servitor

#endif
