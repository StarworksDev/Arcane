#pragma once

// The active scene camera's view, as ActiveSceneCamera (SceneCamera.hpp)
// resolves it. A header of its own (F5 plan 1, Task 4) so the render packet
// (RenderFramePacket.hpp) can carry one without pulling in the registry
// sweep and Astra::Registry.

#include <Arcane/Scene/ViewTransform.hpp>

#include <glm/vec2.hpp>

namespace Arcane
{
    struct SceneCameraView
    {
        // The authored facts, for callers that need to draw the camera rather than
        // look through it (the editor's camera-rect overlay).
        glm::vec2 worldCenter{ 0.0f, 0.0f };   // the camera entity's world position
        float     halfHeight = 0.0f;           // Camera::orthographicSize, meters

        // Derived for the viewport that was asked about: the orthographic
        // ViewTransform (+Y up; its AsAffine2D() is the pixel mapping the
        // overlays still draw through, with a NEGATIVE y scale).
        ViewTransform view{};
    };
}
