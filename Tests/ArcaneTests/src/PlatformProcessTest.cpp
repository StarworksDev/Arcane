// Platform::TerminateProcess (Servitor P0 Task 16 fix round 1): a start-checked
// kill. Pinned against a real sleeper child: a wrong stamp or no stamp never
// kills it; the right stamp does.
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Platform/Process.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace
{
struct Sleeper {
    std::uint32_t pid = 0;
    std::uint64_t start = 0;
#if defined(_WIN32)
    HANDLE process = nullptr;
#endif
};

// A child that would live ~30 s on its own.
Sleeper SpawnSleeper()
{
    Sleeper s;
#if defined(_WIN32)
    wchar_t sys[MAX_PATH];
    REQUIRE(::GetSystemDirectoryW(sys, MAX_PATH) > 0);
    std::wstring cmd = std::wstring(L"\"") + sys + L"\\ping.exe\" 127.0.0.1 -n 30";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    REQUIRE(::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                             &si, &pi));
    ::CloseHandle(pi.hThread);
    s.process = pi.hProcess;
    s.pid = pi.dwProcessId;
    FILETIME created{}, exited{}, kernel{}, user{};
    REQUIRE(::GetProcessTimes(pi.hProcess, &created, &exited, &kernel, &user));
    s.start = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
#else
    std::string exe = "/bin/sleep";
    std::string arg = "30";
    char* argv[] = {exe.data(), arg.data(), nullptr};
    pid_t pid = 0;
    REQUIRE(::posix_spawn(&pid, exe.c_str(), nullptr, nullptr, argv, environ) == 0);
    s.pid = static_cast<std::uint32_t>(pid);
    for (int i = 0; i < 100 && s.start == 0; ++i) {
        s.start = Arcane::Platform::QueryProcess(s.pid).start;
        if (s.start == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(s.start != 0);
#endif
    return s;
}

// True when the sleeper exited within `budget` (and reaps it on POSIX).
bool Exited(Sleeper& s, std::chrono::milliseconds budget)
{
#if defined(_WIN32)
    return ::WaitForSingleObject(s.process, static_cast<DWORD>(budget.count())) == WAIT_OBJECT_0;
#else
    const auto deadline = std::chrono::steady_clock::now() + budget;
    for (;;) {
        int status = 0;
        if (::waitpid(static_cast<pid_t>(s.pid), &status, WNOHANG) == static_cast<pid_t>(s.pid))
            return true;
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
#endif
}

void Cleanup(Sleeper& s)
{
#if defined(_WIN32)
    ::TerminateProcess(s.process, 1);
    ::WaitForSingleObject(s.process, 5000);
    ::CloseHandle(s.process);
#else
    ::kill(static_cast<pid_t>(s.pid), SIGKILL);
    int status = 0;
    ::waitpid(static_cast<pid_t>(s.pid), &status, 0);
#endif
}
} // namespace

TEST_CASE("TerminateProcess kills only the process whose creation stamp matches", "[platform][process]")
{
    Sleeper s = SpawnSleeper();
    CHECK(Arcane::Platform::IsProcessLive(s.pid, s.start));
    CHECK_FALSE(Arcane::Platform::TerminateProcess(s.pid, s.start + 1));   // a recycled pid's stamp
    CHECK_FALSE(Arcane::Platform::TerminateProcess(s.pid, 0));             // no stamp: never a bare-pid kill
    CHECK_FALSE(Arcane::Platform::TerminateProcess(0, s.start));
    CHECK_FALSE(Exited(s, std::chrono::milliseconds(200)));
    const bool killed = Arcane::Platform::TerminateProcess(s.pid, s.start);
    CHECK(killed);
    const bool exited = Exited(s, std::chrono::milliseconds(5000));
    CHECK(exited);
    if (!killed || !exited) {
        Cleanup(s);
        return;
    }
    CHECK_FALSE(Arcane::Platform::IsProcessLive(s.pid, s.start));
    CHECK_FALSE(Arcane::Platform::TerminateProcess(s.pid, s.start));       // already gone
#if defined(_WIN32)
    ::CloseHandle(s.process);
#endif
}
