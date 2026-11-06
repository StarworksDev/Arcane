#include <Arcane/Scene/PhysicsGravity2D.hpp>

#include <Arcane/Config/Bindings/Physics2DBinding.hpp>
#include <Arcane/Config/Config.hpp>
#include <Arcane/Ecs.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneResources.hpp>   // SceneRoot

#include <utility>

namespace Arcane
{
    glm::vec2 ResolveGravity2D(const Astra::Registry& reg)
    {
        const CVarVec2 p = Settings<Arcane::PhysicsWorldSettings2D>().gravity;
        glm::vec2 g{p.x, p.y};
        if (const SceneRoot* sr = reg.GetResource<SceneRoot>())
            if (const Arcane::PhysicsSettings2D* ps = reg.GetComponent<Arcane::PhysicsSettings2D>(sr->entity))
                g = ps->gravity;
        return g;
    }
}
