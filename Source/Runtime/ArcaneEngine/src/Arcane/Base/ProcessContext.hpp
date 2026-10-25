#pragma once
#include <Arcane/Modules/ArcaneEngineApi.hpp>
// ProcessContext: the ONE per-process object (spec 2026-09-15 s3). Owns (or adopts)
// the Astra TypeContext every Runtime and every module imports, carries the
// launch flag (s4 field 1 -- systems NEVER branch on it; they branch on a
// Runtime's NetMode), and the system-factory table a game module registers into
// once per DLL load (SystemFactories() below -- Core-owned, so it outlives every
// module image, and PluginHost clears a module's entries before its image unmaps).
// Created exactly once by the host (ArcaneServer, RuntimeApp, EditorApp,
// ArcaneTests); a second Create() is a refusal.
//
// A consumer that never builds a Runtime -- arcbuild, arccook -- creates no
// ProcessContext and touches no ComponentRegistry; this class documents that
// limit rather than enforcing it (there is nothing to enforce against).
#include <Arcane/Core/AbiAccess.hpp>
#include <Arcane/Core/Api.hpp>
#include <Arcane/EcsFwd.hpp>                 // Arcane::TypeContext (declared, aliased)
#include <Arcane/Plugin/SystemFactory.hpp>   // SystemFactoryTable (a by-value member)
#include <cstdint>
#include <memory>
#include <mutex>
namespace Arcane
{
    class JobSystem;   // Arcane/Jobs/JobSystem.hpp
    struct ProcessContextDesc
    {
        bool                 isDedicatedServerProcess = false;
        Arcane::TypeContext* externalTypeContext      = nullptr;
    };
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4251)  // unique_ptr/SystemFactoryTable members on a dll-exported class: benign under /MD (shared CRT heap)
#endif
    class ARC_ENGINE_API ProcessContext
    {
    public:
        [[nodiscard]] static std::unique_ptr<ProcessContext> Create(ProcessContextDesc desc);
        // DIAGNOSTICS / TEST ONLY -- ambient state, which spec s7 warns against.
        // Production code RECEIVES its ProcessContext (a Runtime holds one by
        // reference; a module reads EngineContext::process), so that the object a
        // caller acts on is the one its caller chose rather than whichever happens
        // to be installed. Reaching for this in engine or host code is the smell
        // that a reference was not threaded through; thread it.
        [[nodiscard]] static ProcessContext* Current() noexcept;
        ~ProcessContext();
        ProcessContext(const ProcessContext&) = delete;
        ProcessContext& operator=(const ProcessContext&) = delete;
        ::Arcane::TypeContext& TypeContext() noexcept { return *m_context; }
        [[nodiscard]] bool  IsDedicatedServerProcess() const noexcept { return m_dedicated; }
        // The process's ONE system-factory table (spec s4). A game module registers
        // into it from OnInit (GameModule::RegisterSystem); every Runtime built on
        // this context instantiates the subset matching its own NetMode.
        [[nodiscard]] SystemFactoryTable& SystemFactories() noexcept { return m_factories; }
        // The process's ONE job pool, shared by every Runtime on this context. Each
        // Runtime holds the returned shared_ptr; this object keeps only a weak
        // reference, so the pool lives exactly as long as the last Runtime and is
        // rebuilt (with the then-current worker count) by the next one. The first
        // caller's `workerCount` sizes the pool; a later caller while the pool is
        // alive gets the same pool and its `workerCount` is ignored. The check and
        // the mint run under one lock, so overlapping Runtime constructors cannot
        // each mint. Hosts destroy every Runtime before ~ProcessContext. Every
        // Runtime on this context ticks on the thread that built the first one
        // (enkiTS gives every unregistered thread pipe 0, so two ticking threads
        // would race on the one pool).
        [[nodiscard]] std::shared_ptr<JobSystem> AcquireJobSystem(std::uint32_t workerCount);
    private:
        friend struct ::Arcane::Detail::AbiManifest::Access;   // hashes these private members (Plugin/AbiManifestEngine.hpp)
        explicit ProcessContext(const ProcessContextDesc& desc);
        std::unique_ptr<::Arcane::TypeContext> m_owned;    // null when adopting
        ::Arcane::TypeContext*                 m_context = nullptr;
        SystemFactoryTable                     m_factories;
        bool                                   m_dedicated = false;
        bool                                   m_slotHeld  = false;
        std::mutex                             m_jobsMutex;
        std::weak_ptr<JobSystem>               m_jobs;
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}
