#include <Arcane/Core/ModuleAttach.hpp>
#include <exception>   // std::terminate (libc++ in Dist reaches it through nothing else)
#include <algorithm>
#include <cstdint>
#include <mutex>
#include <vector>

namespace Arcane
{
    namespace
    {
        struct AttachRegistry
        {
            std::mutex mutex;
            std::vector<ModuleAttachFn> callbacks;
            Astra::TypeContext* current = nullptr;
        };
        AttachRegistry& Registry() { static AttachRegistry registry; return registry; }
    }

    void RegisterModuleAttach(const char*, ModuleAttachFn fn) noexcept
    {
        try
        {
            auto& registry = Registry();
            std::lock_guard lock(registry.mutex);
            registry.callbacks.push_back(fn);
            if (registry.current) fn(registry.current);
        }
        catch (...) { std::terminate(); }
    }

    void AttachAllModules(Astra::TypeContext* ctx) noexcept
    {
        try
        {
            auto& registry = Registry();
            std::lock_guard lock(registry.mutex);
            registry.current = ctx;
            for (auto fn : registry.callbacks) fn(ctx);
        }
        catch (...) { std::terminate(); }
    }

    bool UnregisterModuleAttach(ModuleAttachFn fn) noexcept
    {
        if (!fn)
            return false;
        auto& registry = Registry();
        std::lock_guard lock(registry.mutex);
        const auto it = std::find(registry.callbacks.begin(), registry.callbacks.end(), fn);
        if (it == registry.callbacks.end())
            return false;
        registry.callbacks.erase(it);
        return true;
    }

    std::size_t UnregisterModuleAttachInRange(const void* base, std::size_t size) noexcept
    {
        if (!base || size == 0)
            return 0;
        const auto lo = reinterpret_cast<std::uintptr_t>(base);
        const auto hi = lo + size;
        auto& registry = Registry();
        std::lock_guard lock(registry.mutex);
        const auto before = registry.callbacks.size();
        std::erase_if(registry.callbacks, [&](ModuleAttachFn fn) {
            const auto at = reinterpret_cast<std::uintptr_t>(fn);
            return at >= lo && at < hi;
        });
        return before - registry.callbacks.size();
    }
}
