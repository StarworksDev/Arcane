#pragma once

// RenderSubmissionSystem: reads WorldTransform + SpriteRenderer, submits one
// WORLD-space quad per sprite to the Batcher2D held in the RenderContext2D
// resource. Read-only w.r.t. ECS; the side effect (batcher submission) is
// external. Run by the frame extract (ExtractFrame.hpp), once per frame and
// single-threaded (Batcher2D is not thread-safe) -- never by a scheduler: the
// Render phase is retired (F5 plan 1 Task 5).
//
// THE SYSTEM PROJECTS NOTHING (F4 plan 1 T5). A sprite's four corners come
// from its FULL world basis through SpriteWorldQuad (SpriteGeometry.hpp) in
// metres, +Y up, and go to Batcher2D::QuadWorld verbatim; the HOST pushes the
// frame's view-projection into the batcher (SetViewProjection, right after
// Begin) and the vertex shader does the projection. So a Z turn spins the
// quad in the plane, an X/Y tilt lays it into 3D, and a perspective view
// draws sprites like any other world geometry -- there is no per-axis pixel
// affine in this path any more, and RenderContext2D::view is NOT read here.
//
// Sprite anchor = the sprite asset's PIVOT: the entity's world position is the
// point the quad is placed and rotated about. Pivot (0,0) = BOTTOM-left of the
// image, (1,1) = top-right; the default (0.5, 0.5) puts the position at the
// quad's centre, which lines the sprite up with its physics body / collider
// (also centre-anchored -- PhysicsDebugDraw draws the collider outline
// centred on the body position).
//
// Size comes from the sprite ASSET (SpriteEntry::sizeMeters, resolved through
// the SpriteTable resource) times the entity's world scale -- SpriteRenderer
// carries no size of its own, and the scale rides the world matrix into
// SpriteWorldQuad. Primitives (Circle/Capsule) and unresolved sprites use a
// 1x1 m base, so their scale IS their size in metres.

#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/SpriteGeometry.hpp>
#include <Arcane/Render/VisibilitySystem.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneResources.hpp>

#include <Astra/Registry/Registry.hpp>
#include <Astra/System/System.hpp>

#include <glm/glm.hpp>

#include <cmath>

namespace Arcane
{
    // Reads<> is honest now: the view below is const (Astra adoption 2026-09-11).
    struct RenderSubmissionSystem
        : Astra::SystemTraits<Astra::Reads<WorldTransform, SpriteRenderer, Hidden, WorldBounds>>
    {
        void operator()(Astra::Registry& reg)
        {
            RenderContext2D* ctx = reg.GetResource<RenderContext2D>();
            if (!ctx || !ctx->batcher) return;
            const SpriteTable* spriteTable = reg.GetResource<SpriteTable>();
            const SpriteMaterialTable* materials = reg.GetResource<SpriteMaterialTable>();
            const Arcane::PhysicsInterpBuffer2D* interp = reg.GetResource<Arcane::PhysicsInterpBuffer2D>();
            const VisibleSet* vis = MainVisibleSet(reg);   // nullptr: cull nothing (spec s4)

            auto view = reg.CreateView<const WorldTransform, const SpriteRenderer, Astra::Not<Hidden>>();
            view.ForEach([&](Astra::Entity e, const WorldTransform& world, const SpriteRenderer& sprite)
            {
                if (vis && !vis->Contains(e))
                    return;   // off-screen this frame (the CPU coarse stage)

                // A WORKING COPY of the world matrix: the interpolated pose is
                // re-baked into it below, and everything after reads `m` --
                // the full basis, translation in column 3.
                glm::mat4 m = world.matrix;

                // Render interpolation (Epic 04.2, re-based 2026-09-11 spec s8): a
                // physics body's PREVIOUS world-slot pose lives in Arcane::PhysicsInterpBuffer2D
                // (captured by Arcane::PhysicsSystem2D PASS 2.5 before each step, indexed by
                // PhysicsWorld body SLOT; slotOf is this entity's address into it,
                // rebuilt by the same capture). Blend position (XY) and angle from it
                // to the current WORLD pose by alpha -- Lerp/AngleLerp, the debug
                // overlay's own helpers, so sprite and overlay agree to the bit.
                // World-slot poses are world poses, MORE correct than the retired
                // per-entity local-pose component's local-as-world approximation. ANY miss --
                // no buffer, not yet captured, no entry for this entity, slot past
                // the buffer, or a recycled slot (generation mismatch) -- is the
                // unchanged snap-to-step: `m` stays the authored world matrix.
                //
                // On a hit the 2D pose is written BACK INTO THE MATRIX: the XY
                // translation, and the upper-left 2x2 rotated to the blended
                // angle while its column lengths (the scale) AND its handedness
                // (a mirror) are preserved -- Transform::ToMatrix's own layout
                // for a Z turn, m[0] = (c*sx, s*sx), m[1] = (-s*sy, c*sy), with
                // a SIGNED sx OR sy. The 2D physics pose lives in XY; the Z
                // rows/column and the Z translation are left untouched.
                //
                // Handedness (F4 plan 1 T5 review, Ruling J; final review F1): a
                // negative 2x2 determinant is a mirrored basis. The matrix alone
                // cannot say WHICH axis was flipped -- R(t)*diag(-a,b) ==
                // R(t+pi)*diag(a,-b) -- so the basis has TWO readings, {t, t+pi},
                // and the rule is THE NEARER OF THE TWO TO THE PHYSICS ANGLE
                // (pp.angle, the captured pose): the physics body's angle is the
                // ground truth the author's Transform was baked from, so the
                // reading within a quarter-turn of it is the un-mirrored turn and
                // the other is the mirror's half-turn alias. That reading decides
                // which column carries the mirror sign in the re-bake: the X
                // reading (t from the first column with its sign undone) means
                // diag(-|sx|, |sy|); the Y reading (t+pi) means diag(|sx|, -|sy|).
                // Reading X unconditionally (the T5 form) gave a scale (1, -1)
                // body curRot = t+pi against a physics angle of t, and AngleLerp
                // then swept a half-turn EVERY fixed step; reading the raw column
                // gave the same sweep for scale (-1, 1). Either way the same
                // entity would draw differently with and without a hit (the miss
                // path hands the authored basis to SpriteWorldQuad untouched).
                if (interp && interp->captured)
                {
                    if (const Arcane::PhysicsInterpSlot2D* slot = interp->slotOf.TryGet(e))
                    {
                        if (slot->index < interp->prev.size()
                            && interp->prev[slot->index].generation == slot->generation)
                        {
                            const Arcane::PhysicsInterpPose2D& pp = interp->prev[slot->index];
                            // Handedness of the XY basis: det < 0 is a mirror.
                            const bool mirrored = (m[0].x * m[1].y - m[0].y * m[1].x) < 0.0f;
                            // World rotation from the first basis column (for a
                            // Z-axis turn m[0] = (c*sx, s*sx, 0)), read first as
                            // an X mirror (the column's sign undone).
                            float curRot = mirrored ? std::atan2(-m[0].y, -m[0].x)
                                                    : std::atan2( m[0].y,  m[0].x);
                            // The nearer of {t, t+pi} to the physics angle: if
                            // the X reading is more than a quarter-turn from it
                            // (cos of the delta negative -- wrap-free), the
                            // mirror is on Y and the turn is the other reading.
                            bool mirrorOnY = false;
                            if (mirrored && std::cos(curRot - pp.angle) < 0.0f)
                            {
                                curRot   = std::atan2(m[0].y, m[0].x);   // the raw column: t+pi of the X reading
                                mirrorOnY = true;
                            }
                            const float rot    = Arcane::Detail::Physics2D::AngleLerp(pp.angle, curRot, ctx->alpha);
                            const float sx = glm::length(glm::vec2(m[0])) * ((mirrored && !mirrorOnY) ? -1.0f : 1.0f);
                            const float sy = glm::length(glm::vec2(m[1])) * (mirrorOnY ? -1.0f : 1.0f);
                            const float c = std::cos(rot), s = std::sin(rot);
                            m[3].x = Arcane::Detail::Physics2D::Lerp(pp.position.x, m[3].x, ctx->alpha);
                            m[3].y = Arcane::Detail::Physics2D::Lerp(pp.position.y, m[3].y, ctx->alpha);
                            m[0].x =  c * sx; m[0].y = s * sx;
                            m[1].x = -s * sy; m[1].y = c * sy;
                        }
                    }
                }
                // Only a Rect consults the sprite asset: Circle/Capsule exist to
                // MATCH a collider, so they must stay on the 1x1 m base times
                // scale -- an asset's size/pivot would drift them off their body.
                const SpriteEntry* entry =
                    (sprite.shape == SpriteShape::Rect && spriteTable)
                        ? spriteTable->Resolve(sprite.sprite)
                        : nullptr;
                // Primitives and unresolved sprites draw a 1x1 m base; the sprite
                // asset supplies the base for textured rects. Scale (not a
                // component field) is the sizing mechanism -- see the 2026-07-28
                // sprite-asset spec.
                const glm::vec2 baseSize = entry ? entry->sizeMeters : glm::vec2(1.0f);
                const glm::vec2 pivot    = entry ? entry->pivot      : glm::vec2(0.5f);

                ctx->batcher->SetLayer(static_cast<uint16_t>(sprite.sortingLayer),
                                       static_cast<uint16_t>(sprite.orderInLayer));

                // Draw the sprite's PRIMITIVE shape so it can match its collider.
                // Circle/Capsule go through the batcher's filled SDF primitives
                // in world space (no sprite asset at all -- `entry` is null by
                // construction above, so they are centred on the position);
                // Rect is the textured/tinted world quad.
                switch (sprite.shape)
                {
                case SpriteShape::Circle:
                case SpriteShape::Capsule:
                {
                    // The primitive's frame: the basis columns as UNIT axes
                    // (right = local +x, up = local +y) and their lengths as
                    // the size in metres -- a 1x1 m base times the world scale.
                    // Full-length columns, so a tilted basis still yields a
                    // unit axis in ITS plane. A degenerate (zero-scale) axis
                    // falls back to the world axis rather than normalising to
                    // NaN; its radius/extent is zero anyway.
                    const glm::vec3 col0(m[0]), col1(m[1]);
                    const float sx = glm::length(col0), sy = glm::length(col1);
                    const glm::vec3 right = sx > 0.0f ? col0 / sx : glm::vec3(1.0f, 0.0f, 0.0f);
                    const glm::vec3 up    = sy > 0.0f ? col1 / sy : glm::vec3(0.0f, 1.0f, 0.0f);
                    const glm::vec3 centre(m[3]);
                    const glm::vec2 worldSize = baseSize * glm::vec2(sx, sy);
                    if (sprite.shape == SpriteShape::Circle)
                    {
                        // Filled disc, rotation-invariant. Diameter == worldSize.x.
                        ctx->batcher->CircleWorld(centre, right, up, worldSize.x * 0.5f, sprite.tint);
                        break;
                    }
                    // Horizontal capsule (size.x >= size.y): a central band of
                    // length (size.x - size.y) and height size.y, plus two end
                    // discs of radius size.y/2 at the segment endpoints, all in
                    // the (right, up) frame about the centre. The band's corners
                    // come from SpriteWorldQuad on a UNIT-basis copy of `m`
                    // (scale already folded into the band size), centre pivot.
                    const float r       = worldSize.y * 0.5f;
                    const float halfLen = (worldSize.x - worldSize.y) * 0.5f;
                    glm::mat4 unitBasis = m;
                    unitBasis[0] = glm::vec4(right, 0.0f);
                    unitBasis[1] = glm::vec4(up, 0.0f);
                    const SpriteQuad band = SpriteWorldQuad(
                        unitBasis, glm::vec2(worldSize.x - worldSize.y, worldSize.y), glm::vec2(0.5f));
                    ctx->batcher->QuadWorld(Batcher2D::kMaterialSprite, Guid::Nil(), band.corners,
                                            glm::vec2(0.0f), glm::vec2(1.0f), sprite.tint);
                    ctx->batcher->CircleWorld(centre + right * halfLen, right, up, r, sprite.tint);
                    ctx->batcher->CircleWorld(centre - right * halfLen, right, up, r, sprite.tint);
                    break;
                }
                case SpriteShape::Rect:
                default:
                {
                    // The four world corners, TL,TR,BR,BL, from the full basis
                    // about the pivot.
                    const SpriteQuad quad = SpriteWorldQuad(m, baseSize, pivot);
                    // Texture identity + UVs come from the resolved sprite
                    // asset (its pixel sub-rect, normalized by
                    // ComputeSpriteGeom, (0,0) at the image's TOP-left). TL
                    // keeps uvMin (QuadWorld's order), so the image top lands
                    // on the quad's +Y edge. No asset, or an asset whose source
                    // texture is nil: a nil Guid and the full-range UVs a
                    // full-texture sprite would have anyway.
                    const Guid texId = entry ? entry->textureId : Guid::Nil();
                    const glm::vec2 uvMin = entry ? entry->uvMin : glm::vec2(0.0f, 0.0f);
                    const glm::vec2 uvMax = entry ? entry->uvMax : glm::vec2(1.0f, 1.0f);
                    // Sprite material (Slice 8): a valid Guid resolves to a
                    // registered Batcher2D material id; 0 (unresolved / nil) is
                    // the plain sprite path -- byte-identical when no sprite in
                    // the scene carries a material.
                    const uint16_t materialId =
                        materials && sprite.material.IsValid()
                            ? materials->Resolve(sprite.material) : 0;
                    // The three-way branch is by (has material, has image): a
                    // registered material always goes through the material arm,
                    // a bare image through the built-in sprite arm, and a sprite
                    // that names neither is a tinted quad on the white texel
                    // (nil Guid, full-range UVs) so it still draws.
                    if (materialId != 0)
                        ctx->batcher->QuadWorld(materialId, texId, quad.corners,
                                                uvMin, uvMax, sprite.tint);
                    else if (texId.IsValid())
                        ctx->batcher->QuadWorld(Batcher2D::kMaterialSprite, texId, quad.corners,
                                                uvMin, uvMax, sprite.tint);
                    else
                        ctx->batcher->QuadWorld(Batcher2D::kMaterialSprite, Guid::Nil(), quad.corners,
                                                glm::vec2(0.0f), glm::vec2(1.0f), sprite.tint);
                    break;
                }
                }
            });
        }
    };
}
