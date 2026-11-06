#include <Arcane/Sim/SimHook.hpp>
#include <Arcane/Base/Log.hpp>

#include <algorithm>
#include <cstdint>
#include <exception>   // std::terminate
#include <mutex>
#include <vector>

namespace Arcane
{
    namespace
    {
        struct HookRegistry
        {
            std::mutex mutex;
            std::vector<SimHook> hooks;
            bool sealed = false;
        };
        HookRegistry& Hooks() { static HookRegistry registry; return registry; }

        bool OrderLess(const SimHook& a, const SimHook& b) noexcept { return a.order < b.order; }

        bool SameHook(const SimHook& a, const SimHook& b) noexcept
        {
            return a.order == b.order && a.Prewarm == b.Prewarm && a.RegisterComponents == b.RegisterComponents
                && a.InstallSystems == b.InstallSystems && a.FrameBegin == b.FrameBegin
                && a.EnsureWorld == b.EnsureWorld && a.EditPass == b.EditPass
                && a.ResetWorld == b.ResetWorld && a.ReplaceFixedRate == b.ReplaceFixedRate
                && a.HasWorld == b.HasWorld;
        }

        template <class Fn>
        bool InRange(Fn fn, std::uintptr_t lo, std::uintptr_t hi) noexcept
        {
            if (!fn) return false;
            const auto at = reinterpret_cast<std::uintptr_t>(fn);
            return at >= lo && at < hi;
        }

        bool HookInRange(const SimHook& h, std::uintptr_t lo, std::uintptr_t hi) noexcept
        {
            return InRange(h.Prewarm, lo, hi) || InRange(h.RegisterComponents, lo, hi)
                || InRange(h.InstallSystems, lo, hi) || InRange(h.FrameBegin, lo, hi)
                || InRange(h.EnsureWorld, lo, hi) || InRange(h.EditPass, lo, hi)
                || InRange(h.ResetWorld, lo, hi) || InRange(h.ReplaceFixedRate, lo, hi)
                || InRange(h.HasWorld, lo, hi);
        }
    }

    bool RegisterSimHook(SimHook hook) noexcept
    {
        try
        {
            auto& registry = Hooks();
            {
                std::lock_guard lock(registry.mutex);
                if (!registry.sealed)
                {
                    // upper_bound: an equal order lands AFTER the existing ones.
                    const auto at = std::upper_bound(registry.hooks.begin(), registry.hooks.end(), hook, OrderLess);
                    registry.hooks.insert(at, hook);
                    return true;
                }
            }
            ARC_ERROR("RegisterSimHook: refused (order {}) -- sim hook modules must be linked by the host, not loaded through a plugin",
                      hook.order);
            return false;
        }
        catch (...) { std::terminate(); }
    }

    bool UnregisterSimHook(const SimHook& hook) noexcept
    {
        auto& registry = Hooks();
        std::lock_guard lock(registry.mutex);
        const auto it = std::find_if(registry.hooks.begin(), registry.hooks.end(),
                                     [&](const SimHook& h) { return SameHook(h, hook); });
        if (it == registry.hooks.end())
            return false;
        registry.hooks.erase(it);   // erase keeps the order of the rest
        return true;
    }

    std::size_t UnregisterSimHooksInRange(const void* base, std::size_t size) noexcept
    {
        if (!base || size == 0)
            return 0;
        const auto lo = reinterpret_cast<std::uintptr_t>(base);
        const auto hi = lo + size;
        auto& registry = Hooks();
        std::lock_guard lock(registry.mutex);
        const auto before = registry.hooks.size();
        std::erase_if(registry.hooks, [&](const SimHook& h) { return HookInRange(h, lo, hi); });
        return before - registry.hooks.size();
    }

    void SealSimHooks() noexcept
    {
        auto& registry = Hooks();
        std::lock_guard lock(registry.mutex);
        registry.sealed = true;
    }

    bool SimHooksSealed() noexcept
    {
        auto& registry = Hooks();
        std::lock_guard lock(registry.mutex);
        return registry.sealed;
    }

    std::span<const SimHook> SimHooks() noexcept
    {
        auto& registry = Hooks();
        std::lock_guard lock(registry.mutex);
        return { registry.hooks.data(), registry.hooks.size() };
    }

    void ReplaceSimHooksForTest(std::span<const SimHook> hooks) noexcept
    {
        try
        {
            // Copy first: `hooks` may view the registry's own storage.
            std::vector<SimHook> next(hooks.begin(), hooks.end());
            std::stable_sort(next.begin(), next.end(), OrderLess);
            auto& registry = Hooks();
            std::lock_guard lock(registry.mutex);
            registry.hooks = std::move(next);
        }
        catch (...) { std::terminate(); }
    }

    bool SetSimHooksSealedForTest(bool sealed) noexcept
    {
        auto& registry = Hooks();
        std::lock_guard lock(registry.mutex);
        const bool previous = registry.sealed;
        registry.sealed = sealed;
        return previous;
    }
}
