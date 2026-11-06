#pragma once
// ModalGate: at most ONE dockspace-level modal draws per frame (DrawModals).
// ImGui allows exactly one popup at a given stack level, and every modal in
// DrawModals re-arms itself ("if (!IsPopupOpen) OpenPopup"), so two of them up
// at once close each other every frame and both end up invisible. The gate
// names the winner: a queued error first, then a parked scene intent (the
// unsaved-scene confirm), then a document's unsaved-changes confirm (drawn by
// DocumentHost::DrawAll, earlier in the frame), then the create dialog. A loser keeps its state and
// draws again once everything above it is clear. Pure -- drawing stays host-side.
#include <cstdint>

namespace Arcane::Editor
{
    enum class ModalKind : std::uint8_t { None, Error, UnsavedScene, UnsavedDocument, CreateAsset };

    [[nodiscard]] inline ModalKind ModalAllowed(bool errorQueued,
                                                bool sceneIntentParked,
                                                bool documentUnsavedPending,
                                                bool createOpen)
    {
        if (errorQueued)        return ModalKind::Error;
        if (sceneIntentParked)  return ModalKind::UnsavedScene;
        if (documentUnsavedPending) return ModalKind::UnsavedDocument;
        if (createOpen)         return ModalKind::CreateAsset;
        return ModalKind::None;
    }
}
