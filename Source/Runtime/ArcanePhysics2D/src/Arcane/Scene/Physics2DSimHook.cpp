// The 2D physics SimHook (3D-readiness Plan A, Task 8). Engine stops naming this
// module: the hook is recorded at image load (the ARC_MODULE_ATTACH shape, not
// SystemFactoryTable) and Runtime calls it in SimHook::order. The hook owns the
// world, its gravity (ResolveGravity2D) and the interpolation buffer.
#include <Arcane/Sim/SimHook.hpp>
#include <Arcane/Config/Bindings/Physics2DBinding.hpp>
#include <Arcane/Config/Settings.hpp>
#include <Arcane/Scene/Physics2DDetail.hpp>
#include <Arcane/Scene/Physics2DWorld.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/PhysicsGravity2D.hpp>
#include <Arcane/Scene/PhysicsSystem2D.hpp>
#include <Arcane/Scene/SceneResources.hpp>   // PhysicsInterpBuffer2D

#include <Manifold2D/Physics/PhysicsWorld.hpp>

#include <cstdlib>
#include <tuple>
#include <memory>

namespace Arcane
{
    namespace
    {
        // Test seam: a child process with ARC_TEST_NO_PHYSICS_HOOK=1 boots with no 2D hook.
        bool HookDisabledForTest() noexcept
        {
            const char* e = std::getenv("ARCANE_TEST_NO_PHYSICS_HOOK");
            return e && e[0] == '1';
        }

        void Prewarm() noexcept
        {
            (void)Astra::TypeID<PhysicsWorld2D>::Value();
        }

        void RegisterComponents(Astra::ComponentRegistry& creg) noexcept
        {
            RegisterPhysicsComponents2D(creg);
        }

        void InstallSystems(Astra::SystemScheduler& fixed, float fixedDt) noexcept
        {
            if (!fixed.HasSystem<PhysicsSystem2D>())
                std::ignore = fixed.AddSystem<PhysicsSystem2D>(fixedDt, /*stepWorld*/ true);
        }

        void FrameBegin(Astra::Registry& reg) noexcept
        {
            if (PhysicsWorld2D* res = reg.GetResource<PhysicsWorld2D>())
                res->BeginFrame();
        }

        void EnsureWorld(Astra::Registry& reg, Mosaic::IWorkScheduler* scheduler) noexcept
        {
            const glm::vec2 g = ResolveGravity2D(reg);
            PhysicsWorld2D* res = reg.GetResource<PhysicsWorld2D>();
            if (res && Detail::Physics2D::Access::Solver(*res))
            {
                const auto cur = Detail::Physics2D::Access::Solver(*res)->Gravity();
                if (static_cast<float>(cur.x) == g.x && static_cast<float>(cur.y) == g.y)
                    return;
                // Gravity changed: replace the world. Bodies re-mint from their
                // current Transforms on the next pass (PASS 1 sees every handle
                // invalid against the new world; PASS 2 self-heals).
            }
            // physics.* (settings arc S2): NextWorld -- read here, where a world is minted.
            const PhysicsWorldSettings2D& settings = Settings<PhysicsWorldSettings2D>();
            Manifold2D::Physics::WorldDef wd = Detail::Physics2D::ToWorldDef(settings);
            wd.gravityX = g.x;
            wd.gravityY = g.y;
            auto world = std::make_unique<Manifold2D::Physics::PhysicsWorld>(wd);
            // physics.parallelSolver (default OFF: the serial solver, bit-identical
            // to before). The pool is the scheduler the Runtime hands in (its
            // JobSystem outlives every registry); this hook builds no JobSystem.
            if (settings.parallelSolver)
                world->SetExecutor(scheduler);
            reg.SetResource(Detail::Physics2D::Adopt(std::move(world)));
            reg.SetResource(PhysicsInterpBuffer2D{});
        }

        void EditPass(Astra::Registry& reg, float fixedDt) noexcept
        {
            PhysicsSystem2D{ fixedDt, /*stepWorld*/ false }(reg);
        }

        void ResetWorld(Astra::Registry& reg) noexcept
        {
            reg.RemoveResource<PhysicsWorld2D>();
            reg.RemoveResource<PhysicsInterpBuffer2D>();
        }

        void ReplaceFixedRate(Astra::SystemScheduler& fixed, float fixedDt) noexcept
        {
            fixed.RemoveSystem<PhysicsSystem2D>();
            std::ignore = fixed.AddSystem<PhysicsSystem2D>(fixedDt, /*stepWorld*/ true);
        }

        bool HasWorld(const Astra::Registry& reg) noexcept
        {
            return reg.GetResource<PhysicsWorld2D>() != nullptr;
        }

        // RAII: the hook leaves with this image. A host links this module, so the
        // registration lands before ProcessContext::Create seals the list; a
        // plugin-loaded copy maps after the seal and is refused (SimHook.hpp).
        struct Physics2DSimHookRegistrar
        {
            SimHook hook;
            bool registered = false;

            Physics2DSimHookRegistrar()
            {
                if (HookDisabledForTest())
                    return;
                hook.order = 100;   // D5's 3D hook takes 200
                hook.Prewarm = &Prewarm;
                hook.RegisterComponents = &RegisterComponents;
                hook.InstallSystems = &InstallSystems;
                hook.FrameBegin = &FrameBegin;
                hook.EnsureWorld = &EnsureWorld;
                hook.EditPass = &EditPass;
                hook.ResetWorld = &ResetWorld;
                hook.ReplaceFixedRate = &ReplaceFixedRate;
                hook.HasWorld = &HasWorld;
                registered = RegisterSimHook(hook);
            }
            ~Physics2DSimHookRegistrar()
            {
                if (registered)
                    (void)UnregisterSimHook(hook);
            }
            Physics2DSimHookRegistrar(const Physics2DSimHookRegistrar&) = delete;
            Physics2DSimHookRegistrar& operator=(const Physics2DSimHookRegistrar&) = delete;
        } g_physics2DSimHookRegistrar;
    }
}
