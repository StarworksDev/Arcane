#pragma once

// The engine's own component roster, named once.
//
// Astra assigns ComponentIDs from a first-touch counter, so the ORDER of the
// list below IS the engine's id numbering. Do not reorder it; append only.
// (2026-09-11: Arcane::PhysicsSettings2D was appended after MeshRenderer, which shifted
// the three physics ids up by one -- in-process only, as ever. 2026-09-18, F3
// plan 1 T2: WorldBounds appended after Arcane::PhysicsSettings2D, the same shift again.)
//
// Two places expand this one list, which is the whole point of it existing:
//   - Runtime.cpp's engineModule->Register<...> (RegisterRoster below) -- the
//     registration that MINTS the ids;
//   - ProjectHost.hpp's VerifySharedTypeContext -- the boot check that asks
//     whether the CALLING module resolves every one of them to the same id.
// Before 2026-09-16 the verify probed a single type (Arcane::Transform) and
// the roster lived only in Runtime.cpp, so a module whose cache was wrong for
// SOME types but right for Transform passed the check. That is exactly what
// the editor did: EditorApp's EditModeSchedule member resolved WorldTransform
// in the exe's private TypeContext before the shared one was installed.

#include <Arcane/Scene/Components.hpp>          // Transform / WorldTransform / ... / MeshRenderer / Arcane::PhysicsSettings2D / WorldBounds
#include <Arcane/Scene/TypeList.hpp>            // Arcane::TypeList (Scene's; the physics roster in PhysicsComponents.hpp uses it too)

namespace Arcane
{
    // EXACTLY the order RegisterSceneComponents registers in (SceneModule.hpp):
    // the ten scene types, ending at WorldBounds. The 2D physics types follow in
    // Arcane::PhysicsComponentRoster2D (Scene/PhysicsComponents.hpp), registered
    // by the 2D physics module's SimHook; VerifySharedTypeContext walks both.
    using EngineComponentRoster = TypeList<Transform, WorldTransform, SpriteRenderer,
                                           PostProcess, Identity, Hidden, Camera, MeshRenderer,
                                           Arcane::PhysicsSettings2D, WorldBounds>;
}
