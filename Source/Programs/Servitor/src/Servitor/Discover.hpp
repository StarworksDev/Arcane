#pragma once
// Host discovery and selection (spec s4.4, s7.1; plan rev 8.3 V3/V7): scan a
// project's descriptor directory, pick one live host, report them all for
// `servitor status`, and find the engine's host binaries. Program code: no
// export macro (V4); protocol types stay qualified (no using in a header).
// Includes no ArcaneRemote host header (V3).
#include <Servitor/Client.hpp>
#include <Arcane/Remote/Descriptor.hpp>
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

struct LiveHost {
    Arcane::Remote::Descriptor descriptor;
    std::filesystem::path path;
    bool hung = false;
};

// A descriptor file that could not be read. "access-denied" usually means an
// elevated host wrote it (owner-only DACL); "malformed" is anything else.
struct UnreadableDescriptor {
    std::filesystem::path path;
    std::string reason;
};

struct Scan {
    std::vector<LiveHost> live;
    std::vector<std::filesystem::path> removedStale;
    std::vector<std::filesystem::path> keptStale;   // stale, but the delete failed
    std::vector<UnreadableDescriptor> unreadable;   // left in place, never deleted
    std::int64_t nowUnixMs = 0;                     // the one clock IsHung and heartbeatAgeMs use
};

// The same process: {pid, processStart}, never the pid alone (a recycled pid
// is another process).
bool SameHostProcess(const Arcane::Remote::Descriptor& a, const Arcane::Remote::Descriptor& b);

// Whether two descriptor `project` spellings name one project root (round 3,
// N-2): normalized (weakly_canonical, lexically normal, no trailing separator;
// case-insensitive on Windows), never raw text. Two empty roots (the
// projectless editor) are the same; empty and non-empty are not.
bool SameProjectRoot(const std::string& a, const std::string& b);

// Reads <projectRoot>/Saved/Remote/*.json (ScanDirectory over DescriptorDir).
Scan ScanProject(const std::filesystem::path& projectRoot);
// Reads every *.json directly under `remoteDir`. A stale descriptor (IsStale)
// is deleted and listed in removedStale; a hung one (IsHung) stays, hung = true.
// A missing directory is an empty scan.
Scan ScanDirectory(const std::filesystem::path& remoteDir);
// The descriptors under `dir` whose `client` equals `client`, in file-name
// order. An empty `client` matches nothing (a descriptor with no client is
// never a match). Reads only: it does not delete stale files, so a host that
// already exited is still returned (callers drop it with IsStale).
std::vector<Arcane::Remote::Descriptor> DescriptorsForClient(const std::filesystem::path& dir, const std::string& client);
// The same selection with each descriptor's file path (a LiveHost; `hung` is
// set from the scan clock, `live` is not implied).
std::vector<LiveHost> HostsForClient(const std::filesystem::path& dir, const std::string& client);

// The project root `opt` names: --project as an .arcproj file's folder, or as
// a directory resolved up to its nearest .arcproj; with no --project, the
// nearest ancestor of the working directory holding an .arcproj. An engaged
// EMPTY path means projectless. nullopt fills *error (when non-null) with
// arg.invalid (exit 2) for a --project that is neither (RemoteDirFor's rule).
std::optional<std::filesystem::path> ProjectRootFor(const GlobalOptions& opt, Arcane::Remote::Envelope* error);
// The descriptor directory `opt` names (spec s4.4): --project as an .arcproj
// file's folder, or as a directory resolved up to its nearest .arcproj; with
// no --project, the nearest ancestor of the working directory holding an
// .arcproj, else the projectless editor's <EditorUserDir>/Remote. nullopt
// fills *error (when non-null) with arg.invalid (exit 2) for a --project that
// is neither an .arcproj nor inside a project.
std::optional<std::filesystem::path> RemoteDirFor(const GlobalOptions& opt, Arcane::Remote::Envelope* error);

// Zero live hosts (after --host / --pid) -> target.none (4); two or more ->
// target.ambiguous (2) with candidates "kind:pid"; a chosen descriptor whose
// protocol major this client does not speak -> protocol.mismatch (4). A hung
// host is still selected. nullopt fills *error when non-null.
std::optional<LiveHost> Select(const Scan& scan, const GlobalOptions& opt, Arcane::Remote::Envelope* error);
// {hosts:[{hostKind, pid, port, state, frame, abi, project, heartbeatAgeMs, hung}],
//  stale:[{descriptor, reason, removed}], unreadable:[{descriptor, reason, remediation}]}.
// Never a token. hosts lists only the live hosts `opt` selects by --host and
// --pid (servitor status honours both as a filter, spec s4.6 Rule 3); stale
// and unreadable are the whole scan's.
nlohmann::json StatusData(const Scan& scan, const GlobalOptions& opt = {});

// The directory that holds ArcaneEditor/, ArcaneRuntime/ and ArcaneServer/. First match wins:
// opt.engine, then SERVITOR_ENGINE, then the source sibling of servitor's own folder,
// then the package root three levels up. nullopt fills *error (when non-null) with
// engine.unresolved, exit 4, remediation "pass --engine <root> or set SERVITOR_ENGINE".
std::optional<std::filesystem::path> HostsDir(const GlobalOptions& opt, Arcane::Remote::Envelope* error);
// The same mapping for one explicit root (rules 1 and 2): <root>/sdk/sdk.json present ->
// <root>/Binaries/<engineConfig>-<os>-<arch>-md (engineConfig from that file; the
// -<os>-<arch>-md tail is SERVITOR_OUTPUTDIR's); absent -> <root>/Binaries/SERVITOR_OUTPUTDIR.
// A present sdk.json without a string engineConfig is nullopt.
std::optional<std::filesystem::path> HostsDirForRoot(const std::filesystem::path& root);
// <HostsDir>/<ArcaneEditor|ArcaneRuntime|ArcaneServer>/diagnostics, the host's
// exe-relative report directory; empty when HostsDir is unresolved.
std::filesystem::path HostDiagnosticsDir(Arcane::Remote::HostKind kind, const GlobalOptions& opt = {});

// `servitor status` (a client-side verb row, ruling F17): prints
// MakeOk("status", StatusData(scan), {}) and returns 0. It does not connect
// and does not call ObserveTarget.
int StatusVerb(std::span<const std::string> rest, const GlobalOptions& opt);

} // namespace Servitor

#endif
