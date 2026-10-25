#include <Arcane/Base/ProcessContext.hpp>
#include <Arcane/Core/ModuleAttach.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Jobs/JobSystem.hpp>
#include <Arcane/Sim/SimHook.hpp>
#include <Arcane/Scene/EngineResourceTypes.hpp>   // PrewarmEngineResourceTypes (Base including Scene is within one DLL)
#include <Astra/Core/TypeContext.hpp>
#include <atomic>
namespace Arcane
{
    namespace { std::atomic<ProcessContext*> g_current{nullptr}; }

    ProcessContext::ProcessContext(const ProcessContextDesc& desc) : m_dedicated(desc.isDedicatedServerProcess)
    {
        if (desc.externalTypeContext) m_context = desc.externalTypeContext;
        else { m_owned = std::make_unique<Astra::TypeContext>(); m_context = m_owned.get(); }
    }

    std::unique_ptr<ProcessContext> ProcessContext::Create(ProcessContextDesc desc)
    {
        std::unique_ptr<ProcessContext> pc(new ProcessContext(desc));
        ProcessContext* expected = nullptr;
        if (!g_current.compare_exchange_strong(expected, pc.get()))
        {
            ARC_ERROR("ProcessContext: refused -- this process already has one (spec 2026-09-15 s3: exactly one per process; N Runtimes share it)");
            return nullptr;   // ~ProcessContext with m_slotHeld == false leaves the live slot alone
        }
        pc->m_slotHeld = true;
        // Install the context in THIS module's (ArcaneCore.dll's) per-module Astra slot,
        // Resident: Core never unmaps, so its binders are pinned (Runtime.cpp's residency note).
        Astra::SetTypeContext(pc->m_context, Astra::ModuleResidency::Resident);
        ::Arcane::AttachAllModules(pc->m_context);   // every other module image's slot (R13)
        // Sim hooks claim their resource types first (the 2D physics world), the same defence.
        for (const SimHook& hook : SimHooks())
            if (hook.Prewarm) hook.Prewarm();
        // Hook modules are host-resident: every one the host links registered at
        // image load, before this point. One that a game plugin imports maps later
        // and is refused from here on (Sim/SimHook.hpp).
        SealSimHooks();
        // With the shared context installed here and nowhere else yet, Core claims
        // first-registrar of every engine RESOURCE type (see EngineResourceTypes.hpp
        // for why that matters and why it is defence, not a fix).
        PrewarmEngineResourceTypes();
        return pc;
    }

    ProcessContext* ProcessContext::Current() noexcept { return g_current.load(); }

    std::shared_ptr<JobSystem> ProcessContext::AcquireJobSystem(std::uint32_t workerCount)
    {
        // One lock across lock() -> make_shared -> weak store: two overlapping
        // Runtime constructors cannot each see an expired weak and each mint.
        std::lock_guard<std::mutex> guard(m_jobsMutex);
        if (auto live = m_jobs.lock())
            return live;
        auto made = std::make_shared<JobSystem>(workerCount);
        m_jobs = made;
        return made;
    }

    ProcessContext::~ProcessContext()
    {
        if (!m_slotHeld)
            return;   // a REFUSED instance: never installed anywhere, nothing points at its context -- m_owned frees normally
        g_current.store(nullptr);
        // The live instance's OWNED TypeContext is LEAKED, deliberately: TypeMeta entries
        // registered by a game module hold std::function thunks compiled into that DLL, and
        // after its unload ~TypeContext would call into unmapped code (the heap-leak both hosts
        // documented at their old `new Astra::TypeContext()` sites). Adopted contexts
        // belong to their owner.
        (void)m_owned.release();
    }
}
