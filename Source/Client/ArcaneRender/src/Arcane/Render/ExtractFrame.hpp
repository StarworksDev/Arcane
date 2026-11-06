#pragma once

// THE EXTRACT STAGE (F5 plan 1, spec s4.1): the ONE place a frame reads the
// game-thread world. A frame builder (the runtime host's RenderGraph, the
// editor viewport, the editor's scene-save cover) fills an ExtractRequest
// with its own state and settings -- never a registry read -- and calls
// ExtractFrame once; what comes back is a RenderFramePacket (a PacketRing
// slot) that the graph binds through NriGraphContext::BindPacket and reads
// instead of the world.
//
// Later tasks fill these fields inside ExtractFrame. Grid fade was added in
// the Task 9 review (ruling T9-3) so editor.viewport.grid.fade* reaches
// PlanGrid2D. The struct is engine-internal.
//
// This header names the registry and the device only by forward declaration
// and includes no scene-camera sweep, so a consumer of the request type does
// not inherit either.

#include <Arcane/Render/GridSceneDesc.hpp>
#include <Arcane/Render/OverlayPrimitive.hpp>
#include <Arcane/Render/PostChainDesc.hpp>
#include <Arcane/Render/RenderFramePacket.hpp>
#include <Arcane/Scene/ViewTransform.hpp>
#include <Arcane/Modules/ArcaneRenderApi.hpp>

#include <Astra/Entity/Entity.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>

namespace Astra { class Registry; }

// The ImGui draw lists, by forward declaration only (as NriGraphContext.hpp
// does): ArcaneRender includes no ImGui header (ruling PR-1).
struct ImDrawData;

namespace Arcane
{
    class GpuScene;

    struct ExtractRequest
    {
        glm::uvec2                   viewport{ 0u, 0u };
        std::optional<ViewTransform> editorView;
        bool                         playMode = false;
        bool                         showGrid2d = false;
        bool                         showGrid3d = false;
        bool                         perspective = false;
        GridSceneDesc                grid3d{};
        float                        gridMinorAlpha = 0.35f;
        float                        gridMajorAlpha = 0.55f;
        // editor.viewport.grid.fade{In,Full}Px. Defaults match PlanGrid2D, so a
        // caller that leaves them unset keeps the 8..24 px crossfade.
        float                        gridFadeInPx = 8.0f;
        float                        gridFadeFullPx = 24.0f;
        glm::vec3                    gridLineRgb{ 0.5f, 0.5f, 0.5f };
        float                        gridLineThicknessPx = 1.0f;
        std::int64_t                 gridMaxLinesPerAxis = 4096;
        glm::vec4                    gridAxisX{ 0.85f, 0.25f, 0.25f, 0.9f };
        glm::vec4                    gridAxisY{ 0.3f, 0.8f, 0.3f, 0.9f };
        bool                         physicsDebug = false;
        bool                         physicsWholeWorld = false;
        std::optional<Astra::Entity> physicsSelectedBody;   // the editor's primary selection; Task 9 maps it to its body
        bool                         cameraRect = false;
        glm::vec4                    cameraFrameColor{ 0.45f, 0.62f, 0.78f, 0.75f };
        OverlayPrimitiveList         gizmoPrims{};
        const PostChainDesc*         post = nullptr;
        const ImDrawData*            imgui = nullptr;    // copied in Task 10; the pointer is not stored
        const ImDrawData*            gameUi = nullptr;
        glm::vec3                    lightDirection{ 0.0f };
        glm::vec3                    lightColor{ 0.0f };
        glm::vec3                    ambient{ 0.0f };
        GpuScene*                    device = nullptr;
    };

    // The two views ExtractFrame publishes, resolved from `req` and `reg`.
    // spriteViewFromScene is true only when an orthographic scene Camera
    // produced spriteView. An editorView fills both views from that camera
    // and leaves the flag false. No usable scene camera leaves spriteView as
    // the host's stored RenderContext2D view, still with the flag false.
    //
    // `warn` emits the once-per-process no-camera and multi-camera
    // diagnostics, and is passed through to both camera sweeps. The
    // perspective sweep's degenerate-basis warning fires on every call
    // that asks. ExtractFrame passes true. A host that resolves again
    // before DrawUI passes false, so that line is once per frame.
    struct ResolvedFrameViews
    {
        std::optional<SceneCameraView> spriteView;
        bool                           spriteViewFromScene = false;
        std::optional<ViewTransform>   meshView;
    };

    ARC_RENDER_API ResolvedFrameViews ResolveFrameViews(Astra::Registry& reg,
                                                        const ExtractRequest& req,
                                                        bool warn);

    // Counts the call (ExtractAccounting.hpp), takes the ring's next slot and
    // fills it from `reg` and `req`. The returned packet stays intact until
    // FramesInFlight() further extracts on the same ring.
    //
    // THE SPRITE BATCH: when RenderContext2D::batcher is set, the sprite
    // system submits into THAT batcher -- the host's, already Begin()'d, its
    // globals set and its materials registered -- and the drained batch is
    // copied into the packet. Extract never Begin()s it and never makes one.
    ARC_RENDER_API RenderFramePacket& ExtractFrame(Astra::Registry& reg, const ExtractRequest& req, PacketRing& ring);

    namespace Detail
    {
        // Test-only. The UserCallback skip warns once per process per list.
        // A case that counts those warns resets them first, the same way
        // ResetExtractFrameCallCount resets the extract counter. ArcaneTests
        // runs in random order.
        ARC_RENDER_API void ResetExtractUiSnapshotWarnForTest() noexcept;
    }
}
