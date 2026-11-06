#pragma once
#include <Arcane/Modules/ArcaneEngineApi.hpp>

// SystemFactory: the module contract's system-factory table (Core-DLL split, spec
// docs/specs/2026-09-15-core-dll-split-design.md s4).
//
// THE SHAPE (UE/DOTS): a game module registers its systems ONCE per DLL load --
// a type, an explicit role mask and a phase -- and then EVERY Runtime in the
// process instantiates the subset its own NetMode matches. A dedicated-server
// world takes the Server-masked systems, a client world the Client-masked ones,
// and a Standalone/ListenServer world both. That is what lets one loaded module
// serve N worlds (in-process PIE, an embedded server world beside the editor's
// client world) without the module knowing how many there are.
//
// WHAT "N WORLDS ON ONE MODULE" DIVIDES, EXACTLY (spec s4/s5):
//   * COMPONENT TYPES are SHARED -- registered ONCE per DLL load (spec R1). A
//     module opens its Arcane::ComponentModule on the PRIMARY Runtime's
//     ComponentRegistry (GameModule.hpp), so every world a PluginHost serves
//     shares that ONE registry: Runtime's three-argument ctor builds a secondary
//     on the primary's, and PluginHost::AttachRuntime REFUSES a secondary with a
//     registry of its own. Without that rule a module-defined type would resolve
//     in the primary world and nowhere else -- scene load, AddComponentByTypeName
//     and a cross-world registry snapshot (Astra answers UnknownComponent) would
//     all miss.
//   * SYSTEMS are PER-WORLD, and this table is how: the same factories, a
//     different NetMode, a different instantiated set.
//   * REGISTRY DATA is PER-WORLD: its own entities and resources, its own
//     snapshot/restore across a hot reload.
//
// WHERE IT LIVES: on the ProcessContext (spec s3 / plan 1 P8) -- Core-owned, one
// per process, outliving every module image. The `instantiate` std::function is
// compiled INTO the module, so PluginHost clears a module's entries by owner key
// BEFORE its image unmaps (PluginHost::TeardownImage). The owner key is the
// image BASE, and it is mandatory: see SystemFactoryTable::Add.
//
// NET MODE IS NOT THE LAUNCH FLAG: ProcessContext::IsDedicatedServerProcess()
// describes how the PROCESS was launched; a system branches on its Runtime's
// NetMode. Conflating the two is UE's IsRunningDedicatedServer-vs-NetMode bug
// class, and RoleMaskTest.cpp pins them apart.

#include <Arcane/Core/AbiAccess.hpp>
#include <Arcane/Core/Api.hpp>
#include <Arcane/Core/NetMode.hpp>
#include <Arcane/EcsFwd.hpp>   // Arcane::SystemScheduler (declared, aliased)

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace Arcane
{
    // The engine's per-phase scheduler trio (Arcane/Sim/SystemSchedulers.hpp).
    // Declared, not included: this header is on the game-module include surface
    // and reaches PluginABI.hpp, so it stays cheap.
    struct SystemSchedulers;

    enum class RoleMask : std::uint8_t { Server = 1, Client = 2, Both = 3 };
    // Render is RETIRED (F5 plan 1 Task 5): no host runs the render scheduler
    // (sprites come from the frame extract, Arcane/Render/ExtractFrame.hpp). The
    // value stays so the ABI and older modules are unchanged; a Render-phase
    // registration is accepted, never runs, and warns once per system name.
    enum class SystemPhase : std::uint8_t { FixedUpdate, Update, Render };

    // RoleMask is a bitmask, so it needs the two operators RoleMatches is written
    // in terms of. `&` yields the raw bits rather than a RoleMask: the result of a
    // mask test is a truth value, not a role.
    [[nodiscard]] constexpr RoleMask operator|(RoleMask a, RoleMask b) noexcept
    {
        return static_cast<RoleMask>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
    }
    [[nodiscard]] constexpr std::uint8_t operator&(RoleMask a, RoleMask b) noexcept
    {
        return static_cast<std::uint8_t>(static_cast<std::uint8_t>(a) & static_cast<std::uint8_t>(b));
    }

    // The roles a world in `m` actually plays. ListenServer is BOTH -- it is a
    // server that also presents -- which is exactly why the mask and the mode are
    // separate concepts.
    [[nodiscard]] constexpr RoleMask RolesOf(NetMode m) noexcept
    {
        switch (m)
        {
            case NetMode::Standalone:      return RoleMask::Both;
            case NetMode::DedicatedServer: return RoleMask::Server;
            case NetMode::ListenServer:    return RoleMask::Both;
            case NetMode::Client:          return RoleMask::Client;
        }
        return RoleMask::Both;
    }
    [[nodiscard]] constexpr bool RoleMatches(RoleMask mask, NetMode m) noexcept
    {
        return (mask & RolesOf(m)) != 0;
    }


    struct SystemFactoryEntry
    {
        std::string  name;      // the system type's name (log + census)
        RoleMask     mask  = RoleMask::Both;
        SystemPhase  phase = SystemPhase::FixedUpdate;
        std::function<void(Arcane::SystemScheduler&)> instantiate;   // AddSystem<T>(args...) -- lives in the MODULE; cleared before unmap (P8)
        const void*  owner = nullptr;   // the registering image (PluginHost clears by owner)
    };

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4251)  // std::vector/std::string members on a dll-exported class: benign under /MD (shared CRT heap)
#endif
    class ARC_ENGINE_API SystemFactoryTable
    {
    public:
        SystemFactoryTable() = default;

        // NEITHER COPYABLE NOR MOVABLE, and that is the invariant, not tidiness:
        // there is exactly ONE of these per process, owned by the ProcessContext,
        // and PluginHost clears a module's entries out of THAT instance by owner key
        // before the image unmaps. A copy would hold the module's std::functions past
        // the unmap with nothing tracking it, and BeginOwner/EndOwner's open-owner
        // state would fork. Nothing copies or moves it today; this keeps it that way.
        SystemFactoryTable(const SystemFactoryTable&)            = delete;
        SystemFactoryTable& operator=(const SystemFactoryTable&) = delete;
        SystemFactoryTable(SystemFactoryTable&&)                 = delete;
        SystemFactoryTable& operator=(SystemFactoryTable&&)      = delete;

        // Append one entry. The `owner` field the caller passes is IGNORED -- the
        // table stamps the OPEN owner (BeginOwner below), so a module never has to
        // name, or be able to name, its own image base. An Add with NO owner open
        // is a programmer error: it asserts and DROPS the entry, because an unowned
        // entry could never be cleared before its image unmaps (see the .cpp).
        void Add(SystemFactoryEntry e);

        // Bracket a module image's Init, keyed by the image BASE. BeginOwner first
        // CLEARS whatever that owner registered before, so re-running an image's
        // Init (a secondary re-established across a primary hot reload)
        // re-registers rather than double-registers: an image's Init is its ONE
        // registration point. A NULL key opens nothing (it would be un-clearable
        // and would collide with the next module's null key).
        void BeginOwner(const void* owner) noexcept;
        void EndOwner() noexcept;

        // Drop everything an image registered. PluginHost calls this while the
        // image is still MAPPED -- the std::functions are compiled into it.
        void ClearOwner(const void* owner) noexcept;

        [[nodiscard]] std::size_t Size() const noexcept;
        [[nodiscard]] std::span<const SystemFactoryEntry> Entries() const noexcept;

        // Runs every entry whose mask matches `mode` against the right phase scheduler; returns how many.
        std::size_t InstantiateInto(SystemSchedulers& into, NetMode mode) const;

    private:
        friend struct ::Arcane::Detail::AbiManifest::Access;   // hashes these private members (Plugin/AbiManifestEngine.hpp)
        std::vector<SystemFactoryEntry> m_entries;
        const void*                     m_openOwner = nullptr;
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}
