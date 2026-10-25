#pragma once

// ServerReport: ArcaneServer's `--report` census (Core-DLL split, spec
// docs/specs/2026-09-15-core-dll-split-design.md s6). Every field is honestly
// defaulted (opened=false, loaded=false, ...) so a run that dies before a
// field is ever set reports the true "never got there" state rather than a
// zero that would read as a measured fact -- same discipline
// Arcane::VerifyReport (Source/Developer/ArcaneVerify/src/Arcane/Host/VerifyReport.hpp)
// upholds for the presentation hosts' `--report`.
//
// schemaVersion 1: this is the FIRST version of this report -- ArcaneServer
// did not exist as a real host before this task. The Servitor boundary
// VerifyReport's own header comment describes applies here too: this JSON is
// meant to be parsed WITHOUT linking the engine, so schemaVersion is a
// compatibility promise, not decoration.

#include <cstdint>
#include <Arcane/Core/Constant.hpp>
#include <string>

namespace Arcane::Server
{
    struct ServerReport
    {
        ARC_CONSTANT("file format: server report schema version; readers gate on it")
        static constexpr int kSchemaVersion = 1;

        // --- run identity ---
        std::string netMode;                       // Arcane::ToString(runtime.Mode()), "DedicatedServer" by construction
        bool        isDedicatedServerProcess = false;

        // --- project ---
        bool        projectOpened = false;
        std::string projectName;
        int         projectAbi = 0;

        // --- module ---
        std::string   modulePath;
        bool           moduleLoaded     = false;
        std::uint32_t  moduleGeneration = 0;

        // --- the tick loop ---
        std::uint64_t framesTicked = 0;
        double        fixedDt      = 0.0;
        // ExtractFrame calls this process made. 0: this host has no extract
        // stage. A future call inside the tick stores N.
        std::uint64_t extractFrameCalls = 0;

        // --- systems (Astra::SystemScheduler::Size() -- direct, not derived) ---
        std::size_t fixedUpdate = 0, update = 0, render = 0;
        bool        hasPhysics            = false;
        bool        hasPhysicsWorld       = false;   // additive (schema stays 1): the PhysicsWorld2D resource exists after the ticks
        bool        hasPropagation        = false;
        bool        hasRenderSubmission   = false;   // ALWAYS false: no ClientRuntime exists in this process, by construction

        // --- presentation (all false/absent by construction on a Core-only host) ---
        bool clientAttached             = false;
        bool clientDllLoadedAtBoot      = false;      // the exe's own imports, sampled before any module loads
        bool clientDllLoadedAfterModule = false;      // P10: the game module's own import, reported, not hidden

        // --- why the run ended ---
        std::string exitReason;   // "frames-complete" | "project-open-failed" | "module-load-failed" | ...

        // Compact JSON, `error_handler_t::replace` -- same contract as
        // VerifyReport::ToJson (Source/Developer/ArcaneVerify/src/Arcane/Host/VerifyReport.cpp):
        // a malformed byte anywhere in a string field degrades to U+FFFD
        // rather than throwing out of a caller that is often already exiting.
        [[nodiscard]] std::string ToJson() const;

        // Writes ToJson()'s bytes to `path`. false on any open/write failure;
        // the caller decides whether that is fatal.
        [[nodiscard]] bool WriteTo(const std::string& path) const;
    };
}
