// Windows-only acceptance (rev 8, R8-7): P0 is functional on Windows, and this
// file spawns, redirects and kills hosts with CreateProcessW/TerminateProcess.
// Off Windows it compiles to nothing, so no exclusion-list entry is needed.
// Dist compiles it out too (ruling D-2).
#if defined(_WIN32) && !defined(ARC_BUILD_DIST)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Render/RenderErrorLatch.hpp>
#include <Arcane/Assets/ImageIo.hpp>
#include <Json.hpp>
#include <Servitor/Client.hpp>   // PathUtf8 (argv is UTF-8, Task 30), FileTimeToSys (never clock_cast, R8-7)
#include <Arcane/Remote/Descriptor.hpp>   // ReadDescriptor, IsStale, DescriptorPath: RemoteDirSweep (final fix pass)
#include <Arcane/Project/Project.hpp>     // EditorLock::Self: the sweep test's live descriptor
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

static void DeleteScratch(const std::filesystem::path& project) {
    const auto refs = project / "Verify" / "References";
    std::error_code ec;
    std::filesystem::remove(refs / "servitor-accept-scratch.png", ec);
    if (!std::filesystem::exists(refs, ec)) return;
    for (const auto& entry : std::filesystem::directory_iterator(refs, ec)) {
        if (!entry.is_directory()) continue;
        std::filesystem::remove(entry.path() / "servitor-accept-scratch.png", ec);
    }
}

// Ruling P4 (b): every child host this file starts is owned here. The
// destructor kills each one (TerminateProcess, then CloseHandle) and removes
// the scratch references, so a REQUIRE that aborts by throwing still cleans
// up. Null handles are skipped, so nothing is closed twice.
struct HostGuard {
    HANDLE editor = nullptr;
    HANDLE runtime = nullptr;
    HANDLE server = nullptr;
    std::filesystem::path project;
    ~HostGuard() {
        for (HANDLE h : {editor, runtime, server}) {
            if (h) {
                TerminateProcess(h, 1);
                CloseHandle(h);
            }
        }
        editor = runtime = server = nullptr;
        if (!project.empty())
            DeleteScratch(project);
    }
    HostGuard() = default;
    HostGuard(const HostGuard&) = delete;
    HostGuard& operator=(const HostGuard&) = delete;
};

// Every RunServitor call appends its stdout and stderr here (ruling F23).
static std::string g_servitorTranscript;

// The project's remote directory (Saved/Remote) belongs to the user: it holds
// the audit log, launch logs and capture evidence of any session, not only
// this file's (final fix pass, C-I7 / G-4; it replaces ruling F34's
// remove_all). On construction it prunes only STALE descriptors (a pid that
// is gone or recycled, exactly what `servitor status` would delete) and
// snapshots out/ and the audit log's size. On scope exit, so a REQUIRE abort
// still cleans up, it removes only what the case's children wrote: the
// descriptors <kind>-<pid>.json and launch logs *.servitor-launch-<pid>.log
// of `childPids`, and the out/ files that were not there at the start. The
// audit log, and anything else, is never touched.
struct RemoteDirSweep {
    std::filesystem::path dir;
    std::vector<std::uint32_t> childPids;
    std::vector<std::filesystem::path> outBefore;
    std::uintmax_t auditBefore = 0;
    explicit RemoteDirSweep(std::filesystem::path remoteDir) : dir(std::move(remoteDir)) {
        std::error_code ec;
        // Stale descriptors only: a pid that is gone or recycled. A live one
        // (another session's host) and an unreadable one stay.
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (entry.path().extension() != ".json")
                continue;
            const auto desc = Arcane::Remote::ReadDescriptor(entry.path());
            if (desc && Arcane::Remote::IsStale(*desc)) {
                std::error_code removeEc;
                std::filesystem::remove(entry.path(), removeEc);
            }
        }
        for (const auto& entry : std::filesystem::directory_iterator(dir / "out", ec))
            outBefore.push_back(entry.path());
        auditBefore = AuditBytes();
    }
    ~RemoteDirSweep() {
        std::error_code ec;
        for (const std::uint32_t pid : childPids) {
            if (pid == 0)
                continue;
            const std::string tag = std::to_string(pid);
            for (const char* kind : {"editor", "runtime", "server"})
                std::filesystem::remove(dir / (std::string(kind) + "-" + tag + ".json"), ec);
            for (const char* host : {"ArcaneEditor", "ArcaneRuntime", "ArcaneServer"})
                std::filesystem::remove(dir / "logs" / (std::string(host) + ".servitor-launch-" + tag + ".log"), ec);
        }
        std::vector<std::filesystem::path> created;
        for (const auto& entry : std::filesystem::directory_iterator(dir / "out", ec))
            if (std::find(outBefore.begin(), outBefore.end(), entry.path()) == outBefore.end())
                created.push_back(entry.path());
        for (const auto& file : created)
            std::filesystem::remove(file, ec);
    }
    // The audit log's size now (0 when it does not exist).
    std::uintmax_t AuditBytes() const {
        std::error_code ec;
        const auto bytes = std::filesystem::file_size(dir / "audit.ndjson", ec);
        return ec ? 0 : bytes;
    }
    RemoteDirSweep(const RemoteDirSweep&) = delete;
    RemoteDirSweep& operator=(const RemoteDirSweep&) = delete;
};

// Removes, on scope exit, the crash/hang report files a child host wrote:
// Diagnostics names them "<app>-<stamp>-pid<n>.<ext>" (Diagnostics.cpp), so
// only files whose name carries `-pid<pid>.` and that were NOT there when the
// sweep was armed are removed. Anything that was already present, or names
// another process, is never touched (it may be the user's). Declared after
// RemoteDirSweep and before HostGuard, so the child is dead before it runs.
struct ChildReportSweep {
    std::vector<std::filesystem::path> dirs;
    std::vector<std::filesystem::path> before;
    std::uint32_t pid = 0;
    explicit ChildReportSweep(std::vector<std::filesystem::path> reportDirs) : dirs(std::move(reportDirs)) {
        std::error_code ec;
        for (const auto& dir : dirs)
            for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
                before.push_back(entry.path());
    }
    ~ChildReportSweep() {
        if (pid == 0)
            return;
        const std::string tag = "-pid" + std::to_string(pid) + ".";
        std::error_code ec;
        for (const auto& dir : dirs) {
            std::vector<std::filesystem::path> mine;
            for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
                const std::u8string name = entry.path().filename().u8string();
                const std::string narrow(name.begin(), name.end());
                if (narrow.find(tag) == std::string::npos)
                    continue;
                if (std::find(before.begin(), before.end(), entry.path()) != before.end())
                    continue;
                mine.push_back(entry.path());
            }
            for (const auto& file : mine)
                std::filesystem::remove(file, ec);
        }
    }
    ChildReportSweep(const ChildReportSweep&) = delete;
    ChildReportSweep& operator=(const ChildReportSweep&) = delete;
};

// Every 64-char token in the live descriptors under `dir`.
static std::vector<std::string> ReadTokens(const std::filesystem::path& dir) {
    std::vector<std::string> tokens;
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec))
        return tokens;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.path().extension() != ".json")
            continue;
        std::ifstream in(entry.path());
        const auto j = nlohmann::json::parse(in, nullptr, false);
        if (j.is_object() && j.contains("token") && j["token"].is_string()
            && j["token"].get<std::string>().size() == 64)
            tokens.push_back(j["token"].get<std::string>());
    }
    return tokens;
}

static HANDLE SpawnSleeper() {
    wchar_t sys[MAX_PATH];
    REQUIRE(GetSystemDirectoryW(sys, MAX_PATH) > 0);
    std::wstring cmd = std::wstring(L"\"") + sys + L"\\ping.exe\" 127.0.0.1 -n 30";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');
    REQUIRE(CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi));
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

static HANDLE DuplicateProcess(HANDLE process) {
    HANDLE dup = nullptr;
    REQUIRE(DuplicateHandle(GetCurrentProcess(), process, GetCurrentProcess(), &dup,
                            0, FALSE, DUPLICATE_SAME_ACCESS));
    return dup;
}

// Closes a Win32 handle on scope exit (fix round 1, M7): RunServitor's
// capture files close even when a REQUIRE throws between the two opens.
struct ScopedHandle {
    HANDLE h = INVALID_HANDLE_VALUE;
    ScopedHandle() = default;
    explicit ScopedHandle(HANDLE handle) : h(handle) {}
    ~ScopedHandle() { Close(); }
    void Close() {
        if (h != nullptr && h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
        h = INVALID_HANDLE_VALUE;
    }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
};

// UTF-8 to UTF-16 for a command line: servitor reads its argv as UTF-8
// (CommandLineToArgvW + WideToUtf8, Task 30), so the arguments go in as such.
static std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                                      nullptr, 0);
    REQUIRE(n > 0);
    std::wstring wide(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), wide.data(), n);
    return wide;
}

// One argument as CommandLineToArgvW reads it back: quoted when it holds a
// space, a tab or a quote (or is empty), backslashes doubled before a quote.
static std::wstring QuoteArg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos)
        return arg;
    std::wstring out = L"\"";
    std::size_t slashes = 0;
    for (const wchar_t c : arg) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        if (c == L'"') {
            out.append(slashes * 2 + 1, L'\\');
            out += L'"';
        } else {
            out.append(slashes, L'\\');
            out += c;
        }
        slashes = 0;
    }
    out.append(slashes * 2, L'\\');
    out += L'"';
    return out;
}

// Starts a host from its own directory; the caller hands the handle to a
// HostGuard at once. *pid (when non-null) is the child's process id.
static HANDLE SpawnHost(const std::filesystem::path& exe, const std::vector<std::wstring>& args,
                        DWORD* pid = nullptr) {
    std::wstring cmd = QuoteArg(exe.wstring());
    for (const auto& a : args)
        cmd += L" " + QuoteArg(a);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');
    REQUIRE(CreateProcessW(exe.c_str(), mutableCmd.data(), nullptr, nullptr, FALSE, 0,
                           nullptr, exe.parent_path().c_str(), &si, &pi));
    CloseHandle(pi.hThread);
    if (pid != nullptr)
        *pid = pi.dwProcessId;
    return pi.hProcess;
}

// A servitor stdout that must be one JSON document.
static nlohmann::json Json(const std::string& text) {
    nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    UNSCOPED_INFO("servitor stdout: " << text);
    REQUIRE_FALSE(j.is_discarded());
    return j;
}

static int RunServitor(const std::filesystem::path& servitorExe, const std::vector<std::string>& args,
                       std::string* stdoutText, std::string* stderrText, std::uint32_t timeoutMs);

// servitor's argv for one call: the project, --format json, then `tail`.
static std::vector<std::string> ServitorArgs(const std::filesystem::path& project,
                                             std::initializer_list<std::string> tail) {
    std::vector<std::string> args = {"--project", Servitor::PathUtf8(project), "--format", "json"};
    args.insert(args.end(), tail.begin(), tail.end());
    return args;
}

TEST_CASE("P0 hosts answer the spec envelope", "[remote][gpu]") {
    const auto testsDir = std::filesystem::current_path();   // <repo>\Binaries\Debug-windows-x86_64-md\ArcaneTests
    const auto bin = testsDir.parent_path();                  // <repo>\Binaries\Debug-windows-x86_64-md
    const auto repo = bin.parent_path().parent_path();
    const auto project = repo / "ReferenceProject";
    const auto servitor = bin / "Servitor" / "servitor.exe";
    const auto editor = bin / "ArcaneEditor" / "ArcaneEditor.exe";
    const auto runtime = bin / "ArcaneRuntime" / "ArcaneRuntime.exe";
    const auto server = bin / "ArcaneServer" / "ArcaneServer.exe";
    REQUIRE(std::filesystem::exists(servitor));
    REQUIRE(std::filesystem::exists(editor));
    REQUIRE(std::filesystem::exists(runtime));
    REQUIRE(std::filesystem::exists(server));
    DeleteScratch(project);
    // Final fix pass (C-I7): prunes stale descriptors only; the audit log, and
    // everything else not written by this case's children, stays.
    RemoteDirSweep saved(project / "Saved" / "Remote");
    const std::uintmax_t auditAtStart = saved.AuditBytes();
    g_servitorTranscript.clear();
    auto args = [&project](std::initializer_list<std::string> tail) { return ServitorArgs(project, tail); };

    // The editor runs with --fixed-time 0 (a --headless-only flag) for the
    // scratch bless and its byte-exact compares: they are separate captures,
    // taken with the clock running between the calls. `wait settled` needs no
    // flag: it freezes the host clock itself while it captures (spec s4.7). The
    // runtime is launched WITHOUT --fixed-time, held, so its clock follows wall
    // time, and its own `wait settled` below is the live proof of that freeze.
    const std::wstring proj = project.wstring();
    HostGuard hosts;
    hosts.project = project;
    DWORD editorPid = 0, runtimePid = 0, serverPid = 0;
    hosts.editor = SpawnHost(editor, {L"--headless", L"--project", proj, L"--remote", L"--fixed-time", L"0"}, &editorPid);
    hosts.runtime = SpawnHost(runtime, {L"--headless", L"--project", proj, L"--remote"}, &runtimePid);
    hosts.server = SpawnHost(server, {L"--project", proj, L"--remote"}, &serverPid);
    saved.childPids = {static_cast<std::uint32_t>(editorPid), static_cast<std::uint32_t>(runtimePid),
                       static_cast<std::uint32_t>(serverPid)};

    std::string out, err;
    // A spawned host is booting until it writes its descriptor, and only
    // `wait ready` keeps looking for a host that is not there yet (spec s4.4;
    // every other verb is target.none, exit 4, at once). Wait for all three
    // before any other verb.
    for (const char* kind : {"editor", "runtime", "server"}) {
        INFO("wait ready on the " << kind);
        REQUIRE(RunServitor(servitor, args({"--host", kind, "--timeout", "60", "wait", "ready"}), &out, &err,
                            75000) == 0);
    }
    out.clear();
    const int settled = RunServitor(servitor, args({"--host", "editor", "--timeout", "60", "wait", "settled"}),
                                    &out, &err, 75000);
    REQUIRE(settled == 0);
    auto settledJson = Json(out);   // non-const: a missing key reads null, never asserts
    CHECK(settledJson["ok"] == true);
    CHECK(settledJson["command"] == "host.wait");
    CHECK(settledJson["data"]["done"] == true);
    CHECK(settledJson["data"]["captures"].get<std::uint64_t>() >= 2);   // two byte-equal captures (spec s4.7)
    CHECK(settledJson["host"].contains("frame"));
    CHECK_FALSE(settledJson.contains("hostFrame"));
    CHECK_FALSE(settledJson.contains("result"));
    CHECK(settledJson["client"]["exitCode"] == 0);

    // Snapshot all three hosts' tokens now (ruling F23); the editor's descriptor
    // is gone after `quit`, so a check at the end could not read it.
    std::vector<std::string> tokens;
    const auto tokenDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (std::chrono::steady_clock::now() < tokenDeadline) {
        tokens = ReadTokens(saved.dir);
        if (tokens.size() >= 3)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    REQUIRE(tokens.size() == 3);

    // I2: the held runtime's clock follows wall time (no --fixed-time), so it
    // settles only because the wait freezes that clock for its captures.
    out.clear();
    REQUIRE(RunServitor(servitor, args({"--host", "runtime", "--timeout", "60", "wait", "settled"}),
                        &out, &err, 75000) == 0);
    auto runtimeSettled = Json(out);
    CHECK(runtimeSettled["data"]["done"] == true);
    CHECK(runtimeSettled["data"]["captures"].get<std::uint64_t>() >= 2);

    // Ruling F21, decided live: render.vsync is ArcaneRender's (a Client
    // module the server does not link), but a game module the server maps may
    // bring it in. If the server knows it, it is the probe; if it answers
    // cvar.unknown (exit 6), the probe is ArcaneCore's log.file.flushLevel
    // (Audience::Game, not Hidden or Protected: every context reads it). The
    // choice is written to servitor-latency.txt for the task record.
    std::string kServerProbe;
    {
        out.clear();
        const int vsyncOnServer = RunServitor(servitor, args({"--host", "server", "cvar", "get", "render.vsync"}),
                                              &out, &err, 20000);
        if (vsyncOnServer == 0) {
            kServerProbe = "render.vsync";
        } else {
            REQUIRE(vsyncOnServer == 6);
            CHECK(Json(out)["errors"][0]["code"] == "cvar.unknown");
            kServerProbe = "log.file.flushLevel";
        }
        std::ofstream record("servitor-latency.txt", std::ios::app);
        record << "kServerProbe " << kServerProbe << '\n';
    }

    out.clear();
    const int got = RunServitor(servitor, args({"--host", "editor", "entity", "get", "name:MeshCube",
                                                "--component", "Transform"}),
                                &out, &err, 20000);
    REQUIRE(got == 0);
    auto entity = Json(out);
    REQUIRE(entity["data"]["components"].contains("Transform"));
    const auto& pos = entity["data"]["components"]["Transform"]["position"];
    CHECK(std::abs(pos[0].get<double>() - 0.4) < 1e-4);
    CHECK(std::abs(pos[1].get<double>() - (-0.3)) < 1e-4);
    CHECK(std::abs(pos[2].get<double>() - 0.0) < 1e-4);
    const auto& rot = entity["data"]["components"]["Transform"]["rotation"];
    CHECK(std::abs(rot[0].get<double>() - 0.0) < 1e-4);
    CHECK(std::abs(rot[1].get<double>() - 0.0) < 1e-4);
    CHECK(std::abs(rot[2].get<double>() - 0.0) < 1e-4);
    CHECK(std::abs(rot[3].get<double>() - 1.0) < 1e-4);

    // C8: the live set is a PlayerSafe cvar (a Game-audience set from a
    // runtime or a server is permission.denied, exit 3).
    out.clear();
    REQUIRE(RunServitor(servitor, args({"--host", "editor", "cvar", "set", "render.vsync", "false"}),
                        &out, &err, 20000) == 0);
    out.clear();
    REQUIRE(RunServitor(servitor, args({"--host", "editor", "cvar", "explain", "render.vsync"}),
                        &out, &err, 20000) == 0);
    auto explained = Json(out);
    CHECK(explained["data"]["audience"] == "PlayerSafe");
    CHECK(explained["data"]["setBy"] == "Console");
    CHECK(explained["data"]["apply"] == "Restart");

    out.clear();
    const int blessed = RunServitor(servitor, args({"--host", "editor", "--timeout", "60", "compare",
                                                    "servitor-accept-scratch", "--source", "host", "--bless"}),
                                    &out, &err, 75000);
    REQUIRE(blessed == 0);
    auto blessJson = Json(out);
    CHECK(blessJson["ok"] == true);
    CHECK(blessJson["command"] == "capture.compare");
    CHECK(blessJson["data"]["verdict"] == "Passed");
    CHECK(blessJson["host"]["frame"] == settledJson["host"]["frame"]);   // the idle editor capture

    out.clear();
    const int passed = RunServitor(servitor, args({"--host", "editor", "--timeout", "60", "compare",
                                                   "servitor-accept-scratch", "--source", "host"}),
                                   &out, &err, 75000);
    REQUIRE(passed == 0);
    CHECK(out.find("--bless") == std::string::npos);
    auto passJson = Json(out);
    CHECK(passJson["ok"] == true);
    CHECK(passJson["command"] == "capture.compare");
    CHECK(passJson["data"]["verdict"] == "Passed");

    const auto scratch = project / "Verify" / "References" / "servitor-accept-scratch.png";
    std::uint32_t pw = 0, ph = 0;
    std::vector<unsigned char> pixels;
    REQUIRE(std::filesystem::exists(scratch));
    REQUIRE(Arcane::LoadPngRgba(scratch, pw, ph, pixels));
    // Poison a solid 16x16 block in the middle, in a colour far from the
    // original there. CompareImages (Playwright's ssim-cie94 cascade) judges a
    // lone changed pixel inside varied surroundings (or the 15 px padding at a
    // corner) as antialiasing, so a one-pixel poison can still pass; a flat
    // block's interior is a real difference at every pixel.
    constexpr std::uint32_t kPoisonSide = 16;
    REQUIRE(pw >= kPoisonSide);
    REQUIRE(ph >= kPoisonSide);
    REQUIRE(pixels.size() == static_cast<std::size_t>(pw) * ph * 4);
    const std::size_t centre = (static_cast<std::size_t>(ph / 2) * pw + pw / 2) * 4;
    const unsigned char poison[3] = {static_cast<unsigned char>(pixels[centre + 0] < 128 ? 255 : 0),
                                     static_cast<unsigned char>(pixels[centre + 1] < 128 ? 255 : 0),
                                     static_cast<unsigned char>(pixels[centre + 2] < 128 ? 255 : 0)};
    for (std::uint32_t y = ph / 2 - kPoisonSide / 2; y < ph / 2 + kPoisonSide / 2; ++y) {
        for (std::uint32_t x = pw / 2 - kPoisonSide / 2; x < pw / 2 + kPoisonSide / 2; ++x) {
            const std::size_t at = (static_cast<std::size_t>(y) * pw + x) * 4;
            pixels[at + 0] = poison[0];
            pixels[at + 1] = poison[1];
            pixels[at + 2] = poison[2];
            pixels[at + 3] = 255;
        }
    }
    REQUIRE(Arcane::WritePngRgba(scratch, pw, ph, pixels.data()));
    out.clear();
    const int poisoned = RunServitor(servitor, args({"--host", "editor", "--timeout", "60", "compare",
                                                     "servitor-accept-scratch", "--source", "host"}),
                                     &out, &err, 75000);
    REQUIRE(poisoned == 8);
    CHECK(out.find("--bless") == std::string::npos);
    auto poisonJson = Json(out);
    CHECK(poisonJson["ok"] == true);
    CHECK(poisonJson["data"]["verdict"] == "Failed");
    CHECK(poisonJson["errors"].empty());

    out.clear();
    CHECK(RunServitor(servitor, args({"--host", "runtime", "entity", "get", "name:NoSuchEntity"}),
                      &out, &err, 20000) == 6);
    CHECK(Json(out)["errors"][0]["code"] == "entity.not-found");

    out.clear();
    CHECK(RunServitor(servitor, args({"--host", "runtime", "--timeout", "1", "wait", "state", "playing"}),
                      &out, &err, 5000) == 8);
    auto timed = Json(out);
    CHECK(timed["ok"] == true);
    CHECK(timed["data"]["timedOut"] == true);
    CHECK(timed["errors"].empty());

    out.clear();
    CHECK(RunServitor(servitor, args({"--host", "server", "screenshot", "window"}), &out, &err, 20000) == 2);
    CHECK(Json(out)["errors"][0]["code"] == "capture.no-renderer");

    out.clear();
    REQUIRE(RunServitor(servitor, args({"--host", "runtime", "stats", "frame"}), &out, &err, 20000) == 0);
    const auto idleBefore = Json(out)["data"]["frame"];
    out.clear();
    const int shot = RunServitor(servitor, args({"--host", "runtime", "--timeout", "60", "screenshot", "window"}),
                                 &out, &err, 75000);
    REQUIRE(shot == 0);
    auto shotJson = Json(out);
    CHECK(shotJson["ok"] == true);
    CHECK(shotJson["command"] == "capture.window");
    out.clear();
    REQUIRE(RunServitor(servitor, args({"--host", "runtime", "stats", "frame"}), &out, &err, 20000) == 0);
    CHECK(Json(out)["data"]["frame"] == idleBefore);   // a held runtime captured on a render ticket

    auto stepHost = [&](const char* host) {
        std::string before, after, sink;
        REQUIRE(RunServitor(servitor, args({"--host", host, "stats", "frame"}), &before, &sink, 20000) == 0);
        const auto frame0 = Json(before)["data"]["frame"].get<std::uint64_t>();
        std::string stepOut;
        REQUIRE(RunServitor(servitor, args({"--host", host, "--timeout", "15", "step", "10"}), &stepOut, &sink,
                            20000) == 0);
        auto stepped = Json(stepOut);
        CHECK(stepped["host"]["frame"].get<std::uint64_t>() - frame0 == 10);
        REQUIRE(RunServitor(servitor, args({"--host", host, "stats", "frame"}), &after, &sink, 20000) == 0);
        CHECK(Json(after)["data"]["frame"].get<std::uint64_t>() - frame0 == 10);
    };
    stepHost("runtime");

    // Ruling F34: stop exits 0 and does not move the frame.
    std::string stopBefore, stopAfter, stopSink;
    REQUIRE(RunServitor(servitor, args({"--host", "runtime", "stats", "frame"}), &stopBefore, &stopSink, 20000) == 0);
    out.clear();
    CHECK(RunServitor(servitor, args({"--host", "runtime", "stop"}), &out, &err, 20000) == 0);
    CHECK(Json(out)["command"] == "play.stop");
    REQUIRE(RunServitor(servitor, args({"--host", "runtime", "stats", "frame"}), &stopAfter, &stopSink, 20000) == 0);
    CHECK(Json(stopAfter)["data"]["frame"] == Json(stopBefore)["data"]["frame"]);

    stepHost("server");

    out.clear();
    CHECK(RunServitor(servitor, args({"--host", "runtime", "cvar", "get", "render.vsync"}), &out, &err, 20000) == 0);
    CHECK(Json(out)["command"] == "cvar.get");
    // C8: a PlayerSafe set from the runtime's LocalHost context is allowed.
    out.clear();
    CHECK(RunServitor(servitor, args({"--host", "runtime", "cvar", "set", "render.vsync", "false"}),
                      &out, &err, 20000) == 0);
    CHECK(Json(out)["command"] == "cvar.set");
    out.clear();
    CHECK(RunServitor(servitor, args({"--host", "runtime", "cvar", "explain", "render.vsync"}),
                      &out, &err, 20000) == 0);
    CHECK(Json(out)["data"]["setBy"] == "Console");
    out.clear();
    CHECK(RunServitor(servitor, args({"--host", "server", "cvar", "get", kServerProbe}), &out, &err, 20000) == 0);
    CHECK(Json(out)["command"] == "cvar.get");

    out.clear();
    CHECK(RunServitor(servitor, args({"--all", "cvar", "get", kServerProbe}), &out, &err, 20000) == 0);
    auto fan = Json(out);
    REQUIRE(fan.contains("hosts"));
    CHECK(fan["hosts"].size() == 3);   // editor, runtime and server
    for (auto& row : fan["hosts"]) {
        CHECK(row["envelope"]["command"] == "cvar.get");
        CHECK(row["envelope"]["client"]["exitCode"] == 0);
    }

    out.clear();
    CHECK(RunServitor(servitor, args({"--host", "runtime", "entity", "get", "name:MeshCube", "--component",
                                      "Transform"}),
                      &out, &err, 20000) == 0);
    out.clear();
    CHECK(RunServitor(servitor, args({"--host", "server", "entity", "get", "name:MeshCube", "--component",
                                      "Transform"}),
                      &out, &err, 20000) == 0);
    out.clear();
    CHECK(RunServitor(servitor, args({"--host", "editor", "problems"}), &out, &err, 20000) == 0);
    CHECK(Json(out)["command"] == "problems.list");

    // C8: the editor's Play -> pause -> step 2 -> host.frame +2 -> stop. Task
    // 29's editor host.frame bump has no other live test. Paused first, so
    // only the requested single steps advance the sim.
    {
        std::string playOut, sink;
        REQUIRE(RunServitor(servitor, args({"--host", "editor", "play"}), &playOut, &sink, 20000) == 0);
        auto played = Json(playOut);
        CHECK(played["command"] == "play.start");
        CHECK(played["data"]["playing"] == true);
        CHECK_FALSE(played["data"].contains("alreadyPlaying"));
        // Final fix pass (C-I3): the editor publishes Play as its host state,
        // so `wait state` completes on it (it timed out on every host before).
        std::string stateOut;
        REQUIRE(RunServitor(servitor, args({"--host", "editor", "--timeout", "15", "wait", "state", "playing"}),
                            &stateOut, &sink, 20000) == 0);
        CHECK(Json(stateOut)["host"]["state"] == "playing");
        std::string pauseOut;
        REQUIRE(RunServitor(servitor, args({"--host", "editor", "pause"}), &pauseOut, &sink, 20000) == 0);
        CHECK(Json(pauseOut)["data"]["paused"] == true);
        stateOut.clear();
        REQUIRE(RunServitor(servitor, args({"--host", "editor", "--timeout", "15", "wait", "state", "paused"}),
                            &stateOut, &sink, 20000) == 0);
        CHECK(Json(stateOut)["data"]["state"] == "paused");
        std::string before, stepOut, after;
        REQUIRE(RunServitor(servitor, args({"--host", "editor", "stats", "frame"}), &before, &sink, 20000) == 0);
        const auto frame0 = Json(before)["data"]["frame"].get<std::uint64_t>();
        REQUIRE(RunServitor(servitor, args({"--host", "editor", "--timeout", "15", "step", "2"}), &stepOut, &sink,
                            20000) == 0);
        auto stepped = Json(stepOut);
        CHECK(stepped["command"] == "play.step");
        CHECK(stepped["data"]["advanced"] == 2);
        CHECK(stepped["host"]["frame"].get<std::uint64_t>() - frame0 == 2);
        REQUIRE(RunServitor(servitor, args({"--host", "editor", "stats", "frame"}), &after, &sink, 20000) == 0);
        CHECK(Json(after)["data"]["frame"].get<std::uint64_t>() - frame0 == 2);
        std::string stopOut;
        REQUIRE(RunServitor(servitor, args({"--host", "editor", "stop"}), &stopOut, &sink, 20000) == 0);
        auto stopped = Json(stopOut);
        CHECK(stopped["command"] == "play.stop");
        CHECK(stopped["data"]["playing"] == false);
    }

    out.clear();
    CHECK(RunServitor(servitor, args({"--host", "editor", "--timeout", "8", "quit"}), &out, &err, 10000) == 0);
    CHECK(Json(out)["command"] == "host.quit");
    // Quit within 10 s; past that the guard kills it, and that is a failure.
    CHECK(WaitForSingleObject(hosts.editor, 10000) == WAIT_OBJECT_0);

    // Ruling F23: no host's token appears in ANY servitor.exe output of this case.
    REQUIRE_FALSE(g_servitorTranscript.empty());
    for (const auto& token : tokens)
        CHECK(g_servitorTranscript.find(token) == std::string::npos);
    // C-I7: the case's Drive calls were audited into the project's existing
    // log, which the sweep never removes (it grew; it was not recreated).
    CHECK(saved.AuditBytes() > auditAtStart);

    CHECK(Arcane::RenderErrorCount() == 0);
}

// CreateProcessW with stdout and stderr redirected to files, WaitForSingleObject
// bounded by timeoutMs (TerminateProcess and 124 past it). servitor's own
// --timeout is always under timeoutMs (M2). Appends one line,
// `milliseconds args`, to servitor-latency.txt beside the test exe, and the
// call's stdout and stderr to g_servitorTranscript (ruling F23).
// RenderErrorCount() covers this process; the children's GPU validation is the
// golden gate.
static int RunServitor(const std::filesystem::path& servitorExe, const std::vector<std::string>& args,
                       std::string* stdoutText, std::string* stderrText, std::uint32_t timeoutMs) {
    const auto dir = std::filesystem::temp_directory_path() / "remote-p0-accept";
    std::filesystem::create_directories(dir);
    const auto outPath = dir / "stdout.txt";
    const auto errPath = dir / "stderr.txt";
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    ScopedHandle outFile(CreateFileW(outPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    ScopedHandle errFile(CreateFileW(errPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    REQUIRE(outFile.h != INVALID_HANDLE_VALUE);
    REQUIRE(errFile.h != INVALID_HANDLE_VALUE);

    // Fix round 1 (M2): servitor must answer before this harness gives up, or a
    // host.busy it is still retrying reads as 124 with no envelope. A call
    // that names no --timeout gets one a quarter (at most 5 s) under the
    // harness wait; one that names its own must already be under it.
    std::vector<std::string> argv = args;
    const auto given = std::find(argv.begin(), argv.end(), "--timeout");
    if (given == argv.end()) {
        const std::uint32_t slack = std::min<std::uint32_t>(timeoutMs / 4, 5000);
        char seconds[32];
        std::snprintf(seconds, sizeof(seconds), "%.3f", static_cast<double>(timeoutMs - slack) / 1000.0);
        argv.insert(argv.begin(), {"--timeout", seconds});
    } else {
        REQUIRE(given + 1 != argv.end());
        REQUIRE(std::stod(*(given + 1)) * 1000.0 < static_cast<double>(timeoutMs));
    }

    std::wstring cmd = QuoteArg(servitorExe.wstring());
    std::string joined;
    for (const auto& a : argv) {
        cmd += L" " + QuoteArg(Utf8ToWide(a));
        joined += ' ' + a;
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = outFile.h;
    si.hStdError = errFile.h;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');
    const auto t0 = std::chrono::steady_clock::now();
    const BOOL started = CreateProcessW(servitorExe.c_str(), mutableCmd.data(), nullptr, nullptr, TRUE, 0,
                                        nullptr, servitorExe.parent_path().c_str(), &si, &pi);
    outFile.Close();   // the child holds its own copies; ours close now so the files can be read back
    errFile.Close();
    if (!started)
        return 125;
    ScopedHandle process(pi.hProcess);
    ScopedHandle thread(pi.hThread);
    const DWORD wait = WaitForSingleObject(process.h, timeoutMs);
    int code = 124;
    if (wait == WAIT_OBJECT_0) {
        DWORD exitCode = 124;
        GetExitCodeProcess(process.h, &exitCode);
        code = static_cast<int>(exitCode);
    } else {
        TerminateProcess(process.h, 124);
        WaitForSingleObject(process.h, 5000);   // its handles to the capture files close before they are read
    }
    thread.Close();
    process.Close();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    {
        std::ofstream latency("servitor-latency.txt", std::ios::app);
        latency << ms << joined << '\n';
    }
    UNSCOPED_INFO("servitor" << joined << " -> exit " << code << " in " << ms << " ms");
    std::string capturedOut;
    std::string capturedErr;
    {
        std::ifstream in(outPath, std::ios::binary);
        capturedOut = std::string(std::istreambuf_iterator<char>(in), {});
    }
    {
        std::ifstream in(errPath, std::ios::binary);
        capturedErr = std::string(std::istreambuf_iterator<char>(in), {});
    }
    g_servitorTranscript += capturedOut;   // every call, success or failure (ruling F23)
    g_servitorTranscript += capturedErr;
    if (stdoutText)
        *stdoutText = capturedOut;
    if (stderrText)
        *stderrText = capturedErr;
    return code;
}

TEST_CASE("a hung runtime is exit 7", "[remote][gpu]") {
    const auto bin = std::filesystem::current_path().parent_path();
    const auto repo = bin.parent_path().parent_path();
    const auto project = repo / "ReferenceProject";
    const auto runtime = bin / "ArcaneRuntime" / "ArcaneRuntime.exe";
    const auto servitor = bin / "Servitor" / "servitor.exe";
    REQUIRE(std::filesystem::exists(runtime));
    REQUIRE(std::filesystem::exists(servitor));
    RemoteDirSweep saved(project / "Saved" / "Remote");   // C-I7: stale descriptors only
    // M8: the hang report this child writes is removed on scope exit, after
    // HostGuard has killed it; nothing that was already there is touched.
    ChildReportSweep reports({project / "Saved" / "Diagnostics", runtime.parent_path() / "diagnostics"});
    HostGuard hosts;
    DWORD pid = 0;
    hosts.runtime = SpawnHost(runtime, {L"--headless", L"--project", project.wstring(), L"--remote",
                                        L"--hang-main", L"60"},
                              &pid);
    reports.pid = static_cast<std::uint32_t>(pid);
    saved.childPids = {static_cast<std::uint32_t>(pid)};
    bool sawHung = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
    while (std::chrono::steady_clock::now() < deadline) {
        std::string statusOut, statusErr;
        const int statusCode = RunServitor(servitor, ServitorArgs(project, {"status"}), &statusOut, &statusErr, 5000);
        if (statusCode == 0) {
            const auto status = nlohmann::json::parse(statusOut, nullptr, false);
            if (!status.is_discarded() && status.contains("data") && status["data"].contains("hosts")
                && status["data"]["hosts"].is_array()) {
                for (const auto& host : status["data"]["hosts"]) {
                    if (host.value("hostKind", "") == "runtime"
                        && host.value("pid", std::uint32_t{0}) == static_cast<std::uint32_t>(pid)
                        && host.value("hung", false) == true) {
                        sawHung = true;
                        break;
                    }
                }
            }
        }
        if (sawHung) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    REQUIRE(sawHung);
    const auto callStart = std::chrono::system_clock::now();
    std::string out, err;
    const int code = RunServitor(servitor, ServitorArgs(project, {"--host", "runtime", "--pid", std::to_string(pid),
                                                                  "--timeout", "10", "cvar", "get", "render.vsync"}),
                                 &out, &err, 15000);
    CHECK(code == 7);
    if (code == 7) {
        auto body = Json(out);
        CHECK(body["errors"][0]["code"] == "host.hung");
        CHECK(body["client"]["exitCode"] == 7);
        if (body["errors"][0]["detail"].contains("arcdiag")) {
            const auto path = Servitor::PathFromUtf8(body["errors"][0]["detail"]["arcdiag"].get<std::string>());
            CHECK(std::filesystem::exists(path));
            CHECK(path.extension() == ".arcdiag");
            // FileTimeToSys, as Task 14 does; never clock_cast (ruling S3-8, R8-7).
            const auto mtime = Servitor::FileTimeToSys(std::filesystem::last_write_time(path));
            CHECK(mtime > callStart);
        }
    }
    DWORD procCode = 0;
    REQUIRE(GetExitCodeProcess(hosts.runtime, &procCode));
    CHECK(procCode == STILL_ACTIVE);
}

// Ruling F22: the live server --frames check. N is 1200 (20 s at 60 Hz), so
// the descriptor wait and the pause land long before the budget is spent.
TEST_CASE("server --frames 1200 ticks exactly 1200 across a remote pause, step and resume", "[remote][gpu]") {
    const auto bin = std::filesystem::current_path().parent_path();
    const auto repo = bin.parent_path().parent_path();
    const auto project = repo / "ReferenceProject";
    const auto server = bin / "ArcaneServer" / "ArcaneServer.exe";
    const auto servitor = bin / "Servitor" / "servitor.exe";
    REQUIRE(std::filesystem::exists(server));
    REQUIRE(std::filesystem::exists(servitor));
    constexpr std::uint64_t kFrames = 1200;
    RemoteDirSweep saved(project / "Saved" / "Remote");   // C-I7: stale descriptors only
    const std::uintmax_t auditAtStart = saved.AuditBytes();
    const auto scratch = std::filesystem::temp_directory_path() / "remote-server-frames";
    std::filesystem::remove_all(scratch);
    std::filesystem::create_directories(scratch);
    const auto report = scratch / "server-report.json";
    HostGuard hosts;
    DWORD serverPid = 0;
    hosts.server = SpawnHost(server, {L"--project", project.wstring(), L"--frames", L"1200", L"--report",
                                      report.wstring(), L"--remote"},
                             &serverPid);
    saved.childPids = {static_cast<std::uint32_t>(serverPid)};
    const std::string pid = std::to_string(serverPid);

    bool live = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!live && std::chrono::steady_clock::now() < deadline) {
        std::string statusOut, statusErr;
        if (RunServitor(servitor, ServitorArgs(project, {"status"}), &statusOut, &statusErr, 5000) == 0) {
            const auto status = nlohmann::json::parse(statusOut, nullptr, false);
            if (!status.is_discarded() && status.contains("data") && status["data"].contains("hosts")) {
                for (const auto& host : status["data"]["hosts"]) {
                    if (host.value("hostKind", "") == "server"
                        && host.value("pid", std::uint32_t{0}) == static_cast<std::uint32_t>(serverPid))
                        live = true;
                }
            }
        }
        if (!live)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    REQUIRE(live);

    std::string out, err;
    REQUIRE(RunServitor(servitor, ServitorArgs(project, {"--host", "server", "--pid", pid, "pause"}),
                        &out, &err, 20000) == 0);
    const auto pausedFrame = Json(out)["host"]["frame"].get<std::uint64_t>();
    REQUIRE(pausedFrame < kFrames);                       // the pause landed before the budget was spent
    out.clear();
    REQUIRE(RunServitor(servitor, ServitorArgs(project, {"--host", "server", "--pid", pid, "step", "3"}),
                        &out, &err, 20000) == 0);
    auto stepped = Json(out);
    CHECK(stepped["ok"] == true);
    CHECK(stepped["host"]["frame"].get<std::uint64_t>() - pausedFrame == 3);
    out.clear();
    REQUIRE(RunServitor(servitor, ServitorArgs(project, {"--host", "server", "--pid", pid, "resume"}),
                        &out, &err, 20000) == 0);

    REQUIRE(WaitForSingleObject(hosts.server, 60000) == WAIT_OBJECT_0);
    DWORD exitCode = 1;
    REQUIRE(GetExitCodeProcess(hosts.server, &exitCode));
    CHECK(exitCode == 0);
    std::ifstream in(report);
    const auto rep = nlohmann::json::parse(in, nullptr, false);
    REQUIRE(rep.is_object());
    REQUIRE(rep.contains("framesTicked"));
    CHECK(rep["framesTicked"].get<std::uint64_t>() == kFrames);   // the pause spent none of the budget
    CHECK(saved.AuditBytes() > auditAtStart);   // pause, step and resume are Drive calls (C-I7)
}

// The desk-only TDR lane, gated like G1 (CrashWitnessTest.cpp:122-124). An
// agent must not set ARCANE_DIAG_DESK: a deliberate GPU fault is a
// machine-wide TDR (ruling R106).
TEST_CASE("crash-gpu is the desk-only TDR lane", "[remote][gpu]") {
    if (!std::getenv("ARCANE_DIAG_DESK"))
        SKIP("ARCANE_DIAG_DESK not set -- desk-only TDR lane");
    const auto bin = std::filesystem::current_path().parent_path();
    const auto repo = bin.parent_path().parent_path();
    const auto project = repo / "ReferenceProject";
    const auto runtime = bin / "ArcaneRuntime" / "ArcaneRuntime.exe";
    HostGuard hosts;
    hosts.runtime = SpawnHost(runtime, {L"--headless", L"--project", project.wstring(), L"--frames", L"60",
                                        L"--crash-gpu", L"30"});
    const DWORD wait = WaitForSingleObject(hosts.runtime, 120000);
    DWORD exitCode = 0;
    GetExitCodeProcess(hosts.runtime, &exitCode);
    CHECK(wait == WAIT_OBJECT_0);
    CHECK(exitCode == 1);
}

// HostGuard's destructor is what kills the children. Catch2 REQUIRE aborts by
// throwing, so a forced exception leaving the scope is the same cleanup. This
// case throws std::runtime_error rather than REQUIRE(false), so the suite stays
// green, and checks the three sleepers have exited. It starts no host and
// reads no ARCANE_DIAG_DESK. DuplicateHandle runs before the throw because
// ~HostGuard closes the original handles. Untagged [gpu]: the Windows ~[gpu]
// lane runs it.
TEST_CASE("the host guard kills children when a REQUIRE aborts", "[remote][accept]") {
    HANDLE watchEditor = nullptr;
    HANDLE watchRuntime = nullptr;
    HANDLE watchServer = nullptr;
    bool threw = false;
    try {
        HostGuard guard;
        guard.editor = SpawnSleeper();
        guard.runtime = SpawnSleeper();
        guard.server = SpawnSleeper();
        watchEditor = DuplicateProcess(guard.editor);
        watchRuntime = DuplicateProcess(guard.runtime);
        watchServer = DuplicateProcess(guard.server);
        throw std::runtime_error("forced failure");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    REQUIRE(threw);
    for (HANDLE h : {watchEditor, watchRuntime, watchServer}) {
        REQUIRE(h != nullptr);
        const DWORD wait = WaitForSingleObject(h, 5000);
        CHECK(wait == WAIT_OBJECT_0);
        DWORD exitCode = STILL_ACTIVE;
        CHECK(GetExitCodeProcess(h, &exitCode));
        CHECK(exitCode != STILL_ACTIVE);
        CloseHandle(h);
    }
}

// Final fix pass (C-I7 / G-4): the sweep every acceptance case uses keeps what
// is not the case's own. Runs on a temp directory, so it starts no host and is
// in the ~[gpu] lane.
TEST_CASE("the acceptance sweep removes only what the case's children wrote, never the audit log", "[remote][accept]") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("remote-accept-sweep-" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "out");
    fs::create_directories(dir / "logs");
    const auto touch = [](const fs::path& p, const char* text) { std::ofstream(p, std::ios::binary) << text; };
    touch(dir / "audit.ndjson", "{\"command\":\"earlier\"}\n");
    touch(dir / "user-note.txt", "not the test's");
    touch(dir / "out" / "capture.window-1-old.png", "old");
    touch(dir / "logs" / "ArcaneServer.servitor-launch-777.log", "another launch");
    // A live descriptor that is not a child's (this process), and a stale one
    // (this pid with another creation stamp: recycled, so dead).
    Arcane::Remote::Descriptor live;
    live.hostKind = Arcane::Remote::HostKind::Editor;
    live.pid = static_cast<std::uint32_t>(GetCurrentProcessId());
    live.processStart = Arcane::EditorLock::Self().start;
    live.port = 1;
    live.token = std::string(64, 'a');
    REQUIRE(Arcane::Remote::WriteDescriptorAtomic(dir / ("editor-" + std::to_string(live.pid) + ".json"), live));
    Arcane::Remote::Descriptor stale = live;
    stale.hostKind = Arcane::Remote::HostKind::Runtime;
    stale.processStart += 1;
    REQUIRE(Arcane::Remote::WriteDescriptorAtomic(dir / ("runtime-" + std::to_string(live.pid) + ".json"), stale));
    {
        RemoteDirSweep sweep(dir);
        CHECK_FALSE(fs::exists(dir / ("runtime-" + std::to_string(live.pid) + ".json")));   // stale: pruned
        CHECK(fs::exists(dir / ("editor-" + std::to_string(live.pid) + ".json")));         // live: kept
        const auto auditAtStart = sweep.AuditBytes();
        CHECK(auditAtStart > 0);
        sweep.childPids = {4242};
        touch(dir / "server-4242.json", "{}");
        touch(dir / "logs" / "ArcaneServer.servitor-launch-4242.log", "the child's launch");
        touch(dir / "out" / "capture.window-4242-new.png", "new");
        std::ofstream(dir / "audit.ndjson", std::ios::binary | std::ios::app) << "{\"command\":\"play.step\"}\n";
        CHECK(sweep.AuditBytes() > auditAtStart);
    }
    CHECK(fs::exists(dir / "audit.ndjson"));
    CHECK(fs::exists(dir / "user-note.txt"));
    CHECK(fs::exists(dir / "out" / "capture.window-1-old.png"));
    CHECK(fs::exists(dir / "logs" / "ArcaneServer.servitor-launch-777.log"));
    CHECK(fs::exists(dir / ("editor-" + std::to_string(live.pid) + ".json")));
    CHECK_FALSE(fs::exists(dir / "server-4242.json"));
    CHECK_FALSE(fs::exists(dir / "logs" / "ArcaneServer.servitor-launch-4242.log"));
    CHECK_FALSE(fs::exists(dir / "out" / "capture.window-4242-new.png"));
    fs::remove_all(dir, ec);
}

#endif // defined(_WIN32) && !defined(ARC_BUILD_DIST)
