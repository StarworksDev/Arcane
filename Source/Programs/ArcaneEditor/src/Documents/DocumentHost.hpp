#pragma once

// DocumentHost: the editor's open-document list + the unsaved-close confirm
// state machine + the asset-type -> open-document routing registry (shader-
// editor Slice 5, spec Fold 3). The close flow is PURE state (unit-tested
// headless, ConsoleBuffer pattern); DrawAll is the only ImGui-touching method
// (windows + the confirm modal). One pending close at a time -- a second
// request while one is pending is ignored (the modal is app-modal anyway).

#include "Documents/EditorDocument.hpp"

#include <Arcane/Guid.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace Arcane::Editor
{
    class DocumentHost
    {
    public:
        using OpenFactory =
            std::function<std::unique_ptr<EditorDocument>(const std::filesystem::path&)>;
        // Cheap asset-identity probe (no document construction). Nil = unknown.
        using PeekGuid = std::function<Arcane::Guid(const std::filesystem::path&)>;

        // Lifecycle observer (inspector-ownership arc): the app registers each
        // opened document as an Inspector source and releases it on close.
        // `closing` fires BEFORE the document is destroyed (Close and CloseAll
        // both), so the observer may still read its identity.
        struct DocumentObserver
        {
            std::function<void(EditorDocument&)> opened;
            std::function<void(EditorDocument&)> closing;
        };
        void SetObserver(DocumentObserver observer) { m_observer = std::move(observer); }

        // ---- routing (Fold 3 skeleton) ----------------------------------
        // Register a factory for an asset extension (".arcmat"). Lowercase match.
        // `peek` (optional) lets OpenPath resolve focus-not-reopen BEFORE
        // constructing a document -- constructing one just to discard it is not
        // free (a ShaderEditorDocument's ctor submits compiles on the live
        // document's coalesce keys, cancelling its in-flight work; review m4).
        void RegisterFactory(std::string extension, OpenFactory factory,
                             PeekGuid peek = nullptr);
        // Open a path through its extension's factory. Focus-not-reopen: when a
        // document with the same asset Guid is already open, returns it instead
        // and calls its NoteReopened() (an explicit open re-selects its page).
        // Null when no factory matches or the factory fails.
        EditorDocument* OpenPath(const std::filesystem::path& path);
        // True when an extension factory would take `path` (RegisterFactory's
        // lowercase match) -- the Problems router's "has an editor" fact.
        [[nodiscard]] bool HasFactory(const std::filesystem::path& path) const;

        // ---- list --------------------------------------------------------
        EditorDocument* Add(std::unique_ptr<EditorDocument> doc);
        EditorDocument* FindByGuid(const Arcane::Guid& guid);
        // T5 s7.11: the asset `guid` moved to `newPath`; the open document showing it (if any) retargets
        // through NoteMoved, so its next save writes the new path. No document open = nothing to do.
        void NoteAssetMoved(const Arcane::Guid& guid, const std::filesystem::path& newPath);
        // T5 s7.5: the asset under `doc` is being deleted. Close it UNSAVED with no confirm (a pending confirm on
        // it is dropped); a parked edit gesture commits in the document's destructor. Null = nothing to do.
        void CloseForAssetRemoval(EditorDocument* doc);
        std::size_t Count() const { return m_docs.size(); }
        bool AnyDirty() const;
        // Save every dirty document in place. Every document has a real path
        // (they are only born from files via OpenPath), so no dialog is ever
        // needed here -- unlike the scene. Returns how many saves FAILED
        // (0 = everything clean now); a failed save keeps its dirty flag.
        std::size_t SaveAllDirty();
        // The document whose window held focus at the last DrawAll, or null when
        // the user is not in one. This is what Ctrl+S means by "this document";
        // the app routes the shortcut through it so a focused asset editor owns
        // the key and the scene save stands down. Reads one frame behind (the
        // keybind phase runs before DrawAll), which focus tolerates -- a click
        // that focuses a document lands a frame before any keypress can.
        EditorDocument* FocusedDoc() const;
        // Ctrl+W targets the focused document, or the last one that held focus.
        [[nodiscard]] EditorDocument* CloseTarget() const;
        // Called after drawing documents; tests also use it with fake focus.
        void NoteFocus();

        // ---- close flow (pure, testable) ----------------------------------
        // Ask to close `doc`: clean documents close immediately; dirty ones
        // park as the pending confirm (resolved by the Confirm*/CancelClose
        // calls -- DrawAll renders them as a modal).
        void RequestClose(EditorDocument* doc);
        bool HasPendingConfirm() const { return m_pendingClose != nullptr; }
        EditorDocument* PendingConfirmDoc() const { return m_pendingClose; }
        // Save-then-close; a FAILED save keeps the document open (and pending,
        // so the UI can surface the failure rather than dropping edits).
        void ConfirmSaveAndClose();
        void ConfirmDiscard();   // close without saving
        void CancelClose();      // keep the document open
        // Close EVERYTHING unconditionally (destroys dirty documents too --
        // callers gate on AnyDirty() first). Project-switch teardown.
        void CloseAll();

        // ---- per-frame -----------------------------------------------------
        void TickAll(double dt);
        void FlushGestures();   // FlushGesture on every open document (Play entry)
        // Draw every document window + the pending-close confirm modal.
        // `dockId` (an ImGuiID; 0 = none): the dock node a document window is
        // FORCED into on its first draw after opening -- the host passes the
        // Viewport's node, so asset editors always OPEN as tabs in the main
        // area (UE/Unity shape), overriding any stale floating placement the
        // imgui.ini remembers. After that first frame the user's drags win
        // for the document's lifetime.
        //
        // `modalBlocked`: another dockspace-level modal (a queued error, a parked
        // scene intent) owns the frame (ModalGate.hpp). The pending-close confirm
        // then neither re-arms nor draws -- two modals at popup level 0 close each
        // other every frame and both go invisible. Its state is untouched, so it
        // draws again once the gate clears.
        void DrawAll(unsigned int dockId = 0, bool modalBlocked = false);

        // Visit every open document (compile-result routing and the like).
        template <typename F>
        void ForEach(F&& fn)
        {
            for (const auto& d : m_docs)
                fn(*d);
        }

    private:
        void Close(EditorDocument* doc);   // erase from the list (destroys it)

        struct Route
        {
            std::string ext;
            OpenFactory factory;
            PeekGuid    peek;   // may be null
        };

        std::vector<std::unique_ptr<EditorDocument>> m_docs;
        std::vector<Route> m_factories;
        DocumentObserver m_observer;
        EditorDocument* m_pendingClose = nullptr;
        // The document OpenPath last resolved, owed a window focus on its next
        // draw. Covers BOTH open paths: a brand-new document (which ImGui would
        // focus itself anyway, imgui.cpp:8320-8326/:8617) and the
        // focus-not-reopen hit, where an ALREADY-OPEN document is returned and
        // nothing would otherwise bring its tab forward. Cleared when consumed,
        // and in Close so it can never name a destroyed document.
        EditorDocument* m_focusRequest = nullptr;
        EditorDocument* m_lastActive = nullptr;
        // Documents that already received their initial dock placement (see
        // DrawAll). Erased on close so a reopen docks fresh again.
        std::unordered_set<const EditorDocument*> m_dockPlaced;
    };
}
