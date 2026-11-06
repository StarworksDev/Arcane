#include <Arcane/Render/RenderFramePacket.hpp>

#include <cstddef>

namespace Arcane
{
    namespace
    {
        // clear() keeps that vector's own buffer. Assigning a fresh object
        // would drop it, and the next extract would allocate again.
        // A list or a pass element owns further buffers. clear() on the
        // outer vector destroys those elements and frees the inner ones,
        // so the elements stay and only the inners are cleared. The copy
        // sets the live count (listCount, or passes.size() after it
        // resizes to this frame's chain) and refills that prefix in place.
        // Readers use the live count. Elements past it are not live.
        void ClearVisible(VisibleSet& set) noexcept
        {
            set.view = {};
            set.frustum = {};
            set.entries.clear();
            set.members.clear();
        }

        void ClearMesh(GpuSceneFrame& frame) noexcept
        {
            frame.stage.rows.clear();
            frame.stage.values.clear();
            frame.stage.rowCapacity = 0;
            frame.stage.fullRebuild = false;
            frame.stage.generation = 0;
            frame.batches.clear();
            frame.args.clear();
            frame.cullBatches.clear();
            frame.visibleIndices.clear();
            frame.oracleVisibleIndices.clear();
            frame.transparentDraws.clear();
            frame.rowCount = 0;
            frame.frustum = {};
            frame.stats = {};
        }

        void ClearPost(PostChainDesc& post) noexcept
        {
            for (PostChainPassDesc& pass : post.passes)
            {
                pass.vsBytes.reset();
                pass.psBytes.reset();
                pass.inputs.clear();
            }
            post.templ.reset();
            post.instance.reset();
            post.chainInputSlots = 1;
        }

        void ClearSnapshot(ImDrawSnapshot& snap) noexcept
        {
            for (ImDrawListSnapshot& list : snap.lists)
            {
                list.vertices.clear();
                list.indices.clear();
                list.cmds.clear();
            }
            snap.listCount = 0;
            snap.textures.clear();
            snap.hasTextures = false;
            snap.displayPos = {};
            snap.displaySize = {};
            snap.framebufferScale = { 1.0f, 1.0f };
        }
    }

    void RenderFramePacket::Clear() noexcept
    {
        spriteView.reset();
        spriteViewFromScene = false;
        meshView.reset();
        viewport = glm::uvec2(0u, 0u);
        origin.translation = glm::vec3(0.0f);

        ClearMesh(mesh);
        ClearVisible(visible);
        ClearVisible(meshVisible);

        batchVertices.clear();
        batchIndices.clear();
        batchSpans.clear();
        globals = GlobalParams{};
        viewProjection = glm::mat4(1.0f);

        pickDrawables.clear();
        overlay.Clear();
        overlay.grid3d = {};
        ClearPost(post);
        hasPost = false;
        ClearSnapshot(imgui);
        ClearSnapshot(gameUi);
        selectedIds.clear();

        lightDirection = glm::vec3(0.0f);
        lightColor = glm::vec3(0.0f);
        ambient = glm::vec3(0.0f);
    }

    RenderFramePacket& PacketRing::BeginExtract()
    {
        const std::uint32_t depth = FramesInFlight();
        RenderFramePacket& slot = m_slots[static_cast<std::size_t>(m_extractCount % depth)];
        slot.Clear();
        ++m_extractCount;
        return slot;
    }

    const RenderFramePacket& PacketRing::Slot(std::uint32_t extractsAgo) const
    {
        const std::uint32_t depth = FramesInFlight();
        const std::uint64_t index = (m_extractCount - 1 - extractsAgo) % depth;
        return m_slots[static_cast<std::size_t>(index)];
    }

    RenderFramePacket& PacketRing::Latest()
    {
        const std::uint32_t depth = FramesInFlight();
        return m_slots[static_cast<std::size_t>((m_extractCount - 1) % depth)];
    }
}
