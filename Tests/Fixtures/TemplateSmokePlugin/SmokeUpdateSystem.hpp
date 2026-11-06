#pragma once

// SmokeUpdateSystem: a system -- a functor the scheduler runs each step. Its
// PARAMETERS say what it touches, so the scheduler can order and parallelise
// it: views over components and engine resources such as the sim clock.
//
//     void operator()(Arcane::View<Arcane::Transform>& view,
//                     Arcane::Res<Arcane::Time> time)
//
// Fixed-step transform propagation is not installed in the Update scheduler,
// so this template invents no irrelevant edge. Registrar discovery
// order is irrelevant: derive Arcane::SystemTraits<Arcane::Before<...>> or
// After<...> whenever scheduler order matters. The ARC_SYSTEM declaration
// that selects phase and network role is in SmokeUpdateSystem.cpp.

#include <Arcane/Ecs.hpp>

namespace TemplateSmoke
{
    struct SmokeUpdateSystem
    {
        void operator()(Arcane::Res<Arcane::Time> time)
        {
            (void)time;
        }
    };
}
