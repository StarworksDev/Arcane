#include <Arcane/ImGui/ImGuiHostHooks.hpp>
#include <Arcane/ImGui/ImDrawSnapshot.hpp>
#include <Arcane/ImGui/ImGuiLayer.hpp>
#include <Arcane/ImGui/ImGuiNriNode.hpp>
#include <Arcane/Host/HostOverlay.hpp>
#include <Arcane/Render/Nri/RenderOverlayNode.hpp>
#include <imgui.h>

namespace Arcane
{
    void InstallImGuiHostHooks() noexcept
    {
        SetHostOverlayHooks({
            [](Window& window) -> std::unique_ptr<HostOverlay> { return ImGuiLayer::Create(window); },
            []() noexcept -> void* { return ImGui::GetCurrentContext(); },
            [](void* context) noexcept { ImGui::SetCurrentContext(static_cast<ImGuiContext*>(context)); }
        });
        SetRenderOverlayHooks({
            [](NriGraphContext& context) -> std::unique_ptr<RenderOverlayNode>
            { return ImGuiNriNode::Create(context); },
            [](RenderGraph& graph, NriGraphContext* context, RgTexture target, bool gameUi)
            { AddImGuiNode(graph, context, target, gameUi ? ImGuiNodeSlot::GameUi : ImGuiNodeSlot::HostHud); },
            &CopyImDrawSnapshot
        });
    }
}
