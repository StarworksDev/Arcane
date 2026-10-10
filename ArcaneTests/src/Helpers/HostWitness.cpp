#include "HostWitness.hpp"

#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
// POSIX twin (Linux port, 2026-10-05): fork/execv + waitpid(WNOHANG) slices,
// stdio redirected to files, /proc/<pid>/stat for the CPU sample, SIGKILL at
// the hard cap -- the same observable facts the Windows half records.
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#if defined(__APPLE__)
#include <Arcane/Platform/Process.hpp>
#endif
#endif

namespace Arcane::Test
{
#ifdef _WIN32
    namespace
    {
        // UTF-8 -> UTF-16 for the CreateProcessW boundary, same idiom as
        // ArcaneEditor's RuntimeLaunch::QuoteArg / ModuleBuild::Widen.
        std::wstring Utf8ToWide(const std::string& utf8)
        {
            if (utf8.empty())
                return {};
            const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                                 static_cast<int>(utf8.size()), nullptr, 0);
            std::wstring wide(static_cast<std::size_t>(n), L'\0');
            ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                                   wide.data(), n);
            return wide;
        }

        // Windows command-line quoting for ONE token (CreateProcessW takes a
        // single mutable string, not an argv array). Lifted verbatim from
        // RuntimeLaunch::QuoteArg -- same escaping rules, kept local here
        // because this is the only other spawn site in the tree.
        std::wstring QuoteArg(const std::wstring& arg)
        {
            if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos)
                return arg;

            std::wstring out = L"\"";
            std::size_t backslashes = 0;
            for (wchar_t c : arg)
            {
                if (c == L'\\')
                {
                    ++backslashes;
                    continue;
                }
                if (c == L'"')
                {
                    out.append(backslashes * 2 + 1, L'\\');
                    backslashes = 0;
                    out.push_back(L'"');
                    continue;
                }
                out.append(backslashes, L'\\');
                backslashes = 0;
                out.push_back(c);
            }
            out.append(backslashes * 2, L'\\');
            out.push_back(L'"');
            return out;
        }

        // Best-effort sweep of STALE sibling dirs under arcane-witness-stdio/,
        // left behind by earlier report-less runs (nothing else ever deletes
        // them). Run at ResolveStdioPaths -- i.e. RunWitness entry, before
        // THIS run's dir is created -- never at exit: the caller reads
        // stdoutPath/stderrPath AFTER RunWitness returns (pinned by the
        // "stdout is captured to a file" unit test), so this run's own files
        // must survive past the call. A sibling <pid>-<n> dir is stale when
        // <pid> names a process that is no longer alive; a live pid --
        // including this process's own earlier dirs -- is never touched.
        // Swallows all errors: this is hygiene, never load-bearing.
        void SweepStaleStdioDirs(const std::filesystem::path& stdioRoot)
        {
            std::error_code ec;
            std::filesystem::directory_iterator it(stdioRoot, ec);
            if (ec)
                return;
            const std::filesystem::directory_iterator end;
            for (; it != end; it.increment(ec))
            {
                if (ec)
                    return;
                std::error_code dec;
                if (!it->is_directory(dec) || dec)
                    continue;

                const std::wstring name = it->path().filename().wstring();
                const std::size_t dash = name.find(L'-');
                if (dash == std::wstring::npos)
                    continue;

                DWORD pid = 0;
                for (wchar_t c : name.substr(0, dash))
                {
                    if (c < L'0' || c > L'9')
                    {
                        pid = 0;
                        break;
                    }
                    pid = pid * 10 + static_cast<DWORD>(c - L'0');
                }
                if (pid == 0)
                    continue;   // unparsed name -- never guess, never touch

                bool alive = true;
                HANDLE proc = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                if (!proc)
                {
                    alive = false;
                }
                else
                {
                    DWORD code = 0;
                    if (::GetExitCodeProcess(proc, &code) && code != STILL_ACTIVE)
                        alive = false;
                    ::CloseHandle(proc);
                }

                if (!alive)
                {
                    std::error_code rec;
                    std::filesystem::remove_all(it->path(), rec);
                }
            }
        }

        // stdout/stderr land beside reportPath (same directory, named off its
        // stem) when a report path was given, so a scenario's three artifacts
        // sit together; otherwise a fresh, uniquely-named temp directory --
        // multiple report-less runs in the same process must not collide.
        std::pair<std::filesystem::path, std::filesystem::path> ResolveStdioPaths(
            const WitnessInvocation& inv)
        {
            std::error_code ec;
            if (!inv.reportPath.empty())
            {
                const std::filesystem::path dir = inv.reportPath.parent_path();
                if (!dir.empty())
                    std::filesystem::create_directories(dir, ec);
                const std::wstring stem = inv.reportPath.stem().wstring();
                return { dir / (stem + L".stdout.txt"), dir / (stem + L".stderr.txt") };
            }

            const std::filesystem::path stdioRoot =
                std::filesystem::temp_directory_path() / "arcane-witness-stdio";
            SweepStaleStdioDirs(stdioRoot);

            static std::atomic<int> s_counter{ 0 };
            const std::filesystem::path uniqueDir =
                stdioRoot /
                (std::to_wstring(::GetCurrentProcessId()) + L"-" +
                 std::to_wstring(s_counter.fetch_add(1, std::memory_order_relaxed)));
            std::filesystem::create_directories(uniqueDir, ec);
            return { uniqueDir / "stdout.txt", uniqueDir / "stderr.txt" };
        }

        HANDLE CreateInheritableLogFile(const std::filesystem::path& path)
        {
            SECURITY_ATTRIBUTES sa{};
            sa.nLength        = sizeof(sa);
            sa.bInheritHandle = TRUE;
            // FILE_SHARE_READ so the watchdog can poll this file's size with
            // an independent handle (std::filesystem::file_size) while the
            // child's inherited duplicate keeps writing to it.
            return ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        }

        HANDLE OpenInheritableInputFile(const std::filesystem::path& path)
        {
            SECURITY_ATTRIBUTES sa{};
            sa.nLength        = sizeof(sa);
            sa.bInheritHandle = TRUE;
            return ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, &sa,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        }

        std::uint64_t FileSizeOrZero(const std::filesystem::path& p)
        {
            std::error_code ec;
            const auto sz = std::filesystem::file_size(p, ec);
            return ec ? 0 : static_cast<std::uint64_t>(sz);
        }

        // Kernel + user CPU time, in 100ns units -- a monotonically
        // non-decreasing scalar cheap enough to sample every watchdog slice.
        std::uint64_t ProcessCpu100ns(HANDLE process)
        {
            FILETIME creation{}, exitT{}, kernel{}, user{};
            if (!::GetProcessTimes(process, &creation, &exitT, &kernel, &user))
                return 0;
            ULARGE_INTEGER k{}, u{};
            k.LowPart  = kernel.dwLowDateTime;
            k.HighPart = kernel.dwHighDateTime;
            u.LowPart  = user.dwLowDateTime;
            u.HighPart = user.dwHighDateTime;
            return k.QuadPart + u.QuadPart;
        }
    }
#endif

#ifndef _WIN32
    namespace
    {
        std::uint64_t FileSizeOrZero(const std::filesystem::path& p)
        {
            std::error_code ec;
            const auto sz = std::filesystem::file_size(p, ec);
            return ec ? 0 : static_cast<std::uint64_t>(sz);
        }

        // utime + stime in clock ticks (fields 14/15 of /proc/<pid>/stat) --
        // the same "monotonic CPU scalar" ProcessCpu100ns is on Windows.
        std::uint64_t ProcessCpuTicks(pid_t pid)
        {
#if defined(__APPLE__)
            // No /proc on macOS: proc_pidinfo's task times, in nanoseconds
            // (Platform::QueryProcess). Only ever compared with itself.
            return Arcane::Platform::QueryProcess(static_cast<std::uint32_t>(pid)).cpuNs;
#endif
            std::ifstream f("/proc/" + std::to_string(pid) + "/stat");
            std::string all((std::istreambuf_iterator<char>(f)), {});
            const std::size_t close = all.rfind(')');   // comm may contain spaces
            if (close == std::string::npos)
                return 0;
            std::istringstream rest(all.substr(close + 2));
            std::string field;
            std::uint64_t utime = 0, stime = 0;
            for (int i = 3; i <= 15 && rest >> field; ++i)
            {
                if (i == 14) utime = std::strtoull(field.c_str(), nullptr, 10);
                if (i == 15) stime = std::strtoull(field.c_str(), nullptr, 10);
            }
            return utime + stime;
        }

        // Same placement rule as the Windows ResolveStdioPaths: beside
        // reportPath when one is given, else a unique per-run temp dir.
        std::pair<std::filesystem::path, std::filesystem::path> ResolveStdioPaths(
            const WitnessInvocation& inv)
        {
            std::error_code ec;
            if (!inv.reportPath.empty())
            {
                const std::filesystem::path dir = inv.reportPath.parent_path();
                if (!dir.empty())
                    std::filesystem::create_directories(dir, ec);
                const std::string stem = inv.reportPath.stem().string();
                return { dir / (stem + ".stdout.txt"), dir / (stem + ".stderr.txt") };
            }
            static std::atomic<int> s_counter{ 0 };
            const std::filesystem::path uniqueDir =
                std::filesystem::temp_directory_path() / "arcane-witness-stdio" /
                (std::to_string(::getpid()) + "-" +
                 std::to_string(s_counter.fetch_add(1, std::memory_order_relaxed)));
            std::filesystem::create_directories(uniqueDir, ec);
            return { uniqueDir / "stdout.txt", uniqueDir / "stderr.txt" };
        }

        int ExitCodeOf(int status)
        {
            if (WIFEXITED(status))
                return WEXITSTATUS(status);
            if (WIFSIGNALED(status))
                return 128 + WTERMSIG(status);   // the shell's convention
            return -1;
        }
    }
#endif

    WitnessRun RunWitness(const WitnessInvocation& inv)
    {
        WitnessRun run;

#ifdef _WIN32
        const auto [stdoutPath, stderrPath] = ResolveStdioPaths(inv);
        run.stdoutPath = stdoutPath;
        run.stderrPath = stderrPath;

        HANDLE hOut = CreateInheritableLogFile(stdoutPath);
        HANDLE hErr = CreateInheritableLogFile(stderrPath);
        HANDLE hIn = inv.stdinPath.empty() ? INVALID_HANDLE_VALUE : OpenInheritableInputFile(inv.stdinPath);
        const bool haveOut = (hOut != INVALID_HANDLE_VALUE);
        const bool haveErr = (hErr != INVALID_HANDLE_VALUE);
        const bool haveIn = (hIn != INVALID_HANDLE_VALUE);

        // cmd.exe (and only cmd.exe, empirically -- other targets don't care)
        // misparses its OWN argv[0] when the command-line TEXT spells it with
        // forward slashes: "C:/Windows/System32/cmd.exe /c \"exit 3\"" comes
        // back "The syntax of the command is incorrect." and exit code 1,
        // even though the exact same command with backslashes runs "exit 3"
        // correctly. This is a property of the bytes CreateProcessW hands the
        // child (GetCommandLineW), not of file lookup -- lpApplicationName
        // resolves fine either way. make_preferred() normalises to backslash
        // before either use, so callers (this file's own tests included) can
        // still spell exePath with forward slashes.
        const std::filesystem::path exe =
            std::filesystem::path(inv.exePath).make_preferred();

        std::wstring cmdLine = QuoteArg(exe.wstring());
        for (const std::string& a : inv.args)
        {
            cmdLine.push_back(L' ');
            cmdLine += QuoteArg(Utf8ToWide(a));
        }

        STARTUPINFOW si{};
        si.cb         = sizeof(si);
        si.dwFlags   |= STARTF_USESTDHANDLES;
        si.hStdOutput = haveOut ? hOut : nullptr;
        si.hStdError  = haveErr ? hErr : nullptr;
        si.hStdInput  = haveIn ? hIn : nullptr;   // a scripted stdin, or none

        PROCESS_INFORMATION pi{};

        const auto start = std::chrono::steady_clock::now();

        // lpCommandLine must be a MUTABLE buffer (CreateProcessW may rewrite
        // it in place); cmdLine is a local std::wstring, so .data() is safe.
        const BOOL ok = ::CreateProcessW(
            exe.c_str(),
            cmdLine.data(),
            nullptr, nullptr,
            TRUE,   // inherit handles -- required for the stdout/stderr redirect
            CREATE_NO_WINDOW,
            nullptr,
            inv.workingDir.empty() ? nullptr : inv.workingDir.c_str(),
            &si, &pi);

        // Ours to close either way: the child holds its own duplicate.
        if (haveOut) ::CloseHandle(hOut);
        if (haveErr) ::CloseHandle(hErr);
        if (haveIn) ::CloseHandle(hIn);

        if (ok)
        {
            // Watchdog: 250ms slices up to hardCapMs. Each slice that does
            // NOT observe exit, sample stdout size + CPU time so that if the
            // NEXT slice is the one that hits the cap, progressingAtKill
            // reflects the final window rather than the whole run.
            constexpr DWORD kSliceMs = 250;
            std::uint64_t elapsedMs      = 0;
            std::uint64_t lastStdoutSize = 0;
            std::uint64_t lastCpu        = 0;
            bool          haveSample     = false;
            DWORD         exitCode       = 0;
            bool          exited         = false;

            for (;;)
            {
                DWORD waitMs = kSliceMs;
                if (elapsedMs + kSliceMs >= inv.hardCapMs)
                    waitMs = (inv.hardCapMs > elapsedMs)
                                 ? static_cast<DWORD>(inv.hardCapMs - elapsedMs)
                                 : 0;

                const DWORD wr = ::WaitForSingleObject(pi.hProcess, waitMs);
                elapsedMs += waitMs;

                if (wr == WAIT_OBJECT_0)
                {
                    // Checked: on the (theoretical) failure of
                    // GetExitCodeProcess, leave exited false rather than
                    // trust a spurious exitCode -- run.exitCode then falls
                    // through to its documented "unknown" default (-1).
                    exited = (::GetExitCodeProcess(pi.hProcess, &exitCode) != FALSE);
                    break;
                }

                const std::uint64_t curSize = FileSizeOrZero(run.stdoutPath);
                const std::uint64_t curCpu  = ProcessCpu100ns(pi.hProcess);
                const bool stdoutGrew  = haveSample && (curSize > lastStdoutSize);
                const bool cpuAdvanced = haveSample && (curCpu > lastCpu);
                lastStdoutSize = curSize;
                lastCpu        = curCpu;
                haveSample     = true;

                if (elapsedMs >= inv.hardCapMs)
                {
                    run.timedOut          = true;
                    run.progressingAtKill = stdoutGrew || cpuAdvanced;

                    ::TerminateProcess(pi.hProcess, 1);
                    // Arc A's unkillable-process guard: bounded wait, never
                    // hang the suite even if termination doesn't land.
                    const DWORD killWr = ::WaitForSingleObject(pi.hProcess, 3000);
                    // Checked, same reasoning as the WAIT_OBJECT_0 branch
                    // above: on failure exited stays false and run.exitCode
                    // falls through to -1 rather than reading a spurious 0.
                    if (killWr == WAIT_OBJECT_0)
                        exited = (::GetExitCodeProcess(pi.hProcess, &exitCode) != FALSE);
                    break;
                }
            }

            run.exitCode = exited ? static_cast<int>(exitCode) : -1;

            ::CloseHandle(pi.hThread);
            ::CloseHandle(pi.hProcess);
        }

        run.wallMs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start)
                .count());

        // Report load: only asked for when a path was given; found/parsed
        // are independent facts so a caller can tell "never wrote it" apart
        // from "wrote something we couldn't parse".
        if (!inv.reportPath.empty())
        {
            std::error_code ec;
            run.reportFound = std::filesystem::exists(inv.reportPath, ec);
            if (run.reportFound)
            {
                std::ifstream rf(inv.reportPath, std::ios::binary);
                nlohmann::json parsed = nlohmann::json::parse(rf, nullptr, false);
                run.reportParsed = !parsed.is_discarded();
                if (run.reportParsed)
                    run.report = std::move(parsed);
            }
        }
#else
        const auto [stdoutPath, stderrPath] = ResolveStdioPaths(inv);
        run.stdoutPath = stdoutPath;
        run.stderrPath = stderrPath;

        // Everything the child needs is prepared BEFORE fork: only
        // async-signal-safe calls run between fork and exec.
        const std::string exe = inv.exePath.string();
        std::vector<std::string> argStore;
        argStore.reserve(inv.args.size() + 1);
        argStore.push_back(exe);
        for (const std::string& a : inv.args)
            argStore.push_back(a);
        std::vector<char*> argv;
        for (std::string& a : argStore)
            argv.push_back(a.data());
        argv.push_back(nullptr);
        const std::string cwd = inv.workingDir.string();

        const int outFd = ::open(stdoutPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        const int errFd = ::open(stderrPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        const int nullFd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);

        const auto start = std::chrono::steady_clock::now();
        const pid_t pid = ::fork();
        if (pid == 0)
        {
            // dup2 clears FD_CLOEXEC on the target descriptor.
            if (nullFd >= 0) ::dup2(nullFd, STDIN_FILENO);   // the child never reads stdin
            if (outFd >= 0)  ::dup2(outFd, STDOUT_FILENO);
            if (errFd >= 0)  ::dup2(errFd, STDERR_FILENO);
            if (!cwd.empty() && ::chdir(cwd.c_str()) != 0)
                ::_exit(127);
            ::execv(exe.c_str(), argv.data());
            ::_exit(127);   // exec failed: the shell's "command not found" code
        }
        if (outFd >= 0)  ::close(outFd);
        if (errFd >= 0)  ::close(errFd);
        if (nullFd >= 0) ::close(nullFd);

        if (pid > 0)
        {
            constexpr std::uint32_t kSliceMs = 250;
            std::uint64_t elapsedMs      = 0;
            std::uint64_t lastStdoutSize = 0;
            std::uint64_t lastCpu        = 0;
            bool          haveSample     = false;
            int           status         = 0;
            bool          exited         = false;

            for (;;)
            {
                std::uint32_t waitMs = kSliceMs;
                if (elapsedMs + kSliceMs >= inv.hardCapMs)
                    waitMs = (inv.hardCapMs > elapsedMs)
                                 ? static_cast<std::uint32_t>(inv.hardCapMs - elapsedMs)
                                 : 0;

                // waitpid has no timeout: poll WNOHANG in short steps across the slice.
                const auto sliceEnd = std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMs);
                pid_t w = 0;
                for (;;)
                {
                    w = ::waitpid(pid, &status, WNOHANG);
                    if (w != 0 || std::chrono::steady_clock::now() >= sliceEnd)
                        break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                elapsedMs += waitMs;

                if (w == pid)
                {
                    exited = true;
                    break;
                }
                if (w < 0 && errno != EINTR)
                    break;   // lost the child (should not happen): exitCode stays -1

                const std::uint64_t curSize = FileSizeOrZero(run.stdoutPath);
                const std::uint64_t curCpu  = ProcessCpuTicks(pid);
                const bool stdoutGrew  = haveSample && (curSize > lastStdoutSize);
                const bool cpuAdvanced = haveSample && (curCpu > lastCpu);
                lastStdoutSize = curSize;
                lastCpu        = curCpu;
                haveSample     = true;

                if (elapsedMs >= inv.hardCapMs)
                {
                    run.timedOut          = true;
                    run.progressingAtKill = stdoutGrew || cpuAdvanced;
                    ::kill(pid, SIGKILL);
                    // Same bounded reap as the Windows half: never hang the suite.
                    for (int i = 0; i < 600 && !exited; ++i)
                    {
                        if (::waitpid(pid, &status, WNOHANG) == pid)
                            exited = true;
                        else
                            std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    }
                    break;
                }
            }
            run.exitCode = exited ? ExitCodeOf(status) : -1;
        }

        run.wallMs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start)
                .count());

        if (!inv.reportPath.empty())
        {
            std::error_code ec;
            run.reportFound = std::filesystem::exists(inv.reportPath, ec);
            if (run.reportFound)
            {
                std::ifstream rf(inv.reportPath, std::ios::binary);
                nlohmann::json parsed = nlohmann::json::parse(rf, nullptr, false);
                run.reportParsed = !parsed.is_discarded();
                if (run.reportParsed)
                    run.report = std::move(parsed);
            }
        }
#endif

        return run;
    }

    std::optional<Arcane::Verdict> GradeProcessFacts(const WitnessRun& run)
    {
        if (run.timedOut)
            return Arcane::Verdict::Errored;
        if (!run.reportFound || !run.reportParsed)
            return Arcane::Verdict::Indeterminate;
        return std::nullopt;
    }

    WitnessScratch::WitnessScratch(const std::filesystem::path& srcStagedDir,
                                    std::string scenarioName)
    {
#ifdef _WIN32
        const std::uint32_t pid = ::GetCurrentProcessId();
#else
        const std::uint32_t pid = static_cast<std::uint32_t>(::getpid());
#endif
        const std::filesystem::path root = std::filesystem::temp_directory_path() / "arcane-witness";
        m_dir = root / (scenarioName + "-" + std::to_string(pid));

        std::error_code ec;
        std::filesystem::remove_all(m_dir, ec);   // stale leftover from a prior KEPT run
        std::filesystem::create_directories(root, ec);
        std::filesystem::copy(srcStagedDir, m_dir,
                               std::filesystem::copy_options::recursive, ec);
    }

    WitnessScratch::~WitnessScratch()
    {
        if (std::uncaught_exceptions() > 0)
        {
            // Catch2 REQUIRE failures throw during unwind -- this is how a
            // failing scenario keeps its artifact instead of deleting the
            // one thing that would explain the failure.
            std::cerr << "KEPT WITNESS ARTIFACT: " << m_dir.string() << "\n";
            return;
        }

        std::error_code ec;
        std::filesystem::remove_all(m_dir, ec);
    }

    const std::filesystem::path& WitnessScratch::Dir() const
    {
        return m_dir;
    }
}
