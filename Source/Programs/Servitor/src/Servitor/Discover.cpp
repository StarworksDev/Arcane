#include <Servitor/Discover.hpp>

#if !defined(ARC_BUILD_DIST)

#include <Arcane/Platform/Paths.hpp>
#include <Arcane/Platform/Process.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iterator>
#include <limits>
#include <string_view>
#include <system_error>
#include <utility>

namespace Servitor {

namespace {

namespace fs = std::filesystem;

// Why a descriptor ReadDescriptor rejected could not be read. An elevated
// host's descriptor is owner-only (AM5), so a non-elevated servitor is denied.
std::string UnreadableReason(const fs::path& path)
{
#if defined(_WIN32)
    std::FILE* f = nullptr;
    const errno_t err = ::_wfopen_s(&f, path.c_str(), L"rb");
    if (f != nullptr)
        std::fclose(f);
    return err == EACCES ? "access-denied" : "malformed";
#else
    errno = 0;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    const int err = errno;
    if (f != nullptr)
        std::fclose(f);
    return f == nullptr && err == EACCES ? "access-denied" : "malformed";
#endif
}

std::string UnreadableRemediation(const std::string& reason)
{
    if (reason == "access-denied")
        return "the host may be running elevated (its descriptor is owner-only); run servitor elevated too";
    return "the file is not a host descriptor; a host rewrites its own every second";
}

std::string Candidate(const LiveHost& host)
{
    return std::string(Arcane::Remote::ToString(host.descriptor.hostKind)) + ":" + std::to_string(host.descriptor.pid);
}

bool Fill(Arcane::Remote::Envelope* error, Arcane::Remote::Envelope env)
{
    if (error != nullptr)
        *error = std::move(env);
    return false;
}

std::optional<fs::path> Unresolved(Arcane::Remote::Envelope* error, std::string message)
{
    Fill(error, Arcane::Remote::MakeError("", "engine.unresolved", std::move(message),
                                          "pass --engine <root> or set SERVITOR_ENGINE"));
    return std::nullopt;
}

// SERVITOR_ENGINE as a path; empty when unset or empty. Wide on Windows so a
// non-ANSI root survives (the arcbuild Environment pattern).
fs::path EngineFromEnvironment()
{
#if defined(_WIN32)
    const wchar_t* value = ::_wgetenv(L"SERVITOR_ENGINE");
#else
    const char* value = std::getenv("SERVITOR_ENGINE");
#endif
    if (value == nullptr || *value == 0)
        return {};
    return fs::path(value);
}

bool HoldsHostFolders(const fs::path& dir)
{
    std::error_code ec;
    for (const char* folder : {"ArcaneEditor", "ArcaneRuntime", "ArcaneServer"}) {
        if (fs::is_directory(dir / folder, ec))
            return true;
    }
    return false;
}

std::optional<fs::path> ResolveRoot(const fs::path& root, const char* source, Arcane::Remote::Envelope* error)
{
    if (auto dir = HostsDirForRoot(root))
        return dir;
    return Unresolved(error, std::string("the engine root ") + PathUtf8(root) + " (from " + source +
                                 ") has sdk/sdk.json without a string engineConfig");
}

// The nearest directory from `start` upward that holds an .arcproj file.
std::optional<fs::path> NearestProject(fs::path start)
{
    std::error_code ec;
    for (fs::path dir = start; !dir.empty(); dir = dir.parent_path()) {
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code fileEc;
            if (it->path().extension() == ".arcproj" && it->is_regular_file(fileEc))
                return dir;
        }
        ec.clear();
        if (dir == dir.parent_path())
            break;
    }
    return std::nullopt;
}

} // namespace

Scan ScanProject(const fs::path& projectRoot)
{
    return ScanDirectory(Arcane::Remote::DescriptorDir(projectRoot, false));
}

Scan ScanDirectory(const fs::path& remoteDir)
{
    Scan scan;
    scan.nowUnixMs = NowUnixMs();
    if (remoteDir.empty())
        return scan;
    std::vector<fs::path> files;
    std::error_code ec;
    for (fs::directory_iterator it(remoteDir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code fileEc;
        if (it->path().extension() == ".json" && it->is_regular_file(fileEc))
            files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    for (const fs::path& file : files) {
        // The descriptor holds the bearer token: nothing here logs it.
        auto desc = Arcane::Remote::ReadDescriptor(file);
        if (!desc) {
            // Never deleted: an unreadable file cannot be proved stale.
            scan.unreadable.push_back({file, UnreadableReason(file)});
            continue;
        }
        if (Arcane::Remote::IsStale(*desc)) {
            std::error_code removeEc;
            fs::remove(file, removeEc);
            (removeEc ? scan.keptStale : scan.removedStale).push_back(file);
            continue;
        }
        LiveHost host;
        host.hung = Arcane::Remote::IsHung(*desc, scan.nowUnixMs);
        host.descriptor = std::move(*desc);
        host.path = file;
        scan.live.push_back(std::move(host));
    }
    return scan;
}

std::vector<LiveHost> HostsForClient(const fs::path& dir, const std::string& client)
{
    std::vector<LiveHost> hosts;
    if (dir.empty() || client.empty())
        return hosts;
    std::vector<fs::path> files;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code fileEc;
        if (it->path().extension() == ".json" && it->is_regular_file(fileEc))
            files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    const std::int64_t now = NowUnixMs();
    for (const fs::path& file : files) {
        auto desc = Arcane::Remote::ReadDescriptor(file);
        if (!desc || desc->client != client)
            continue;
        LiveHost host;
        host.hung = Arcane::Remote::IsHung(*desc, now);
        host.descriptor = std::move(*desc);
        host.path = file;
        hosts.push_back(std::move(host));
    }
    return hosts;
}

std::vector<Arcane::Remote::Descriptor> DescriptorsForClient(const fs::path& dir, const std::string& client)
{
    std::vector<Arcane::Remote::Descriptor> out;
    for (LiveHost& host : HostsForClient(dir, client))
        out.push_back(std::move(host.descriptor));
    return out;
}

std::optional<fs::path> ProjectRootFor(const GlobalOptions& opt, Arcane::Remote::Envelope* error)
{
    std::error_code ec;
    if (!opt.project.empty()) {
        // An .arcproj names its folder; a directory resolves like the working
        // directory does, up to the nearest .arcproj (spec s4.4).
        // argv is UTF-8 on every platform (main.cpp converts Windows' wide
        // command line), so the path is built from UTF-8, never the ANSI page.
        fs::path given;
        try {
            given = PathFromUtf8(opt.project);
        } catch (const std::exception&) {
            Fill(error, Arcane::Remote::MakeError("", "arg.invalid", "--project is not valid UTF-8",
                                                  "pass the project's folder or its .arcproj"));
            return std::nullopt;
        }
        const fs::path project = fs::absolute(given, ec);
        std::optional<fs::path> root;
        if (project.extension() == ".arcproj" && fs::is_regular_file(project, ec))
            root = project.parent_path();
        else if (fs::is_directory(project, ec))
            root = NearestProject(project);
        if (!root) {
            Fill(error, Arcane::Remote::MakeError("", "arg.invalid",
                                                  "--project " + opt.project +
                                                      " is neither an .arcproj file nor inside a project directory",
                                                  "pass the project's folder or its .arcproj"));
            return std::nullopt;
        }
        return root;
    }
    const fs::path cwd = fs::current_path(ec);
    if (!ec) {
        if (auto root = NearestProject(cwd))
            return root;
    }
    return fs::path{};   // projectless
}

std::optional<fs::path> RemoteDirFor(const GlobalOptions& opt, Arcane::Remote::Envelope* error)
{
    const auto root = ProjectRootFor(opt, error);
    if (!root)
        return std::nullopt;
    if (root->empty())
        return Arcane::Remote::DescriptorDir({}, true);   // a projectless editor's descriptors
    return Arcane::Remote::DescriptorDir(*root, false);
}

namespace {

// One root, normalized for comparison: weakly_canonical (resolves "." and
// "..", and on Windows the existing parts' case), lexically normal, generic
// separators, no trailing separator; lowercased on Windows.
std::string NormalizedRoot(const std::string& text)
{
    if (text.empty())
        return {};
    fs::path p;
    try {
        p = PathFromUtf8(text);
    } catch (const std::exception&) {
        return text;   // not a path this system can represent: compare as written
    }
    std::error_code ec;
    fs::path canonical = fs::weakly_canonical(p, ec);
    if (ec)
        canonical = p;
    std::string out = PathUtf8(canonical.lexically_normal());
    while (out.size() > 1 && out.back() == '/' && !(out.size() == 3 && out[1] == ':'))
        out.pop_back();
#if defined(_WIN32)
    for (char& c : out)
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
#endif
    return out;
}

} // namespace

bool SameProjectRoot(const std::string& a, const std::string& b)
{
    if (a.empty() || b.empty())
        return a.empty() && b.empty();
    return a == b || NormalizedRoot(a) == NormalizedRoot(b);
}

bool SameHostProcess(const Arcane::Remote::Descriptor& a, const Arcane::Remote::Descriptor& b)
{
    return a.pid == b.pid && a.processStart == b.processStart;
}

std::optional<LiveHost> Select(const Scan& scan, const GlobalOptions& opt, Arcane::Remote::Envelope* error)
{
    std::vector<const LiveHost*> matches;
    for (const LiveHost& host : scan.live) {
        if (!opt.hostKind.empty() && opt.hostKind != Arcane::Remote::ToString(host.descriptor.hostKind))
            continue;
        if (opt.pid != 0 && opt.pid != host.descriptor.pid)
            continue;
        matches.push_back(&host);
    }

    if (matches.empty()) {
        const bool filtered = !opt.hostKind.empty() || opt.pid != 0;
        std::string message = scan.live.empty() ? "no live host found"
                                                : "no live host matches the --host / --pid given";
        std::string remediation = scan.live.empty() ? "start a host with --remote (servitor launch <kind>)"
                                                    : "pick one of the candidates with --host and --pid";
        for (const UnreadableDescriptor& u : scan.unreadable) {
            if (u.reason == "access-denied") {
                remediation += "; " + PathUtf8(u.path) + " could not be read: " + UnreadableRemediation(u.reason);
                break;
            }
        }
        auto env = Arcane::Remote::MakeError("", "target.none", std::move(message), std::move(remediation));
        if (filtered) {
            for (const LiveHost& host : scan.live)
                env.errors.front().candidates.push_back(Candidate(host));
        }
        Fill(error, std::move(env));
        return std::nullopt;
    }

    if (matches.size() > 1) {
        auto env = Arcane::Remote::MakeError("", "target.ambiguous",
                                             std::to_string(matches.size()) + " live hosts match",
                                             "pick one with --host editor|runtime|server and --pid <n>");
        for (const LiveHost* host : matches)
            env.errors.front().candidates.push_back(Candidate(*host));
        Fill(error, std::move(env));
        return std::nullopt;
    }

    const LiveHost& chosen = *matches.front();
    if (!SpeaksProtocolMajor(chosen.descriptor.protocol)) {
        auto env = ProtocolMismatch("", chosen.descriptor.protocol);
        env.host.pid = chosen.descriptor.pid;
        env.host.kind = Arcane::Remote::ToString(chosen.descriptor.hostKind);
        Fill(error, std::move(env));
        return std::nullopt;
    }
    return chosen;
}

nlohmann::json StatusData(const Scan& scan, const GlobalOptions& opt)
{
    nlohmann::json hosts = nlohmann::json::array();
    for (const LiveHost& host : scan.live) {
        const Arcane::Remote::Descriptor& d = host.descriptor;
        // --host / --pid select hosts here as Select does (Rule 3: honoured, never dropped).
        if (!opt.hostKind.empty() && opt.hostKind != Arcane::Remote::ToString(d.hostKind))
            continue;
        if (opt.pid != 0 && opt.pid != d.pid)
            continue;
        // --client NAME lists only the hosts that client launched.
        if (opt.clientSet && d.client != opt.client)
            continue;
        // max(0, now - heartbeat) on the scan's clock, the one IsHung used;
        // unsigned so a garbled heartbeat cannot overflow.
        std::int64_t age = 0;
        if (scan.nowUnixMs > d.heartbeatUnixMs) {
            const std::uint64_t diff =
                static_cast<std::uint64_t>(scan.nowUnixMs) - static_cast<std::uint64_t>(d.heartbeatUnixMs);
            age = static_cast<std::int64_t>(std::min<std::uint64_t>(
                diff, static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())));
        }
        hosts.push_back({
            {"hostKind", Arcane::Remote::ToString(d.hostKind)},
            {"pid", d.pid},
            {"port", d.port},
            {"state", d.state},
            {"frame", d.frame},
            {"abi", d.abi},
            {"project", d.project},
            {"heartbeatAgeMs", age},
            {"hung", host.hung},
            {"client", d.client},
        });
    }
    nlohmann::json stale = nlohmann::json::array();
    for (const fs::path& p : scan.removedStale)
        stale.push_back({{"descriptor", PathUtf8(p)}, {"reason", "pid-not-running"}, {"removed", true}});
    for (const fs::path& p : scan.keptStale)
        stale.push_back({{"descriptor", PathUtf8(p)}, {"reason", "pid-not-running"}, {"removed", false}});
    nlohmann::json unreadable = nlohmann::json::array();
    for (const UnreadableDescriptor& u : scan.unreadable)
        unreadable.push_back({{"descriptor", PathUtf8(u.path)},
                              {"reason", u.reason},
                              {"remediation", UnreadableRemediation(u.reason)}});
    return {{"hosts", std::move(hosts)}, {"stale", std::move(stale)}, {"unreadable", std::move(unreadable)}};
}

std::optional<fs::path> HostsDirForRoot(const fs::path& root)
{
    const std::string outputDir = SERVITOR_OUTPUTDIR;   // <cfg>-<os>-<arch>-md, set by premake (V7)
    const fs::path sdk = root / "sdk" / "sdk.json";
    std::error_code ec;
    if (!fs::exists(sdk, ec))
        return root / "Binaries" / outputDir;
    std::ifstream in(sdk, std::ios::binary);
    if (!in.is_open())
        return std::nullopt;
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto json = Arcane::Remote::ParseJsonBounded(text, Arcane::Remote::kMaxRequestDepth);   // a flat local file
    if (json.is_discarded() || !json.is_object())
        return std::nullopt;
    const auto config = json.find("engineConfig");
    if (config == json.end() || !config->is_string() || config->get<std::string>().empty())
        return std::nullopt;
    const std::size_t dash = outputDir.find('-');
    const std::string tail = dash == std::string::npos ? std::string{} : outputDir.substr(dash);
    return root / "Binaries" / (config->get<std::string>() + tail);
}

std::optional<fs::path> HostsDir(const GlobalOptions& opt, Arcane::Remote::Envelope* error)
{
    if (!opt.engine.empty()) {
        fs::path engine;
        try {
            engine = PathFromUtf8(opt.engine);   // argv is UTF-8 (main.cpp), never the ANSI page
        } catch (const std::exception&) {
            return Unresolved(error, "--engine is not valid UTF-8");
        }
        return ResolveRoot(engine, "--engine", error);
    }
    if (const fs::path env = EngineFromEnvironment(); !env.empty())
        return ResolveRoot(env, "SERVITOR_ENGINE", error);
    const fs::path exe = Arcane::Platform::ExecutablePath();
    const fs::path sibling = exe.parent_path().parent_path();   // Binaries/<cfg>/ above Servitor/
    if (!exe.empty() && HoldsHostFolders(sibling))
        return sibling;
    const fs::path package = sibling.parent_path();             // <root>/Packages/servitor/servitor
    std::error_code ec;
    if (!exe.empty() && fs::exists(package / "sdk" / "sdk.json", ec))
        return ResolveRoot(package, "the package root", error);
    return Unresolved(error, "servitor could not find the engine's host binaries: no --engine, no SERVITOR_ENGINE, "
                             "no ArcaneEditor/ArcaneRuntime/ArcaneServer beside " +
                                 PathUtf8(sibling) + ", and no sdk/sdk.json under " + PathUtf8(package));
}

std::filesystem::path HostDiagnosticsDir(Arcane::Remote::HostKind kind, const GlobalOptions& opt)
{
    const auto bin = HostsDir(opt, nullptr); // rev 8.3 V7: --engine, SERVITOR_ENGINE, sibling, package
    if (!bin)
        return {};
    const char* folder = kind == Arcane::Remote::HostKind::Editor   ? "ArcaneEditor"
                         : kind == Arcane::Remote::HostKind::Server ? "ArcaneServer"
                                                                    : "ArcaneRuntime";
    return *bin / folder / "diagnostics";
}

int StatusVerb(std::span<const std::string> /*rest: Run refuses any; the row takes none*/, const GlobalOptions& opt)
{
    Arcane::Remote::ClientBlock client;
    Arcane::Remote::Envelope env;
    if (const auto dir = RemoteDirFor(opt, &env)) {
        env = Arcane::Remote::MakeOk("status", StatusData(ScanDirectory(*dir), opt), {});
    } else {
        env.command = "status";
    }
    client.exitCode = Arcane::Remote::ExitFromHost(env);
    return PrintEnvelope(env, client, opt);
}

} // namespace Servitor

#endif
