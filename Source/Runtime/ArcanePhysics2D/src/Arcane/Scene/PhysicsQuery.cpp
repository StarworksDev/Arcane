// The 2D implementation of the solver-free physics queries declared in
// ArcaneScene's PhysicsQuery.hpp (3D-readiness Plan A, Task 6). The targets are
// not exported; a namespace-scope registrar (the ARC_MODULE_ATTACH shape, not a
// function-local static) hands them to the Scene slot when this image loads.
#include <Arcane/Scene/PhysicsQuery.hpp>
#include <Arcane/Scene/Physics2DDetail.hpp>
#include <Arcane/Scene/PhysicsDebugGeometry.hpp>
#include <Arcane/Scene/PhysicsComponents.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Config/Settings.hpp>

#include <Manifold2D/Physics/Narrowphase/NarrowphaseTrace.hpp>
#include <Manifold2D/Physics/PhysicsWorld.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

namespace Arcane
{
    namespace
    {
        namespace Phys = ::Manifold2D::Physics;

        glm::vec2 RotateVec(glm::vec2 v, float angle)
        {
            const float c = std::cos(angle);
            const float s = std::sin(angle);
            return glm::vec2(v.x * c - v.y * s, v.x * s + v.y * c);
        }

        Phys::FixtureHandle UnpackFixture(std::uint64_t packed) noexcept
        {
            Phys::FixtureHandle h;
            h.index      = static_cast<std::uint32_t>(packed >> 32);
            h.generation = static_cast<std::uint32_t>(packed);
            return h;
        }

        // The published debug.physics.* values, as MakePhysicsDebugDrawOptions
        // (Render) builds them; Render's header is not reachable from here.
        PhysicsDebugDrawOptions2D PublishedOptions()
        {
            PhysicsDebugDrawOptions2D o;
            static_cast<PhysicsDebugSettings2D&>(o)      = Settings<PhysicsDebugSettings2D>();
            static_cast<PhysicsDebugDrawSettings2D&>(o)  = Settings<PhysicsDebugDrawSettings2D>();
            static_cast<PhysicsDebugColorSettings2D&>(o) = Settings<PhysicsDebugColorSettings2D>();
            static_cast<PhysicsDebugTraceSettings2D&>(o) = Settings<PhysicsDebugTraceSettings2D>();
            return o;
        }

        PhysicsDebugGeometry FillDebug(const Astra::Registry& reg, const PhysicsDebugExtractRequest2D& request)
        {
            PhysicsDebugGeometry geo;
            if (!request.view) return geo;   // a perspective view draws no 2D overlay

            const PhysicsWorld2D* phys = reg.GetResource<PhysicsWorld2D>();
            const Phys::PhysicsWorld* solver = phys ? Detail::Physics2D::Access::Solver(*phys) : nullptr;
            if (!phys || !solver) return geo;

            std::optional<Detail::Physics2D::BodyHandle> selected;
            if (request.selected)
            {
                const auto& entities = Detail::Physics2D::Access::Entities(*phys);
                if (const auto it = entities.find(*request.selected); it != entities.end())
                    selected = it->second;
            }

            // Today's extract gate: whole-world, or outside play with a resolved body.
            const bool drawPhysics = request.physicsWholeWorld || (!request.playMode && selected.has_value());
            if (drawPhysics)
            {
                PhysicsDebugDrawOptions2D opts = PublishedOptions();
                opts.view = *request.view;
                opts.alpha = request.alpha;
                opts.interp = reg.GetResource<PhysicsInterpBuffer2D>();
                opts.velocities = false;
                opts.comMarkers = false;
                opts.orientations = false;
                opts.contacts = request.physicsWholeWorld;
                if (!request.physicsWholeWorld && selected)
                    opts.onlyBody = Detail::Physics2D::PackBody(*selected);
                GatherPhysicsDebug(*solver, opts, Settings<PhysicsDebugStyleSettings2D>(), geo);
            }

            if (request.inspectorFixtureA && request.inspectorFixtureB)
            {
                Phys::NarrowphaseTrace trace;
                (void)solver->DebugCollide(UnpackFixture(*request.inspectorFixtureA),
                                           UnpackFixture(*request.inspectorFixtureB), trace);
                // The overlay gather clears its output, so it fills a local and APPENDS
                // after the ordinary primitives (spec 3.1).
                PhysicsDebugGeometry overlay;
                GatherNarrowphaseWorldOverlay(trace, request.inspectorStepIndex, *request.view,
                    request.inspectorThickness, request.inspectorEmphasis,
                    Settings<PhysicsDebugTraceSettings2D>(), Settings<PhysicsDebugColorSettings2D>(),
                    Settings<PhysicsDebugStyleSettings2D>(), overlay);
                geo.primitives.insert(geo.primitives.end(), overlay.primitives.begin(), overlay.primitives.end());
            }
            return geo;
        }

        void FillSilhouettes(const Astra::Registry& reg, std::vector<PhysicsColliderSilhouette2D>& out)
        {
            const PhysicsWorld2D* res = reg.GetResource<PhysicsWorld2D>();
            if (!res || !Detail::Physics2D::Access::Solver(*res)) return;
            const Phys::PhysicsWorld& world = *Detail::Physics2D::Access::Solver(*res);

            // Read-only: the view only reads Collider2D / PhysicsBodyRef2D, but
            // Astra's CreateView is non-const and the slot signature is const.
            auto& mutableReg = const_cast<Astra::Registry&>(reg);
            auto colliderView = mutableReg.CreateView<const Collider2D, const PhysicsBodyRef2D>();
            colliderView.ForEach([&](Astra::Entity entity, const Collider2D& col, const PhysicsBodyRef2D& ref)
            {
                if (ref.handle == 0) return;
                const Phys::BodyHandle body = Detail::Physics2D::UnpackBody(ref.handle);
                if (!world.IsValid(body)) return;

                const Phys::Vec2 bp = world.Position(body);
                const glm::vec2  bodyPos(static_cast<float>(bp.x), static_cast<float>(bp.y));
                const float      bodyAngle = static_cast<float>(world.GetAngle(body));

                const WorldTransform* xf = std::as_const(mutableReg).GetComponent<WorldTransform>(entity);
                const float worldZ = xf ? xf->matrix[3].z : 0.0f;

                const glm::vec2 scale = ref.appliedScale;
                const float     sx    = std::abs(scale.x);
                const float     sy    = std::abs(scale.y);
                const float     sMax  = std::max(sx, sy);

                for (const Fixture2D& fx : col.fixtures)
                {
                    const glm::vec2 localScaled(fx.localPos.x * scale.x, fx.localPos.y * scale.y);
                    const glm::vec2 worldCenter  = bodyPos + RotateVec(localScaled, bodyAngle);
                    const float     fixtureAngle = bodyAngle + fx.localAngle;

                    PhysicsColliderSilhouette2D d;
                    d.entity = entity;
                    d.center = glm::vec3(worldCenter, worldZ);
                    d.angle  = fixtureAngle;

                    switch (fx.kind)
                    {
                    case ShapeKind2D::Circle:
                        d.kind   = PhysicsColliderSilhouette2D::Kind::Circle;
                        d.radius = fx.radius * sMax;
                        break;
                    case ShapeKind2D::Capsule:
                        d.kind    = PhysicsColliderSilhouette2D::Kind::Capsule;
                        d.halfLen = fx.halfLen * sx;
                        d.radius  = fx.radius  * sy;
                        break;
                    case ShapeKind2D::Aabb:
                        d.kind        = PhysicsColliderSilhouette2D::Kind::Box;
                        d.halfExtents = glm::vec2(fx.halfW * sx, fx.halfH * sy);
                        break;
                    case ShapeKind2D::Polygon:
                        // v1: no vertex data -- the fixture halfW/halfH box stands in.
                        d.kind        = PhysicsColliderSilhouette2D::Kind::Box;
                        d.halfExtents = glm::vec2(fx.halfW * sx, fx.halfH * sy);
                        break;
                    }
                    out.push_back(d);
                }
            });
        }

        // RAII, the SimHook registrar's shape: Scene holds these pointers into
        // this image, so they leave with it. After the sim-hook seal (a
        // plugin-loaded copy of this module) SetPhysicsQueryFns2D refuses.
        struct PhysicsQueryRegistrar2D
        {
            bool registered = false;
            PhysicsQueryRegistrar2D()
            {
                // The "a second 2D query registration is refused" test pins the refusal.
                registered = SetPhysicsQueryFns2D(&FillDebug, &FillSilhouettes);
            }
            ~PhysicsQueryRegistrar2D()
            {
                if (registered)
                    (void)ClearPhysicsQueryFns2D(&FillDebug, &FillSilhouettes);
            }
            PhysicsQueryRegistrar2D(const PhysicsQueryRegistrar2D&) = delete;
            PhysicsQueryRegistrar2D& operator=(const PhysicsQueryRegistrar2D&) = delete;
        } g_physicsQueryRegistrar2D;
    }
}
