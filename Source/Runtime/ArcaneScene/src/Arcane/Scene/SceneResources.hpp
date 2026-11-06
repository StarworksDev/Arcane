#pragma once

// Registry resources (singletons) for the scene slice. RenderContext2D,
// SpriteTable and SpriteMaterialTable are set by the host each frame; SceneRoot
// marks the subtree that IS the scene.

#include <Arcane/Ecs.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Scene/SceneCacheRecords.hpp>
#include <Arcane/Scene/ViewTransform.hpp>   // RenderContext2D::view (F4 plan 1 T3)

#include <cstdint>
#include <unordered_map>

namespace Arcane
{
    struct SceneRoot { Arcane::Entity entity; };

    struct RenderContext2D
    {
        // THE ONE camera (F4 plan 1, spec s3): the ViewTransform the host pushed
        // through ClientRuntime::SetView. Sprites do not read it
        // (RenderSubmissionSystem draws world quads; ExtractFrame sets the
        // batcher's view-projection from packet.spriteView). The physics
        // overlay uses packet.spriteView too, and reads only alpha here.
        // ExtractFrame still copies this view into packet.spriteView when the
        // scene has no usable Camera (the host SetView fallback).
        // The default (identity matrices, viewport 0) is a host that never pushed
        // a view: it has no Affine2D (AsAffine2D() is nullopt), so that fallback
        // has no pixel map; sprites are world quads (F4 plan 1 T5) and draw
        // through the identity view-projection. ExtractFrame warns when no scene
        // camera exists.
        class Batcher2D* batcher = nullptr;   // set by the host between Begin and Drain
        ViewTransform    view{};
        float            alpha = 0.0f;        // RunLoop::Alpha() in [0,1); host-set each frame
    };

    // Sprite-asset resolution: SpriteRenderer::sprite (an .arcsprite Guid) ->
    // the resolved record above. Same shape and lifetime rules as
    // SpriteMaterialTable below: the map is OWNED by the host (transient
    // pointer resource, set each frame, never serialized). Unresolved (nil /
    // absent / texture still loading) -> null, and submission falls back to a
    // 1x1 m tint quad, so a sprite always draws.
    struct SpriteTable
    {
        const std::unordered_map<Guid, SpriteEntry>* sprites = nullptr;

        // A counter OWNED by the cache that owns the map (SpriteCache), bumped
        // by the owning cache on any publish -- an entry emplaced or replaced
        // by a resolve, Invalidate, Clear; BoundsSystem re-walks every drawable
        // when it changes (spec s2.3): a sprite asset whose sizeMeters/pivot
        // changed touches no component, so no Changed<> view would otherwise
        // ever see it. Null = never changes (a fixture with a static map).
        const std::uint64_t* generation = nullptr;

        const SpriteEntry* Resolve(const Guid& g) const
        {
            if (!sprites || !g.IsValid()) return nullptr;
            auto it = sprites->find(g);
            return it != sprites->end() ? &it->second : nullptr;
        }
    };

    // Sprite-material resolution (Slice 8): material asset Guid -> the id a
    // compiled material was registered under in the frame's Batcher2D. The map
    // is OWNED by the host's SpriteMaterialCache (transient pointer resource,
    // like RenderContext2D::batcher -- set each frame, never serialized).
    // Unresolved (absent / still compiling / failed) -> 0, the plain sprite
    // built-in, so a sprite always draws.
    struct SpriteMaterialTable
    {
        const std::unordered_map<Guid, uint16_t>* materials = nullptr;

        uint16_t Resolve(const Guid& g) const
        {
            if (!materials || !g.IsValid()) return 0;
            auto it = materials->find(g);
            return it != materials->end() ? it->second : 0;
        }
    };

    // .arcmesh Guid -> the resolved record above. Same shape and lifetime
    // rules as SpriteTable above: the map is OWNED by the host's MeshCache
    // (transient pointer resource, set each frame, never serialized).
    // Unresolved (nil / absent / failed to load or validate) -> null, and
    // GpuSceneSync (Render/GpuSceneSync.hpp) skips the entity entirely -- unlike a
    // sprite's 1x1 m untextured placeholder, there is no meaningful
    // placeholder mesh, so a broken reference draws nothing rather than the
    // wrong shape.
    struct MeshTable
    {
        const std::unordered_map<Guid, MeshEntry>* meshes = nullptr;

        // A counter OWNED by the cache that owns the map (MeshCache), bumped by
        // the owning cache on any publish -- an entry emplaced on a Ready
        // resolve, Invalidate, Clear; BoundsSystem re-walks every drawable when
        // it changes (spec s2.3): a primitive parameter edit or a reimport
        // (MeshDocument::Save -> MeshCache::Invalidate -> re-resolve) changes
        // `bounds` without touching any component, so no Changed<> view would
        // otherwise ever see it. Null = never changes (a fixture with a static
        // map).
        const std::uint64_t* generation = nullptr;

        const MeshEntry* Resolve(const Guid& g) const
        {
            if (!meshes || !g.IsValid()) return nullptr;
            auto it = meshes->find(g);
            return it != meshes->end() ? &it->second : nullptr;
        }
    };

    // .arcmat Guid -> the resolved record above. Same shape and lifetime
    // rules as MeshTable/SpriteTable.
    struct MeshMaterialTable
    {
        const std::unordered_map<Guid, ResolvedMeshMaterial>* materials = nullptr;

        const ResolvedMeshMaterial* Resolve(const Guid& g) const
        {
            if (!materials || !g.IsValid()) return nullptr;
            auto it = materials->find(g);
            return it != materials->end() ? &it->second : nullptr;
        }
    };
}
