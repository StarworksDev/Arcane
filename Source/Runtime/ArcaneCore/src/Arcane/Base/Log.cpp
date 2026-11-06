#include <Arcane/Base/Log.hpp>

#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/dist_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <Arcane/Base/Assert.hpp>
#include <Arcane/Base/LogFileSettings.hpp>
#include <Arcane/Core/Constant.hpp>

#include <Mosaic/Assert.hpp>
#include <Mosaic/Log.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace
{
    spdlog::level::level_enum ToSpd(Mosaic::LogLevel l) noexcept
    {
        switch (l)
        {
            case Mosaic::LogLevel::Trace:    return spdlog::level::trace;
            case Mosaic::LogLevel::Debug:    return spdlog::level::debug;
            case Mosaic::LogLevel::Info:     return spdlog::level::info;
            case Mosaic::LogLevel::Warn:     return spdlog::level::warn;
            case Mosaic::LogLevel::Error:    return spdlog::level::err;
            case Mosaic::LogLevel::Critical: return spdlog::level::critical;
            case Mosaic::LogLevel::Off:      return spdlog::level::off;
        }
        return spdlog::level::info;
    }

    // noexcept sink: forward into the engine logger. category + message are fmt
    // ARGUMENTS (literal format) so a stray {} in a message cannot fmt-inject.
    void MosaicLogSinkImpl(const Mosaic::LogRecord& r, void* /*user*/) noexcept
    {
        try
        {
            Arcane::Log::Engine()->log(
                spdlog::source_loc{r.location.file_name(),
                                   static_cast<int>(r.location.line()),
                                   r.location.function_name()},
                ToSpd(r.level), "[{}] {}", r.category, r.message);
        }
        catch (...) {}
    }
}

namespace Arcane::Log
{
    namespace
    {
        std::shared_ptr<spdlog::logger> s_engine;
        std::once_flag s_initOnce;

        // ---- Backlog ring (spec S5.6, task 4) ----------------------------
        // Fixed static storage: never allocates, never resized. g_writeIndex
        // is the total count of lines ever recorded (monotonic); the line at
        // absolute index k lives at g_lines[k % kBacklogLines]. g_frozen is
        // checked FIRST by the sink, before any formatting or ring touch, so
        // a frozen backlog costs the logging thread nothing but the load.
        char g_lines[kBacklogLines][kBacklogLineBytes];
        std::atomic<std::uint64_t> g_writeIndex{0};
        std::atomic<bool> g_frozen{false};

        // A spdlog sink that copies every formatted line it sees into the
        // ring above. Left at the default (trace) sink level -- the engine
        // logger's own level filter decides what reaches sinks at all; this
        // sink records whatever it is handed.
        class BacklogSink final : public spdlog::sinks::base_sink<std::mutex>
        {
        protected:
            void sink_it_(const spdlog::details::log_msg& msg) override
            {
                // Checked first, before formatting: a frozen backlog must not
                // be touched, and there is no reason to pay for formatting a
                // line nobody will ever read.
                if (g_frozen.load(std::memory_order_acquire))
                    return;

                spdlog::memory_buf_t formatted;
                formatter_->format(msg, formatted);
                const std::size_t n = std::min(formatted.size(), kBacklogLineBytes - 1);

                const std::uint64_t idx = g_writeIndex.fetch_add(1, std::memory_order_acq_rel);
                char* dst = g_lines[static_cast<std::size_t>(idx % kBacklogLines)];
                std::memcpy(dst, formatted.data(), n);
                dst[n] = '\0';
            }

            void flush_() override {}
        };

        // ---- Rotating file sink + bounded flush helper (task 4) ----------
        //
        // THE ATTACH POINT (final review, finding I1). The engine logger's own
        // sink vector is written EXACTLY ONCE BY THIS FILE, inside Init()'s
        // call_once, and never again: every later attach/detach here happens
        // inside s_distSink, whose add_sink/remove_sink take the dist sink's
        // own mutex. (EditorApp's Console sink still mutates the vector
        // directly -- owed: move it inside the dist sink.) That
        // matters because AttachFileSink is NOT startup-only -- Diagnostics'
        // RetargetDumpDir re-attaches the log sink when the editor switches
        // projects, on the main thread, while worker threads are logging. A
        // push_back/erase on spdlog::logger::sinks_ under a concurrent
        // logger::log() (which iterates that vector unsynchronised) is a
        // use-after-free waiting to happen; a dist sink is the vendored,
        // internally-locked way to have a mutable sink set.
        //
        // The logger therefore holds [stderr, dist]; the file sink and the
        // backlog sink live INSIDE dist. Flushing the logger flushes dist,
        // which flushes its children, so flush_on(warn) and the bounded
        // flush helper below keep working exactly as before. Sub-sinks added
        // after Init keep their own (default-pattern) formatters -- the same
        // formatting the raw push_back gave them.
        std::shared_ptr<spdlog::sinks::dist_sink_mt> s_distSink;
        std::shared_ptr<spdlog::sinks::basic_file_sink_mt> s_fileSink;
        std::shared_ptr<BacklogSink> s_backlogSink;
        std::filesystem::path s_fileSinkPath;

        // The flush helper: one lazily-started thread, parked on
        // s_flushRequestCv, that runs the actual (potentially blocking)
        // logger flush off the caller's thread. See Log.hpp's
        // FlushFileSinkBounded comment for why this exists.
        //
        // s_flushMutex is a timed_mutex (not a plain mutex), and the two
        // condvars are condition_variable_any (the general form that works
        // with any Lockable, including unique_lock<timed_mutex>): review
        // fix round 1 -- FlushFileSinkBounded's own lock acquisition must be
        // bounded by timeoutMs too, not just its post-acquisition wait, so
        // the crash thread can never block forever even if the faulting
        // thread died holding s_flushMutex mid-request.
        std::thread s_flushThread;
        std::atomic<bool> s_helperRunning{false};
        std::timed_mutex s_flushMutex;
        std::condition_variable_any s_flushRequestCv;
        std::condition_variable_any s_flushDoneCv;
        bool s_flushRequested = false;
        bool s_flushDone = true;
        bool s_helperShouldStop = false;

        void FlushHelperMain()
        {
            std::unique_lock<std::timed_mutex> lock(s_flushMutex);
            for (;;)
            {
                s_flushRequestCv.wait(lock, [] { return s_flushRequested || s_helperShouldStop; });
                if (s_helperShouldStop)
                    return;
                s_flushRequested = false;
                lock.unlock();
                if (s_engine)
                    s_engine->flush();
                lock.lock();
                s_flushDone = true;
                s_flushDoneCv.notify_all();
            }
        }

        // Off the crash path: only ever called from AttachFileSink, which is
        // ordinary attach-time code (startup, or a live project retarget) --
        // never the crash thread. The exchange makes the start idempotent,
        // so a re-attach never spawns a second helper.
        void EnsureFlushHelperStarted()
        {
            if (s_helperRunning.exchange(true, std::memory_order_acq_rel))
                return;
            s_helperShouldStop = false;
            s_flushThread = std::thread(FlushHelperMain);
        }

        void StopFlushHelper()
        {
            if (!s_helperRunning.exchange(false, std::memory_order_acq_rel))
                return;
            {
                std::lock_guard<std::timed_mutex> lock(s_flushMutex);
                s_helperShouldStop = true;
            }
            s_flushRequestCv.notify_all();
            if (s_flushThread.joinable())
                s_flushThread.join();
        }

        // Review fix round 1 (finding 1): nothing in the process calls
        // Log::Shutdown() today, so without this, a joinable s_flushThread
        // would reach ~std::thread() at static destruction and std::terminate
        // the process (exit code 3) the first time any file sink was ever
        // attached. This guard's destructor runs StopFlushHelper() first --
        // C++ guarantees statics in one translation unit destruct in the
        // reverse of their construction order, and this is declared LAST
        // among the flush-helper statics (and after s_engine), so every
        // object StopFlushHelper() touches, including s_engine for the
        // helper's final in-flight flush, is still alive when this runs.
        struct FlushHelperShutdownGuard
        {
            ~FlushHelperShutdownGuard() { StopFlushHelper(); }
        };
        FlushHelperShutdownGuard s_flushHelperShutdownGuard;

        // settings arc S2: the other modules' Mosaic level setters (Log.hpp).
        // Registration happens at module load and the set at a log.level
        // publish, never on a hot path.
        std::mutex                 s_mosaicLevelMutex;
        std::vector<MosaicLevelFn> s_mosaicLevelTargets;
        std::atomic<int>           s_mosaicLevel{ static_cast<int>(Mosaic::LogLevel::Info) };
    }

    void Init(spdlog::level::level_enum level)
    {
        // call_once: safe under concurrent first-use via the ARC_* macros.
        // The first caller's level wins; later Init() calls are no-ops.
        std::call_once(s_initOnce, [level] {
            auto existing = spdlog::get("Arcane");
            // STDERR, not stdout. Hosts use stdout as a DATA channel --
            // `--print-engine-info` prints one line of JSON the Arcane Hub parses
            // to learn the plugin ABI. Sharing the stream meant one stray ARC_WARN
            // ahead of that line would break the Hub's "read one line" contract.
            // Diagnostics belong on stderr anyway; the editor's Console panel reads
            // the Mosaic sink, so it is unaffected by which stream this uses.
            s_engine = existing ? existing : spdlog::stderr_color_mt("Arcane");
            s_engine->set_level(level);
            ARC_CONSTANT("the editor Console parses this prefix (Panels/ConsoleModel.cpp, CategoryForMessage)")
            s_engine->set_pattern("%^[%H:%M:%S.%e] [%n] [%l]%$ %v");
            // The one and only mutation of the logger's own sink vector, made
            // here under call_once -- before any other thread can reach the
            // logger, since Engine() is the only way to get it and every
            // caller funnels through this call_once. Everything attachable
            // later goes inside this dist sink instead (see its declaration).
            s_distSink = std::make_shared<spdlog::sinks::dist_sink_mt>();
            s_engine->sinks().push_back(s_distSink);
            // The remote log ring sees every line from the first one on.
            s_distSink->add_sink(MakeRingSink());
            // THIS module's (ArcaneCore.dll's) Mosaic copy -- Astra/Manifold2D code
            // running inside Core routes here; Client and the hosts keep installing
            // into their own copies via InstallMosaicSink()/InstallMosaicHandler().
            // Mosaic's g_logSink / g_assertHandler are per-module inline atomics, so
            // Core's copy is a THIRD one and nobody outside Core can reach it: the
            // inline installers in Log.hpp/Assert.hpp install into the CALLER's
            // module by construction. Mirrored here rather than called, because both
            // installers are inline for exactly that reason.
            Mosaic::SetLogSink(MosaicSink(), nullptr);
            Mosaic::SetAssertHandler(Arcane::Assert::MosaicHandler(), nullptr);
        });
    }

    void SetLevel(spdlog::level::level_enum level)
    {
        Engine()->set_level(level);
    }

    void Shutdown()
    {
        StopFlushHelper();
        if (s_distSink)
        {
            if (s_fileSink)    s_distSink->remove_sink(s_fileSink);
            if (s_backlogSink) s_distSink->remove_sink(s_backlogSink);
        }
        s_distSink.reset();
        s_fileSink.reset();
        s_backlogSink.reset();
        s_fileSinkPath.clear();
        if (s_engine)
        {
            spdlog::drop("Arcane");
            s_engine.reset();
        }
    }

    spdlog::logger* Engine()
    {
        Init();
        return s_engine.get();
    }

    Mosaic::LogSink MosaicSink() noexcept { return &MosaicLogSinkImpl; }

    void RegisterMosaicLevelTarget(MosaicLevelFn fn) noexcept
    {
        if (!fn) return;
        std::lock_guard lock(s_mosaicLevelMutex);
        if (std::find(s_mosaicLevelTargets.begin(), s_mosaicLevelTargets.end(), fn) == s_mosaicLevelTargets.end())
            s_mosaicLevelTargets.push_back(fn);
        fn(static_cast<Mosaic::LogLevel>(s_mosaicLevel.load(std::memory_order_relaxed)));
    }

    void UnregisterMosaicLevelTarget(MosaicLevelFn fn) noexcept
    {
        std::lock_guard lock(s_mosaicLevelMutex);
        std::erase(s_mosaicLevelTargets, fn);
    }

    void UnregisterMosaicLevelTargetsInRange(const void* base, std::size_t size) noexcept
    {
        if (!base || size == 0) return;
        const auto* b = static_cast<const unsigned char*>(base);
        std::lock_guard lock(s_mosaicLevelMutex);
        std::erase_if(s_mosaicLevelTargets, [b, size](MosaicLevelFn fn) {
            const auto* p = reinterpret_cast<const unsigned char*>(fn);
            return p >= b && p < b + size;
        });
    }

    void SetMosaicLevelEverywhere(Mosaic::LogLevel level) noexcept
    {
        s_mosaicLevel.store(static_cast<int>(level), std::memory_order_relaxed);
        Mosaic::SetLogLevel(level);   // ArcaneCore.dll's own copy
        std::lock_guard lock(s_mosaicLevelMutex);
        for (MosaicLevelFn fn : s_mosaicLevelTargets)
            fn(level);
    }

    Mosaic::LogLevel CoreMosaicLevel() noexcept
    {
        return Mosaic::GetLogLevel();
    }

    bool AttachFileSink(const std::filesystem::path& file)
    {
        // Deliberately checks s_engine/s_distSink directly rather than calling
        // Engine() (which would lazily Init()): attaching a file sink before
        // the engine logger exists is refused, not auto-bootstrapped.
        if (!s_engine || !s_distSink)
            return false;

        // Detach and DESTROY any previously-attached file sink first: this
        // closes its file handle. Windows refuses to rename an open file, so
        // the rename chain below must run after the handle is gone. The
        // detach goes through the dist sink's own lock, so a worker thread
        // logging through the logger at this instant is safe (finding I1).
        if (s_fileSink)
        {
            s_distSink->remove_sink(s_fileSink);
            s_fileSink.reset();
        }

        // Rotate whatever already sits at `file` (log.file.keepCount = N):
        // delete .N and any stray past it (a run with a larger N left them),
        // shift .N-1->.N .. .1->.2, then file -> .1. N = 0 deletes the file.
        const LogFileSettings fileSettings = Settings<LogFileSettings>();   // a copy: no snapshot reference kept
        std::error_code ec;
        if (std::filesystem::exists(file, ec))
        {
            const int keep = std::max(fileSettings.keepCount, 0);
            const std::filesystem::path dir = file.parent_path();
            const std::string stem = file.stem().string();
            const std::string ext = file.extension().string();
            const auto rotated = [&](int n) { return dir / (stem + "." + std::to_string(n) + ext); };

            // Every <stem>.<n><ext> with n >= max(N, 1): .N is overwritten by
            // the shift below, and anything past it is a stray. A directory
            // scan rather than probing .N+1, .N+2 ..., because strays need not
            // be contiguous. Collected first, removed after the scan. Names are
            // compared in the native encoding: a foreign file in the folder
            // whose name has no narrow spelling must not throw out of here.
            const std::filesystem::path stemPath = file.stem();
            const std::filesystem::path extPath = file.extension();
            const auto rotationIndex = [&](const std::filesystem::path& entry) {   // -1: not <stem>.<n><ext>
                const std::filesystem::path namePath = entry.filename();
                const auto& name = namePath.native();
                const auto& stemN = stemPath.native();
                const auto& extN = extPath.native();
                if (name.size() <= stemN.size() + 1 + extN.size() || name.compare(0, stemN.size(), stemN) != 0
                    || name[stemN.size()] != '.' || name.compare(name.size() - extN.size(), extN.size(), extN) != 0)
                    return -1;
                int n = 0;
                for (std::size_t i = stemN.size() + 1; i < name.size() - extN.size(); ++i)
                {
                    if (name[i] < '0' || name[i] > '9' || n > 1'000'000)
                        return -1;
                    n = n * 10 + static_cast<int>(name[i] - '0');
                }
                return n;
            };
            std::vector<std::filesystem::path> stale;
            ec.clear();
            for (std::filesystem::directory_iterator it(dir.empty() ? std::filesystem::path(".") : dir, ec), end;
                 !ec && it != end; it.increment(ec))
                if (rotationIndex(it->path()) >= std::max(keep, 1))
                    stale.push_back(it->path());
            for (const std::filesystem::path& path : stale)
            {
                ec.clear();
                std::filesystem::remove(path, ec);
            }
            for (int n = keep - 1; n >= 1; --n)
            {
                ec.clear();
                if (std::filesystem::exists(rotated(n), ec))
                {
                    ec.clear();
                    std::filesystem::rename(rotated(n), rotated(n + 1), ec);
                }
            }
            ec.clear();
            if (keep > 0)
                std::filesystem::rename(file, rotated(1), ec);
            else
                std::filesystem::remove(file, ec);
        }

        try
        {
            ec.clear();
            const std::filesystem::path dir = file.parent_path();
            if (!dir.empty())
                std::filesystem::create_directories(dir, ec);

            auto newSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(file.string(), /*truncate*/ true);
            s_distSink->add_sink(newSink);
            s_fileSink = newSink;
            s_fileSinkPath = file;

            if (!s_backlogSink)
            {
                s_backlogSink = std::make_shared<BacklogSink>();
                s_distSink->add_sink(s_backlogSink);
            }

            // log.file.flushLevel (Live; LogFileSettings.cpp re-applies it).
            s_engine->flush_on(static_cast<spdlog::level::level_enum>(std::clamp(fileSettings.flushLevel, 0, 6)));
            EnsureFlushHelperStarted();
            return true;
        }
        catch (...)
        {
            s_fileSink.reset();
            s_fileSinkPath.clear();
            return false;
        }
    }

    std::filesystem::path FileSinkPath()
    {
        return s_fileSinkPath;
    }

    void FreezeBacklog() noexcept
    {
        g_frozen.store(true, std::memory_order_release);
    }

    void ThawBacklog() noexcept
    {
        g_frozen.store(false, std::memory_order_release);
    }

    void UnfreezeBacklogForTests() noexcept
    {
        ThawBacklog();
    }

    std::size_t BacklogLineCount() noexcept
    {
        const std::uint64_t total = g_writeIndex.load(std::memory_order_acquire);
        return static_cast<std::size_t>(total < kBacklogLines ? total : kBacklogLines);
    }

    std::size_t BacklogLine(std::size_t i, std::span<char> buf) noexcept
    {
        if (buf.empty())
            return 0;

        const std::uint64_t total = g_writeIndex.load(std::memory_order_acquire);
        const std::size_t count = static_cast<std::size_t>(total < kBacklogLines ? total : kBacklogLines);
        if (i >= count)
            return 0;

        const std::uint64_t oldest = total - count;
        const char* src = g_lines[static_cast<std::size_t>((oldest + i) % kBacklogLines)];

        // No lock: a concurrent writer may be mid-copy into this exact slot.
        // Every write NUL-terminates within [0, kBacklogLineBytes), so a
        // bounded scan can never run past the buffer even on a torn read.
        std::size_t len = 0;
        while (len < kBacklogLineBytes && src[len] != '\0')
            ++len;

        const std::size_t n = std::min(len, buf.size());
        std::memcpy(buf.data(), src, n);
        return n;
    }

    bool FlushFileSinkBounded(std::uint32_t timeoutMs) noexcept
    {
        if (!s_helperRunning.load(std::memory_order_acquire) || !s_fileSink)
            return false;

        try
        {
            // Review fix round 1 (finding 3): a single deadline covers BOTH
            // the lock acquisition and the completion wait, so the whole
            // call is bounded by timeoutMs total -- not timeoutMs for each
            // step. If the faulting thread died holding s_flushMutex
            // mid-request, try_lock_until times out and returns false
            // instead of blocking forever (the old std::mutex-based
            // unique_lock construction could not do this: acquiring a plain
            // std::mutex has no timeout).
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

            std::unique_lock<std::timed_mutex> lock(s_flushMutex, std::defer_lock);
            if (!lock.try_lock_until(deadline))
                return false;

            s_flushDone = false;
            s_flushRequested = true;
            s_flushRequestCv.notify_one();
            return s_flushDoneCv.wait_until(lock, deadline, [] { return s_flushDone; });
        }
        catch (...)
        {
            return false;
        }
    }
}
