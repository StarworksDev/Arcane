// ImGuiNriNode -- see the header for where the node sits in the frame, why
// the work is split across declaration and record time, and why there is no
// pool-epoch discipline in this file.
//
// Same include-order rule as every file under Render/Nri/ (NriCommon.hpp):
// NRI headers first, because Extensions/NRIDeviceCreation.h declares
// nri::Message::ERROR and <windows.h> (via Arcane/Base/Log.hpp -> spdlog)
// #defines ERROR via wingdi.h.
#include <NRI.h>

#include "ImGuiNriNode.hpp"

#include <Arcane/Base/Log.hpp>
#include <Arcane/Render/Nri/NriCommon.hpp>
#include <Arcane/Render/Nri/NriGraphContext.hpp>

#undef ERROR

#include <Arcane/ImGui/ImDrawSnapshot.hpp>       // ImDrawSnapshotRebuild (ruling PR-1)
#include <Arcane/Render/RenderFramePacket.hpp>   // Packet()->imgui / gameUi

namespace Arcane
{
    std::unique_ptr<ImGuiNriNode> ImGuiNriNode::Create(NriGraphContext& context)
    {
        std::unique_ptr<ImGuiNriNode> node(new ImGuiNriNode());
        // The bins are cached BY THE VEHICLE and outlive this node, which
        // NriPipelineCache's fill contract (rule 2) requires: the cache
        // dereferences the blob after the fill callback returns.
        if (!node->m_renderer.Init(context.Device(), context.Pipelines(),
                                    context.ShaderBytecode("imgui_vs"),
                                    context.ShaderBytecode("imgui_ps")))
        {
            return nullptr;   // already logged
        }
        return node;
    }

    void ImGuiNriNode::Release(Graveyard& graveyard, std::uint64_t fence)
    {
        m_renderer.Release(graveyard, fence);
    }

    void ImGuiNriNode::PrepareFrame(ImDrawData* drawData, Graveyard& graveyard,
                                     std::uint64_t fence)
    {
        m_renderer.NewFrameTexUpdates(drawData, graveyard, fence);
    }

    bool ImGuiNriNode::InvalidateUserTexture(nri::Texture* texture, Graveyard& graveyard,
                                              std::uint64_t fence)
    {
        return m_renderer.InvalidateUserTexture(texture, graveyard, fence);
    }

    bool ImGuiNriNode::InvalidateUserTextureNow(nri::Texture* texture)
    {
        return m_renderer.InvalidateUserTextureNow(texture);
    }

    void ImGuiNriNode::Record(RenderGraphNodeContext& context, ImDrawData* drawData, RgTexture target)
    {
        m_renderer.RenderDrawData(drawData, context, target);
    }

    void AddImGuiNode(RenderGraph& graph, NriGraphContext* context, RgTexture target,
                      ImGuiNodeSlot slot)
    {
        // WHICH backend, and WHICH of the frame's two draw datas -- see
        // ImGuiNodeSlot. Captured as a bool so both lambdas carry one byte
        // rather than re-deciding, and so the null-context (device-less) drive
        // differs in the node NAME alone.
        const bool gameUi = slot == ImGuiNodeSlot::GameUi;
        // Distinct names: a node name is what a compile error, a barrier dump
        // and the [nri] frame-shape cases all identify a node by, and two nodes
        // called "imgui" in one frame would be indistinguishable in all three.
        const char* const nodeName = gameUi ? "gameui" : "imgui";

        // The texture protocol runs HERE, at declaration time, exactly like
        // AddBatch2DNode's Drain and AddPickNodes' PrepareDrawables: it is the
        // last point before the frame's command buffer opens, and a texture
        // upload on this path goes through a helper that submits and waits.
        if (context)
        {
            if (RenderOverlayNode* node = gameUi ? context->ImGuiGame() : context->ImGuiHud())
            {
                // THE CONTEXT'S LANE (Task 8-pre), not the device's: an atlas
                // rebuild destroys a texture keyed to THIS graph's fence, and
                // a second context on the same device has a fence timeline
                // whose values mean nothing to it.
                //
                // A bound packet's snapshot names the same ImTextureData objects
                // the live draw data does, so this declaration-time rebuild can
                // SetTexID before the record lambda's own rebuild calls GetTexID.
                // A preview graph has no packet and keeps the live pointers.
                if (context->Packet())
                {
                    const ImDrawSnapshot& snap = gameUi ? context->Packet()->gameUi : context->Packet()->imgui;
                    ImDrawSnapshotRebuild rebuilt(snap);
                    node->PrepareFrame(rebuilt.Data(), context->Graves(), context->Graph().DebugSubmitCount());
                }
                else
                {
                    node->PrepareFrame(gameUi ? context->CurrentGameUiDrawData()
                                              : context->CurrentImGuiDrawData(),
                                       context->Graves(),
                                       context->Graph().DebugSubmitCount());
                }
            }
        }

        graph.AddNode(nodeName, RenderGraph::NodeKind::Raster,
            [&graph, target](RenderGraphBuilder& builder)
            {
                // The SAME ColorWrite the tonemap (and the outline composite)
                // declared on this backbuffer -- see the header: consecutive
                // same-state writes derive no transition, and the ROP's
                // destination read is inside AccessBits::COLOR_ATTACHMENT.
                builder.Write(target, RgUsage::ColorWrite);
                graph.SetColorAttachments(std::span<const RgTexture>(&target, 1));
            },
            [context, target, gameUi](RenderGraphNodeContext& nodeContext)
            {
                if (!context)
                    return;   // device-less declaration-shape drive
                if (RenderOverlayNode* node = gameUi ? context->ImGuiGame() : context->ImGuiHud())
                {
                    if (context->Packet())
                    {
                        const ImDrawSnapshot& snap = gameUi ? context->Packet()->gameUi : context->Packet()->imgui;
                        ImDrawSnapshotRebuild rebuilt(snap);
                        node->Record(nodeContext, rebuilt.Data(), target);
                    }
                    else
                    {
                        node->Record(nodeContext,
                                     gameUi ? context->CurrentGameUiDrawData()
                                            : context->CurrentImGuiDrawData(),
                                     target);
                    }
                }
            });
    }
}
