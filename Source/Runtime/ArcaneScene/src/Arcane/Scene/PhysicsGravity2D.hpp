#pragma once
#include <Arcane/Modules/ArcaneSceneApi.hpp>

// The 2D gravity a world minted for this registry starts with. Lives in Scene
// (not Engine) so the 2D physics hook and Runtime::ResolvedGravity read one
// function and Engine resolves no gravity itself.
#include <Arcane/EcsFwd.hpp>

#include <glm/vec2.hpp>

namespace Arcane
{
    // The scene-root PhysicsSettings2D component when present, else the
    // `physics.gravity` setting (its project rung, else its default (0, -9.81;
    // +Y up)). Layered on purpose: gravity is authored content, so the built-in
    // default yields to the project and the project to the per-scene component.
    [[nodiscard]] ARC_SCENE_API glm::vec2 ResolveGravity2D(const Astra::Registry& reg);
}
