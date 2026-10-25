#pragma once
#include <Arcane/Core/Api.hpp>
#include <Astra/Core/TypeContext.hpp>

#include <cstddef>

// Module attach (ruling R13). Astra's current-TypeContext slot is per module IMAGE
// by design, so with one DLL per engine module every image must be handed the
// process context or it silently runs on its own default one (different type ids).
// Each module's ARC_MODULE registers an attach callback at image load; every site
// that installs the process context calls AttachAllModules(ctx), which runs all
// callbacks and remembers ctx so a module registering later attaches at once.
namespace Arcane
{
    using ModuleAttachFn = void(*)(Astra::TypeContext*);
    ARC_CORE_API void RegisterModuleAttach(const char* moduleName, ModuleAttachFn fn) noexcept;
    ARC_CORE_API void AttachAllModules(Astra::TypeContext* ctx) noexcept;
    // Removes the first registration of `fn`; true when one was removed. The
    // ARC_MODULE_ATTACH registrar's destructor calls it, so a module image's
    // callback leaves with the image on EVERY unmap path (Task 8 fix round 2),
    // including a refused or raw Plugin::Load that mapped it as a dependency.
    ARC_CORE_API bool UnregisterModuleAttach(ModuleAttachFn fn) noexcept;
    // Fallback net (3D-readiness Plan A Task 8 fix round 1): drops every attach
    // callback whose code lies in [base, base + size) and returns how many.
    // PluginHost and Plugin::Load call it for a dependency image a plugin load
    // mapped, before that image unmaps, so the next AttachAllModules (every
    // Runtime constructor) does not call into freed code.
    ARC_CORE_API std::size_t UnregisterModuleAttachInRange(const void* base, std::size_t size) noexcept;
}

// RAII: the registrar registers at image load and unregisters at image unload
// (its static destructor runs inside FreeLibrary / dlclose, while the image is
// still mapped).
#define ARC_MODULE_ATTACH(Name)                                                                   \
    namespace {                                                                                   \
        struct ArcModuleAttachRegistrar_##Name {                                                  \
            static void Attach(Astra::TypeContext* c) {                                           \
                Astra::SetTypeContext(c, Astra::ModuleResidency::Resident);                       \
            }                                                                                     \
            ArcModuleAttachRegistrar_##Name() { ::Arcane::RegisterModuleAttach(#Name, &Attach); } \
            ~ArcModuleAttachRegistrar_##Name() { (void)::Arcane::UnregisterModuleAttach(&Attach); } \
            ArcModuleAttachRegistrar_##Name(const ArcModuleAttachRegistrar_##Name&) = delete;     \
            ArcModuleAttachRegistrar_##Name& operator=(const ArcModuleAttachRegistrar_##Name&) = delete; \
        } arcModuleAttachRegistrar_##Name;                                                        \
    }
