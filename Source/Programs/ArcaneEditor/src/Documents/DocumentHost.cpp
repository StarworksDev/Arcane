#include "Documents/DocumentHost.hpp"

#include <Arcane/Base/Log.hpp>

#include <imgui.h>

#include <algorithm>
#include <cctype>

namespace Arcane::Editor
{
    namespace
    {
        std::string LowerExt(const std::filesystem::path& p)
        {
            std::string ext = p.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return ext;
        }
    }

    void DocumentHost::RegisterFactory(std::string extension, OpenFactory factory,
                                       PeekGuid peek)
    {
        m_factories.push_back(Route{ std::move(extension), std::move(factory),
                                     std::move(peek) });
    }

    EditorDocument* DocumentHost::OpenPath(const std::filesystem::path& path)
    {
        const std::string ext = LowerExt(path);
        for (const Route& route : m_factories)
        {
            if (route.ext != ext)
                continue;
            // Focus-not-reopen, cheap form first (review m4): resolve the asset
            // identity WITHOUT constructing a document -- a discarded construct
            // is not free (compile submits on the live doc's coalesce keys).
            // Every path below arms m_focusRequest: "open this asset" means the
            // user wants to be LOOKING AT it, whether that opens a window or
            // re-surfaces one already open behind another tab.
            if (route.peek)
                if (const Arcane::Guid peeked = route.peek(path); peeked.IsValid())
                    if (EditorDocument* open = FindByGuid(peeked))
                    {
                        open->NoteReopened();   // re-select its page like a fresh open (final fix R)
                        return m_focusRequest = open;
                    }
            std::unique_ptr<EditorDocument> doc = route.factory(path);
            if (!doc)
                return nullptr;   // factory already logged the cause
            // Fallback dedup for peek-less routes.
            if (doc->AssetGuid().IsValid())
                if (EditorDocument* open = FindByGuid(doc->AssetGuid()))
                {
                    open->NoteReopened();
                    return m_focusRequest = open;
                }
            return m_focusRequest = Add(std::move(doc));
        }
        ARC_WARN("DocumentHost: no editor registered for '{}'", ext);
        return nullptr;
    }

    bool DocumentHost::HasFactory(const std::filesystem::path& path) const
    {
        const std::string ext = LowerExt(path);
        return std::any_of(m_factories.begin(), m_factories.end(), [&](const Route& r) { return r.ext == ext; });
    }

    EditorDocument* DocumentHost::Add(std::unique_ptr<EditorDocument> doc)
    {
        m_docs.push_back(std::move(doc));
        if (m_observer.opened) m_observer.opened(*m_docs.back());
        return m_docs.back().get();
    }

    EditorDocument* DocumentHost::FindByGuid(const Arcane::Guid& guid)
    {
        for (const auto& d : m_docs)
            if (d->AssetGuid() == guid)
                return d.get();
        return nullptr;
    }

    void DocumentHost::NoteAssetMoved(const Arcane::Guid& g, const std::filesystem::path& p) { if (EditorDocument* d = FindByGuid(g)) d->NoteMoved(p); }

    void DocumentHost::CloseForAssetRemoval(EditorDocument* doc)   // T5 s7.5: unsaved, no confirm; a parked gesture commits in the dtor
    {
        if (!doc) return;
        if (m_pendingClose == doc) m_pendingClose = nullptr;
        if (m_focusRequest == doc) m_focusRequest = nullptr;
        if (m_lastActive == doc) m_lastActive = nullptr;
        Close(doc);
    }

    bool DocumentHost::AnyDirty() const
    {
        for (const auto& d : m_docs)
            if (d->Dirty())
                return true;
        return false;
    }

    std::size_t DocumentHost::SaveAllDirty()
    {
        std::size_t failed = 0;
        for (const auto& d : m_docs)
            if (d->Dirty() && !d->Save())
                ++failed;
        return failed;
    }

    EditorDocument* DocumentHost::FocusedDoc() const
    {
        // At most one window holds focus, so the first hit is the answer.
        for (const auto& d : m_docs)
            if (d->WindowFocused())
                return d.get();
        return nullptr;
    }

    EditorDocument* DocumentHost::CloseTarget() const
    {
        if (EditorDocument* focused = FocusedDoc()) return focused;
        return m_lastActive;
    }

    void DocumentHost::NoteFocus()
    {
        if (EditorDocument* focused = FocusedDoc()) m_lastActive = focused;
    }

    void DocumentHost::RequestClose(EditorDocument* doc)
    {
        if (!doc || m_pendingClose)
            return;   // one confirm at a time
        if (!doc->Dirty())
        {
            Close(doc);
            return;
        }
        m_pendingClose = doc;
    }

    void DocumentHost::ConfirmSaveAndClose()
    {
        if (!m_pendingClose)
            return;
        if (!m_pendingClose->Save())
            return;   // failed/refused save keeps it open AND pending
        EditorDocument* doc = m_pendingClose;
        m_pendingClose = nullptr;
        Close(doc);
    }

    void DocumentHost::ConfirmDiscard()
    {
        if (!m_pendingClose)
            return;
        EditorDocument* doc = m_pendingClose;
        m_pendingClose = nullptr;
        Close(doc);
    }

    void DocumentHost::CancelClose()
    {
        m_pendingClose = nullptr;
    }

    void DocumentHost::CloseAll()
    {
        m_pendingClose = nullptr;
        m_lastActive = nullptr;
        if (m_observer.closing) for (const auto& d : m_docs) m_observer.closing(*d);
        m_docs.clear();
        m_dockPlaced.clear();
    }

    void DocumentHost::Close(EditorDocument* doc)
    {
        if (m_lastActive == doc) m_lastActive = nullptr;
        m_dockPlaced.erase(doc);   // a reopen docks fresh again
        if (m_observer.closing) m_observer.closing(*doc);
        m_docs.erase(std::remove_if(m_docs.begin(), m_docs.end(),
                                    [doc](const auto& d) { return d.get() == doc; }),
                     m_docs.end());
    }

    void DocumentHost::TickAll(double dt)
    {
        for (const auto& d : m_docs)
            d->Tick(dt);
    }

    void DocumentHost::FlushGestures()
    {
        for (const auto& d : m_docs)
            d->FlushGesture();
    }

    void DocumentHost::DrawAll(unsigned int dockId, bool modalBlocked)
    {
        // Snapshot the pointers: a close request mutates m_docs after the loop.
        std::vector<EditorDocument*> toClose;
        for (const auto& d : m_docs)
        {
            // First draw after OPENING forces the document into the target
            // node (Cond_Always -- the imgui.ini remembers pre-docking
            // floating placements for reopened assets, so FirstUseEver would
            // silently lose). One frame only: afterwards the user's drags own
            // the window for the document's lifetime.
            if (dockId != 0 && m_dockPlaced.insert(d.get()).second)
                ImGui::SetNextWindowDockID(static_cast<ImGuiID>(dockId),
                                           ImGuiCond_Always);
            // The CENTER document is the thing the user types into, so it gets
            // real window focus -- which is also what makes its dock node
            // select its tab (imgui.cpp:19611-19613 applies g.NavWindow back as
            // the node's selection). Side panels must NOT use this; they switch
            // by tab selection alone (EditorPanels' SelectDockTab), or the last
            // focus call in the frame steals the center selection from here.
            if (d.get() == m_focusRequest)
            {
                ImGui::SetNextWindowFocus();
                m_focusRequest = nullptr;
            }
            bool requestClose = false;
            d->Draw(requestClose);
            if (requestClose)
                toClose.push_back(d.get());
        }
        for (EditorDocument* d : toClose)
            RequestClose(d);
        NoteFocus();

        // Deliberately NOT folded into EditorApp's ModalErrorQueue -- DocumentHost
        // is self-contained; see the architecture-pass spec sec 7.
        if (m_pendingClose && !modalBlocked)
        {
            ImGui::OpenPopup("Unsaved Changes##dochost");
            if (ImGui::BeginPopupModal("Unsaved Changes##dochost", nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::Text("Save changes to '%s' before closing?",
                            m_pendingClose->Title().c_str());
                ImGui::Separator();
                if (ImGui::Button("Save"))
                {
                    ImGui::CloseCurrentPopup();
                    ConfirmSaveAndClose();
                }
                ImGui::SameLine();
                if (ImGui::Button("Discard"))
                {
                    ImGui::CloseCurrentPopup();
                    ConfirmDiscard();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel"))
                {
                    ImGui::CloseCurrentPopup();
                    CancelClose();
                }
                ImGui::EndPopup();
            }
        }
    }
}
