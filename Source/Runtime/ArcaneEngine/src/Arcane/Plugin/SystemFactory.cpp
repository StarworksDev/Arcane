#include <Arcane/Plugin/SystemFactory.hpp>

#include <Arcane/Base/Assert.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Sim/SystemSchedulers.hpp>

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>

namespace Arcane
{
    namespace
    {
        const char* PhaseName(SystemPhase phase) noexcept
        {
            switch (phase)
            {
                case SystemPhase::FixedUpdate: return "FixedUpdate";
                case SystemPhase::Update:      return "Update";
                case SystemPhase::Render:      return "Render";
            }
            return "Unknown";
        }

        // THE RENDER PHASE IS RETIRED (F5 plan 1 Task 5): sprites are submitted
        // once per frame by the frame extract (Arcane/Render/ExtractFrame.hpp),
        // and no host runs the render scheduler any more. A Render-phase
        // registration is still ACCEPTED -- the enum value and the registration
        // ABI are unchanged, so an older module still loads -- but its system
        // never runs, and the author is told so ONCE per system name for the
        // process (a hot reload re-registers the same names and stays quiet).
        void WarnRetiredRenderPhase(const std::string& name)
        {
            static std::mutex mutex;
            static std::unordered_set<std::string> warned;
            {
                std::scoped_lock lock(mutex);
                if (!warned.insert(name).second)
                    return;
            }
            ARC_WARN("SystemFactories: '{}' is registered in the Render phase, which F5 retired -- "
                     "Render-phase systems never run; draw output now comes from components the "
                     "frame extract reads (Arcane/Render/ExtractFrame.hpp)", name);
        }
    }

    void SystemFactoryTable::Add(SystemFactoryEntry e)
    {
        // THE OWNER KEY IS NOT OPTIONAL. `instantiate` is a std::function compiled
        // INTO a module image; the only thing that can drop it before that image
        // unmaps is PluginHost's ClearOwner(image base), and an entry with no owner
        // matches no ClearOwner call. Keeping one would leave a callable into freed
        // code in a PROCESS-LIFETIME table, and the very next Runtime ctor would
        // call it through InstantiateModuleSystems. Registration is therefore legal
        // ONLY inside the BeginOwner/EndOwner bracket PluginHost holds across a
        // module's Init (GameModule::RegisterSystem's documented window) -- a
        // lazy RegisterSystem from OnUpdate, or any future non-Init path, is a
        // programmer error: fatal in Debug, and the entry is DROPPED rather than
        // kept as a landmine.
        ARC_ASSERT(m_openOwner != nullptr,
                   "SystemFactoryTable::Add outside an open owner bracket -- register systems from "
                   "GameModule::OnInit (the owner key is the module image, and an unowned entry can "
                   "never be cleared before that image unmaps)");
        if (!m_openOwner)
        {
            ARC_ERROR("SystemFactories: dropped the registration of '{}' -- no module image owns it "
                      "(register systems from OnInit, spec 2026-09-15 s4)", e.name);
            return;
        }

        // Role is deliberately absent from this identity. Separate Server and
        // Client declarations for one type/phase would both run in Standalone
        // and ListenServer worlds, creating the same scheduler system twice.
        const auto duplicate = std::ranges::find_if(
            m_entries,
            [this, &e](const SystemFactoryEntry& existing)
            {
                return existing.owner == m_openOwner &&
                       existing.name == e.name &&
                       existing.phase == e.phase;
            });
        if (duplicate != m_entries.end())
        {
            ARC_ERROR("SystemFactories: dropped duplicate registration of '{}' in phase {} for one module image",
                      e.name, PhaseName(e.phase));
            return;
        }

        if (e.phase == SystemPhase::Render)
            WarnRetiredRenderPhase(e.name);

        e.owner = m_openOwner;
        m_entries.push_back(std::move(e));
    }

    void SystemFactoryTable::BeginOwner(const void* owner) noexcept
    {
        // A null key would be un-clearable AND would collide with the next module's
        // null key (purging the wrong entries). PluginHost refuses registration for
        // an image with no known range rather than calling this; the guard makes the
        // table safe on its own terms.
        if (!owner)
        {
            m_openOwner = nullptr;
            return;
        }
        ClearOwner(owner);
        m_openOwner = owner;
    }

    void SystemFactoryTable::EndOwner() noexcept
    {
        m_openOwner = nullptr;
    }

    void SystemFactoryTable::ClearOwner(const void* owner) noexcept
    {
        std::erase_if(m_entries, [owner](const SystemFactoryEntry& e) { return e.owner == owner; });
    }

    std::size_t SystemFactoryTable::Size() const noexcept { return m_entries.size(); }

    std::span<const SystemFactoryEntry> SystemFactoryTable::Entries() const noexcept
    {
        return std::span<const SystemFactoryEntry>(m_entries.data(), m_entries.size());
    }

    std::size_t SystemFactoryTable::InstantiateInto(SystemSchedulers& into, NetMode mode) const
    {
        std::size_t n = 0;
        for (const SystemFactoryEntry& e : m_entries)
        {
            if (!RoleMatches(e.mask, mode) || !e.instantiate)
                continue;
            switch (e.phase)
            {
                case SystemPhase::FixedUpdate: e.instantiate(into.fixedUpdate); break;
                case SystemPhase::Update:      e.instantiate(into.update);      break;
                case SystemPhase::Render:      e.instantiate(into.render);      break;
            }
            ++n;
        }
        if (n != 0)
            ARC_TRACE("SystemFactories: instantiated {} module system(s) for NetMode {}", n, ToString(mode));
        return n;
    }
}
