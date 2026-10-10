// The envelope-to-view model (crash window plan 2, task 6; spec §6 "Window"
// content). What the window shows and what Copy Details copies.
//
// PURE and std-only on purpose, exactly like ReporterArgs.hpp and
// SymbolizedText.hpp: this TU source-compiles into ArcaneTests
// (premake5.lua, ArcaneTests' `files` list) so the [reporter] units drive
// the wording and the thread ordering directly. That list is NOT gated on
// the target OS, so nothing here -- and nothing ReportView.cpp includes --
// may reach windows.h. LogTail.hpp/.cpp (the filesystem half) stay out of
// this header's includes; since node-page phase s8.1 they compile into the
// tests and the editor too, std-only like the rest.
#pragma once

#include "ReporterArgs.hpp"
#include "SymbolizedText.hpp"
#include <Arcane/Base/DiagEnvelope.hpp>

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane::Reporter
{
    struct ThreadView
    {
        std::string label;              // "thread 4242 (faulting)"
        std::string text;               // its frames as text (unchanged)
        std::vector<SymFrame> frames;   // the symbolized branch only; empty on the portable fallback
    };

    struct ReportView
    {
        std::string title;          // window title: "<product> -- crashed"
        std::string headline;       // "Arcane Editor crashed"
        std::string whenLine;       // "2026-09-23T10:11:12Z | phase: switch_plugin_load | build: Arcane 0.1 Debug"
        std::string reasonText;     // the envelope's reason, or "(no reason recorded)"
        std::string injectedText;   // one line per module; "" when none; "(not scanned)" is never claimed here
        std::vector<ThreadView> threads;   // faulting first; one thread from the portable stack when no symbolization
        std::string gpuText;        // queues / fault / layers when the envelope has any; "" otherwise
        std::string logTail;        // the last 200 lines
        std::string reportFolder;   // parent of the envelope
        std::string relaunchLine;   // Args::relaunch, else envelope.commandLine
        bool        isHang = false;         // hang | gpu-stall: Keep Waiting + Terminate and Collect, Relaunch disabled
        bool        canRelaunch = false;    // !relaunchLine.empty() && !isHang
        bool        isAbnormalExit = false; // monitor-synthesized: no stack, no threads selector
    };

    // "crashed" | "stopped responding" | "the GPU stopped responding" | "the GPU device was lost"
    // | "an assertion failed" | "terminated" | "ran out of memory" | "hit a recoverable check" | "exited abnormally"
    [[nodiscard]] std::string PlainWordsKind(std::string_view kind);

    [[nodiscard]] ReportView BuildReportView(const Diag::Envelope& e, const Args& a,
                                             const Symbolized* symbolized, std::string_view logTail);

    // The last `n` lines of `text` (pure; used by the log tail and the monitor).
    [[nodiscard]] std::string LastLines(std::string_view text, std::size_t n);

    // Everything, as text: header, reason, injected, the selected thread, GPU, log tail.
    [[nodiscard]] std::string DetailsText(const ReportView& v, std::size_t threadIndex);

    // "ArcaneEditor" -> "Arcane Editor": a space after a leading "Arcane" when
    // more follows (and no space is already there); anything else unchanged.
    // The envelope carries no product name, so the editor passes this as Args::product.
    [[nodiscard]] std::string DisplayProduct(std::string_view appName);

    // DetailsText split at the reason (node-page phase s8.1): the window draws
    // the header above its well, so the well shows only the body.
    [[nodiscard]] std::string DetailsHeader(const ReportView& v);                          // headline, when, reason
    [[nodiscard]] std::string DetailsBody(const ReportView& v, std::size_t threadIndex);   // injected .. folder

    enum class CopyState { Idle, Copied, Failed };
    [[nodiscard]] std::string_view CopyButtonLabel(CopyState state) noexcept;

    // ReporterWindow's kBtn* ids, one to one (same values).
    enum class ReporterButton : int { OpenFolder = 100, Copy = 101, Close = 102, Relaunch = 103, KeepWaiting = 104, Terminate = 105 };
    // Left to right: OpenFolder, Copy, [KeepWaiting, Terminate if isHang],
    // [Relaunch if !relaunchLine.empty()], Close (always last).
    [[nodiscard]] std::vector<ReporterButton> VisibleButtons(const ReportView& v);

    // The IANA time-zone database is a C++20 library feature Apple's libc++
    // (Xcode 16) does not ship: there `TimeZone` is an incomplete stand-in,
    // the only zone a caller can pass is nullptr, and FormatLocalStamp
    // returns "" -- the documented "no zone" result (macOS port, 2026-10-07).
#if defined(__cpp_lib_chrono) && __cpp_lib_chrono >= 201907L
    #define ARC_HAS_TZDB 1
    using TimeZone = std::chrono::time_zone;
#else
    struct TimeZone;   // never defined: no tzdb in this standard library
#endif

    // "2026-09-29T16:57:12Z" -> "2026-09-29 11:57" in `zone`; "" when the
    // stamp does not parse or `zone` is null.
    [[nodiscard]] std::string FormatLocalStamp(std::string_view isoUtc, const TimeZone* zone);

    // The same stamp in the PROCESS's local zone (the C library's TZ rules,
    // localtime_r/localtime_s), for a standard library with no tzdb, so a
    // title keeps its "<headline> -- <local stamp>" shape there. "" when the
    // stamp does not parse.
    [[nodiscard]] std::string FormatSystemLocalStamp(std::string_view isoUtc);
}
