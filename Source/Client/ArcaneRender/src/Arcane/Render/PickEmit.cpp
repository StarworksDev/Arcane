#include <Arcane/Render/PickEmit.hpp>
#include <Arcane/Core/Constant.hpp>
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>

namespace Arcane
{
    uint32_t PickPassId(const std::vector<Astra::Entity>& ordered, Astra::Entity e)
    {
        if (e == Astra::Entity::Invalid())
            return 0u;
        for (std::size_t k = 0; k < ordered.size(); ++k)
            if (ordered[k] == e)
                return static_cast<uint32_t>(k + 1);
        return 0u;
    }

    glm::ivec2 PickSampleTexel(glm::vec2 pixel1x, uint32_t ss, uint32_t idW, uint32_t idH)
    {
        const int s = (int)ss;
        int x = (int)std::floor(pixel1x.x) * s + s / 2;
        int y = (int)std::floor(pixel1x.y) * s + s / 2;
        x = std::clamp(x, 0, (int)idW - 1);
        y = std::clamp(y, 0, (int)idH - 1);
        return glm::ivec2(x, y);
    }

    // ------------------------------------------------------------------
    // The id pass's geometry -- ONE emitter, so no recorder can assign ids a
    // different way. See the header's block comment.
    // ------------------------------------------------------------------

    uint32_t PickKindCode(PickDrawable::Kind kind)
    {
        switch (kind)
        {
        case PickDrawable::Kind::Quad:    return 0u;
        case PickDrawable::Kind::Circle:  return 1u;
        case PickDrawable::Kind::Capsule: return 2u;
        case PickDrawable::Kind::Box:     return 3u;
        case PickDrawable::Kind::Mesh:    return 4u;   // never reaches the 2D path
        }
        return 0u;
    }

    glm::vec2 PickBoundHalfExtents(const PickDrawable& d)
    {
        switch (d.kind)
        {
        case PickDrawable::Kind::Circle:  return glm::vec2(d.radius, d.radius);
        case PickDrawable::Kind::Capsule: return glm::vec2(d.halfLen + d.radius, d.radius);
        case PickDrawable::Kind::Box:     return d.halfExtents;
        case PickDrawable::Kind::Quad:
            // Half the TL->TR and TL->BL edge lengths: the corners are the
            // geometry, so the bound is derived, never authored.
            return glm::vec2(0.5f * glm::length(d.corners[1] - d.corners[0]),
                             0.5f * glm::length(d.corners[3] - d.corners[0]));
        case PickDrawable::Kind::Mesh:
        default:                          return glm::vec2(0.0f);
        }
    }

    void BuildPickIdGeometry(std::span<const PickDrawable> drawables,
                             std::vector<PickIdVertex>& outVertices,
                             std::vector<uint32_t>& outIndices)
    {
        outVertices.clear();
        outIndices.clear();

        // Bounding-quad corner sign pattern for the physics shapes: one loop
        // around the bound, (-,-) (+,-) (+,+) (-,+), so the same two-triangle
        // index pattern a sprite Quad's TL,TR,BR,BL corners use serves both.
        // Winding is irrelevant: the 2D id pipeline culls nothing.
        ARC_CONSTANT("math: unit-quad corner signs")
        static const glm::vec2 kSigns[4] = {
            { -1.0f, -1.0f }, { 1.0f, -1.0f }, { 1.0f, 1.0f }, { -1.0f, 1.0f } };

        for (std::size_t di = 0; di < drawables.size(); ++di)
        {
            const PickDrawable& d = drawables[di];
            const uint32_t id     = (uint32_t)di + 1u;   // 1-based

            // A Mesh drawable has no quad: the pick node rasterises its resident
            // triangles through the second pipeline. It KEEPS its index, so
            // the id the mesh path writes and the ids this path writes share
            // one numbering.
            if (d.kind == PickDrawable::Kind::Mesh)
                continue;

            const uint32_t code   = PickKindCode(d.kind);
            const uint32_t base   = (uint32_t)outVertices.size();

            if (d.kind == PickDrawable::Kind::Quad)
            {
                // The four world corners VERBATIM. Kind code 0 covers the
                // whole quad in the PS, so `local` is unused (zero).
                for (int i = 0; i < 4; ++i)
                {
                    PickIdVertex v;
                    v.pos     = d.corners[static_cast<std::size_t>(i)];
                    v.local   = glm::vec2(0.0f);
                    v.radius  = 0.0f;
                    v.halfLen = 0.0f;
                    v.kind    = code;
                    v.id      = id;
                    outVertices.push_back(v);
                }
            }
            else
            {
                // Rotate the bounding quad by the drawable's world angle in the
                // XY plane about its centre. The PS coverage test uses the
                // UNROTATED `local`.
                const glm::vec2 bound = PickBoundHalfExtents(d);
                const float c = std::cos(d.angle);
                const float s = std::sin(d.angle);
                for (int i = 0; i < 4; ++i)
                {
                    const glm::vec2 local(kSigns[i].x * bound.x, kSigns[i].y * bound.y);
                    const glm::vec2 rot(c * local.x - s * local.y,
                                        s * local.x + c * local.y);
                    PickIdVertex v;
                    v.pos     = d.center + glm::vec3(rot, 0.0f);
                    v.local   = local;
                    v.radius  = d.radius;
                    v.halfLen = d.halfLen;
                    v.kind    = code;
                    v.id      = id;
                    outVertices.push_back(v);
                }
            }

            const uint32_t quad[6] = { base, base + 1, base + 2,
                                       base, base + 2, base + 3 };
            outIndices.insert(outIndices.end(), quad, quad + 6);
        }
    }
}
