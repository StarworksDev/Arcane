// PhysicsSystem2D's pass body. It used to be header-only under the pre-attach
// TypeID rule; AttachAllModules makes one translation unit enough.

#include <Arcane/Modules/ArcanePhysics2DApi.hpp>
#include <Arcane/Physics2D.hpp>
#include <Arcane/Scene/Physics2DDetail.hpp>
#include <Arcane/Scene/Physics2DWorld.hpp>

#include <glm/vec2.hpp>

#include <cmath>
#include <utility>
#include <vector>

namespace Arcane
{
    ARC_PHYSICS2D_API void PhysicsSystem2D::operator()(Arcane::Registry& reg)
    {
        namespace Phys = Detail::Physics2D::Phys;

            PhysicsWorld2D* res = reg.GetResource<PhysicsWorld2D>();
            if (!res || !res->world) return;

            Phys::PhysicsWorld& world        = *res->world;
            auto&                  entityToBody  = res->entityToBody;

            // Event policy (spec s7.4, s7.5): PhysicsSystem2D is the single owner. Both
            // fixtures must opt into contact events; the hit threshold is Live.
            world.SetContactEventsRequireBoth(true);
            world.SetHitEventThreshold(static_cast<Phys::Real>(Settings<PhysicsEventSettings2D>().hitThreshold));

            // ------------------------------------------------------------------
            // PASS 1: DESTROY -- remove body rows for dead or un-physicised
            // entities, and (PAUSED passes only, spec s4.1a) for entities whose
            // RigidBody2D or Collider2D changed since the last reconcile -- an
            // author edit of shape, mass or body type -- so PASS 2 re-mints them
            // from the current components in this same pass. Stepping passes
            // skip the change criteria: PASS 4 writes velocity every step, which
            // would otherwise read as an edit. Collect first; erase after (map
            // iteration) -- and the change views are read BEFORE any erase so
            // nothing is invalidated under them.
            //
            // Implicit assumption: a handle becomes invalid ONLY through this
            // pass. The CREATE pass self-heals any stale handle by overwriting
            // the map entry when it calls AddBody for the same entity.
            // ------------------------------------------------------------------
            {
                std::vector<Arcane::Entity> toRemove;
                if (!m_stepWorld)
                {
                    reg.CreateView<const PhysicsBodyRef2D, const Collider2D, Arcane::Changed<Collider2D>, Arcane::With<RigidBody2D>>()
                        .Since(res->lastReconcile)
                        .ForEach([&](Arcane::Entity entity, const PhysicsBodyRef2D&, const Collider2D&) { toRemove.push_back(entity); });
                    reg.CreateView<const PhysicsBodyRef2D, const RigidBody2D, Arcane::Changed<RigidBody2D>, Arcane::With<Collider2D>>()
                        .Since(res->lastReconcile)
                        .ForEach([&](Arcane::Entity entity, const PhysicsBodyRef2D&, const RigidBody2D&) { toRemove.push_back(entity); });
                }
                for (auto& [entity, handle] : entityToBody)
                {
                    const bool dead       = !reg.IsValid(entity);
                    const bool noBody     = !dead && !reg.HasComponent<RigidBody2D>(entity);
                    const bool noCollider = !dead && !reg.HasComponent<Collider2D>(entity);
                    if (dead || noBody || noCollider)
                        toRemove.push_back(entity);
                }
                for (Arcane::Entity e : toRemove)
                {
                    auto it = entityToBody.find(e);
                    if (it == entityToBody.end()) continue;   // listed twice, or never minted
                    if (world.IsValid(it->second))
                    {
                        res->RetireBody(it->second);
                        world.RemoveBody(it->second);
                    }
                    entityToBody.erase(it);
                    // Clear the ref too, never leave it at the dead {index, gen}:
                    // a FRESH world (gravity re-mint, Play->Stop restore, a
                    // structural undo) reissues handles from the same sequence,
                    // so a stale one becomes SOME OTHER entity's live handle and
                    // PASS 3.5 / PASS 4 -- which trust world.IsValid alone --
                    // would then move that entity's body on this one's edits.
                    // A dead entity has no component to clear (and nothing to
                    // alias through); a live one without RigidBody2D/Collider2D
                    // may have shed PhysicsBodyRef2D with them.
                    if (!reg.IsValid(e)) continue;
                    if (PhysicsBodyRef2D* ref = reg.GetComponent<PhysicsBodyRef2D>(e))
                        ref->handle = 0;
                }
            }

            // ------------------------------------------------------------------
            // PASS 1.5: ENSURE PhysicsBodyRef2D. The Inspector can never add one
            // (ComponentCatalog structure-locks it), so an editor-authored
            // RigidBody2D + Collider2D entity would otherwise never match PASS 2's
            // view. Collected, then added -- AddComponent moves the entity
            // between archetypes, never inside a ForEach.
            // ------------------------------------------------------------------
            {
                std::vector<Arcane::Entity> missing;
                reg.CreateView<const RigidBody2D, const Collider2D, Arcane::Not<PhysicsBodyRef2D>>()
                    .ForEach([&](Arcane::Entity entity, const RigidBody2D&, const Collider2D&) { missing.push_back(entity); });
                for (Arcane::Entity e : missing)
                    reg.AddComponent<PhysicsBodyRef2D>(e, PhysicsBodyRef2D{});
            }

            // ------------------------------------------------------------------
            // PASS 2: CREATE/SYNC -- add body rows for new physics entities.
            // Iterate via a View (archetype-stable, deterministic) so order does
            // not depend on unordered_map hash/bucket layout.
            // ------------------------------------------------------------------
            {
                auto view = reg.CreateView<const RigidBody2D, const Collider2D, PhysicsBodyRef2D, const Transform>();
                view.ForEach([&](Arcane::Entity   entity,
                                 const RigidBody2D&    rb,
                                 const Collider2D&     col,
                                 PhysicsBodyRef2D& ref,
                                 const Transform& lt)
                {
                    // Skip entities that already have a tracked live handle.
                    if (ref.handle != 0 &&
                        entityToBody.count(entity) &&
                        world.IsValid(Detail::Physics2D::UnpackBody(ref.handle)))
                    {
                        return;
                    }

                    // Skip entities with no fixtures (cannot build a body) --
                    // and clear the ref on the way out, for the same reason
                    // PASS 1's erase does: an entity that reaches here with a
                    // handle holds one from a world that no longer tracks it
                    // (the fresh-world path above, where entityToBody is
                    // empty), and a fresh world will hand that same
                    // {index, gen} to the next entity it mints.
                    if (col.fixtures.empty())
                    {
                        ref.handle = 0;
                        return;
                    }

                    // ---- PRIMARY FIXTURE (fixtures[0]) ----
                    // Build the BodyDef from RigidBody2D dynamics params + fixture[0].
                    const Fixture2D& fx0 = col.fixtures[0];

                    Phys::BodyDef def;
                    def.type = Detail::Physics2D::ToVendor(rb.type);
                    // THE DEGENERATE CASE, STATED ONCE FOR THE WHOLE FILE
                    // (Task 3, F1): Transform is 3D but Manifold2D is a 2D
                    // solver, so this system reads and writes the XY PLANE and
                    // the Z-AXIS TURN ONLY. position.z, scale.z, and any
                    // out-of-plane component of `rotation` are not inputs to the
                    // simulation -- they are neither consulted here nor
                    // preserved by SetAngle-style writes; see the write-back
                    // pass for what that costs. This is deliberate and is NOT an
                    // oversight of the widening: 3D physics is a separate
                    // decision that has not been made.
                    def.position = Phys::Vec2(lt.position.x, lt.position.y);

                    // Primary fixture shape, scaled by the authored Transform.scale.
                    def.shape = Detail::Physics2D::MakeScaledShape(fx0, glm::vec2(lt.scale));

                    // Material + filter + local transform from fixture[0].
                    // T6 fix: categoryBits / maskBits / localPos / localAngle were
                    // previously silently dropped because AddBody's auto-fixture
                    // used hardcoded defaults.  BodyDef now carries these fields
                    // and AddBody's auto-fixture reads them (see PhysicsWorld.cpp).
                    def.isSensor      = fx0.isSensor;
                    def.contactEvents = fx0.contactEvents;
                    def.sensorEvents  = fx0.sensorEvents;
                    def.hitEvents     = fx0.hitEvents;
                    def.restitution   = static_cast<Phys::Real>(fx0.restitution);
                    def.friction      = static_cast<Phys::Real>(fx0.friction);
                    def.density       = static_cast<Phys::Real>(fx0.density);
                    def.categoryBits  = fx0.categoryBits;
                    def.maskBits      = fx0.maskBits;
                    def.localPos      = Phys::Vec2(fx0.localPos.x * lt.scale.x,
                                                   fx0.localPos.y * lt.scale.y);
                    def.localAngle    = static_cast<Phys::Real>(fx0.localAngle);

                    // Body-level dynamics from RigidBody2D.
                    def.linearDamping = rb.linearDamping;
                    def.fixedRotation = rb.fixedRotation;
                    def.bullet        = rb.bullet;

                    // Optional mass override: RigidBody2D.mass > 0 beats density-derived.
                    if (rb.mass > 0.0f)
                        def.mass = rb.mass;

                    Phys::BodyHandle handle = world.AddBody(def);
                    std::vector<Phys::FixtureHandle> fxs{ world.GetBodyFixture(handle, 0) };

                    // ---- ADDITIONAL FIXTURES (fixtures[1..N-1]) ----
                    // AddBody already installed fixture[0] as the primary shape.
                    // Call AddFixture for each subsequent fixture so the body has
                    // one physics fixture per authored Fixture2D descriptor.
                    for (std::size_t i = 1; i < col.fixtures.size(); ++i)
                    {
                        Phys::FixtureDef fd = Detail::Physics2D::MakeFixtureDef(col.fixtures[i], glm::vec2(lt.scale));
                        fxs.push_back(world.AddFixture(handle, fd));
                    }

                    // Authored Z rotation, applied on the live handle after every
                    // fixture is in (SetAngle re-registers the body's proxies from
                    // its pose, so one call covers them all -- the same property
                    // PASS 3.5 relies on for a moved static body). BodyDef carries
                    // no angle, so a mint used to start every body at 0 whatever
                    // the Transform said; a PAUSED pass hid that because its PASS
                    // 3.5 reconciled the angle in the same call, but a STEPPING
                    // pass has no reconcile -- Play's first fixedUpdate frame (Play
                    // re-mints from the authored state), ArcaneRuntime's boot --
                    // and PASS 4 then wrote the zero back over the authored
                    // quaternion: rotate the capsule in Edit, press Play, the
                    // rotation is undone (2026-09-12 desk finding). Only the Z
                    // turn reaches the body, as everywhere else in this file.
                    world.SetAngle(handle, static_cast<Phys::Real>(RotationZ(lt.rotation)));

                    // Authored velocity applied after AddBody so we call SetVelocity
                    // on a live handle (also wakes sleeping Dynamic bodies).
                    if (rb.velocity.x != 0.0f || rb.velocity.y != 0.0f)
                        world.SetVelocity(handle, Phys::Vec2(rb.velocity.x, rb.velocity.y));

                    ref.handle           = Detail::Physics2D::PackBody(handle);
                    ref.appliedScale     = glm::vec2(lt.scale);   // 2D solver: scale.z is not a fixture dimension
                    entityToBody[entity] = handle;
                    const Identity* identity = std::as_const(reg).GetComponent<Identity>(entity);
                    res->RecordBody(entity, identity ? identity->id : Guid{}, handle, std::move(fxs));
                });
            }

            // ------------------------------------------------------------------
            // PASS 2.5: CAPTURE PREVIOUS POSES (Epic 04.2 render interpolation).
            // Snapshot every live body's PRE-STEP world pose into PhysicsInterpBuffer2D
            // (opt-in resource; skipped if absent). Captured before Step so prev ==
            // the step-N-1 pose; multiple steps/frame leave prev = second-to-last.
            // Gated on m_stepWorld: a paused/mint-only pass does not step, so the
            // buffer (and the render alpha) stay frozen -> a frozen scene renders
            // static. Iterates the WORLD (not ECS) so world-direct joint/polygon
            // bodies with no entity are covered too.
            // ------------------------------------------------------------------
            if (m_stepWorld)
            {
                if (PhysicsInterpBuffer2D* interp = reg.GetResource<PhysicsInterpBuffer2D>())
                {
                    const std::uint32_t n = world.Count();
                    interp->prev.resize(n);
                    for (std::uint32_t i = 0; i < n; ++i)
                    {
                        if (world.Alive(i))
                        {
                            const Phys::BodyHandle h = world.HandleOf(i);
                            const Phys::Vec2       p = world.PosSlot(i);
                            interp->prev[i] = PhysicsInterpPose2D{
                                glm::vec2(static_cast<float>(p.x), static_cast<float>(p.y)),
                                static_cast<float>(world.GetAngle(h)),
                                h.generation };
                        }
                        else
                        {
                            interp->prev[i].generation = 0;   // dead slot never matches
                        }
                    }

                    // The entity -> slot map the sprite path reads (RenderSystems.hpp
                    // carries no PhysicsBodyRef2D term: PhysicsComponents.hpp would drag
                    // Manifold2D into every game module's include surface). Rebuilt
                    // from entityToBody in the SAME capture that filled `prev`, so the
                    // two are exactly as fresh as each other; a body PASS 1 removed
                    // this pass is already gone from the map (no stale address).
                    interp->slotOf.Clear();
                    interp->slotOf.Reserve(entityToBody.size());
                    for (const auto& [entity, handle] : entityToBody)
                    {
                        if (!world.IsValid(handle))
                            continue;
                        interp->slotOf[entity] = PhysicsInterpSlot2D{ handle.index, handle.generation };
                    }

                    interp->captured = true;
                }
            }

            // ------------------------------------------------------------------
            // PASS 3: STEP -- advance the world by one fixed timestep.
            // Skipped when paused (stepWorld=false): no narrowphase, no solve.
            // ------------------------------------------------------------------
            if (m_stepWorld)
            {
                world.Step(m_fixedDt);
                res->CaptureStep();   // spec s7.2: replace StepEvents, append FrameEvents
            }

            // ------------------------------------------------------------------
            // PASS 3.5: AUTHOR RECONCILE (paused only). When the sim is frozen the
            // AUTHOR owns pos/rot: push a diverged Transform edit into the body
            // BEFORE PASS 4 reflects the (now-matching) body pose back. Stateless --
            // a paused body cannot move itself, so the live body pose is the baseline
            // and any divergence is an author edit. Skipped while stepping (Play =
            // body owns pos/rot; PASS 4 drives lt as before). Scale handled in Task 3.
            //
            // Gated on Changed<Transform> since the last pass (spec 2026-09-11
            // s6.5): chunk-reject over every body, then exactly the bodies whose
            // Transform was written -- Transform is tracked. The exact compares
            // below STAY: a position-only edit changes Transform but must not
            // rebuild every fixture, and only a real divergence teleports.
            // ------------------------------------------------------------------
            if (!m_stepWorld)
            {
                auto view = reg.CreateView<PhysicsBodyRef2D, const Transform, Arcane::Changed<Transform>,
                                           const Collider2D, Arcane::With<RigidBody2D>>();
                view.Since(res->lastReconcile).ForEach([&](Arcane::Entity   /*entity*/,
                                                            PhysicsBodyRef2D&  ref,
                                                            const Transform& lt,
                                                            const Collider2D& col)
                {
                    if (ref.handle == 0) return;
                    if (!world.IsValid(Detail::Physics2D::UnpackBody(ref.handle)))       return;
                    ++res->reconciled;

                    // SCALE: rebuild fixtures when lt.scale changed. Exact compare --
                    // physics never writes scale, so appliedScale can't drift; this
                    // both detects the edit and suppresses per-frame re-rebuild. Runs
                    // before the pose branch (rebuild does not move the body).
                    const glm::vec2 planarScale(lt.scale);   // 2D solver: see the CREATE pass banner
                    if (planarScale != ref.appliedScale)
                    {
                        std::vector<Phys::FixtureHandle> neu =
                            Detail::Physics2D::RebuildScaledFixtures(world, Detail::Physics2D::UnpackBody(ref.handle), col, planarScale);
                        ref.appliedScale = planarScale;
                        // R16: the record follows the new handles. Each dropped
                        // generation is appended (handle + Collider2D index) and
                        // stays resolvable until CaptureStep reads its End.
                        if (const auto rec = res->bodyRecords.find(ref.handle);
                            rec != res->bodyRecords.end())
                        {
                            auto& record = rec->second;
                            record.retiredFixtures.reserve(record.retiredFixtures.size() + record.fixtures.size());
                            for (std::uint32_t i = 0; i < record.fixtures.size(); ++i)
                                record.retiredFixtures.push_back(Detail::Physics2D::RetiredFixture{ record.fixtures[i], i });
                            record.fixtures = std::move(neu);
                        }
                    }

                    // POS/ROT: stateless author reconcile. Only the Z-axis turn
                    // of the authored quaternion reaches the body -- an author
                    // who tilts an entity out of the XY plane in the Inspector
                    // is editing something the 2D solver has no state for, so
                    // that part of the edit is simply not a divergence here.
                    const Phys::Vec2 bp = world.Position(Detail::Physics2D::UnpackBody(ref.handle));
                    const float      ba = static_cast<float>(world.GetAngle(Detail::Physics2D::UnpackBody(ref.handle)));
                    if (std::abs(lt.position.x - static_cast<float>(bp.x)) > Detail::Physics2D::kAuthorPosEps ||
                        std::abs(lt.position.y - static_cast<float>(bp.y)) > Detail::Physics2D::kAuthorPosEps ||
                        Detail::Physics2D::AngleDelta(RotationZ(lt.rotation), ba) > Detail::Physics2D::kAuthorRotEps)
                    {
                        // SetPosition + SetAngle are BOTH load-bearing for a moved STATIC
                        // body: SetPosition updates the pose but NOT the static broadphase
                        // tree; SetAngle re-registers it from the already-updated position.
                        // Keep SetAngle unconditional -- gating it (e.g. "skip when rotation
                        // unchanged") would leave a moved static collider with a stale proxy
                        // that never refreshes. Velocity is zeroed unconditionally ("don't
                        // fling on resume") -- for a Kinematic body with authored rb.velocity
                        // this is a SPEC #1 design consequence (velocity re-apply is a non-goal).
                        world.SetPosition(Detail::Physics2D::UnpackBody(ref.handle), Phys::Vec2(lt.position.x, lt.position.y));
                        world.SetAngle(Detail::Physics2D::UnpackBody(ref.handle), static_cast<Phys::Real>(RotationZ(lt.rotation)));
                        world.SetVelocity(Detail::Physics2D::UnpackBody(ref.handle), Phys::Vec2(0.0f, 0.0f));
                        world.SetAngularVelocity(Detail::Physics2D::UnpackBody(ref.handle), static_cast<Phys::Real>(0));
                    }
                });
            }

            // ------------------------------------------------------------------
            // PASS 4: WRITE-BACK -- STEPPING passes only (spec s4.1, 2026-09-11).
            // A paused pass has nothing to reflect: the author owns the pose and
            // PASS 3.5 already pushed edits body-ward. The unconditional write
            // this replaced stamped every physics entity's Transform changed on
            // every Edit frame (propagation recomposed them all, defeating the
            // change detection for exactly the entities physics touches) and
            // flattened an authored out-of-plane rotation to its Z turn.
            // ------------------------------------------------------------------
            if (m_stepWorld)
            {
                auto view = reg.CreateView<const PhysicsBodyRef2D, Transform, RigidBody2D>();
                view.ForEach([&](Arcane::Entity   /*entity*/,
                                 const PhysicsBodyRef2D& ref,
                                 Transform& lt,
                                 RigidBody2D&    rb)
                {
                    if (ref.handle == 0) return;
                    if (!world.IsValid(Detail::Physics2D::UnpackBody(ref.handle)))          return;

                    // THE 2D WRITE-BACK, NAMED DELIBERATELY (Task 3, F1). The
                    // solver owns the XY plane and the Z-axis turn, so that is
                    // exactly what is written:
                    //   * position.z is PRESERVED -- physics has no depth state,
                    //     so an authored Z would be destroyed by writing 0 here,
                    //     and stomping an author's value with a value the solver
                    //     never computed is the worse of the two errors.
                    //   * rotation is REPLACED by a pure Z-axis quaternion. Any
                    //     out-of-plane component the author put there is LOST on
                    //     the next fixed step. That is not an oversight of the
                    //     widening: a body simulated by a 2D solver has one
                    //     degree of rotational freedom, and there is no
                    //     out-of-plane state for it to round-trip through. An
                    //     entity whose orientation must survive physics should
                    //     not be a physics body until the solver is 3D.
                    //   * scale is untouched, as it always was (physics never
                    //     writes scale -- see the appliedScale reconcile above).
                    const Phys::Vec2 pos = world.Position(Detail::Physics2D::UnpackBody(ref.handle));
                    lt.position = glm::vec3(pos.x, pos.y, lt.position.z);
                    lt.rotation = RotationAboutZ(static_cast<float>(world.GetAngle(Detail::Physics2D::UnpackBody(ref.handle))));

                    // Mirror post-step velocity back into RigidBody2D for Dynamic
                    // bodies so authored velocity field stays consistent with physics.
                    // Kinematic velocity is authored and never written back: the solver
                    // does not modify it, so rb.velocity retains its authored value.
                    if (rb.type == BodyType2D::Dynamic)
                    {
                        const Phys::Vec2 vel = world.Velocity(Detail::Physics2D::UnpackBody(ref.handle));
                        rb.velocity = glm::vec2(vel.x, vel.y);
                    }
                });
            }

            // Time base for the paused reconcile and the re-mint criteria
            // (PhysicsWorld2D::lastReconcile): AFTER PASS 4, so a stepping
            // pass's own write-back marks are never newer than it. Scheduled
            // (Runtime::InstallEngineSystems) or bare (Runtime::PhysicsEditPass,
            // tests) alike -- the advance-after-every-pass contract, spec
            // 2026-09-11-astra-adoption s6.3.
            res->lastReconcile = reg.CurrentTick();
            reg.AdvanceTick();
        
    }
}
