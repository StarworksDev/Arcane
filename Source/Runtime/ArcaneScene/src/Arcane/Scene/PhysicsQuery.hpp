#pragma once
#include <Arcane/Modules/ArcaneSceneApi.hpp>

// Solver-free 2D physics queries (3D-readiness Plan A, Task 6). Render asks for
// debug geometry and collider silhouettes through these forwarders; the 2D
// solver module registers the implementations at image load. Nothing here
// names a solver type, and packed ids are uint64. The suffix names the
// dimension: a future 3D set sits beside this one and never calls the 2D setter.

// Not Ecs.hpp: Render's debug-draw TUs must not preprocess Registry.hpp
// (RenderRegistryIncludeTest). Only the forward declaration and Entity.
#include <Arcane/EcsFwd.hpp>
#include <Astra/Entity/Entity.hpp>
#include <Arcane/Scene/ViewTransform.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace Arcane
{
    struct PhysicsDebugExtractRequest2D
    {
        glm::uvec2 viewport{0u, 0u};
        std::optional<Affine2D> view;
        float alpha = 0.0f;
        std::optional<Astra::Entity> selected;
        bool physicsWholeWorld = false;
        bool playMode = false;
        std::optional<std::uint64_t> inspectorFixtureA;
        std::optional<std::uint64_t> inspectorFixtureB;
        int inspectorStepIndex = -1;
        std::optional<float> inspectorThickness;
        std::optional<float> inspectorEmphasis;
    };

    struct PhysicsColliderSilhouette2D
    {
        Astra::Entity entity{};
        enum class Kind : std::uint8_t { Circle, Capsule, Box };
        Kind kind = Kind::Circle;
        glm::vec3 center{0.0f};
        glm::vec2 halfExtents{0.0f};
        float radius = 0.0f;
        float halfLen = 0.0f;
        float angle = 0.0f;
    };

    struct PhysicsDebugPrimitive
    {
        enum class Kind { Line, Circle } kind = Kind::Line;
        glm::vec2 a{};
        glm::vec2 b{};
        float size = 0.0f;
        glm::vec4 color{};
    };

    class PhysicsDebugGeometry
    {
    public:
        void Line(glm::vec2 a, glm::vec2 b, float thickness, glm::vec4 color)
        { primitives.push_back({PhysicsDebugPrimitive::Kind::Line, a, b, thickness, color}); }
        void Circle(glm::vec2 center, float radius, glm::vec4 color)
        { primitives.push_back({PhysicsDebugPrimitive::Kind::Circle, center, {}, radius, color}); }
        std::vector<PhysicsDebugPrimitive> primitives;
    };

    using PhysicsDebugFillFn2D =
        PhysicsDebugGeometry (*)(const Astra::Registry&, const PhysicsDebugExtractRequest2D&);
    using PhysicsSilhouetteFillFn2D =
        void (*)(const Astra::Registry&, std::vector<PhysicsColliderSilhouette2D>&);

    // Stores the pair. True when both slots were empty or already hold this
    // pair. A different second registration is refused: the first pair stays,
    // one error is logged, and the result is false.
    // Also refused after the sim-hook seal (Sim/SimHook.hpp): the 2D module is
    // host-linked, so a registration after ProcessContext::Create is a
    // plugin-loaded copy whose pointers would leave with the plugin.
    [[nodiscard]] ARC_SCENE_API bool SetPhysicsQueryFns2D(PhysicsDebugFillFn2D debug, PhysicsSilhouetteFillFn2D silhouettes) noexcept;
    // Empties both slots when they hold exactly this pair (the registrar's
    // destructor, before its image unmaps). True when it cleared them.
    ARC_SCENE_API bool ClearPhysicsQueryFns2D(PhysicsDebugFillFn2D debug, PhysicsSilhouetteFillFn2D silhouettes) noexcept;
    // Fallback net (Task 8 fix round 2): empties both slots when either holds a
    // function inside [base, base + size). True when it cleared them. Plugin::Load
    // runs it over a dependency image a refused load mapped, before the unmap.
    ARC_SCENE_API bool ClearPhysicsQueryFns2DInRange(const void* base, std::size_t size) noexcept;
    ARC_SCENE_API PhysicsDebugGeometry FillPhysicsDebug2D(const Astra::Registry& reg, const PhysicsDebugExtractRequest2D& request);
    ARC_SCENE_API void FillColliderSilhouettes2D(const Astra::Registry& reg, std::vector<PhysicsColliderSilhouette2D>& out);
}
