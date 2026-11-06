#pragma once

// One view's coarse visibility result (F3, spec s4): the entities whose
// WorldBounds passed that view's widened frustum. Its own header (F5 plan 1,
// Task 4) so the render packet (RenderFramePacket.hpp) can own a copy without
// pulling in the registry sweep; BuildVisibleSet, SceneVisibility and
// MainVisibleSet stay in VisibilitySystem.hpp.

#include <Arcane/Math/Aabb.hpp>
#include <Arcane/Scene/Frustum.hpp>
#include <Arcane/Scene/ViewTransform.hpp>

#include <Astra/Entity/Entity.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Arcane
{
    struct VisibleEntry
    {
        Astra::Entity entity{};
        Aabb          box;
        float         nearDepth = 0.0f;   // view-space distance to the box's nearest point along -Z, clamped >= 0
    };

    struct VisibleSet
    {
        ViewTransform              view;
        Frustum                    frustum;   // already Widened(VisibilitySlackMeters())
        std::vector<VisibleEntry>  entries;
        std::vector<std::uint64_t> members;   // bitset by entity index

        [[nodiscard]] bool Contains(Astra::Entity e) const noexcept
        {
            const std::uint32_t idx = static_cast<std::uint32_t>(e.GetID());
            const std::size_t word = idx / 64u;
            return word < members.size() && ((members[word] >> (idx % 64u)) & 1ull);
        }

        void Clear()
        {
            entries.clear();
            std::fill(members.begin(), members.end(), 0ull);
        }

        void Insert(Astra::Entity e, const Aabb& box, float nearDepth)
        {
            const std::uint32_t idx = static_cast<std::uint32_t>(e.GetID());
            const std::size_t word = idx / 64u;
            if (word >= members.size())
                members.resize(word + 1, 0ull);
            members[word] |= (1ull << (idx % 64u));
            entries.push_back(VisibleEntry{ e, box, nearDepth });
        }
    };
}
