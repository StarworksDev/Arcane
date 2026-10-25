#pragma once
#include <Arcane/Core/Api.hpp>

#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Core/WorkScheduler.hpp>
#include <Astra/Registry/Registry.hpp>
#include <Astra/System/SystemScheduler.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

// The sim hook (3D-readiness spec 3.1, Plan A Task 8). Engine no longer names a
// physics module: each physics module records ONE SimHook at image load (the
// same shape as ModuleAttach, NOT SystemFactoryTable -- that table drops entries
// while no owner is open) and Runtime calls every hook in `order`. A hook owns
// its world, its gravity and its interpolation buffer; Engine passes none.
//
// HOOK MODULES ARE HOST-RESIDENT (Task 8 fix round 1). ProcessContext::Create
// seals the list (SealSimHooks). A hook module the HOST links has registered by
// then: its registrar runs at image load, before main. A module that only a game
// plugin imports maps later, after the seal, and is refused. A hook leaves
// products in a live Runtime that no list purge can recall (the systems
// InstallSystems adds, the component descriptors RegisterComponents
// instantiates from the hook's image), so a plugin-loaded hook module would
// leave them dangling when it unmaps with the plugin. After the seal the list
// only shrinks: through the registrar's own RAII unregister, and through
// UnregisterSimHooksInRange, the fallback net PluginHost runs before it unmaps
// an image (the ComponentRegistry::UnregisterModuleRange shape).
namespace Arcane
{
    struct SimHook
    {
        std::uint32_t order = 0; // ascending; RegisterComponents runs in this order, so component ids do not depend on DLL load order
        void (*Prewarm)() noexcept = nullptr;
        void (*RegisterComponents)(Astra::ComponentRegistry&) noexcept = nullptr;
        void (*InstallSystems)(Astra::SystemScheduler& fixed, float fixedDt) noexcept = nullptr;
        void (*FrameBegin)(Astra::Registry&) noexcept = nullptr;
        void (*EnsureWorld)(Astra::Registry&, Mosaic::IWorkScheduler* scheduler) noexcept = nullptr;
        void (*EditPass)(Astra::Registry&, float fixedDt) noexcept = nullptr;
        void (*ResetWorld)(Astra::Registry&) noexcept = nullptr;
        void (*ReplaceFixedRate)(Astra::SystemScheduler& fixed, float fixedDt) noexcept = nullptr;
        // True when this hook's world resource is live in `reg`. Lets a host that
        // links no solver header (ArcaneServer's witness) ask without naming the type.
        bool (*HasWorld)(const Astra::Registry&) noexcept = nullptr;
    };

    // Inserts in ascending `order`; equal orders keep registration order. After
    // SealSimHooks it refuses: one ARC_ERROR, the list is unchanged, false.
    [[nodiscard]] ARC_CORE_API bool RegisterSimHook(SimHook hook) noexcept;
    // Removes the first entry equal to `hook` (every field). True when one was
    // removed. A registrar's destructor passes the hook it registered.
    ARC_CORE_API bool UnregisterSimHook(const SimHook& hook) noexcept;
    // Fallback net: drops every hook with a non-null function pointer inside
    // [base, base + size) and returns how many. PluginHost calls it while that
    // image is still mapped (TeardownImage, EndLoadSession).
    ARC_CORE_API std::size_t UnregisterSimHooksInRange(const void* base, std::size_t size) noexcept;
    // Closes the list to registration. ProcessContext::Create calls it once the
    // host's hook modules have registered and their Prewarm has run.
    ARC_CORE_API void SealSimHooks() noexcept;
    [[nodiscard]] ARC_CORE_API bool SimHooksSealed() noexcept;
    // A view of the list. Valid until the next change; after the seal the list
    // changes only when a hook module's image leaves (see above).
    ARC_CORE_API std::span<const SimHook> SimHooks() noexcept;
    // Test seam: replaces the whole list (sorted the same way), sealed or not.
    ARC_CORE_API void ReplaceSimHooksForTest(std::span<const SimHook> hooks) noexcept;
    // Test seam: sets the seal and returns the previous state.
    ARC_CORE_API bool SetSimHooksSealedForTest(bool sealed) noexcept;
}
