#pragma once

// Engine logger (Base module): console-sink spdlog logger named "Arcane".
// Deliberately separate from Core's Logger (Util/Logger.hpp), which serves
// engine-agnostic named-category logging for library code; this is the
// engine runtime's own console logger. File sinks and per-module categories
// can grow here when the engine needs them.
// spdlog is header-only in this workspace: each module has its OWN spdlog
// registry. Consumers must reach this logger via Engine() / the ARC_*
// macros -- spdlog::get("Arcane") in another module returns null.

#include <Arcane/Core/Api.hpp>

#include <spdlog/spdlog.h>

#include <Mosaic/Log.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>
#include <Arcane/Core/Constant.hpp>

namespace Arcane
{
    // One retained log line (the remote log ring, LogRing.cpp). Separate from the
    // crash backlog below: this ring is mutexed, holds structured records, and
    // is read by the remote surface and any host.
    struct LogRecord
    {
        std::uint64_t seq = 0;     // starts at 1; never repeats in-process
        std::int64_t unixMs = 0;
        int level = 0;             // spdlog level as int
        std::string logger;
        std::string text;
    };

    // Runs on the LOGGING thread, under the ring's mutex. It must not re-enter
    // Log (no ARC_* calls, no Subscribe/Unsubscribe/ReadSince).
    using LogSinkFn = void (*)(const LogRecord& record, void* user);

    struct LogSubscription { std::uint64_t id = 0; };
}

namespace Arcane::Log
{
    ARC_CORE_API void Init(spdlog::level::level_enum level = spdlog::level::info);
    ARC_CORE_API void Shutdown();

    // Runtime level for every sink (the logger's level gates them all; no
    // sink is leveled individually). spdlog set_level is runtime-safe. Driven
    // by the `log.level` cvar Init registers (node-page phase s8.2).
    ARC_CORE_API void SetLevel(spdlog::level::level_enum level);

    // Never returns null: lazily calls Init() with defaults if needed.
    ARC_CORE_API spdlog::logger* Engine();

    // Mosaic diagnostics: the log SINK that forwards Mosaic/Manifold2D/Astra
    // records into the engine logger (Engine()). Defined in Log.cpp so it lives
    // once, in Arcane.dll, routing every module's records to one spdlog instance.
    // ARC_INTERNAL_BEGIN: the Mosaic log-sink seam is the library's own install point
    ARC_CORE_API Mosaic::LogSink MosaicSink() noexcept;

    // Mosaic's LEVEL is a per-module inline atomic too (Mosaic/Log.hpp
    // detail::g_logLevel). log.level therefore reaches Astra/Manifold2D output
    // only if EVERY module's copy is set (settings arc S2; the 2026-10-03
    // inventory bug was that Mosaic::SetLogLevel was never called). Each module
    // registers its own setter, and ApplyLogSettings calls them all.
    // Registering applies the current level at once.
    using MosaicLevelFn = void (*)(Mosaic::LogLevel level) noexcept;
    ARC_CORE_API void RegisterMosaicLevelTarget(MosaicLevelFn fn) noexcept;
    ARC_CORE_API void UnregisterMosaicLevelTarget(MosaicLevelFn fn) noexcept;
    // Drop setters whose code address lies in [base, base+size).
    // CVarRegistry::UnregisterModuleRange calls this while the image is still
    // mapped (a plugin's unload, and PluginHost's dependency images).
    ARC_CORE_API void UnregisterMosaicLevelTargetsInRange(const void* base, std::size_t size) noexcept;
    // Sets ArcaneCore.dll's own copy and every registered module's.
    ARC_CORE_API void SetMosaicLevelEverywhere(Mosaic::LogLevel level) noexcept;
    // ArcaneCore.dll's own copy. Other modules read theirs with Mosaic::GetLogLevel().
    ARC_CORE_API Mosaic::LogLevel CoreMosaicLevel() noexcept;

    // THIS module's setter. It is inline, so its address is the CALLING module's copy.
    inline void SetThisModuleMosaicLevel(Mosaic::LogLevel level) noexcept { Mosaic::SetLogLevel(level); }

    // Install the sink into the CALLING module's Mosaic storage, and register
    // that module's level setter. Inline on purpose: Mosaic's g_logSink and
    // g_logLevel are per-module inline atomics, so each module (Arcane.dll,
    // ArcaneRuntime.exe, the plugin, tests) installs into its own copy.
    inline void InstallMosaicSink() noexcept
    {
        Mosaic::SetLogSink(MosaicSink(), nullptr);
        RegisterMosaicLevelTarget(&SetThisModuleMosaicLevel);
    }
    // A module that unloads (GameModule.hpp's Shutdown) unregisters its setter first.
    inline void UninstallMosaicLevelTarget() noexcept { UnregisterMosaicLevelTarget(&SetThisModuleMosaicLevel); }
    // ARC_INTERNAL_END

    // ------------------------------------------------------------------
    // Sequenced log ring (Servitor P0 task 2). Every line the engine logger
    // emits is pushed as a LogRecord; overflow drops the oldest. Capacity is
    // the log.remote.ringLines cvar (default 4096).
    ARC_CORE_API LogSubscription Subscribe(LogSinkFn fn, void* user);
    ARC_CORE_API void Unsubscribe(LogSubscription sub);
    // The OLDEST maxLines retained records with seq > seqExclusive, in order.
    // A reader that wants only what comes next anchors on LatestSeq() first.
    ARC_CORE_API std::vector<LogRecord> ReadSince(std::uint64_t seqExclusive, std::size_t maxLines);
    // Seq of the newest record pushed so far; 0 before the first line.
    ARC_CORE_API std::uint64_t LatestSeq();
    ARC_CORE_API std::size_t RingCapacity();

    // Internal: the ring's spdlog sink, attached inside Log::Init's dist sink.
    // Not exported; used by Log.cpp only.
    std::shared_ptr<spdlog::sinks::sink> MakeRingSink();

    // ------------------------------------------------------------------
    // File sink + backlog (spec S5.6, crash window plan 1 task 4).
    //
    // Attach a rotating file sink beside the stderr one. Whatever file
    // currently sits at `file` is rotated out of the way first: delete
    // <stem>.N.log (and any stray past it), shift .N-1->.N ... .1->.2, then
    // <file> -> <stem>.1.log, N = log.file.keepCount (default 5; 0 deletes
    // the old file instead); a fresh, truncated file is then opened at
    // `file`, flushing at log.file.flushLevel (default warn). Safe to
    // call repeatedly with the same path (each call rotates again) or with a
    // different one (the old sink is simply detached first). Returns false
    // if Log::Init() has not run yet (no engine logger to attach to) or if
    // the file could not be opened.
    //
    // NOT startup-only, and deliberately so: Diagnostics::RetargetDumpDir
    // re-attaches this sink on a LIVE process when the editor switches
    // projects. The attach/detach therefore happens inside an
    // internally-locked spdlog dist sink (Log.cpp), never on the logger's own
    // sink vector, so worker threads may keep logging straight through a
    // re-attach. Concurrent AttachFileSink calls with each OTHER are still
    // the caller's problem to serialise (Diagnostics does it under its report
    // mutex); this must be called off the crash path either way -- it opens
    // files and starts the flush helper thread.
    ARC_CORE_API bool AttachFileSink(const std::filesystem::path& file);
    // The path passed to the most recent successful AttachFileSink, or an
    // empty path when no file sink is attached.
    ARC_CORE_API std::filesystem::path FileSinkPath();

    // The backlog: the last kBacklogLines formatted lines the engine logger
    // produced, ring-buffered. Backed by a fixed static array -- no
    // allocation, ever, on any of the paths below.
    ARC_CONSTANT("crash-path capacity: the static lock-free log backlog ring the crash report reads (lines)")
    inline constexpr std::size_t kBacklogLines = 512;
    ARC_CONSTANT("crash-path capacity: the static lock-free log backlog ring the crash report reads (bytes per line)")
    inline constexpr std::size_t kBacklogLineBytes = 512;

    // FreezeBacklog: a single atomic store. Safe to call from the FAULTING
    // thread (e.g. a SEH filter or signal handler) -- it takes no lock and
    // touches no memory the logger itself owns beyond the flag. Once frozen,
    // the backlog sink checks the flag before doing anything else and simply
    // declines to record further lines, so the ring stays exactly as the
    // faulting thread left it for the crash thread to read.
    ARC_CORE_API void FreezeBacklog() noexcept;
    // Undoes FreezeBacklog(). A single atomic store, same discipline as the
    // freeze. Production caller: Diagnostics' crash thread at the end of a
    // SURVIVABLE report (a hang keeps the host alive -- spec S5.4 -- so the
    // ring must start recording again, or every later line in the session is
    // lost). A FATAL report deliberately never calls it: that process is
    // already on its way down and the ring must stay exactly as the fault
    // left it.
    ARC_CORE_API void ThawBacklog() noexcept;
    // Test-only alias for ThawBacklog, kept for the tests that already name
    // it. FreezeBacklog is process-global (one static ring for the whole
    // module), so a test that freezes it must unfreeze it again.
    ARC_CORE_API void UnfreezeBacklogForTests() noexcept;
    // min(total lines ever recorded, kBacklogLines). Lock-free, heap-free.
    ARC_CORE_API std::size_t BacklogLineCount() noexcept;
    // Copies line i (0 = oldest retained) into buf, NUL-free, and returns the
    // number of bytes copied (at most min(strlen(line), buf.size())). Reads
    // the ring directly with no lock of any kind -- a line concurrently being
    // written by another thread may come back torn (part old, part new
    // content); that is an accepted tradeoff for staying lock-free on the
    // crash path. Never allocates.
    ARC_CORE_API std::size_t BacklogLine(std::size_t i, std::span<char> buf) noexcept;

    // FlushFileSinkBounded: there must be NO thread creation on the crash
    // path, yet spdlog's file sink can only be flushed by a thread that is
    // willing to take its internal mutex -- one the faulting thread might
    // already hold mid-write. The fix is to never do that flush from the
    // caller's own thread. The first successful AttachFileSink() call (which
    // never happens on the crash path -- it happens during normal startup,
    // long before any crash) lazily starts one dedicated helper thread that
    // parks on a condition variable, waiting to be asked to flush. Calling
    // FlushFileSinkBounded computes a single deadline (now + timeoutMs) that
    // covers the WHOLE call: it takes the shared mutex with a bounded
    // try_lock_until (never an unbounded lock -- review fix round 1, finding
    // 3: even acquiring that mutex must not be able to block forever), then
    // signals the helper and waits on a SEPARATE completion condition
    // variable up to that same deadline. It returns true only if the helper
    // reports completion in time, and false on any timeout -- including
    // acquiring the mutex, or the helper itself being stuck on a mutex the
    // dying faulting thread held (in that case the helper thread leaks, but
    // the process is already on its way down). Returns false immediately,
    // with no wait, if no helper thread was ever started (AttachFileSink
    // never succeeded) or no file sink is attached. The caller-side
    // wait/signal uses only pre-constructed synchronization primitives and
    // never allocates. Log::Shutdown() stops and joins the helper thread;
    // a static-destruction guard in Log.cpp also calls it at process exit
    // so a joinable helper thread never reaches ~std::thread() (which would
    // std::terminate the process) even when nothing ever calls Shutdown().
    ARC_CORE_API bool FlushFileSinkBounded(std::uint32_t timeoutMs) noexcept;
}

#define ARC_TRACE(...)    ::Arcane::Log::Engine()->trace(__VA_ARGS__)
#define ARC_DEBUG(...)    ::Arcane::Log::Engine()->debug(__VA_ARGS__)
#define ARC_INFO(...)     ::Arcane::Log::Engine()->info(__VA_ARGS__)
#define ARC_WARN(...)     ::Arcane::Log::Engine()->warn(__VA_ARGS__)
#define ARC_ERROR(...)    ::Arcane::Log::Engine()->error(__VA_ARGS__)
#define ARC_CRITICAL(...) ::Arcane::Log::Engine()->critical(__VA_ARGS__)
