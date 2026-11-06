#pragma once

// The version-1 binary layout of PhysicsBodyRef2D: what Registry::Save wrote
// when the component was an 8-byte solver handle {index, generation} followed
// by appliedScale. Only SerializationTraits<PhysicsBodyRef2D>::Migrate reads it.
// Kept out of PhysicsComponents.hpp so the ABI manifest's field scan (which
// walks that game-facing header) does not see a disk-format record.

#include <cstdint>

namespace Arcane::Detail
{
    struct PhysicsBodyRef2DDiskV1
    {
        std::uint32_t index;
        std::uint32_t generation;
        float         scaleX;
        float         scaleY;
    };
    static_assert(sizeof(PhysicsBodyRef2DDiskV1) == 16);
    static_assert(alignof(PhysicsBodyRef2DDiskV1) == 4);
}
