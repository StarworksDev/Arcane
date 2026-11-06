#pragma once

// THE RENDER FRAME PACKET (F5 plan 1, spec s4.1): everything one frame's graph
// declaration and record read, copied out of the game-thread state by the one
// extract stage, then immutable. A packet OWNS its data -- no span into a
// batcher, no registry pointer -- so the world may change the moment extract
// returns.
//
// THE RING: PacketRing holds kMaxFramesInFlight packets and cycles through
// FramesInFlight() of them, so a slot handed out by BeginExtract stays intact
// until FramesInFlight() further extracts have passed -- the same depth every
// per-frame pool, ring and fence uses (FramePacing.hpp).
//
// REGISTRY-FREE BY CONTRACT: this header includes no Astra registry, no scene
// camera sweep, no visibility sweep, and no ImGui header
// (RenderFramePacketTest scans it). ImGui stays an optional vendor module
// behind RenderOverlayHooks (Nri/RenderOverlayNode.hpp; ruling PR-1).

#include <Arcane/Material/GlobalParams.hpp>
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/FramePacing.hpp>
#include <Arcane/Render/GpuSceneTypes.hpp>
#include <Arcane/Render/GridSceneDesc.hpp>
#include <Arcane/Render/OverlayPrimitive.hpp>
#include <Arcane/Render/PickEmit.hpp>
#include <Arcane/Render/PostChainDesc.hpp>
#include <Arcane/Render/VisibleSet.hpp>
#include <Arcane/Scene/SceneCameraView.hpp>
#include <Arcane/Scene/ViewTransform.hpp>
#include <Arcane/Modules/ArcaneRenderApi.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace Arcane
{
    struct RenderOrigin
    {
        glm::vec3 translation{ 0.0f };
    };

    // Ruling PR-1: Render-owned plain data: no ImGui type appears here, so
    // ArcaneRender never includes the ImGui header. ArcaneImGui's
    // ImDrawSnapshot.cpp fills and rebuilds these and static_asserts that
    // ImDrawSnapshotVert is ImDrawVert's layout and that ImDrawIdx is 16-bit
    // (ThirdParty/imgui/imconfig.h:121 leaves the 32-bit override commented
    // out).
    struct ImDrawSnapshotVert
    {
        glm::vec2     pos{ 0.0f };
        glm::vec2     uv{ 0.0f };
        std::uint32_t col = 0;
    };

    // One ImDrawList's copy. One vertex/index/command vector PER LIST, never
    // flattened: a command's vtxOffset addresses its own list's buffer.
    struct ImDrawListSnapshot
    {
        std::vector<ImDrawSnapshotVert> vertices;   // ImDrawVert bytes
        std::vector<std::uint16_t>      indices;    // ImDrawIdx
        struct Cmd
        {
            glm::vec4     clip{ 0.0f };           // ImDrawCmd::ClipRect
            std::uint64_t texture = 0;            // ImTextureID (ImU64), used only when managed == nullptr. ImTextureID_Invalid is 0.
            void*         managed = nullptr;      // TexRef._TexData (an ImTextureData*). Not owned.
            std::uint32_t idxOffset = 0;
            std::uint32_t elemCount = 0;
            std::uint32_t vtxOffset = 0;
        };
        std::vector<Cmd> cmds;
    };

    // One ImDrawData's copy; it owns its lists. CopyImDrawSnapshot /
    // ImDrawSnapshotRebuild (ArcaneImGui, Arcane/ImGui/ImDrawSnapshot.hpp) are
    // the only code that converts to and from the live ImGui types.
    //
    // lists.size() is retained storage. listCount is the live prefix.
    // Readers stop there. Elements at and past listCount stay so the next
    // frame reuses their vertex, index and command buffers.
    struct ImDrawSnapshot
    {
        std::vector<ImDrawListSnapshot> lists;
        std::uint32_t listCount = 0;
        std::vector<void*> textures;            // ImTextureData* pointer copy. Not owned.
        bool hasTextures = false;               // false when src->Textures was null, including an empty default ImDrawData
        glm::vec2 displayPos{ 0.0f };
        glm::vec2 displaySize{ 0.0f };
        glm::vec2 framebufferScale{ 1.0f };
    };

    struct RenderFramePacket
    {
        std::optional<SceneCameraView> spriteView;
        bool                           spriteViewFromScene = false;   // true only when a scene Camera produced spriteView
        std::optional<ViewTransform> meshView;     // single space: RenderFramePacketTest scans this declaration
        glm::uvec2                   viewport{ 0u, 0u };
        RenderOrigin                 origin{};

        GpuSceneFrame                mesh{};
        VisibleSet                   visible{};       // the main (sprite) view's set: SceneVisibility::views[0]
        VisibleSet                   meshVisible{};   // views[1]: the mesh view's set when it differs from the main view

        // THE COPY CONTRACT: Batch2DDrained's spans die at the batcher's next
        // Begin() (Batcher2D.hpp); the packet owns a copy of the vertices,
        // indices and draw spans, so it outlives the batcher's frame.
        std::vector<Batch2DVertex>   batchVertices;
        std::vector<std::uint32_t>   batchIndices;
        std::vector<Batch2DDrawSpan> batchSpans;
        GlobalParams                 globals{};
        glm::mat4                    viewProjection{ 1.0f };

        std::vector<PickDrawable>    pickDrawables;
        OverlayPrimitiveList         overlay{};
        PostChainDesc                post{};
        bool                         hasPost = false;
        ImDrawSnapshot               imgui{};
        ImDrawSnapshot               gameUi{};
        // The one field written after extract, before the frame is recorded.
        // The editor writes it on the packet handed to ArmGraphViewportFrame
        // (the slot ExtractFrame just returned). The runtime pick-probe writes
        // it through PacketRing::Latest().
        std::vector<std::uint32_t>   selectedIds;

        glm::vec3 lightDirection{ 0.0f };
        glm::vec3 lightColor{ 0.0f };
        glm::vec3 ambient{ 0.0f };

        ARC_RENDER_API void Clear() noexcept;
    };

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4251)  // std members on a dll-exported class: benign under /MD
#endif
    // kMaxFramesInFlight slots, FramesInFlight() of them in use. Owns no GPU
    // object: the GPU never reads packet memory.
    class ARC_RENDER_API PacketRing
    {
    public:
        // The slot at extractCount % FramesInFlight(), cleared; then
        // extractCount advances.
        RenderFramePacket&       BeginExtract();
        // The slot the last BeginExtract returned (Slot(0), non-const). Only
        // after a BeginExtract.
        RenderFramePacket&       Latest();
        // The slot `extractsAgo` extracts back; 0 is Latest(). Valid for
        // extractsAgo < FramesInFlight() once that many extracts have run.
        const RenderFramePacket& Slot(std::uint32_t extractsAgo) const;

    private:
        std::array<RenderFramePacket, kMaxFramesInFlight> m_slots{};
        std::uint64_t m_extractCount = 0;
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}
