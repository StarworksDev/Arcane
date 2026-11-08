#pragma once

// EditorDocument: the thin per-asset editing unit (shader-editor Slice 5, spec
// Fold 3) -- open/dirty/save lifecycle over one GUID asset. Deliberately NOT a
// docking framework: each document draws its own ImGui window; DocumentHost
// owns the list, the unsaved-close confirm flow, and the asset-type -> factory
// routing. The shader editor is the first implementation.

#include "Panels/InspectorSource.hpp"

#include <Arcane/Guid.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane::Editor
{
    // What a save GESTURE did (EditorDocument::RequestSave).
    enum class SaveGestureResult : std::uint8_t
    {
        Saved,      // Save() ran and succeeded
        Refused,    // Save() ran and failed/refused (read-only file, failed write)
        Deferred,   // parked behind a confirm (the material's save-with-errors) -- not a refusal
    };

    class EditorDocument : public InspectorSource
    {
    public:
        virtual ~EditorDocument() = default;

        virtual const std::string& Title() const = 0;     // window title (stable, unique)
        virtual Arcane::Guid AssetGuid() const = 0;       // identity (nil for unsaved-new)
        virtual bool Dirty() const = 0;
        // Whether a preview context this document owns still has a mesh/texture upload in flight
        // (Plan C Task 5): the settled-capture paths must not converge while it does.
        virtual bool PreviewUploadsPending() const noexcept { return false; }
        virtual bool Save() = 0;                          // false = save failed/refused
        // What a USER save gesture (Ctrl+S, from the document or from an
        // Inspector page showing it) runs. Default: Save(). A document with a
        // pre-save guard (the material's save-with-errors confirm) overrides
        // it; Save() itself stays unguarded for the close flow's save-then-close.
        // The result lets a caller report a refusal (the Inspector route warns).
        virtual SaveGestureResult RequestSave() { return Save() ? SaveGestureResult::Saved : SaveGestureResult::Refused; }

        // True when this document's window, or a child of it, held keyboard
        // focus at its last Draw. The document owns its own Begin/End, so it is
        // the only thing that can answer.
        //
        // Documents bind their own Ctrl+S with ImGui::Shortcut, which routes to
        // the focused window on its own and needs none of this. What needs it is
        // the APP: its scene-level Ctrl+S is a raw-scancode keybind that knows
        // nothing about ImGui's routing, so without a way to ask "is the user
        // inside a document?" one keypress would save the document AND the
        // scene. DocumentHost::FocusedDoc is that question.
        virtual bool WindowFocused() const { return false; }

        // Per-frame: advance async work (compiles, previews). dt in seconds.
        virtual void Tick(double dt) {}

        // Draw the document's ImGui window. Set `requestClose` true when the
        // user asked to close it (window X / shortcut); the HOST runs the
        // dirty-confirm flow -- documents never delete themselves.
        virtual void Draw(bool& requestClose) = 0;

        // ---- Inspector source (inspector-ownership spec s3.2) -----------
        // A document that can SELECT opts in by overriding Page()/PageFor()/
        // SelectionKey()/RestoreSelection()/Resolves() and bumping
        // SelectionEpoch() on every selection change (the host's per-frame
        // poll turns the bump into InspectorHost::NotifySelected). The
        // defaults are "this document contributes no page and never drives
        // the Inspector".
        std::string SourceName() const override { return Title(); }
        std::string_view Kind() const override { return {}; }
        InspectorPage* Page() override { return nullptr; }
        InspectorPage* PageFor(std::string_view) override { return nullptr; }
        std::string SelectionKey() const override { return {}; }
        bool RestoreSelection(std::string_view) override { return false; }
        bool Resolves(std::string_view) const override { return false; }
        // Monotonic; changes whenever the document's own selection changes.
        [[nodiscard]] virtual std::uint64_t SelectionEpoch() const { return 0; }
        // --select-in-document: select by a human path ("Player/Jump"). False
        // = the document has no such notion or the path did not resolve.
        virtual bool SelectByPath(std::string_view) { return false; }
        // DocumentHost::OpenPath resolved to THIS already-open document (a
        // double-click in the Browser, the asset page's Open, a Problems
        // locator): re-select its page as a fresh open would (spec s3 "opening
        // a document selects its default page"; final fix R). A plain tab
        // click never calls this. Default: nothing to re-select.
        virtual void NoteReopened() {}
        // T5 s7.11: the file moved. Assign the path, recompute the stem-fallback title, rebuild the label keeping
        // "###<kind>doc_<guid>" (the dock slot holds). PURE, so no new kind can keep the save-writes-the-old-path bug.
        virtual void NoteMoved(const std::filesystem::path& newPath) = 0;
        // T5 s7.5: unsaved data's references, from typed data
        virtual std::vector<Arcane::Guid> LiveReferences() const { return {}; }

        // Commit-close any parked edit gesture NOW. Play entry calls it on
        // every document (spec s3.3(b), drafting pick 9.28.5), so a drag open
        // when Play starts lands as one Edit-mode step before InPlayMode() flips.
        virtual void FlushGesture() {}
    };

    // The Inspector's Ctrl+S (InspectorWindowsResult::saveRequested): the
    // source is a document -> its save GESTURE. `doc` is null for the scene
    // and asset sources (nothing saved). The app's route and the tests share
    // it; the route warns on Refused (integration residual 2b).
    struct InspectorSaveOutcome
    {
        EditorDocument* doc = nullptr;
        SaveGestureResult result = SaveGestureResult::Saved;   // meaningful only with a doc
    };
    inline InspectorSaveOutcome RequestSaveFromInspector(InspectorSource* src)
    {
        InspectorSaveOutcome out;
        out.doc = dynamic_cast<EditorDocument*>(src);
        if (out.doc) out.result = out.doc->RequestSave();   // the guarded gesture, never the raw Save (final fix S)
        return out;
    }
}
