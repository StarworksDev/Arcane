#include <Arcane/Scene/PhysicsQuery.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Sim/SimHook.hpp>   // SimHooksSealed: the query slot shares the hook seal

#include <cstddef>
#include <cstdint>

namespace Arcane
{
    namespace
    {
        // Written only by image-load registrars; read-only once any thread runs Extract.
        PhysicsDebugFillFn2D     g_debugFill2D = nullptr;
        PhysicsSilhouetteFillFn2D g_silhouetteFill2D = nullptr;
    }

    bool SetPhysicsQueryFns2D(PhysicsDebugFillFn2D debug, PhysicsSilhouetteFillFn2D silhouettes) noexcept
    {
        // The 2D query module is a sim hook module: host-linked, registered
        // before ProcessContext::Create seals the hooks (Sim/SimHook.hpp).
        if (SimHooksSealed())
        {
            ARC_ERROR("SetPhysicsQueryFns2D: refused -- physics modules must be linked by the host, not loaded through a plugin");
            return false;
        }
        const bool empty = g_debugFill2D == nullptr && g_silhouetteFill2D == nullptr;
        if (!empty && (g_debugFill2D != debug || g_silhouetteFill2D != silhouettes))
        {
            ARC_ERROR("SetPhysicsQueryFns2D: refused -- a different 2D physics query pair is already registered");
            return false;
        }
        g_debugFill2D = debug;
        g_silhouetteFill2D = silhouettes;
        return true;
    }

    bool ClearPhysicsQueryFns2D(PhysicsDebugFillFn2D debug, PhysicsSilhouetteFillFn2D silhouettes) noexcept
    {
        if (g_debugFill2D != debug || g_silhouetteFill2D != silhouettes)
            return false;
        g_debugFill2D = nullptr;
        g_silhouetteFill2D = nullptr;
        return true;
    }

    bool ClearPhysicsQueryFns2DInRange(const void* base, std::size_t size) noexcept
    {
        if (!base || size == 0)
            return false;
        const auto lo = reinterpret_cast<std::uintptr_t>(base);
        const auto inside = [&](auto fn) {
            const auto at = reinterpret_cast<std::uintptr_t>(fn);
            return fn != nullptr && at >= lo && at - lo < size;
        };
        if (!inside(g_debugFill2D) && !inside(g_silhouetteFill2D))
            return false;
        g_debugFill2D = nullptr;
        g_silhouetteFill2D = nullptr;
        return true;
    }

    PhysicsDebugGeometry FillPhysicsDebug2D(const Astra::Registry& reg, const PhysicsDebugExtractRequest2D& request)
    {
        return g_debugFill2D ? g_debugFill2D(reg, request) : PhysicsDebugGeometry{};
    }

    void FillColliderSilhouettes2D(const Astra::Registry& reg, std::vector<PhysicsColliderSilhouette2D>& out)
    {
        if (g_silhouetteFill2D) g_silhouetteFill2D(reg, out);
    }
}
