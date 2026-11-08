#pragma once
// DeferredTextureDrops (3D-readiness Plan C, N1): the device-side texture drops
// the editor raises during a frame, held until that frame's draw lists are
// recorded and submitted.
//
// WHY. The editor's ImGui draw lists name cache textures by RAW POINTER
// (ImTextureID is the nri::Texture*). A Browser row draws a texture's
// thumbnail, and later in the SAME DrawEditorUi a Delete confirm or an
// asset-op undo runs InvalidateAssetCaches. A device-wide drop there buries
// the texture at each context's last SUBMITTED fence, which the chrome frame
// still being built is not part of: the chrome Execute reaps it, and ImGuiNri's
// record then builds a view over a destroyed texture (the drop hook already
// took the old entry). The cache cannot see a draw list, so the editor holds
// the drop instead of the cache guessing.
//
// THE SAFE POINT is MainLoop right after PresentFrame, whatever it returned:
// PresentChromeFrame's RenderToDrawData has discharged the frame's ImGui, so
// no draw list built this frame can be recorded again, and the next frame's
// viewport render and UI have not begun. Holding a drop raised anywhere else in
// the frame is just as safe, so every caller raises and nothing decides per
// phase. The texture stays resident, the old picture, for at most the rest of
// the frame. CPU-side caches (the Assets artifact, the resolver) still update
// at once: nothing on the GPU reads them.
//
// A drop raised while the drain runs (a hook that raises) waits for the next
// drain rather than extending this one.
#include <Arcane/Guid.hpp>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

namespace Arcane::Editor
{
    class DeferredTextureDrops
    {
    public:
        using DropFn = std::function<void(const Arcane::Guid&)>;

        DeferredTextureDrops() = default;
        explicit DeferredTextureDrops(DropFn drop) : m_drop(std::move(drop)) {}

        // The device-wide drop the drain runs per id (EditorApp: InvalidateTextureOnDevice).
        void SetDrop(DropFn drop) { m_drop = std::move(drop); }

        // Holds `id` until the next Drain. A nil id is ignored; a repeat is held once.
        void Raise(const Arcane::Guid& id)
        {
            if (!id.IsValid())
                return;
            if (std::find(m_pending.begin(), m_pending.end(), id) == m_pending.end())
                m_pending.push_back(id);
        }

        // Runs every held drop in raise order, then forgets them. With no drop
        // installed the ids are dropped on the floor: no context, nothing to drop.
        void Drain()
        {
            std::vector<Arcane::Guid> ids;
            ids.swap(m_pending);
            if (!m_drop)
                return;
            for (const Arcane::Guid& id : ids)
                m_drop(id);
        }

        [[nodiscard]] std::size_t Pending() const noexcept { return m_pending.size(); }

    private:
        DropFn                    m_drop;
        std::vector<Arcane::Guid> m_pending;
    };
}
