#pragma once
#include <Arcane/Modules/ArcaneImGuiApi.hpp>

// The ImGui side of the render packet's draw-data copy (F5 plan 1 Task 4,
// ruling PR-1). The snapshot TYPES are Render-owned plain data
// (Arcane/Render/RenderFramePacket.hpp) so ArcaneRender never includes
// <imgui.h>; the two functions that read or write ImGui types live here.
//
// CopyImDrawSnapshot: extract's copy, reached through
// RenderOverlayHooks::snapshot (InstallImGuiHostHooks installs it). A command
// that carries a UserCallback is skipped (the NRI renderer asserts there is
// none) and the function returns false; every other command and every list
// stays, and `out`'s buffers keep their capacity. A null src clears `out` in
// place (listCount 0; the list elements stay) and returns true. Readers use
// listCount, not lists.size(): elements past listCount are retained storage.
// A managed command (TexRef._TexData set)
// keeps that pointer and never calls GetTexID: on the first frame and on an
// atlas rebuild the TexID is still invalid until declaration-time
// NewFrameTexUpdates stamps it.
//
// ImDrawSnapshotRebuild: an ImDrawData built by direct field assignment over
// lists this object owns (never AddDrawList: its write-pointer asserts do not
// hold for a hand-filled list). Data()->Textures points at this object's own
// vector of the snapshot's ImTextureData pointers, so it stays valid for the
// rebuild's lifetime; the ImTextureData objects are the live ones.

#include <Arcane/Render/RenderFramePacket.hpp>

#include <imgui.h>

#include <vector>

namespace Arcane
{
    ARC_IMGUI_API bool CopyImDrawSnapshot(const ImDrawData* src, ImDrawSnapshot& out);

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4251)  // std members on a dll-exported class: benign under /MD
#endif
    class ARC_IMGUI_API ImDrawSnapshotRebuild
    {
    public:
        explicit ImDrawSnapshotRebuild(const ImDrawSnapshot& snap);
        ~ImDrawSnapshotRebuild();
        ImDrawData* Data() { return &m_data; }

        ImDrawSnapshotRebuild(const ImDrawSnapshotRebuild&) = delete;
        ImDrawSnapshotRebuild& operator=(const ImDrawSnapshotRebuild&) = delete;

    private:
        ImDrawData m_data{};
        ImVector<ImTextureData*> m_textures;
        std::vector<ImDrawList*> m_owned;
    };
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}
