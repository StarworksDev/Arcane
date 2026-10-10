// ArcaneTests/death-fixture/DeathFixtureMain.cpp -- dies on request so the
// crash path is tested as a real process (spec S10). No engine beyond Core.
//
// `--reporter` turns the hand-off on (the reporter is staged beside this exe
// by premake); `--attended` is the desk switch that lets it show a window.
//
// R18 (controller notes): Mosaic is header-only with per-module statics, so
// this exe has its OWN assert handler / log sink slots -- installing them
// right after Log::Init mirrors the hosts (ArcaneRuntime/src/main.cpp:26-28)
// exactly. Without them a fixture ARC_ASSERT would fall through Mosaic's
// default handler straight to abort(), and a later task's "assert" case
// would observe kind `terminate` instead of `assert`.
#include <Arcane/Base/Assert.hpp>
#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Base/Log.hpp>
#if defined(_WIN32)
// R37: FAST_FAIL_FATAL_APP_EXIT comes from winnt.h -- named, never hardcoded.
// Guarded like every other Win32 include in the tree; __fastfail itself is an
// MSVC intrinsic (<intrin.h>).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <intrin.h>
#endif
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>

namespace DeathFixturePureCall
{
    // A pure virtual call, made so no optimizer can remove it:
    //  - from Base's CONSTRUCTOR, where the vptr is Base's and stays so (the
    //    old destructor shape loses its vptr reset to GCC's lifetime DSE);
    //  - through a volatile-laundered `this`, so the call cannot be resolved
    //    statically;
    //  - in a NAMED namespace: inside the anonymous one the hierarchy is
    //    closed, GCC -O2 proves the only concrete type is Derived and
    //    devirtualizes the call into Derived::Pure -- a no-op, and the
    //    Release fixture exited 0.
    struct Base;
    inline Base* volatile g_pureTarget = nullptr;
    struct Base { Base() { g_pureTarget = this; g_pureTarget->Pure(); } virtual ~Base() = default; virtual void Pure() = 0; };
    struct Derived : Base { void Pure() override {} };
}

namespace
{
    volatile int g_sink = 0;
    // The frame ESCAPES (its address is published), so no compiler may turn
    // this accumulator recursion into a loop -- Clang's tail-recursion
    // elimination does exactly that to the non-escaping form at -O2, and the
    // "overflow" then spins forever instead of faulting.
    // The stack-overflow death is the point: C4717 ("recursive on all control
    // paths") describes exactly that, so it is silenced for this function only.
    volatile char* volatile g_escape = nullptr;
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4717)
#endif
    int Recurse(int depth) { volatile char pad[4096]; pad[0] = static_cast<char>(depth); g_escape = pad; g_sink += pad[0]; return Recurse(depth + 1) + 1; }
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}

int main(int argc, char** argv)
{
    std::string dir, die; int hangSeconds = 0; bool hangAtExit = false; unsigned exitSeconds = 3, hangThreshold = 2;
    bool reporter = false, attended = false, monitor = false;
    int stalls = 1, reportAt = 0; std::string reportKind;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto next = [&](std::string& out) { if (i + 1 < argc) out = argv[++i]; };
        if (a == "--dir") next(dir); else if (a == "--die") next(die);
        else if (a == "--hang") { std::string v; next(v); hangSeconds = std::atoi(v.c_str()); }
        else if (a == "--hang-at-exit") hangAtExit = true;
        else if (a == "--exit-seconds") { std::string v; next(v); exitSeconds = static_cast<unsigned>(std::atoi(v.c_str())); }
        else if (a == "--hang-seconds") { std::string v; next(v); hangThreshold = static_cast<unsigned>(std::atoi(v.c_str())); }
        else if (a == "--reporter") reporter = true;     // spawn the STAGED reporter beside this exe (tests, desk)
        else if (a == "--attended") attended = true;     // desk only: let the reporter show its window
        // Task 9 (spec s5.8): pre-launch the STAGED reporter in monitor mode.
        // It IS the reporter exe, so `--monitor` implies the spawn is allowed.
        else if (a == "--monitor") monitor = true;
        // Task 8 desk levers for the hang protocol. `--stalls N` repeats the
        // --hang sleep N times with ONE beat between (the beat is the
        // recovery the reporter's window closes on, D11, and what re-arms the
        // host's once-per-stall rule for the next one). `--report-at S KIND`
        // has a helper thread call WriteReport("KIND: fixture") S seconds in,
        // while a hang reporter is still up. R96 (fix round 1): NOT an ensure
        // -- an ensure is lightweight and never reaches the hand-off at all,
        // so it proved nothing. KIND `hang` is D12's case (written, no second
        // reporter); KIND `gpu-crash` is R95's (its own reporter, beside the
        // hang window, because the hang protocol is keyed on the kind).
        else if (a == "--stalls") { std::string v; next(v); stalls = std::atoi(v.c_str()); }
        else if (a == "--report-at") { std::string v; next(v); reportAt = std::atoi(v.c_str()); next(reportKind); }
    }
    Arcane::Log::Init(spdlog::level::info);
    Arcane::Log::InstallMosaicSink();
    Arcane::Assert::InstallMosaicHandler();
    Arcane::Diagnostics::Config cfg;
    cfg.appName = "DeathFixture"; cfg.dumpDir = dir; cfg.unattended = !attended; cfg.spawnReporter = reporter;
    cfg.hangSeconds = hangThreshold; cfg.exitSeconds = exitSeconds;
    if (monitor) { cfg.launchMonitor = true; cfg.spawnReporter = true; }
    Arcane::Diagnostics::Install(cfg);
    // R32: WARN, not INFO -- WARN is the file sink's flush_on level, so this
    // line is ON DISK before a `--die fastfail` takes the process with no
    // unwinding at all. The monitor's log-tail assertion reads it back.
    ARC_WARN("death fixture: mode {}", die);

    // `volatile`: a store through a CONSTANT null is UB the optimizer may
    // replace with a trap instruction (SIGILL, not an access violation).
    if (die == "av")                { int* volatile p = nullptr; *p = 1; }
    else if (die == "assert")       { ARC_ASSERT(false, "fixture assert"); }
    // NOT `return 0` here: an ensure is the one mode that SURVIVES, so it must
    // leave by the ordinary exit below -- which calls Diagnostics::Shutdown().
    // Returning straight out of main skipped it and left the watchdog's
    // std::thread joinable at static destruction, whose destructor calls
    // std::terminate: the "survivable" mode died (and, before task 7's
    // handlers existed, wedged unkillably on the CRT's abort box). Task 8
    // removed the trap itself -- the watchdog is a raw thread an atexit hook
    // stops, which is what lets the `--hang` mode below return from main --
    // but going out through Shutdown() is still the shape a host has, so this
    // mode keeps exercising it.
    else if (die == "ensure")       { (void)ARC_ENSURE(false, "fixture ensure"); }
    else if (die == "terminate")    { throw std::runtime_error("fixture terminate"); }
    else if (die == "abort")        { std::abort(); }
#if defined(_WIN32)
    else if (die == "invalid-parameter") { char buf[4]; strcpy_s(buf, 4, "toolong"); }
#else
    // POSIX has no CRT invalid-parameter handler. Its counterpart is a libc
    // contract check: exactly what _FORTIFY_SOURCE compiles strcpy into,
    // spelled out so it fires at -O0 too. glibc reports the overflow and
    // abort()s -- which the crash path files as `terminate`, the honest kind
    // on this platform (CrashPathTest.cpp's family table says so). macOS libc
    // traps instead (SIGTRAP on Apple silicon), filed as `crash`.
    else if (die == "invalid-parameter")
    {
        char buf[4];
        const char* volatile src = "toolong";
        __builtin___strcpy_chk(buf, src, sizeof(buf));
        g_sink += buf[0];
    }
#endif
    else if (die == "purecall")     { DeathFixturePureCall::Derived d; (void)d; }   // Base's ctor makes a pure virtual call
    else if (die == "stack-overflow") { return Recurse(0); }
#if defined(_WIN32)
    // Task 9, the monitor's row (spec s5.8): __fastfail raises a
    // NON-CONTINUABLE exception that no handler in this process ever sees --
    // not the unhandled-exception filter, not the CRT -- and the process dies
    // with 0xC0000409 (STATUS_STACK_BUFFER_OVERRUN). The host writes nothing;
    // only the pre-launched monitor can report it.
    else if (die == "fastfail")     { __fastfail(FAST_FAIL_FATAL_APP_EXIT); }
#endif
    else if (die == "oom")
    {
        // Controller ruling R19: ONE impossible allocation, not a loop. A
        // bounded loop commits real memory per iteration (Windows reserves
        // commit charge at allocation time, not on first touch), climbing
        // toward the machine's RAM+pagefile limit before failing -- slow,
        // unpredictable, and hostile to the desk/CI machine under a 30s test
        // cap. This size can never be satisfied, so operator new fails
        // immediately with std::bad_alloc (or std::bad_array_new_length,
        // which derives from it) -- deterministic and instant.
        //
        // The size is computed into a local first rather than written
        // directly as `new char[std::numeric_limits<std::size_t>::max() / 2]`
        // -- tested empirically on this toolchain (MSVC /MDd): with the size
        // as a manifest compile-time constant, the compiler silently elides
        // the whole allocation (the C++14 new-expression elision rule) since
        // `p` is never dereferenced -- no throw, no crash, exit 0 after a
        // ~1.4s stall. Routing the same value through a runtime local
        // defeats that elision and reliably throws in ~100-200ms; volatile
        // plus the use below additionally stop the read of `p` itself from
        // being optimized away.
        std::size_t n = std::numeric_limits<std::size_t>::max() / 2;
        volatile char* p = new char[n];
        // ...and the pointer ESCAPES: Clang -O2 elides even a runtime-sized
        // new whose result is only null-tested (it can never be null, so the
        // whole allocation folds away and the fixture exits 0).
        g_escape = p;
        g_sink += p ? 1 : 0;
    }
    if (hangSeconds > 0)
    {
        std::thread reporterThread;
        if (reportAt > 0 && !reportKind.empty())
            reporterThread = std::thread([reportAt, reportKind] {
                std::this_thread::sleep_for(std::chrono::seconds(reportAt));
                (void)Arcane::Diagnostics::WriteReport((reportKind + ": fixture").c_str()); });
        // One beat ARMS the hang trigger -- until a first Heartbeat() the
        // watchdog is deliberately silent, so a host that never beats (a
        // headless tool, every other mode in this fixture) gets silence
        // rather than a spurious report hangSeconds after boot.
        // Then stop beating: the watchdog must report a hang and the process
        // must stay alive. Returning from main() WITHOUT Shutdown() is part
        // of the case -- the atexit hook is what stops the raw watchdog
        // thread now, and exit code 0 is the proof it did.
        for (int s = 0; s < (stalls > 0 ? stalls : 1); ++s)
        {
            Arcane::Diagnostics::Heartbeat();   // stall 2+: THIS beat is the recovery (D11)
            std::this_thread::sleep_for(std::chrono::seconds(hangSeconds));
        }
        if (reporterThread.joinable()) reporterThread.join();
        return 0;
    }
    if (hangAtExit)
    {
        // The exit sentinel's case (spec S5.7): the host asks to quit and then
        // never gets out. Nothing here ever calls Shutdown(), so the watchdog
        // is still armed and its deadline is the ONLY thing that can end this
        // process -- exit code 12, with a "hang at exit" report on disk.
        Arcane::Diagnostics::RequestCleanExit();
        for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));   // never exits on its own
    }
    Arcane::Diagnostics::Shutdown();
    return 0;
}
