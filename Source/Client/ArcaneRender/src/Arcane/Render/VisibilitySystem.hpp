#pragma once

// The CPU coarse visibility stage (F3, spec s4): one VisibleSet per view,
// linear over every WorldBounds. A registry RESOURCE (SceneVisibility)
// because RenderSubmissionSystem is an Astra::System and must read it; the
// host builds views[0] between the fixed scheduler (Sim/SystemSchedulers.hpp)
// and the frame extract's sprite sweep (ExtractFrame.hpp) from the
// viewport's ViewTransform.
//
// NO RESOURCE => NOTHING IS CULLED. Every consumer (the sprite sweep,
// CollectPickables, the GPU batch builder) reads MainVisibleSet() and treats
// nullptr as "draw everything" -- device-less hosts and every pre-F3 test keep
// their behaviour.
//
// THE SLACK: WorldBounds is the fixed-step pose; the sprite sweep renders a
// pose interpolated toward it (Arcane::PhysicsInterpBuffer2D), so a fast sprite at the
// screen edge can sit a fraction of one step outside its box. The frustum is
// widened by VisibilitySlackMeters() -- render.cull.frustumSlackMeters
// (RenderLookSettings.hpp; Live, read once per pass) -- before every test.
// Plan 2's GPU cull uses the SAME widened planes (VisibleSet::frustum).
//
// HEADER-ONLY, device-free (the GpuSceneSync idiom): ArcaneTests
// drives it under ~[gpu]. No spatial structure -- the trigger is a measured
// BuildVisibleSet above 0.5 ms (spec s4).
#include <Arcane/Math/Aabb.hpp>
#include <Arcane/Render/RenderLookSettings.hpp>
#include <Arcane/Render/VisibleSet.hpp>        // VisibleEntry / VisibleSet (their own header, F5 plan 1 Task 4)
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/Frustum.hpp>
#include <Arcane/Scene/ViewTransform.hpp>

#include <Astra/Registry/Registry.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace Arcane
{
    // The published render.cull.frustumSlackMeters: how far every view's
    // frustum is widened before the coarse test.
    [[nodiscard]] inline float VisibilitySlackMeters()
    {
        return Settings<RenderCullSettings>().frustumSlackMeters;
    }

    struct SceneVisibility
    {
        std::vector<VisibleSet> views;   // index 0 = the main view; shadow views / previews append (later arcs)

        // Transient: Registry::Save writes it through the no-op Serialize, so a
        // restore brings back an EMPTY set; the no-op Serialize keeps this off Astra's
        // reflected/trivially-copyable auto-serialization path, the same
        // reason BoundsSystemState (BoundsSystem.hpp) carries one.
        template<typename Archive> void Serialize(Archive& /*ar*/) {}
    };

    [[nodiscard]] inline const VisibleSet* MainVisibleSet(const Astra::Registry& reg) noexcept
    {
        const SceneVisibility* sv = reg.GetResource<SceneVisibility>();
        return (sv && !sv->views.empty()) ? &sv->views[0] : nullptr;
    }

    // View-space depth of the box's nearest point: the eight corners through
    // `view`, the largest z (nearest, since the camera looks down -Z),
    // negated and clamped at 0 (a box that straddles the eye is "at" it).
    [[nodiscard]] inline float NearViewDepth(const Aabb& box, const glm::mat4& view) noexcept
    {
        float nearest = -std::numeric_limits<float>::infinity();
        for (int i = 0; i < 8; ++i)
        {
            const glm::vec3 c((i & 1) ? box.max.x : box.min.x,
                              (i & 2) ? box.max.y : box.min.y,
                              (i & 4) ? box.max.z : box.min.z);
            nearest = std::max(nearest, (view * glm::vec4(c, 1.0f)).z);
        }
        return std::max(0.0f, -nearest);
    }

    inline void BuildVisibleSet(Astra::Registry& reg, const ViewTransform& view, VisibleSet& out)
    {
        out.Clear();
        out.view    = view;
        out.frustum = Frustum::From(view).Widened(VisibilitySlackMeters());
        reg.CreateView<const WorldBounds, Astra::Not<Hidden>>().ForEach(
            [&](Astra::Entity e, const WorldBounds& wb)
            {
                if (out.frustum.Contains(wb.box))
                    out.Insert(e, wb.box, NearViewDepth(wb.box, view.view));
            });
    }
}
